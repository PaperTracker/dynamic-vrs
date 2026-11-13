#include "render_target_hook.hpp"
#include <windows.h>

#include "common/log.hpp"
#include "layer/diagnostics/submit_diagnostics.hpp"
#include "vrs_binding.hpp"
#include "vrs_state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace dvr::layer {
namespace {

using ContextOmSetRenderTargetsFunction = void(__fastcall*)(ID3D11DeviceContext* context, UINT renderTargetViewCount,
                                                            ID3D11RenderTargetView* const* renderTargetViews,
                                                            ID3D11DepthStencilView* depthStencilView);

template <typename Function> struct ContextMethodHook {
    ID3D11DeviceContext* owner = nullptr;
    void** slot = nullptr;
    std::atomic<Function> original = nullptr;
    ContextMethodHook* next = nullptr;
    std::vector<Function> observed;
};
// Records have stable addresses until process exit. Readers never take the render
// mutex before forwarding the original call; different contexts keep their own target.
template <typename Function> struct ContextHookState {
    std::vector<std::unique_ptr<ContextMethodHook<Function>>> storage;
    std::atomic<ContextMethodHook<Function>*> head = nullptr;
    std::atomic<Function> fallback = nullptr;
};
using ContextRsSetViewportsFunction = void(__fastcall*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
ContextHookState<ContextOmSetRenderTargetsFunction> g_renderTargetHooks;
ContextHookState<ContextRsSetViewportsFunction> g_viewportHooks;

template <typename Function>
Function originalContextCall(const ContextHookState<Function>& state, ID3D11DeviceContext* context, size_t index) {
    auto** slot = &(*reinterpret_cast<void***>(context))[index];
    auto* head = state.head.load(std::memory_order_acquire);
    for (auto* record = head; record; record = record->next)
        if (record->slot == slot) return record->original.load(std::memory_order_acquire);
    // A dispatch table may change between the caller fetching our entry and
    // entering it. Preserve the original associated with that live context.
    for (auto* record = head; record; record = record->next)
        if (record->owner == context) return record->original.load(std::memory_order_acquire);
    return state.fallback.load(std::memory_order_acquire);
}

template <typename Function> bool isD3dRuntimeEntry(Function function) {
    HMODULE owner = nullptr;
    return function &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(function), &owner) &&
           owner == GetModuleHandleW(L"d3d11.dll");
}

void __fastcall contextOmSetRenderTargets(ID3D11DeviceContext* context, UINT renderTargetViewCount,
                                          ID3D11RenderTargetView* const* renderTargetViews,
                                          ID3D11DepthStencilView* depthStencilView) {
    if (RenderTargetHookBypass::active()) {
        if (const auto original = originalContextCall(g_renderTargetHooks, context, 33))
            original(context, renderTargetViewCount, renderTargetViews, depthStencilView);
        return;
    }
    ContextThreadGuard deviceLock(context);
    if (g_alignmentArmed.load()) {
        std::scoped_lock lock(g_vrsMutex);
        observeAlignmentTargetLocked(context);
    }
    if (const auto original = originalContextCall(g_renderTargetHooks, context, 33))
        original(context, renderTargetViewCount, renderTargetViews, depthStencilView);
    std::scoped_lock lock(g_vrsMutex);
    applyVrsToTargetsLocked(context, renderTargetViewCount, renderTargetViews);
    if (g_alignmentArmed.load()) observeAlignmentTargetLocked(context);
    if (context == g_vrsState.context) installContextRenderTargetHook(context);
}

void __fastcall contextRsSetViewports(ID3D11DeviceContext* context, UINT count, const D3D11_VIEWPORT* viewports) {
    if (RenderTargetHookBypass::active()) {
        if (const auto original = originalContextCall(g_viewportHooks, context, 44))
            original(context, count, viewports);
        return;
    }
    ContextThreadGuard deviceLock(context);
    if (const auto original = originalContextCall(g_viewportHooks, context, 44)) original(context, count, viewports);
    std::scoped_lock lock(g_vrsMutex);
    if (context != g_vrsState.context) return;
    ID3D11RenderTargetView* current = nullptr;
    context->OMGetRenderTargets(1, &current, nullptr);
    applyVrsToTargetsLocked(context, current ? 1U : 0U, &current);
    if (current) current->Release();
    if (g_alignmentArmed.load()) observeAlignmentTargetLocked(context);
    installContextRenderTargetHook(context);
}

template <typename Function>
bool installContextMethod(ContextHookState<Function>& state, ID3D11DeviceContext* context, size_t index,
                          Function replacement, const char* method) {

    auto** vtable = *reinterpret_cast<void***>(context);
    void** slot = &vtable[index];
    const auto current = reinterpret_cast<Function>(*slot);
    if (current == replacement) return true;
    // D3D11 legitimately rewrites its per-context dispatch table (e.g. when
    // changing multithread protection). Only rebase onto an actual D3D11 entry;
    // never wrap an unknown third-party hook that may already chain back to us.
    if (!isD3dRuntimeEntry(current)) {
        static std::array<std::pair<void*, void*>, 8> reported{};
        static size_t reportedCount = 0;
        const std::pair<void*, void*> key{context, reinterpret_cast<void*>(current)};
        if (reportedCount < reported.size() &&
            std::find(reported.begin(), reported.begin() + reportedCount, key) == reported.begin() + reportedCount) {
            reported[reportedCount++] = key;
            logMessage("stage=vrs-context-hook result=foreign-entry context=%p current=%p method=%s", context,
                       reinterpret_cast<void*>(current), method);
        }
        return false;
    }
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return false;
    auto* record = state.head.load(std::memory_order_acquire);
    while (record && record->slot != slot) record = record->next;
    const bool first = record == nullptr;
    if (!record) {
        auto created = std::make_unique<ContextMethodHook<Function>>();
        created->owner = context;
        created->slot = slot;
        created->original.store(current, std::memory_order_relaxed);
        created->next = state.head.load(std::memory_order_relaxed);
        record = created.get();
        state.storage.push_back(std::move(created));
        state.head.store(record, std::memory_order_release);
    } else {
        record->original.store(current, std::memory_order_release);
    }
    Function empty = nullptr;
    state.fallback.compare_exchange_strong(empty, current);
    const auto prior =
        InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(current));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    if (prior != reinterpret_cast<void*>(current) && prior != reinterpret_cast<void*>(replacement))
        return false; // Another dispatch change won the race; observe it next frame.
    if (std::find(record->observed.begin(), record->observed.end(), current) == record->observed.end()) {
        record->observed.push_back(current);
        logMessage("stage=vrs-context-hook result=%s context=%p slot=%p original=%p method=%s",
                   first ? "installed" : "runtime-dispatch-rearmed", context, slot, reinterpret_cast<void*>(current),
                   method);
    }
    return true;
}

} // namespace

bool installContextRenderTargetHook(ID3D11DeviceContext* context) {
    const bool targets =
        installContextMethod(g_renderTargetHooks, context, 33, &contextOmSetRenderTargets, "OMSetRenderTargets");
    installContextMethod(g_viewportHooks, context, 44, &contextRsSetViewports, "RSSetViewports");
    return targets;
}

} // namespace dvr::layer
