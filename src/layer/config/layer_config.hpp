#pragma once
#include <windows.h>
#include <dxgiformat.h>

#include "common/gaze_packet.hpp"
#include "common/vrs_control_protocol.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace dvr::layer {

using dvr::control::EdgeDegrees;
using dvr::control::RingCount;
using dvr::control::TierCount;

enum class VrsMode {
    Off,
    FixedCenter,
    Gaze,
};

// Gaze stabilization (ini keys of the same name). The sharp zone holds still while the eye fixates and every
// ring is pushed outward while it moves fast or the tracker loses it.
struct GazeStabilizerConfig {
    float deadbandDegrees = 0.5F;        // gazeDeadbandDegrees: movement the rings ignore
    float smoothingHz = 4.0F;            // gazeSmoothingHz: low-pass cutoff while fixating; 0 = off
    float saccadeDegreesPerSecond = 180; // gazeSaccadeDegreesPerSecond: faster than this is a saccade
    float saccadeWidenDegrees = 4.0F;    // gazeSaccadeWidenDegrees
    float saccadeHoldMs = 150;           // gazeSaccadeHoldMs: widening kept after the last fast sample
    float lostWidenDegrees = 8.0F;       // gazeLostWidenDegrees: invalid, silent or disconnected gaze
    float staleMs = 300;                 // gazeStaleMs: a valid point this old counts as stale; 0 = never
    float lostMs = 1000;                 // gazeLostMs: after this long the center drifts to head-forward
    float releaseMs = 250;               // gazeWidenReleaseMs: time constant of the return to normal
};

// The layer's settings: read from [loader] in dynamic-vrs.ini at startup, after which the runtime control
// changes most of them. Read and written under g_vrsMutex once the layer runs. dvr::control::Settings is the
// wire form of the subset the control panel edits.
struct LayerSettings {
    VrsMode mode = VrsMode::Off;
    // PaperTracker's gaze export port; this layer subscribes to it.
    std::uint16_t gazeExportPort = dvr::gaze::DefaultPort;
    // Gaze axis correction (ini: gazeFlipX / gazeFlipY). The export convention is yaw right and pitch up;
    // flip only to diagnose a mirrored setup.
    bool gazeFlipX = false;
    bool gazeFlipY = false;
    // Application render-target orientation, separate from gaze/output coordinates.
    // Startup-only diagnostic override until the target-to-submit transform is verified.
    bool rasterFlipY = false;
    // Optional application-specific rule, established by target/submit correlation.
    DXGI_FORMAT rasterFlipYFormat = DXGI_FORMAT_UNKNOWN;
    UINT rasterFlipYSamples = 1;
    // Outer radius in degrees of the 1x1, 2x1, 2x2 and 4x2 tiers (ini: vrsRing*Degrees); 4x4 beyond the last.
    // 0 skips a tier, EdgeDegrees fills the rest of the view with it. Default: 1x1, 2x1, then 2x2 to the edge.
    std::array<float, RingCount> ringDegrees = {15.0F, 25.0F, EdgeDegrees, 0.0F};
    // Rings still given as a fraction of the eye viewport by an old ini (vrsInnerRadiusX/Y and
    // vrsMiddleRadiusX/Y without degrees): converted to degrees once the eye projection is known.
    // X and Y radius per ring; 0/0 for a ring given in degrees.
    std::array<std::array<float, 2>, RingCount> legacyRingFraction = {};
    // Width of the dithered band around each ring, in degrees; 0 = hard edges.
    float ditherDegrees = 0.0F;
    // Decisive test: nonzero applies that rate to the whole target, leaving no sharp region. Values match
    // vrsOuterRate.
    int uniformRate = 0;
    // Explicit diagnostic size, or 0/0 to follow the current submitted eye texture.
    UINT targetWidth = 0;
    UINT targetHeight = 0;
    // 0 = per-eye centers (each ellipse follows its own eye), 1 = average both eyes and share one center.
    // Independent eyes can leave the two sharp regions slightly offset, which reads as double vision.
    int gazeCombine = 0;
    // Shifts the received gaze points on the publisher's plane, in head-origin degrees.
    float gazeForwardYawDegrees = 0.0F;
    float gazeForwardPitchDegrees = 0.0F;
    GazeStabilizerConfig gazeStabilizer;
    // Optional presentation aids, independent of VRS quality/performance measurements.
    int debugBlurMip = 0;
    bool debugRadiusRings = false;
    bool debugTargetAlignment = false;
};

extern LayerSettings g_settings;
// The dynamic-vrs.ini this process reads; empty until the layer is initialized.
extern std::filesystem::path g_configPath;

std::filesystem::path layerConfigPath();
// Reads every [loader] setting into g_settings, including the startup mode.
void loadLayerSettings();
// The current settings in the control protocol's form.
dvr::control::Settings runtimeSettingsLocked();

} // namespace dvr::layer
