#pragma once
#include "common/gaze_packet.hpp"
#include "eye_viewport.hpp"
#include "layer/config/layer_config.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace dvr::layer {

// The rings of one eye as fractions of its viewport: tangent distance times scale. A skipped ring has
// radius 0; one that reaches the edge is infinite.
struct EyeRings {
    float scaleX = 0.5F, scaleY = 0.5F;
    std::array<float, RingCount> tangent{};
    float radiusX(uint32_t ring) const { return scaleX * tangent[ring]; }
    float radiusY(uint32_t ring) const { return scaleY * tangent[ring]; }
};

// Head-forward centers of each eye viewport, set once by ensureFixedCenters.
extern EyeViewportPoint g_fixedCenters[2];

struct EyeProjectionData {
    bool ready = false;
    struct {
        float m[4][4]{};
    } projection;
    struct {
        float m[3][4]{};
    } eyeToHead;
    float tangentLeft = 0.0F;
    float tangentRight = 0.0F;
    float tangentTop = 0.0F;
    float tangentBottom = 0.0F;
};

extern EyeProjectionData g_eyeProjection[2];

// Reads both eyes' projection from IVRSystem once it is available; converts legacy ring fractions then.
bool ensureEyeProjection();
// OpenXR supplies projections and eye-to-head transforms at the frame's display time.
// Called under g_vrsMutex. Invalidates cached centers and patterns when they change.
void setEyeProjectionsLocked(const EyeProjectionData (&eyes)[2]);
void resetEyeProjectionsLocked();
// The export's angles name a point P = d * (tan(yaw), tan(pitch), -1) on the publisher's
// calibration plane in head space. The eye sees it along R^T * (P - t), where [R|t] is
// eyeToHead; that difference includes the parallax between the eye and the head origin.
bool projectGazeToViewport(int eyeIndex, const dvr::gaze::Angles& angles, EyeViewportPoint& point);

// Viewport fraction per unit of tangent from the eye axis, per axis: the scale of every ring of that eye.
std::pair<float, float> eyeTangentScale(int eye);
// Ring angles in effect: the configured rings pushed outward by the current widening. A skipped ring
// stays 0 and one that reaches the edge stays EdgeDegrees.
std::array<float, RingCount> appliedRingDegrees(float widen);
EyeRings eyeRings(int eye, float widen);
bool ensureFixedCenters();

} // namespace dvr::layer
