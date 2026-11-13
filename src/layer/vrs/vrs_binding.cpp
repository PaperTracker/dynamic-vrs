#include "vrs_binding.hpp"

#include "common/log.hpp"
#include "gaze_tracking.hpp"
#include "rate_image.hpp"
#include "render_target_hook.hpp"
#include "vrs_state.hpp"

#include <mutex>

namespace dvr::layer {
namespace {

uint64_t g_patternFrames = 0;
ULONGLONG g_lastPatternProgressMs = 0;

DXGI_FORMAT formatFamily(DXGI_FORMAT format) {
    if (format >= DXGI_FORMAT_R32G32B32A32_TYPELESS && format <= DXGI_FORMAT_R32G32B32A32_SINT)
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    if (format >= DXGI_FORMAT_R16G16B16A16_TYPELESS && format <= DXGI_FORMAT_R16G16B16A16_SINT)
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    if (format >= DXGI_FORMAT_R32G32_TYPELESS && format <= DXGI_FORMAT_R32G32_SINT) return DXGI_FORMAT_R32G32_TYPELESS;
    if (format >= DXGI_FORMAT_R10G10B10A2_TYPELESS && format <= DXGI_FORMAT_R10G10B10A2_UINT)
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    if (format >= DXGI_FORMAT_R8G8B8A8_TYPELESS && format <= DXGI_FORMAT_R8G8B8A8_SINT)
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    return format;
}

bool rasterFlipYForTarget(DXGI_FORMAT viewFormat, UINT samples = 1) {
    if (g_settings.rasterFlipY || g_vrsState.submittedFlipY) return true;
    if (g_settings.rasterFlipYFormat == DXGI_FORMAT_UNKNOWN || viewFormat != g_settings.rasterFlipYFormat ||
        g_vrsState.submittedFormat == DXGI_FORMAT_UNKNOWN)
        return false;
    if (g_settings.rasterFlipYSamples != 0 && samples != g_settings.rasterFlipYSamples) return false;
    // A format-only rule cannot distinguish intermediates from the final surface
    // when they belong to the same family. Do not flip the final image by guessing.
    if (formatFamily(viewFormat) == formatFamily(g_vrsState.submittedFormat)) {
        static std::once_flag warning;
        std::call_once(warning, [] {
            logMessage("stage=vrs-raster-transform result=ambiguous-final-format action=ignore-format-rule");
        });
        return false;
    }
    return true;
}

bool matchesVrsDimensions(UINT targetWidth, UINT targetHeight) {
    const UINT width =
        g_settings.targetWidth && g_settings.targetHeight ? g_settings.targetWidth : g_vrsState.renderWidth;
    const UINT height =
        g_settings.targetWidth && g_settings.targetHeight ? g_settings.targetHeight : g_vrsState.renderHeight;
    if (width == 0 || height == 0) return false;
    if (targetWidth == width && targetHeight == height) return true;
    if (!g_openXrActive || (g_settings.targetWidth && g_settings.targetHeight) || targetWidth < 256 ||
        targetHeight < 128)
        return false;
    // OpenXR games can render a scaled stereo image before upscaling into the swapchain.
    // Both images have the same normalized eye coordinates. Allow two pixels of rounding.
    const auto difference = static_cast<int64_t>(targetWidth) * height - static_cast<int64_t>(targetHeight) * width;
    return std::abs(difference) <= 2 * static_cast<int64_t>(std::max(width, height));
}

void releaseVrsContext() {
    if (g_vrsState.context && g_vrsState.active) {
        disableFixedVrsLocked(g_vrsState.context);
    }
    releaseCom(g_vrsState.context);
    releaseCom(g_vrsState.device);
    g_vrsState.renderWidth = 0;
    g_vrsState.renderHeight = 0;
    g_vrsState.active = false;
}

bool initializeNvapi() {
    if (g_vrsState.nvapiInitialized) {
        return true;
    }
    const NvAPI_Status status = NvAPI_Initialize();
    logMessage("stage=vrs-nvapi-initialize status=%d", status);
    g_vrsState.nvapiInitialized = status == NVAPI_OK;
    return g_vrsState.nvapiInitialized;
}

bool initializeVrsContext(ID3D11Device* device, ID3D11DeviceContext* context, UINT renderWidth, UINT renderHeight) {
    releaseVrsContext();
    if (g_settings.uniformRate == 0 && !ensureFixedCenters()) return false;
    device->AddRef();
    context->AddRef();
    g_vrsState.device = device;
    g_vrsState.context = context;
    g_vrsState.renderWidth = renderWidth;
    g_vrsState.renderHeight = renderHeight;
    const bool hooked = installContextRenderTargetHook(context);
    logMessage("stage=vrs-context result=ready render=%ux%u", renderWidth, renderHeight);
    return hooked;
}

const char* eyeLayoutName(EyeLayout layout) {
    switch (layout) {
    case EyeLayout::SideBySide: return "side-by-side";
    case EyeLayout::Array: return "array";
    case EyeLayout::PerEye: return "per-eye";
    default: return "unknown";
    }
}

// The eye whose slice of an eye array a view renders alone, or -1 when it covers both.
int targetSlice(const D3D11_RENDER_TARGET_VIEW_DESC& view) {
    UINT first = 0;
    UINT count = 0;
    if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
        first = view.Texture2DArray.FirstArraySlice;
        count = view.Texture2DArray.ArraySize;
    } else if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
        first = view.Texture2DMSArray.FirstArraySlice;
        count = view.Texture2DMSArray.ArraySize;
    } else {
        return -1;
    }
    return count == 1 && first < 2 ? static_cast<int>(first) : -1;
}

