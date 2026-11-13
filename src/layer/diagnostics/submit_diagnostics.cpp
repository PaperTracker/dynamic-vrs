#include "submit_diagnostics.hpp"

#include "common/log.hpp"
#include "debug_blur.hpp"
#include "layer/vrs/gaze_tracking.hpp"
#include "layer/vrs/rate_image.hpp"
#include "layer/vrs/render_target_hook.hpp"
#include "layer/vrs/vrs_binding.hpp"
#include "layer/vrs/vrs_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace dvr::layer {
namespace {

dvr::layer::diagnostics::DebugBlurRenderer& debugRenderer() {
    static dvr::layer::diagnostics::DebugBlurRenderer renderer;
    return renderer;
}

bool debugBlurCenters(ID3D11Texture2D* texture, EyeViewportPoint& left, EyeViewportPoint& right, float& widen) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    const RtVrsEntry* fallback = nullptr;
    for (const auto& entry : g_rtVrsEntries) {
        if (entry.device == device && entry.width == desc.Width && entry.height == desc.Height && entry.patternValid &&
            entry.appliedConfigurationRevision == g_configurationRevision) {
            if (entry.rasterFlipY != (g_settings.rasterFlipY || g_vrsState.submittedFlipY)) {
                fallback = &entry;
                continue;
            }
            left = entry.appliedLeft;
            right = entry.appliedRight;
            widen = entry.appliedWiden;
            device->Release();
            return true;
        }
    }
    if (device) device->Release();
    if (fallback) {
        left = fallback->appliedLeft;
        right = fallback->appliedRight;
        widen = fallback->appliedWiden;
        return true;
    }
    // Before the first eligible render pass there is no VRS texture to visualize.
    frameFoveationCenters(left, right);
    return false;
}

} // namespace

void observeAlignmentTargetLocked(ID3D11DeviceContext* context) {
    if (!g_alignmentArmed || !g_alignmentProbe || context != g_vrsState.context || !g_vrsState.active) return;
    ID3D11RenderTargetView* view = nullptr;
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (!view) return;
    view->GetResource(&resource);
    if (resource &&
        SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
        D3D11_TEXTURE2D_DESC desc{};
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
        texture->GetDesc(&desc);
        view->GetDesc(&viewDesc);
        if (matchesVrsTarget(desc, viewDesc)) {
            D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            context->RSGetViewports(&count, viewports);
            std::array<float, 8> bounds{};
            for (UINT i = 0; i < std::min(count, 2U); ++i) {
                bounds[i * 4] = viewports[i].TopLeftX;
                bounds[i * 4 + 1] = viewports[i].TopLeftY;
                bounds[i * 4 + 2] = viewports[i].Width;
                bounds[i * 4 + 3] = viewports[i].Height;
            }
            g_alignmentProbe->observe(texture, static_cast<uint32_t>(viewDesc.Format), count, bounds);
        }
    }
    if (texture) texture->Release();
    if (resource) resource->Release();
    view->Release();
}

