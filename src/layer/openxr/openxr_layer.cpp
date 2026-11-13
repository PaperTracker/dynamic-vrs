#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include "view_geometry.hpp"

#include "common/ini_file.hpp"
#include "common/log.hpp"
#include "common/paths.hpp"
#include "layer/config/layer_config.hpp"
#include "layer/control/frame_control.hpp"
#include "layer/control/panel_launcher.hpp"
#include "layer/diagnostics/submit_diagnostics.hpp"
#include "layer/vrs/eye_projection.hpp"
#include "layer/vrs/gaze_tracking.hpp"
#include "layer/vrs/rate_image.hpp"
#include "layer/vrs/render_target_hook.hpp"
#include "layer/vrs/vrs_binding.hpp"
#include "layer/vrs/vrs_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dvr::layer::openxr {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char LayerName[] = "XR_APILAYER_DYNAMIC_VRS_d3d11";
struct Dispatch {
    XrInstance instance = XR_NULL_HANDLE;
    PFN_xrGetInstanceProcAddr get = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
};
struct Session {
    std::shared_ptr<Dispatch> dispatch;
    XrSpace headSpace = XR_NULL_HANDLE;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool active = false;
    bool panelStarted = false;
    bool preparedFrame = false;
};
struct AcquiredImage {
    uint32_t index;
    bool waited = false;
    bool processed = false;
};
struct Swapchain {
    std::shared_ptr<Session> session;
    XrSwapchainCreateInfo info{};
    std::vector<ComPtr<ID3D11Texture2D>> images;
    std::deque<AcquiredImage> acquired;
    EyeLayout layout = EyeLayout::Unknown;
    unsigned int eyeMask = 0;
    uint64_t diagnosticRevision = 0;
};
// Only metadata is guarded here. Runtime calls and D3D11 work run after dropping this mutex.
std::mutex g_mutex;
std::unordered_map<XrInstance, std::shared_ptr<Dispatch>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<Session>> g_sessions;
std::unordered_map<XrSwapchain, std::shared_ptr<Swapchain>> g_swapchains;
std::weak_ptr<Session> g_activeSession;

template <typename Handle, typename Value>
std::shared_ptr<Value> lookup(const std::unordered_map<Handle, std::shared_ptr<Value>>& map, Handle handle) {
    std::scoped_lock lock(g_mutex);
    const auto found = map.find(handle);
    return found == map.end() ? nullptr : found->second;
}

template <typename Function> bool resolve(Dispatch& dispatch, const char* name, Function& function) {
    return XR_SUCCEEDED(dispatch.get(dispatch.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&function))) &&
           function;
}

bool layerEnabled() {
    const IniFile ini(thisModuleDirectory() / L"dynamic-vrs.ini");
    if (ini.integer(L"enabled", 0) != 1) return false;
    g_configPath = ini.path();
    return true;
}

void updateProjections(const std::shared_ptr<Session>& session, const XrViewLocateInfo& info, const XrViewState& state,
                       const XrView* views) {
    constexpr XrViewStateFlags validViews = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & validViews) != validViews || !session->headSpace) return;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(session->dispatch->locateSpace(session->headSpace, info.space, info.displayTime, &head))) return;
    constexpr XrSpaceLocationFlags validHead =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((head.locationFlags & validHead) != validHead) return;
    EyeProjectionData eyes[2];
    if (!eyeProjectionFromView(views[0], head.pose, eyes[0]) || !eyeProjectionFromView(views[1], head.pose, eyes[1]))
        return;
    std::scoped_lock lock(g_vrsMutex);
    setEyeProjectionsLocked(eyes);
}

bool observeProjectionLayer(const std::shared_ptr<Session>& session, const XrFrameEndInfo& info) {
    std::scoped_lock lock(g_mutex);
    for (auto& [handle, chain] : g_swapchains)
        if (chain->session == session) {
            chain->layout = EyeLayout::Unknown;
            chain->eyeMask = 0;
        }
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        if (!info.layers[i] || info.layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[i]);
        if (projection.viewCount != 2 || !projection.views) continue;
        const auto& left = projection.views[0].subImage;
        const auto& right = projection.views[1].subImage;
        const auto l = g_swapchains.find(left.swapchain), r = g_swapchains.find(right.swapchain);
        if (l == g_swapchains.end() || r == g_swapchains.end() || l->second->session != session ||
            r->second->session != session)
            continue;
        auto& a = *l->second;
        auto& b = *r->second;
        const EyeLayout layout = classifyEyeLayout(a.info, left, b.info, right, &a == &b);
        a.layout = b.layout = layout;
        if (layout != EyeLayout::Unknown) {
            a.eyeMask |= 1;
            b.eyeMask |= 2;
        }
        return layout != EyeLayout::Unknown;
    }
    return false;
}

