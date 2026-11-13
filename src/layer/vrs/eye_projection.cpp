#include "eye_projection.hpp"

#include "common/log.hpp"
#include "common/openvr_abi.hpp"
#include "vrs_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>

#ifdef DVR_HAS_OPENVR
#include <openvr.h>
#endif

namespace dvr::layer {

EyeViewportPoint g_fixedCenters[2];
EyeProjectionData g_eyeProjection[2];

namespace {

bool g_fixedCentersReady = false;

// Rings an old ini gave as viewport fractions become the angle they cover on the left eye, the mean of
// the horizontal and vertical radius. Runs once, when the eye projection first becomes available.
void resolveLegacyRingsLocked() {
    constexpr float degreesPerRadian = 180.0F / 3.14159265358979323846F;
    const auto [scaleX, scaleY] = eyeTangentScale(0);
    bool changed = false;
    for (uint32_t ring = 0; ring < RingCount; ++ring) {
        auto& fraction = g_settings.legacyRingFraction[ring];
        if (fraction[0] <= 0.0F || g_settings.ringDegrees[ring] > 0.0F) continue;
        const float degrees =
            0.5F * (std::atan(fraction[0] / scaleX) + std::atan(fraction[1] / scaleY)) * degreesPerRadian;
        g_settings.ringDegrees[ring] = std::round(degrees * 2.0F) / 2.0F;
        fraction = {};
        changed = true;
    }
    if (!changed) return;
    float previous = 0.0F;
    for (float& ring : g_settings.ringDegrees)
        if (ring > 0.0F) previous = ring = std::max(ring, previous);
    logMessage("stage=vrs-geometry result=legacy-fractions-converted rings=%.2f,%.2f,%.2f,%.2f",
               g_settings.ringDegrees[0], g_settings.ringDegrees[1], g_settings.ringDegrees[2],
               g_settings.ringDegrees[3]);
    g_runtimeControl.rebase(runtimeSettingsLocked());
}

// Projects an eye-local direction (w = 0, so IPD translation is not applied) into
// normalized viewport coordinates.
bool projectEyeDirection(const EyeProjectionData& eye, const float directionX, const float directionY,
                         const float directionZ, EyeViewportPoint& point, bool clampToViewport = false) {
    const float direction[4] = {directionX, directionY, directionZ, 0.0F};
    float clip[4]{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            clip[row] += eye.projection.m[row][column] * direction[column];
        }
        if (!std::isfinite(clip[row])) {
            return false;
        }
    }
    if (clip[3] <= 0.0F) {
        return false;
    }
    const float x = 0.5F * (1.0F + clip[0] / clip[3]);
    const float y = 0.5F * (1.0F - clip[1] / clip[3]);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        (!clampToViewport && (x < 0.0F || x > 1.0F || y < 0.0F || y > 1.0F))) {
        return false;
    }
    point = {std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F)};
    return true;
}

// Head-forward direction (0,0,-1 in head space) as seen from each eye viewport:
// the center the fixed pattern uses.
bool projectHeadForward(int eyeIndex, EyeViewportPoint& point) {
    const EyeProjectionData& eye = g_eyeProjection[eyeIndex];
    // Inverse eye rotation: R^T * (0,0,-1) = -R[2][*]. w = 0, so IPD does not enter.
    return projectEyeDirection(eye, -eye.eyeToHead.m[2][0], -eye.eyeToHead.m[2][1], -eye.eyeToHead.m[2][2], point);
}

} // namespace

// Viewport fraction per unit of tangent from the eye axis, per axis: the scale of every ring of that eye.
std::pair<float, float> eyeTangentScale(int eye) {
    if (g_eyeProjection[eye].ready) {
        const auto& p = g_eyeProjection[eye].projection;
        return {0.5F * std::abs(p.m[0][0]), 0.5F * std::abs(p.m[1][1])};
    }
    return {0.5F, 0.5F};
}

// Ring angles in effect: the configured rings pushed outward by the current widening. A skipped ring
// stays 0 and one that reaches the edge stays EdgeDegrees.
std::array<float, RingCount> appliedRingDegrees(float widen) {
    auto rings = g_settings.ringDegrees;
    for (float& ring : rings)
        if (ring > 0.0F && ring < EdgeDegrees) ring = std::min(ring + widen, EdgeDegrees - 1.0F);
    return rings;
}

EyeRings eyeRings(int eye, float widen) {
    constexpr float radiansPerDegree = 3.14159265358979323846F / 180.0F;
    EyeRings result;
    std::tie(result.scaleX, result.scaleY) = eyeTangentScale(eye);
    const auto degrees = appliedRingDegrees(widen);
    for (uint32_t ring = 0; ring < RingCount; ++ring)
        result.tangent[ring] = degrees[ring] >= EdgeDegrees ? std::numeric_limits<float>::infinity()
                                                            : std::tan(degrees[ring] * radiansPerDegree);
    return result;
}