void captureAlignmentSubmission(int eye, const OpenVrTexture* texture) {
    if (eye != 0 || !g_alignmentArmed.load() || !texture || texture->type != 0 || !texture->handle) return;
    auto* source = static_cast<ID3D11Texture2D*>(texture->handle);
    ContextThreadGuard deviceLock(source);
    std::scoped_lock lock(g_vrsMutex);
    if (!g_alignmentArmed || !g_alignmentProbe) return;
    observeAlignmentTargetLocked(g_vrsState.context);
    g_alignmentArmed = false;
    const auto start = GetTickCount64();
    const bool truncated = g_alignmentProbe->truncated();
    const auto results = g_alignmentProbe->capture(source);
    logMessage("stage=alignment-probe result=captured targets=%zu truncated=%d elapsedMs=%llu", results.size(),
               truncated, static_cast<unsigned long long>(GetTickCount64() - start));
    for (const auto& r : results) {
        logMessage("stage=alignment-target texture=%p size=%ux%u samples=%u format=%u sameAsSubmit=%d captured=%d "
                   "viewportCount=%u vp0=%.1f,%.1f,%.1f,%.1f vp1=%.1f,%.1f,%.1f,%.1f",
                   reinterpret_cast<void*>(r.texture), r.width, r.height, r.samples, r.format, r.sameAsSubmit,
                   r.captured, r.viewportCount, r.viewports[0], r.viewports[1], r.viewports[2], r.viewports[3],
                   r.viewports[4], r.viewports[5], r.viewports[6], r.viewports[7]);
        for (unsigned i = 0; i < 2; ++i) {
            const auto& e = r.eyes[i];
            logMessage("stage=alignment-score texture=%p eye=%u identity=%.4f flipped=%.4f best=%.4f "
                       "flipY=%d shiftRows=%d grid=64 scaleY=%.3f",
                       reinterpret_cast<void*>(r.texture), i, e.identity, e.flipped, e.best, e.flipY, e.shiftRows,
                       e.scaleY);
        }
    }
}

// The caller holds the device lock and g_vrsMutex through rendering and copyback.
static ID3D11Texture2D* renderSubmissionVisualAidLocked(int eye, const OpenVrTexture* texture,
                                                        const OpenVrTextureBounds* bounds,
                                                        const dvr::layer::diagnostics::DebugBlurBounds& region) {
    if ((!g_settings.debugRadiusRings && g_settings.debugBlurMip == 0) || g_settings.mode == VrsMode::Off ||
        g_settings.uniformRate != 0 || (eye != 0 && eye != 1)) {
        return nullptr;
    }
    const EyeLayout layout = g_vrsState.layout;
    if (layout == EyeLayout::Unknown) return nullptr;
    const bool singleEye = layout == EyeLayout::Array || layout == EyeLayout::PerEye;
    if (!ensureFixedCenters()) return nullptr;
    EyeViewportPoint centerLeft = g_fixedCenters[0];
    EyeViewportPoint centerRight = g_fixedCenters[1];
    float widen = 0.0F;
    const bool applied =
        debugBlurCenters(static_cast<ID3D11Texture2D*>(texture->handle), centerLeft, centerRight, widen);
    if (!applied) {
        static ULONGLONG lastReport = 0;
        const auto now = GetTickCount64();
        if (now - lastReport >= 2000) {
            lastReport = now;
            logMessage("stage=debug-blur result=skipped reason=no-current-pattern eye=%d revision=%llu", eye,
                       static_cast<unsigned long long>(g_configurationRevision));
        }
        return nullptr;
    }
    if (bounds && bounds->uMin > bounds->uMax) {
        centerLeft.x = 1.0F - centerLeft.x;
        centerRight.x = 1.0F - centerRight.x;
    }
    if (bounds && bounds->vMin > bounds->vMax) {
        centerLeft.y = 1.0F - centerLeft.y;
        centerRight.y = 1.0F - centerRight.y;
    }
    static unsigned int centerLogs = 0;
    if (++centerLogs <= 6U) {
        logMessage("stage=debug-blur-center source=%s left=%.3f,%.3f right=%.3f,%.3f",
                   applied ? "vrs-pattern" : "initial-pattern", centerLeft.x, centerLeft.y, centerRight.x,
                   centerRight.y);
    }
    // Green: the sharp zone. Yellow: the next ring that does not reach the edge, or the sharp zone again.
    const EyeRings rings[2] = {eyeRings(0, widen), eyeRings(1, widen)};
    uint32_t next = 1;
    while (next < RingCount && !(rings[0].tangent[next] > 0.0F && std::isfinite(rings[0].tangent[next]))) ++next;
    if (next == RingCount) next = 0;
    // The renderer wants a finite positive radius: a skipped sharp zone is a dot, one to the edge a big ellipse.
    const auto radius = [](float value) { return std::clamp(value, 0.001F, 4.0F); };
    const dvr::layer::diagnostics::DebugBlurParameters parameters{
        {{centerLeft.x, centerLeft.y, radius(rings[0].radiusX(0)), radius(rings[0].radiusY(0))},
         {centerRight.x, centerRight.y, radius(rings[1].radiusX(0)), radius(rings[1].radiusY(0))}},
        static_cast<float>(g_settings.debugBlurMip),
        g_settings.debugRadiusRings,
        {radius(rings[0].radiusX(next)), radius(rings[1].radiusX(next))},
        {radius(rings[0].radiusY(next)), radius(rings[1].radiusY(next))},
        singleEye,
        region};
    auto* result = debugRenderer().render(static_cast<ID3D11Texture2D*>(texture->handle), eye, parameters);
    if (result == nullptr) {
        static ULONGLONG lastReport = 0;
        const auto now = GetTickCount64();
        if (now - lastReport >= 2000) {
            lastReport = now;
            D3D11_TEXTURE2D_DESC desc{};
            static_cast<ID3D11Texture2D*>(texture->handle)->GetDesc(&desc);
            logMessage("stage=debug-blur result=unavailable action=original-image format=%u size=%ux%u samples=%u "
                       "array=%u bounds=%.5f,%.5f,%.5f,%.5f",
                       static_cast<unsigned int>(desc.Format), desc.Width, desc.Height, desc.SampleDesc.Count,
                       desc.ArraySize, region.left, region.top, region.right, region.bottom);
        }
    } else {
        static std::once_flag readyOnce;
        std::call_once(readyOnce, [] {
            logMessage("stage=debug-blur result=active mode=%s mip=%d purpose=boundary-visualization-only",
                       g_settings.debugRadiusRings ? "radius-rings" : "blur", g_settings.debugBlurMip);
        });
    }
    return result;
}