XrResult XRAPI_CALL destroyInstance(XrInstance instance) {
    const auto dispatch = lookup(g_instances, instance);
    if (!dispatch) return XR_ERROR_HANDLE_INVALID;
    std::shared_ptr<Session> active;
    {
        std::scoped_lock lock(g_mutex);
        active = g_activeSession.lock();
    }
    const auto result = dispatch->destroyInstance(instance);
    if (XR_SUCCEEDED(result)) {
        if (active && active->dispatch == dispatch && active->active) releaseD3d11Vrs(active->context.Get());
        std::scoped_lock lock(g_mutex);
        std::erase_if(g_swapchains, [&](const auto& pair) { return pair.second->session->dispatch == dispatch; });
        std::erase_if(g_sessions, [&](const auto& pair) { return pair.second->dispatch == dispatch; });
        if (active && active->dispatch == dispatch) g_activeSession.reset();
        g_instances.erase(instance);
    }
    return result;
}

XrResult XRAPI_CALL createSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* handle) {
    const auto dispatch = lookup(g_instances, instance);
    if (!dispatch) return XR_ERROR_HANDLE_INVALID;
    const auto result = dispatch->createSession(instance, info, handle);
    if (XR_FAILED(result)) return result;
    auto session = std::make_shared<Session>();
    session->dispatch = dispatch;
    const XrGraphicsBindingD3D11KHR* binding = nullptr;
    if (info) {
        auto* next = static_cast<const XrBaseInStructure*>(info->next);
        for (; next; next = next->next)
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
                binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
    }
    bool available = false;
    {
        std::scoped_lock lock(g_mutex);
        available = g_activeSession.expired();
        g_sessions[*handle] = session;
    }
    if (!binding || !binding->device || !available || !layerEnabled()) return result;
    session->device = binding->device;
    binding->device->GetImmediateContext(&session->context);
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    space.poseInReferenceSpace.orientation.w = 1;
    if (!session->context || XR_FAILED(dispatch->createReferenceSpace(*handle, &space, &session->headSpace)))
        return result;
    {
        std::scoped_lock lock(g_mutex);
        if (!g_activeSession.expired()) return result;
        g_activeSession = session;
    }
    {
        std::scoped_lock lock(g_vrsMutex);
        loadLayerSettings();
        session->active = g_runtimeControl.start(runtimeSettingsLocked());
        if (session->active) g_openXrActive = true;
    }
    if (!session->active) {
        std::scoped_lock lock(g_mutex);
        if (g_activeSession.lock() == session) g_activeSession.reset();
    }
    logMessage("stage=openxr-session result=%s graphics=d3d11 session=%p",
               session->active ? "ready" : "control-unavailable", *handle);
    return result;
}