// Follows how the application submits its eyes; rate images of another layout are dropped.
void updateEyeLayoutLocked(ID3D11DeviceContext* context, int eye, const OpenVrTexture* texture,
                           const OpenVrTextureBounds* bounds, UINT arraySize) {
    const bool flipY = bounds && bounds->vMin > bounds->vMax;
    if (flipY != g_vrsState.submittedFlipY) {
        g_vrsState.submittedFlipY = flipY;
        logMessage("stage=vrs-layout submittedFlipY=%d", flipY ? 1 : 0);
    }
    EyeLayout layout = g_vrsState.layout;
    if (arraySize >= 2) {
        layout = EyeLayout::Array;
    } else if (eye == 0) {
        g_vrsState.leftEyeHandle = texture->handle;
        if (layout == EyeLayout::Array) layout = EyeLayout::Unknown;
    } else if (eye == 1) {
        layout = texture->handle == g_vrsState.leftEyeHandle ? EyeLayout::SideBySide : EyeLayout::PerEye;
    }
    if (layout == g_vrsState.layout) return;
    disableFixedVrsLocked(context);
    releaseRtVrsEntriesLocked();
    g_vrsState.layout = layout;
    logMessage("stage=vrs-layout layout=%s", eyeLayoutName(layout));
}

void refreshCachedVrPatternsLocked(ID3D11DeviceContext* context) {
    // Uploading an existing rate image does not require the corresponding color
    // target to be bound. At Submit that target is often the desktop preview.
    ++g_patternFrames;
    const auto now = GetTickCount64();
    const bool report = now - g_lastPatternProgressMs >= 2000;
    unsigned int matching = 0;
    for (auto& entry : g_rtVrsEntries) {
        if (entry.device != g_vrsState.device || !matchesVrsDimensions(entry.width, entry.height)) continue;
        ++matching;
        refreshRtVrsPatternLocked(entry, context);
        if (report) {
            logMessage("stage=vrs-frame frames=%llu render=%ux%u rasterFlipY=%d updates=%u binds=%u sourceSeq=%llu "
                       "patternSeq=%llu "
                       "nextLeft=%.3f,%.3f nextRight=%.3f,%.3f",
                       static_cast<unsigned long long>(g_patternFrames), entry.width, entry.height,
                       entry.rasterFlipY ? 1 : 0, entry.patternUpdates, entry.binds,
                       static_cast<unsigned long long>(g_vrsState.gaze.lastSequence),
                       static_cast<unsigned long long>(entry.lastGazeSequence), entry.appliedLeft.x,
                       entry.appliedLeft.y, entry.appliedRight.x, entry.appliedRight.y);
        }
    }
    if (report) {
        g_lastPatternProgressMs = now;
        if (!matching)
            logMessage("stage=vrs-frame frames=%llu result=no-matching-rate-texture render=%ux%u",
                       static_cast<unsigned long long>(g_patternFrames), g_vrsState.renderWidth,
                       g_vrsState.renderHeight);
    }
}

} // namespace