bool ensureEyeProjection() {
    if (g_eyeProjection[0].ready && g_eyeProjection[1].ready) {
        return true;
    }
#ifdef DVR_HAS_OPENVR
    HMODULE openVr = GetModuleHandleW(L"openvr_api.dll");
    const auto getInterface =
        openVr ? reinterpret_cast<GetGenericInterfaceFunction>(GetProcAddress(openVr, "VR_GetGenericInterface"))
               : nullptr;
    int error = 0;
    auto* vrSystem = getInterface ? static_cast<vr::IVRSystem*>(getInterface(vr::IVRSystem_Version, &error)) : nullptr;
    if (vrSystem == nullptr) {
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        const auto eye = static_cast<vr::EVREye>(index);
        EyeProjectionData& data = g_eyeProjection[index];
        const auto projection = vrSystem->GetProjectionMatrix(eye, 0.1F, 100.0F);
        const auto transform = vrSystem->GetEyeToHeadTransform(eye);
        std::memcpy(data.projection.m, projection.m, sizeof(data.projection.m));
        std::memcpy(data.eyeToHead.m, transform.m, sizeof(data.eyeToHead.m));
        vrSystem->GetProjectionRaw(eye, &data.tangentLeft, &data.tangentRight, &data.tangentTop, &data.tangentBottom);
        data.ready = std::isfinite(data.tangentLeft) && std::isfinite(data.tangentRight) &&
                     std::isfinite(data.tangentTop) && std::isfinite(data.tangentBottom) &&
                     data.tangentRight > data.tangentLeft && data.tangentBottom > data.tangentTop;
    }
    if (!g_eyeProjection[0].ready || !g_eyeProjection[1].ready) {
        return false;
    }
    logMessage("stage=eye-projection result=ready rawL=%.3f,%.3f rawR=%.3f,%.3f", g_eyeProjection[0].tangentLeft,
               g_eyeProjection[0].tangentRight, g_eyeProjection[1].tangentLeft, g_eyeProjection[1].tangentRight);
    resolveLegacyRingsLocked();
    return true;
#else
    return false;
#endif
}

void resetEyeProjectionsLocked() {
    g_eyeProjection[0] = {};
    g_eyeProjection[1] = {};
    g_fixedCentersReady = false;
    g_foveationFrame = {};
}

void setEyeProjectionsLocked(const EyeProjectionData (&eyes)[2]) {
    bool changed = false;
    for (int eye = 0; eye < 2; ++eye) {
        if (!eyes[eye].ready) return;
        const auto& previous = g_eyeProjection[eye];
        changed |= !previous.ready;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                changed |=
                    std::abs(previous.projection.m[row][column] - eyes[eye].projection.m[row][column]) > 0.00001F;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                changed |= std::abs(previous.eyeToHead.m[row][column] - eyes[eye].eyeToHead.m[row][column]) > 0.00001F;
    }
    if (!changed) return;
    g_eyeProjection[0] = eyes[0];
    g_eyeProjection[1] = eyes[1];
    g_fixedCentersReady = false;
    g_foveationFrame.valid = false;
    ++g_configurationRevision;
    resolveLegacyRingsLocked();
}

// The export's angles name a point P = d * (tan(yaw), tan(pitch), -1) on the publisher's
// calibration plane in head space. The eye sees it along R^T * (P - t), where [R|t] is
// eyeToHead; that difference includes the parallax between the eye and the head origin.
bool projectGazeToViewport(int eyeIndex, const dvr::gaze::Angles& angles, EyeViewportPoint& point) {
    const EyeProjectionData& eye = g_eyeProjection[eyeIndex];
    constexpr float radiansPerDegree = 3.14159265358979323846F / 180.0F;
    const float flipX = g_settings.gazeFlipX ? -1.0F : 1.0F;
    const float flipY = g_settings.gazeFlipY ? -1.0F : 1.0F;
    const float d = dvr::gaze::PlaneDistanceMeters;
    const float head[3] = {d * (flipX * std::tan(angles.yawDegrees * radiansPerDegree) +
                                std::tan(g_settings.gazeForwardYawDegrees * radiansPerDegree)),
                           d * (flipY * std::tan(angles.pitchDegrees * radiansPerDegree) +
                                std::tan(g_settings.gazeForwardPitchDegrees * radiansPerDegree)),
                           -d};
    float local[3]{};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            local[column] += eye.eyeToHead.m[row][column] * (head[row] - eye.eyeToHead.m[row][3]);
        }
    }
    return projectEyeDirection(eye, local[0], local[1], local[2], point, true);
}

bool ensureFixedCenters() {
    if (g_fixedCentersReady) {
        return true;
    }
    if (ensureEyeProjection()) {
        EyeViewportPoint centers[2];
        if (projectHeadForward(0, centers[0]) && projectHeadForward(1, centers[1])) {
            g_fixedCenters[0] = centers[0];
            g_fixedCenters[1] = centers[1];
            g_fixedCentersReady = true;
            logMessage("stage=vrs-fixed-center result=ready left=%.6f,%.6f right=%.6f,%.6f basis=head-forward",
                       centers[0].x, centers[0].y, centers[1].x, centers[1].y);
            return true;
        }
    }
    static std::once_flag warningOnce;
    std::call_once(warningOnce, [] { logMessage("stage=vrs-fixed-center result=unavailable action=skip-vrs"); });
    return false;
}

} // namespace dvr::layer
