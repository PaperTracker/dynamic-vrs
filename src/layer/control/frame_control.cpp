#include "frame_control.hpp"

#include "common/log.hpp"
#include "layer/vrs/gaze_tracking.hpp"
#include "layer/vrs/rate_image.hpp"
#include "layer/vrs/render_target_hook.hpp"
#include "layer/vrs/vrs_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <vector>

#ifdef DVR_HAS_OPENVR
#include <openvr.h>
#endif

namespace dvr::layer {
namespace {

unsigned int g_submittedEyeMask = 0;

void applyRuntimeControlLocked(const dvr::control::Settings& s) {
    if (g_settings.debugTargetAlignment && s.mode != 0 && s.rings &&
        (!g_settings.debugRadiusRings || g_settings.mode == VrsMode::Off)) {
        if (!g_alignmentProbe) g_alignmentProbe = std::make_unique<dvr::layer::diagnostics::TargetAlignmentProbe>();
        g_alignmentProbe->reset();
        g_alignmentArmed = true;
        logMessage("stage=alignment-probe result=armed");
    }
    // Disable the old rate state before changing mode; Off callbacks return early.
    if (g_vrsState.context) {
        disableFixedVrsLocked(g_vrsState.context);
        if (g_vrsState.nvapiInitialized) NvAPI_D3D11_RSSetShadingRateResourceView(g_vrsState.context, nullptr);
    }
    g_settings.mode = static_cast<VrsMode>(s.mode);
    if (g_settings.mode != VrsMode::Gaze) {
        g_vrsState.gaze.receiver.close();
        g_vrsState.gaze.openLogged = false;
        g_vrsState.gaze.connectedLogged = false;
    }
    std::copy(std::begin(s.ringDegrees), std::end(s.ringDegrees), g_settings.ringDegrees.begin());
    g_settings.legacyRingFraction = {};
    g_settings.ditherDegrees = s.ditherDegrees;
    g_settings.uniformRate = s.uniformRate;
    g_settings.debugRadiusRings = s.rings != 0;
    g_settings.debugBlurMip = s.blurMip;
    g_settings.targetWidth = s.targetWidth;
    g_settings.targetHeight = s.targetHeight;
    g_settings.gazeCombine = s.combine;
    g_settings.gazeFlipX = s.flipX != 0;
    g_settings.gazeFlipY = s.flipY != 0;
    g_settings.gazeForwardYawDegrees = s.forwardYaw;
    g_settings.gazeForwardPitchDegrees = s.forwardPitch;
    ++g_configurationRevision;
    // Timing statistics describe the current settings only.
    g_metrics.samples = 0;
    g_metrics.next = 0;
    g_foveationFrame.valid = false;
    g_foveationFrame.sawEyeBind = false;
    if (g_gpuFrameProbe) g_gpuFrameProbe->arm(s.mode, s.uniformRate);
    if (g_settings.mode == VrsMode::Off) {
        g_runtimeControl.applied(false, g_vrsState.renderWidth, g_vrsState.renderHeight);
    }
    logMessage("stage=vrs-control result=accepted revision=%llu mode=%u rings=%.2f,%.2f,%.2f,%.2f dither=%.2f "
               "uniform=%u",
               static_cast<unsigned long long>(g_configurationRevision), s.mode, s.ringDegrees[0], s.ringDegrees[1],
               s.ringDegrees[2], s.ringDegrees[3], s.ditherDegrees, s.uniformRate);
}

// Raster pixels per pixel-shader invocation for each tier.
constexpr float TierPixels[TierCount] = {1.0F, 2.0F, 4.0F, 8.0F, 16.0F};

void sampleFrameTimingLocked() {
    if (g_openXrActive) return;
#ifdef DVR_HAS_OPENVR
    static ULONGLONG nextResolveMs = 0;
    if (!g_metrics.compositor) {
        const auto now = GetTickCount64();
        if (now < nextResolveMs) return;
        nextResolveMs = now + 2000;
        HMODULE openVr = GetModuleHandleW(L"openvr_api.dll");
        const auto getInterface =
            openVr ? reinterpret_cast<GetGenericInterfaceFunction>(GetProcAddress(openVr, "VR_GetGenericInterface"))
                   : nullptr;
        int error = 0;
        g_metrics.compositor = getInterface ? getInterface(vr::IVRCompositor_Version, &error) : nullptr;
        if (g_metrics.compositor || !g_metrics.compositorResolved)
            logMessage("stage=metrics result=%s interface=%s error=%d",
                       g_metrics.compositor ? "frame-timing" : "frame-timing-unavailable", vr::IVRCompositor_Version,
                       error);
        g_metrics.compositorResolved = true;
        if (!g_metrics.compositor) return;
    }
    vr::Compositor_FrameTiming timing{};
    timing.m_nSize = sizeof(timing);
    // The current frame has no GPU times yet; the previous one does.
    if (!static_cast<vr::IVRCompositor*>(g_metrics.compositor)->GetFrameTiming(&timing, 1) ||
        timing.m_nFrameIndex == g_metrics.lastFrameIndex || !(timing.m_flPreSubmitGpuMs > 0.0F))
        return;
    g_metrics.lastFrameIndex = timing.m_nFrameIndex;
    g_metrics.sceneGpuMs[g_metrics.next] = timing.m_flPreSubmitGpuMs;
    g_metrics.intervalMs[g_metrics.next] = timing.m_flClientFrameIntervalMs;
    g_metrics.next = (g_metrics.next + 1) % g_metrics.sceneGpuMs.size();
    g_metrics.samples = std::min(g_metrics.samples + 1, g_metrics.sceneGpuMs.size());
#endif
}

float percentile(const std::array<float, 450>& values, std::size_t count, double fraction) {
    if (count == 0) return 0.0F;
    std::vector<float> sorted(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(count - 1) + 0.5);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), sorted.end());
    return sorted[index];
}

