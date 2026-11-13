#pragma once
#include "common/vrs_control_protocol.hpp"
#include "eye_projection.hpp"

#include <cstdint>

// The foveation centers of each frame: the PaperTracker gaze export, read and stabilized in gaze mode,
// otherwise the head-forward centers. Called under g_vrsMutex.

namespace dvr::layer {

// Samples the centers once per frame into g_foveationFrame.
bool latchFoveationFrameLocked();
// The latched centers, re-latched after a control change.
bool frameFoveationCenters(EyeViewportPoint& left, EyeViewportPoint& right, uint64_t* sequence = nullptr,
                           float* widen = nullptr);
// Keep source metadata separate from the render-target-owned GPU rate images.
void updateGazeSampleLocked();
void resetGazeStateLocked();
dvr::control::GazeState gazeStabilizerStateLocked();

} // namespace dvr::layer