XrResult XRAPI_CALL destroySession(XrSession handle) {
    const auto session = lookup(g_sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    // Our reference space is a child of this session, so the runtime destroys it with the session.
    const auto result = session->dispatch->destroySession(handle);
    if (XR_SUCCEEDED(result)) {
        if (session->active) releaseD3d11Vrs(session->context.Get());
        std::scoped_lock lock(g_mutex);
        std::erase_if(g_swapchains, [&](const auto& pair) { return pair.second->session == session; });
        g_sessions.erase(handle);
        if (g_activeSession.lock() == session) g_activeSession.reset();
    }
    return result;
}

XrResult XRAPI_CALL locateViews(XrSession handle, const XrViewLocateInfo* info, XrViewState* state, uint32_t capacity,
                                uint32_t* count, XrView* views) {
    const auto session = lookup(g_sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto result = session->dispatch->locateViews(handle, info, state, capacity, count, views);
    if (XR_SUCCEEDED(result) && session->active && info &&
        info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && state && count && *count == 2 &&
        capacity >= 2 && views)
        updateProjections(session, *info, *state, views);
    return result;
}

XrResult XRAPI_CALL createSwapchain(XrSession handle, const XrSwapchainCreateInfo* info, XrSwapchain* chainHandle) {
    const auto session = lookup(g_sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto result = session->dispatch->createSwapchain(handle, info, chainHandle);
    if (XR_FAILED(result)) return result;
    auto chain = std::make_shared<Swapchain>();
    chain->session = session;
    chain->info = *info;
    chain->info.next = nullptr;
    if (session->active && (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) && info->faceCount == 1 &&
        info->arraySize <= 2 && info->mipCount == 1 &&
        !(info->createFlags & (XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT | XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))) {
        uint32_t count = 0;
        if (XR_SUCCEEDED(session->dispatch->enumerateSwapchainImages(*chainHandle, 0, &count, nullptr)) && count) {
            std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_SUCCEEDED(session->dispatch->enumerateSwapchainImages(
                    *chainHandle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
                for (const auto& image : images) chain->images.emplace_back(image.texture);
        }
        logMessage("stage=openxr-swapchain size=%ux%u array=%u samples=%u format=%lld images=%zu", info->width,
                   info->height, info->arraySize, info->sampleCount, static_cast<long long>(info->format),
                   chain->images.size());
    }
    {
        std::scoped_lock lock(g_mutex);
        g_swapchains[*chainHandle] = chain;
    }
    return result;
}

XrResult XRAPI_CALL destroySwapchain(XrSwapchain handle) {
    const auto chain = lookup(g_swapchains, handle);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    const auto result = chain->session->dispatch->destroySwapchain(handle);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_mutex);
        g_swapchains.erase(handle);
    }
    return result;
}

XrResult XRAPI_CALL acquireSwapchainImage(XrSwapchain handle, const XrSwapchainImageAcquireInfo* info,
                                          uint32_t* index) {
    const auto chain = lookup(g_swapchains, handle);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    const auto result = chain->session->dispatch->acquireSwapchainImage(handle, info, index);
    if (result == XR_SUCCESS && index) {
        std::scoped_lock lock(g_mutex);
        chain->acquired.push_back({*index});
    }
    return result;
}

XrResult XRAPI_CALL waitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo* info) {
    const auto chain = lookup(g_swapchains, handle);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    const auto result = chain->session->dispatch->waitSwapchainImage(handle, info);
    ComPtr<ID3D11Texture2D> texture;
    EyeLayout layout = EyeLayout::Unknown;
    bool firstImage = false;
    if (result == XR_SUCCESS) {
        std::scoped_lock lock(g_mutex);
        const auto acquired = std::find_if(chain->acquired.begin(), chain->acquired.end(),
                                           [](const auto& image) { return !image.waited; });
        if (acquired != chain->acquired.end()) {
            acquired->waited = true;
            if (acquired->index < chain->images.size()) texture = chain->images[acquired->index];
            layout = chain->layout;
            if (texture && layout != EyeLayout::Unknown) {
                firstImage = !chain->session->preparedFrame;
                chain->session->preparedFrame = true;
            }
        }
    }
    if (texture && layout != EyeLayout::Unknown) prepareOpenXrVrs(texture.Get(), layout, firstImage);
    return result;
}

XrResult XRAPI_CALL releaseSwapchainImage(XrSwapchain handle, const XrSwapchainImageReleaseInfo* info) {
    const auto chain = lookup(g_swapchains, handle);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    ComPtr<ID3D11Texture2D> texture;
    unsigned int eyeMask = 0;
    EyeLayout layout = EyeLayout::Unknown;
    {
        std::scoped_lock lock(g_mutex);
        if (!chain->acquired.empty()) {
            auto& image = chain->acquired.front();
            if (image.waited && !image.processed && image.index < chain->images.size()) {
                texture = chain->images[image.index];
                eyeMask = chain->eyeMask;
                layout = chain->layout;
                image.processed = true;
            }
        }
    }
    if (texture && layout != EyeLayout::Unknown) {
        for (int eye = 0; eye < 2; ++eye) {
            if (!(eyeMask & (1U << eye))) continue;
            const bool drawn = drawOpenXrVisualAid(eye, texture.Get());
            if (drawn) {
                std::scoped_lock lock(g_vrsMutex);
                if (chain->diagnosticRevision != g_configurationRevision) {
                    chain->diagnosticRevision = g_configurationRevision;
                    logMessage("stage=openxr-visual-aid result=drawn mode=%s eye=%d",
                               g_settings.debugRadiusRings ? "rings" : "blur", eye);
                }
            }
        }
    }
    const auto result = chain->session->dispatch->releaseSwapchainImage(handle, info);
    if (result == XR_SUCCESS) {
        std::scoped_lock lock(g_mutex);
        if (!chain->acquired.empty()) chain->acquired.pop_front();
    }
    return result;
}

XrResult XRAPI_CALL beginFrame(XrSession handle, const XrFrameBeginInfo* info) {
    const auto session = lookup(g_sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto result = session->dispatch->beginFrame(handle, info);
    if (XR_SUCCEEDED(result) && session->active) {
        finishOpenXrControlFrame(session->context.Get());
        {
            std::scoped_lock lock(g_mutex);
            session->preparedFrame = false;
        }
        {
            std::scoped_lock lock(g_vrsMutex);
            g_foveationFrame.valid = false;
            g_foveationFrame.sawEyeBind = false;
        }
    }
    return result;
}

XrResult XRAPI_CALL endFrame(XrSession handle, const XrFrameEndInfo* info) {
    const auto session = lookup(g_sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto result = session->dispatch->endFrame(handle, info);
    if (XR_SUCCEEDED(result) && session->active && info && !observeProjectionLayer(session, *info)) {
        ContextThreadGuard deviceLock(session->context.Get());
        std::scoped_lock lock(g_vrsMutex);
        disableFixedVrsLocked(session->context.Get());
        g_vrsState.layout = EyeLayout::Unknown;
    }
    if (XR_SUCCEEDED(result) && session->active && !session->panelStarted && info && info->layerCount) {
        session->panelStarted = true;
        logMessage("stage=control-panel result=%s source=openxr",
                   launchControlPanel(layerConfigPath(), thisModuleDirectory()));
    }
    return result;
}

XrResult XRAPI_CALL getInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(&getInstanceProcAddr);
        return XR_SUCCESS;
    }
    const auto dispatch = lookup(g_instances, instance);
    if (!dispatch) {
        *function = nullptr;
        return XR_ERROR_HANDLE_INVALID;
    }
    const auto result = dispatch->get(instance, name, function);
    if (XR_FAILED(result) || !*function) return result;
#define INTERCEPT(Name, Handler)                                                                                       \
    if (std::strcmp(name, Name) == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(&Handler)
    INTERCEPT("xrDestroyInstance", destroyInstance);
    else INTERCEPT("xrCreateSession", createSession);
    else INTERCEPT("xrDestroySession", destroySession);
    else INTERCEPT("xrLocateViews", locateViews);
    else INTERCEPT("xrCreateSwapchain", createSwapchain);
    else INTERCEPT("xrDestroySwapchain", destroySwapchain);
    else INTERCEPT("xrAcquireSwapchainImage", acquireSwapchainImage);
    else INTERCEPT("xrWaitSwapchainImage", waitSwapchainImage);
    else INTERCEPT("xrReleaseSwapchainImage", releaseSwapchainImage);
    else INTERCEPT("xrBeginFrame", beginFrame);
    else INTERCEPT("xrEndFrame", endFrame);
#undef INTERCEPT
    return result;
}

XrResult XRAPI_CALL createApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                           XrInstance* instance) {
    if (!info || !instance || !layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !layerInfo->nextInfo ||
        std::strcmp(layerInfo->nextInfo->layerName, LayerName) != 0 ||
        !layerInfo->nextInfo->nextCreateApiLayerInstance || !layerInfo->nextInfo->nextGetInstanceProcAddr)
        return XR_ERROR_INITIALIZATION_FAILED;
    auto dispatch = std::make_shared<Dispatch>();
    dispatch->get = layerInfo->nextInfo->nextGetInstanceProcAddr;
    auto next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    const auto result = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (XR_FAILED(result)) return result;
    dispatch->instance = *instance;
    bool complete = true;
#define RESOLVE(Name, Member) complete &= resolve(*dispatch, Name, dispatch->Member)
    RESOLVE("xrDestroyInstance", destroyInstance);
    RESOLVE("xrCreateSession", createSession);
    RESOLVE("xrDestroySession", destroySession);
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace);
    RESOLVE("xrLocateSpace", locateSpace);
    RESOLVE("xrLocateViews", locateViews);
    RESOLVE("xrCreateSwapchain", createSwapchain);
    RESOLVE("xrDestroySwapchain", destroySwapchain);
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages);
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage);
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage);
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage);
    RESOLVE("xrBeginFrame", beginFrame);
    RESOLVE("xrEndFrame", endFrame);
#undef RESOLVE
    if (!complete) {
        if (dispatch->destroyInstance) dispatch->destroyInstance(*instance);
        *instance = XR_NULL_HANDLE;
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    {
        std::scoped_lock lock(g_mutex);
        g_instances[*instance] = dispatch;
    }
    logMessage("stage=openxr-instance result=ready application=%s", info->applicationInfo.applicationName);
    return result;
}
} // namespace
} // namespace dvr::layer::openxr

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char* name, XrNegotiateApiLayerRequest* request) {
    using namespace dvr::layer::openxr;
    if (!loader || !name || !request || std::strcmp(name, LayerName) != 0 ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(XrNegotiateLoaderInfo) ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loader->minApiVersion > XR_CURRENT_API_VERSION ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize != sizeof(XrNegotiateApiLayerRequest))
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = std::min(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = getInstanceProcAddr;
    request->createApiLayerInstance = createApiLayerInstance;
    return XR_SUCCESS;
}