// Once per completed frame: the panel draws the sharp zone and gaze at display rate, so geometry
// and gaze go out every frame. The sorted timing statistics are refreshed every 250 ms.
void publishMetricsLocked() {
    const auto now = GetTickCount64();
    if (now - g_metrics.lastStatisticsMs >= 250) {
        g_metrics.lastStatisticsMs = now;
        g_metrics.gpuMsMedian = percentile(g_metrics.sceneGpuMs, g_metrics.samples, 0.5);
        g_metrics.gpuMsP95 = percentile(g_metrics.sceneGpuMs, g_metrics.samples, 0.95);
        g_metrics.frameIntervalMs = percentile(g_metrics.intervalMs, g_metrics.samples, 0.5);
    }
    dvr::control::Metrics m;
    // Rings as the rate image of this frame draws them.
    const float widen = g_foveationFrame.valid ? g_foveationFrame.widen : 0.0F;
    const auto degrees = appliedRingDegrees(widen);
    std::copy(degrees.begin(), degrees.end(), m.ringDegrees);
    for (int eye = 0; eye < 2; ++eye) {
        const auto rings = eyeRings(eye, widen);
        for (uint32_t ring = 0; ring < RingCount; ++ring)
            if (std::isfinite(rings.tangent[ring])) {
                m.radii[eye][ring][0] = rings.radiusX(ring);
                m.radii[eye][ring][1] = rings.radiusY(ring);
            }
    }
    if (g_foveationFrame.valid) {
        m.center[0] = g_foveationFrame.left.x;
        m.center[1] = g_foveationFrame.left.y;
    }
    m.widenDegrees = widen;
    m.gazeState = g_settings.mode == VrsMode::Gaze ? static_cast<uint32_t>(gazeStabilizerStateLocked()) : 0U;
    if (g_settings.mode == VrsMode::Off) {
        m.coverage[0] = 1.0F;
    } else if (g_settings.uniformRate != 0) {
        const auto tier = dvr::control::tierOfRate(static_cast<uint32_t>(g_settings.uniformRate));
        m.coverage[tier] = 1.0F;
        m.shadingWork = 1.0F / TierPixels[tier];
    } else {
        std::copy(std::begin(g_metrics.coverage), std::end(g_metrics.coverage), m.coverage);
        m.shadingWork = m.coverage[0];
        for (uint32_t tier = 1; tier < TierCount; ++tier) m.shadingWork += m.coverage[tier] / TierPixels[tier];
    }
    m.gpuMsMedian = g_metrics.gpuMsMedian;
    m.gpuMsP95 = g_metrics.gpuMsP95;
    m.frameIntervalMs = g_metrics.frameIntervalMs;
    m.timingSamples = static_cast<uint32_t>(g_metrics.samples);
    if (now - g_metrics.gazeWindowMs >= 1000) {
        g_metrics.gazeHz = g_metrics.gazeWindowMs ? static_cast<float>(g_metrics.gazePackets) * 1000.0F /
                                                        static_cast<float>(now - g_metrics.gazeWindowMs)
                                                  : 0.0F;
        g_metrics.gazePackets = 0;
        g_metrics.gazeWindowMs = now;
    }
    const bool gaze = g_settings.mode == VrsMode::Gaze;
    m.gazeConnected = gaze && g_vrsState.gaze.receiver.connected();
    m.gazeValid = m.gazeConnected && g_metrics.gazeValid;
    m.gazeHz = m.gazeConnected ? g_metrics.gazeHz : 0.0F;
    m.gazeAgeMs =
        m.gazeConnected && g_metrics.lastGazePacketMs ? static_cast<float>(now - g_metrics.lastGazePacketMs) : 0.0F;
    g_runtimeControl.publishMetrics(m);
}

} // namespace

namespace {
void finishControlFrameLocked(ID3D11DeviceContext* context) {
    dvr::control::Settings requested;
    if (g_runtimeControl.poll(requested)) applyRuntimeControlLocked(requested);
    if (g_gpuFrameProbe) g_gpuFrameProbe->frame(context);
    if (g_settings.mode == VrsMode::Off)
        g_runtimeControl.applied(false, g_vrsState.renderWidth, g_vrsState.renderHeight);
    sampleFrameTimingLocked();
    publishMetricsLocked();
}
} // namespace

void finishOpenXrControlFrame(ID3D11DeviceContext* context) {
    ContextThreadGuard deviceLock(context);
    std::scoped_lock lock(g_vrsMutex);
    finishControlFrameLocked(context);
    g_runtimeControl.heartbeat();
}

bool finishRuntimeControlFrame(int eye, const OpenVrTexture* texture) {
    if (!texture || texture->type != 0 || !texture->handle) return false;
    ContextThreadGuard deviceLock(static_cast<ID3D11Texture2D*>(texture->handle));
    std::scoped_lock lock(g_vrsMutex);
    if (eye == 0 || eye == 1) g_submittedEyeMask |= 1U << eye;
    const bool completed = g_submittedEyeMask == 3U;
    if (completed) {
        g_submittedEyeMask = 0;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        static_cast<ID3D11Texture2D*>(texture->handle)->GetDevice(&device);
        if (device) device->GetImmediateContext(&context);
        finishControlFrameLocked(context);
        releaseCom(context);
        releaseCom(device);
    }
    g_runtimeControl.heartbeat();
    return completed;
}

} // namespace dvr::layer