bool matchesVrsTarget(const D3D11_TEXTURE2D_DESC& target, const D3D11_RENDER_TARGET_VIEW_DESC& view) {
    if (g_vrsState.layout == EyeLayout::Unknown || !matchesVrsDimensions(target.Width, target.Height)) return false;
    // The driver shades at most 16 samples per invocation, so the coarse rates shrink as MSAA grows: with 8x
    // every rate from 2x2 up halves the shading and 2x1 saves nothing.
    const bool multiSampled =
        target.SampleDesc.Count == 2 || target.SampleDesc.Count == 4 || target.SampleDesc.Count == 8;
    if (g_vrsState.layout == EyeLayout::Array) {
        // Single-pass instanced rendering: both eyes are slices of one target.
        const bool singleSample = target.SampleDesc.Count == 1 &&
                                  view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
                                  view.Texture2DArray.MipSlice == 0;
        const bool multiSample = multiSampled && view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        return target.ArraySize == 2 && (singleSample || multiSample);
    }
    const bool singleSample = target.SampleDesc.Count == 1 && view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D &&
                              view.Texture2D.MipSlice == 0;
    const bool multiSample = multiSampled && view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS;
    return target.ArraySize == 1 && (singleSample || multiSample);
}

void applyVrsToTargetsLocked(ID3D11DeviceContext* context, UINT renderTargetViewCount,
                             ID3D11RenderTargetView* const* renderTargetViews) {
    if (g_settings.mode == VrsMode::Off) return;
    // Resources and active-state bookkeeping belong to the submitted device's
    // immediate context; do not share that state with deferred/foreign contexts.
    if (context != g_vrsState.context) {
        return;
    }

    // The gaze pattern is refreshed regardless of whether the current target size
    // matches the submitted eye texture. Applications switch to differently sized
    // targets after loading, and gating this on a size match leaves the rate image
    // frozen at the last gaze point.
    if (g_settings.mode == VrsMode::Gaze) {
        updateGazeSampleLocked();
    }

    // Bind during the actual render pass, selecting the submitted eye dimensions.
    // Unrelated square targets must not receive a side-by-side eye pattern.
    ID3D11Device* targetDevice = nullptr;
    D3D11_TEXTURE2D_DESC targetDesc{};
    D3D11_RENDER_TARGET_VIEW_DESC targetViewDesc{};
    bool hasTarget = false;
    if (renderTargetViewCount > 0 && renderTargetViews && renderTargetViews[0]) {
        renderTargetViews[0]->GetDesc(&targetViewDesc);
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* texture = nullptr;
        renderTargetViews[0]->GetResource(&resource);
        if (resource &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
            texture->GetDesc(&targetDesc);
            texture->GetDevice(&targetDevice);
            hasTarget = true;
        }
        if (texture) {
            texture->Release();
        }
        if (resource) {
            resource->Release();
        }
    }
    const bool eligible = hasTarget && targetDevice != nullptr && matchesVrsTarget(targetDesc, targetViewDesc);
    if (!eligible) {
        if (hasTarget && g_vrsState.targetMismatchLogs < 8U) {
            ++g_vrsState.targetMismatchLogs;
            logMessage("stage=vrs-target result=skipped observed=%ux%u samples=%u view=%u filter=%ux%u count=%u",
                       targetDesc.Width, targetDesc.Height, targetDesc.SampleDesc.Count,
                       static_cast<unsigned int>(targetViewDesc.ViewDimension), g_settings.targetWidth,
                       g_settings.targetHeight, g_vrsState.targetMismatchLogs);
        }
        if (targetDevice != nullptr) {
            targetDevice->Release();
        }
        disableFixedVrsLocked(context);
        return;
    }
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&viewportCount, viewports);
    EyeViewports eyeViewports;
    if (!resolveEyeViewports(g_vrsState.layout, targetDesc.Width, targetDesc.Height, viewportCount, viewports,
                             eyeViewports)) {
        targetDevice->Release();
        disableFixedVrsLocked(context);
        return;
    }
    if (!g_rtVrsEntries.empty() && g_rtVrsEntries.front().device != targetDevice) {
        // Device changed: every cached rate view is stale.
        releaseRtVrsEntriesLocked();
    }
    if (!g_foveationFrame.sawEyeBind) {
        if (!latchFoveationFrameLocked()) {
            targetDevice->Release();
            disableFixedVrsLocked(context);
            return;
        }
        g_foveationFrame.sawEyeBind = true;
        // Every pass of this image must use one gaze sample, even if inference
        // publishes a newer value between the HDR and final-surface draws.
        for (auto& cached : g_rtVrsEntries)
            if (cached.device == targetDevice && matchesVrsDimensions(cached.width, cached.height))
                refreshRtVrsPatternLocked(cached, context);
    }
    RtVrsEntry* entry = ensureRtVrsEntryLocked(targetDevice, targetDesc.Width, targetDesc.Height,
                                               rasterFlipYForTarget(targetViewDesc.Format, targetDesc.SampleDesc.Count),
                                               g_vrsState.layout);
    if (targetDevice != nullptr) {
        targetDevice->Release();
    }
    if (entry == nullptr) {
        disableFixedVrsLocked(context);
        return;
    }
    if (entry->viewports != eyeViewports) {
        entry->viewports = eyeViewports;
        entry->patternValid = false;
        static ULONGLONG lastReport = 0;
        const auto now = GetTickCount64();
        if (now - lastReport >= 2000) {
            lastReport = now;
            logMessage("stage=vrs-viewport render=%ux%u count=%u left=%.1f,%.1f,%.1f,%.1f "
                       "right=%.1f,%.1f,%.1f,%.1f",
                       entry->width, entry->height, viewportCount, eyeViewports[0].left, eyeViewports[0].top,
                       eyeViewports[0].width, eyeViewports[0].height, eyeViewports[1].left, eyeViewports[1].top,
                       eyeViewports[1].width, eyeViewports[1].height);
        }
    }
    refreshRtVrsPatternLocked(*entry, context);
    applyRtVrsLocked(context, *entry, targetSlice(targetViewDesc));
}

