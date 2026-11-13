#include <windows.h>
#include <d3d11.h>

#include "common/log.hpp"
#include "common/openvr_abi.hpp"
#include "common/paths.hpp"
#include "layer/config/layer_config.hpp"
#include "layer/control/panel_launcher.hpp"

#ifdef DVR_HAS_NVAPI
#include "layer/control/frame_control.hpp"
#include "layer/diagnostics/submit_diagnostics.hpp"
#include "layer/vrs/vrs_binding.hpp"
#include "layer/vrs/vrs_state.hpp"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <utility>

// The layer's entry points and the IVRCompositor::Submit hooks. Everything VRS-related happens after a
// successful Submit, in the modules above.

namespace dvr::layer {
namespace {

std::once_flag g_initializeOnce;
std::atomic<bool> g_initialized = false;
std::atomic<unsigned int> g_submitCount = 0;
std::atomic<void*> g_lastLeftHandle = nullptr;
std::atomic<bool> g_controlReady = false;
std::once_flag g_controlPanelOnce;
// Every IVRCompositor version whose Submit takes (eye, texture, bounds, flags). A game uses the one its
// OpenVR SDK was built with, and each has its own vtable in SteamVR's client.
constexpr int FirstCompositorVersion = 16;
constexpr int LastCompositorVersion = 29;
constexpr std::size_t CompositorVersionCount = LastCompositorVersion - FirstCompositorVersion + 1;
CompositorSubmitFunction g_originalSubmit[CompositorVersionCount] = {};
char g_compositorNames[CompositorVersionCount][24] = {};
thread_local unsigned int g_submitDepth = 0;

#ifndef DVR_HAS_NVAPI
void enableFixedVrsForNextFrame(int, const OpenVrTexture*, const OpenVrTextureBounds*, bool = true) {
    if (g_settings.mode != VrsMode::Off) {
        static std::once_flag warningOnce;
        std::call_once(warningOnce,
                       [] { logMessage("stage=vrs-bind result=unavailable reason=layer-built-without-nvapi"); });
    }
}
#endif

void logSubmit(unsigned int count, const char* interfaceName, int eye, const OpenVrTexture* texture,
               const OpenVrTextureBounds* bounds, int submitFlags) {
    if (count >= 32) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    bool hasD3d11Desc = false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (texture && texture->type == 0 && texture->handle) {
        auto* d3dTexture = static_cast<ID3D11Texture2D*>(texture->handle);
        d3dTexture->GetDesc(&desc);
        d3dTexture->GetDevice(&device);
        if (device) {
            device->GetImmediateContext(&context);
        }
        hasD3d11Desc = true;
    }

    bool sharesDoubleWideTexture = false;
    if (eye == 0) {
        g_lastLeftHandle.store(texture ? texture->handle : nullptr, std::memory_order_relaxed);
    } else if (eye == 1 && texture) {
        sharesDoubleWideTexture = texture->handle == g_lastLeftHandle.load(std::memory_order_relaxed);
    }

    logMessage("stage=submit interface=%s count=%u eye=%d texture=%p handle=%p type=%d "
               "colorSpace=%d flags=%d bounds=%s%.4f,%.4f,%.4f,%.4f d3d11=%d "
               "width=%u height=%u array=%u mips=%u samples=%u format=%u bind=0x%X "
               "misc=0x%X device=%p context=%p sharesDoubleWide=%d",
               interfaceName, count, eye, texture, texture ? texture->handle : nullptr, texture ? texture->type : -1,
               texture ? texture->colorSpace : -1, submitFlags, bounds ? "" : "<null>", bounds ? bounds->uMin : 0.0f,
               bounds ? bounds->vMin : 0.0f, bounds ? bounds->uMax : 0.0f, bounds ? bounds->vMax : 0.0f,
               hasD3d11Desc ? 1 : 0, desc.Width, desc.Height, desc.ArraySize, desc.MipLevels, desc.SampleDesc.Count,
               static_cast<unsigned int>(desc.Format), desc.BindFlags, desc.MiscFlags, device, context,
               sharesDoubleWideTexture ? 1 : 0);

    if (context) {
        context->Release();
    }
    if (device) {
        device->Release();
    }
}

int forwardSubmit(const char* interfaceName, CompositorSubmitFunction original, void* compositor, int eye,
                  const OpenVrTexture* texture, const OpenVrTextureBounds* bounds, int submitFlags) {
    const bool topLevel = g_submitDepth++ == 0;
    if (topLevel) {
        const unsigned int count = g_submitCount.fetch_add(1, std::memory_order_relaxed);
        logSubmit(count, interfaceName, eye, texture, bounds, submitFlags);
    }
    bool visualAidDrawn = false;
#ifdef DVR_HAS_NVAPI
    if (topLevel) {
        captureAlignmentSubmission(eye, texture);
        visualAidDrawn = drawSubmissionVisualAid(eye, texture, bounds);
    }
#endif
    // Preserve the original submission and its opaque pose/depth/motion metadata.
    const int result = original ? original(compositor, eye, texture, bounds, submitFlags) : 1;
    if (visualAidDrawn) {
        static std::atomic<unsigned int> diagnosticSubmits = 0;
        if (diagnosticSubmits.fetch_add(1, std::memory_order_relaxed) < 8U) {
            logMessage("stage=debug-blur-submit eye=%d flags=%d compositorResult=%d", eye, submitFlags, result);
        }
    }
    --g_submitDepth;
    if (topLevel && result == 0) {
        bool completedFrame = eye == 1;
#ifdef DVR_HAS_NVAPI
        completedFrame = finishRuntimeControlFrame(eye, texture);
#endif
        enableFixedVrsForNextFrame(eye, texture, bounds, completedFrame);
#ifdef DVR_HAS_NVAPI
        // Overlay and utility applications load the layer too but never submit eye textures, so the
        // panel waits for the first frame a game completes.
        if (completedFrame && g_controlReady.load(std::memory_order_acquire)) {
            std::call_once(g_controlPanelOnce, [] {
                logMessage("stage=control-panel result=%s",
                           launchControlPanel(layerConfigPath(), thisModuleDirectory()));
            });
        }
#endif
    }
    return result;
}

template <std::size_t Index>
int __fastcall compositorSubmit(void* compositor, int eye, const OpenVrTexture* texture,
                                const OpenVrTextureBounds* bounds, int submitFlags) {
    return forwardSubmit(g_compositorNames[Index], g_originalSubmit[Index], compositor, eye, texture, bounds,
                         submitFlags);
}

template <std::size_t... Index>
std::array<CompositorSubmitFunction, sizeof...(Index)> submitReplacements(std::index_sequence<Index...>) {
    return {&compositorSubmit<Index>...};
}

bool patchSubmit(const char* interfaceName, std::size_t submitIndex, GetGenericInterfaceFunction getInterface,
                 CompositorSubmitFunction replacement, CompositorSubmitFunction& original) {
    int interfaceError = 0;
    void* object = getInterface(interfaceName, &interfaceError);
    if (!object) {
        return false; // SteamVR dropped this version, or does not have it yet.
    }

    auto** vtable = *reinterpret_cast<void***>(object);
    void** submitSlot = &vtable[submitIndex];
    original = reinterpret_cast<CompositorSubmitFunction>(*submitSlot);
    if (original == replacement) {
        logMessage("stage=hook interface=%s result=already-installed slot=%p", interfaceName, submitSlot);
        return true;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(submitSlot, sizeof(*submitSlot), PAGE_READWRITE, &oldProtection)) {
        logMessage("stage=hook interface=%s result=protect-failed slot=%p win32Error=%lu", interfaceName, submitSlot,
                   GetLastError());
        original = nullptr;
        return false;
    }

    InterlockedExchangePointer(submitSlot, reinterpret_cast<void*>(replacement));
    FlushInstructionCache(GetCurrentProcess(), submitSlot, sizeof(*submitSlot));
    DWORD ignoredProtection = 0;
    VirtualProtect(submitSlot, sizeof(*submitSlot), oldProtection, &ignoredProtection);
    logMessage("stage=hook interface=%s result=installed object=%p vtable=%p slot=%p "
               "original=%p replacement=%p",
               interfaceName, object, vtable, submitSlot, reinterpret_cast<void*>(original),
               reinterpret_cast<void*>(replacement));
    return true;
}

bool initializeLayer() {
    loadLayerSettings();
    const auto initialMode = g_settings.mode;
    HMODULE openVr = GetModuleHandleW(L"openvr_api.dll");
    if (!openVr) {
        logMessage("stage=initialize result=openvr-missing");
        return false;
    }
    const auto getInterface =
        reinterpret_cast<GetGenericInterfaceFunction>(GetProcAddress(openVr, "VR_GetGenericInterface"));
    if (!getInterface) {
        logMessage("stage=initialize result=export-missing");
        return false;
    }

    const auto replacements = submitReplacements(std::make_index_sequence<CompositorVersionCount>{});
    unsigned int hooked = 0;
    for (std::size_t index = 0; index < CompositorVersionCount; ++index) {
        const int version = FirstCompositorVersion + static_cast<int>(index);
        snprintf(g_compositorNames[index], sizeof(g_compositorNames[index]), "IVRCompositor_%03d", version);
        // IVRCompositor_029 put GetSubmitTexture in front of Submit.
        if (patchSubmit(g_compositorNames[index], version >= 29 ? 6 : 5, getInterface, replacements[index],
                        g_originalSubmit[index]))
            ++hooked;
    }
    const bool initialized = hooked != 0;
#ifdef DVR_HAS_NVAPI
    if (initialized) {
        std::scoped_lock lock(g_vrsMutex);
        const bool controlReady = g_runtimeControl.start(runtimeSettingsLocked());
        logMessage("stage=vrs-control result=%s pid=%lu protocol=%u", controlReady ? "ready" : "unavailable",
                   GetCurrentProcessId(), dvr::control::Version);
        g_controlReady.store(controlReady, std::memory_order_release);
    }
#endif
    logMessage("stage=initialize result=%s hookedVersions=%u nativeVrsMode=%s",
               initialized ? "ready" : "no-supported-interface", hooked,
               initialMode == VrsMode::Gaze ? "gaze" : (initialMode == VrsMode::FixedCenter ? "fixed" : "off"));
    return initialized;
}

int initializeOnce(const std::filesystem::path& configPath) {
    std::call_once(g_initializeOnce, [&configPath] {
        g_configPath = configPath;
        g_initialized.store(initializeLayer(), std::memory_order_release);
    });
    return g_initialized.load(std::memory_order_acquire) ? 1 : 0;
}

} // namespace
} // namespace dvr::layer

using dvr::logMessage;
using dvr::layer::initializeOnce;

extern "C" __declspec(dllexport) int __cdecl DynamicVRSLayer_Initialize() { return initializeOnce({}); }

extern "C" __declspec(dllexport) int __cdecl DynamicVRSLayer_InitializeWithConfig(const wchar_t* iniPath) {
    if (!iniPath || !std::filesystem::path(iniPath).is_absolute()) {
        logMessage("stage=initialize result=invalid-config-path");
        return 0;
    }
    return initializeOnce(iniPath);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        dvr::setLogComponent(L"native-layer");
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