bool drawSubmissionVisualAid(int eye, const OpenVrTexture* source, const OpenVrTextureBounds* bounds) {
    {
        std::scoped_lock lock(g_vrsMutex);
        if ((!g_settings.debugRadiusRings && g_settings.debugBlurMip == 0) || g_settings.mode == VrsMode::Off ||
            g_settings.uniformRate != 0)
            return false;
    }
    if (!source || source->type != 0 || !source->handle) return false;
    dvr::layer::diagnostics::DebugBlurBounds region;
    if (bounds) {
        if (!std::isfinite(bounds->uMin) || !std::isfinite(bounds->vMin) || !std::isfinite(bounds->uMax) ||
            !std::isfinite(bounds->vMax))
            return false;
        region = {std::min(bounds->uMin, bounds->uMax), std::min(bounds->vMin, bounds->vMax),
                  std::max(bounds->uMin, bounds->uMax), std::max(bounds->vMin, bounds->vMax)};
    }
    auto* texture = static_cast<ID3D11Texture2D*>(source->handle);
    ContextThreadGuard deviceLock(texture);
    std::scoped_lock lock(g_vrsMutex);
    auto* result = renderSubmissionVisualAidLocked(eye, source, bounds, region);
    if (!result) return false;
    disableFixedVrsLocked(g_vrsState.context);
    const UINT slice = g_vrsState.layout == EyeLayout::Array ? static_cast<UINT>(eye) : 0U;
    RenderTargetHookBypass bypass;
    return debugRenderer().copyTo(result, texture, slice, region);
}

bool drawOpenXrVisualAid(int eye, ID3D11Texture2D* texture) {
    if (!texture) return false;
    OpenVrTextureBounds bounds{0, 0, 1, 1};
    {
        std::scoped_lock lock(g_vrsMutex);
        if (g_vrsState.layout == EyeLayout::SideBySide) bounds = {eye * 0.5F, 0, (eye + 1) * 0.5F, 1};
    }
    const OpenVrTexture source{texture, 0, 0};
    captureAlignmentSubmission(eye, &source);
    return drawSubmissionVisualAid(eye, &source, &bounds);
}

} // namespace dvr::layer