void prepareFrameTexture(ID3D11Texture2D* d3dTexture, EyeLayout layout, bool completedFrame, int eye = 0,
                         const OpenVrTextureBounds* bounds = nullptr) {
    if (!d3dTexture) return;
    D3D11_TEXTURE2D_DESC textureDesc{};
    d3dTexture->GetDesc(&textureDesc);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    d3dTexture->GetDevice(&device);
    if (device) {
        device->GetImmediateContext(&context);
    }
    if (!device || !context) {
        releaseCom(context);
        releaseCom(device);
        return;
    }
    ContextThreadGuard deviceLock(context);
    std::scoped_lock lock(g_vrsMutex);
    if (g_settings.mode == VrsMode::Off) {
        releaseCom(context);
        releaseCom(device);
        return;
    }

    if (!initializeNvapi()) {
        releaseCom(context);
        releaseCom(device);
        return;
    }
    const bool needsResources = g_vrsState.device != device || g_vrsState.context != context ||
                                g_vrsState.renderWidth != textureDesc.Width ||
                                g_vrsState.renderHeight != textureDesc.Height;
    if (needsResources && !initializeVrsContext(device, context, textureDesc.Width, textureDesc.Height)) {
        releaseCom(context);
        releaseCom(device);
        return;
    }
    g_vrsState.submittedFormat = textureDesc.Format;
    if (layout == EyeLayout::Unknown) {
        const OpenVrTexture texture{d3dTexture, 0, 0};
        updateEyeLayoutLocked(context, eye, &texture, bounds, textureDesc.ArraySize);
    } else if (layout != g_vrsState.layout || g_vrsState.submittedFlipY) {
        disableFixedVrsLocked(context);
        releaseRtVrsEntriesLocked();
        g_vrsState.layout = layout;
        g_vrsState.submittedFlipY = false;
        logMessage("stage=vrs-layout layout=%s source=openxr", eyeLayoutName(layout));
    }
    if (completedFrame) {
        latchFoveationFrameLocked();
        g_foveationFrame.sawEyeBind = false;
    }
    // Check the actual immediate context encountered at Submit, without an
    // arbitrary context-count cap or retaining unobserved deferred contexts.
    if (!needsResources) installContextRenderTargetHook(context);

    // Gaze refresh follows submit: applications switch render target and context after
    // loading, so hooking only the context present at creation freezes the last gaze point.
    if (g_settings.mode == VrsMode::Gaze) {
        updateGazeSampleLocked();
    }
    if (completedFrame) refreshCachedVrPatternsLocked(context);

    // A configuration change must also reach a target kept bound across frames.
    // Ordinary gaze refresh above never depends on whichever RT is bound here.
    if (needsResources || g_boundConfigurationRevision != g_configurationRevision ||
        (g_openXrActive && completedFrame)) {
        ID3D11RenderTargetView* current = nullptr;
        context->OMGetRenderTargets(1, &current, nullptr);
        applyVrsToTargetsLocked(context, current ? 1U : 0U, &current);
        releaseCom(current);
    }
    releaseCom(context);
    releaseCom(device);
}

void enableFixedVrsForNextFrame(int eye, const OpenVrTexture* texture, const OpenVrTextureBounds* bounds,
                                bool completedFrame) {
    if (!texture || texture->type != 0) return;
    prepareFrameTexture(static_cast<ID3D11Texture2D*>(texture->handle), EyeLayout::Unknown, completedFrame, eye,
                        bounds);
}

void prepareOpenXrVrs(ID3D11Texture2D* texture, EyeLayout layout, bool completedFrame) {
    if (layout != EyeLayout::Unknown) prepareFrameTexture(texture, layout, completedFrame);
}

void releaseD3d11Vrs(ID3D11DeviceContext* context) {
    ContextThreadGuard deviceLock(context);
    std::scoped_lock lock(g_vrsMutex);
    if (g_vrsState.context && context != g_vrsState.context) return;
    disableFixedVrsLocked(context);
    if (g_vrsState.nvapiInitialized) NvAPI_D3D11_RSSetShadingRateResourceView(context, nullptr);
    releaseRtVrsEntriesLocked();
    releaseVrsContext();
    g_vrsState.gaze.receiver.close();
    g_vrsState.layout = EyeLayout::Unknown;
    g_vrsState.leftEyeHandle = nullptr;
    g_vrsState.submittedFormat = DXGI_FORMAT_UNKNOWN;
    g_vrsState.submittedFlipY = false;
    g_vrsState.gaze.lastSequence = 0;
    g_vrsState.gaze.openLogged = g_vrsState.gaze.connectedLogged = false;
    g_metrics = {};
    g_boundConfigurationRevision = 0;
    resetEyeProjectionsLocked();
    resetGazeStateLocked();
    g_alignmentArmed = false;
    g_alignmentProbe.reset();
    g_gpuFrameProbe.reset();
    g_runtimeControl.stop();
    g_openXrActive = false;
}

} // namespace dvr::layer
