#pragma once
#include <windows.h>
#include "common/gaze_udp_receiver.hpp"
#include <d3d11.h>

#include <nvapi.h>

#include "eye_projection.hpp"
#include "layer/control/control_server.hpp"
#include "layer/diagnostics/alignment_probe.hpp"
#include "layer/diagnostics/gpu_frame_probe.hpp"
#include "rate_pattern.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Render-side state shared by the layer's VRS modules. Everything here is guarded by g_vrsMutex; Submit-side
// code takes the device's multithread lock first (ContextThreadGuard).

namespace dvr::layer {

template <typename T> void releaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

// The gaze data channel is UDP; the independent control panel keeps its own IPC.
struct GazeReader {
    dvr::gaze::Receiver receiver;
    unsigned loggedError = 0;
    bool openLogged = false, connectedLogged = false, disabledLogged = false;
    uint64_t lastSequence = 0;
    float leftX = 0.5F, leftY = 0.5F, rightX = 0.5F, rightY = 0.5F;
    unsigned int updates = 0;
};

struct FixedVrsState {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    UINT renderWidth = 0;
    UINT renderHeight = 0;
    DXGI_FORMAT submittedFormat = DXGI_FORMAT_UNKNOWN;
    EyeLayout layout = EyeLayout::Unknown;
    void* leftEyeHandle = nullptr; // the texture last submitted for the left eye
    // The game submits its eye image upside down (vMin > vMax), as Unity does for render textures; its
    // eye targets are then upside down too, and so must the rate image be.
    bool submittedFlipY = false;
    GazeReader gaze;
    bool nvapiInitialized = false;
    bool active = false;
    unsigned int targetMismatchLogs = 0;
};

struct RtVrsEntry {
    ID3D11Device* device = nullptr;
    UINT width = 0;
    UINT height = 0;
    bool rasterFlipY = false;
    EyeLayout layout = EyeLayout::SideBySide;
    EyeViewports viewports;
    UINT tileWidth = 0;
    UINT tileHeight = 0;
    ID3D11Texture2D* rateTexture = nullptr;
    ID3D11NvShadingRateResourceView* rateView = nullptr;
    // A single-slice RTV addresses layer zero of the bound rate view. Give it a 2D image for its eye.
    ID3D11Texture2D* sliceTextures[2] = {};
    ID3D11NvShadingRateResourceView* sliceViews[2] = {};
    std::vector<NvU8> pattern;
    uint64_t lastGazeSequence = 0;
    bool patternValid = false;
    uint64_t appliedConfigurationRevision = 0;
    // The centers actually uploaded to this RT's rate texture, also used by
    // the diagnostic blur so it cannot move independently of the VRS pattern.
    EyeViewportPoint appliedLeft;
    EyeViewportPoint appliedRight;
    float appliedWiden = 0.0F;
    unsigned int binds = 0;
    unsigned int patternUpdates = 0;
    unsigned int verifications = 0;
    unsigned int patternLogs = 0;
    bool dumpWritten = false;
};

struct FoveationFrameSample {
    EyeViewportPoint left, right;
    float widen = 0.0F; // degrees every ring is pushed outward this frame
    uint64_t sequence = 0, configurationRevision = 0;
    bool valid = false, sawEyeBind = false;
};

// Rolling measurements for the control panel, written under g_vrsMutex.
struct LiveMetrics {
    std::array<float, 450> sceneGpuMs{}, intervalMs{}; // about five seconds at 90 Hz
    std::size_t samples = 0, next = 0;
    uint32_t lastFrameIndex = 0;
    void* compositor = nullptr;
    bool compositorResolved = false;
    float coverage[TierCount] = {};
    uint32_t lastGazeSequence = 0, gazePackets = 0;
    bool gazeValid = false;
    ULONGLONG gazeWindowMs = 0, lastGazePacketMs = 0, lastStatisticsMs = 0;
    float gazeHz = 0, gpuMsMedian = 0, gpuMsP95 = 0, frameIntervalMs = 0;
};

extern std::mutex g_vrsMutex;
extern FixedVrsState g_vrsState;
extern std::vector<RtVrsEntry> g_rtVrsEntries;
extern dvr::control::Server g_runtimeControl;
// Bumped by every accepted control change; rate images and binds record the revision they reflect.
extern uint64_t g_configurationRevision;
extern uint64_t g_boundConfigurationRevision;
// The gaze sample every pass of the current frame uses.
extern FoveationFrameSample g_foveationFrame;
extern LiveMetrics g_metrics;
extern bool g_openXrActive;
extern std::atomic<bool> g_alignmentArmed;
extern std::unique_ptr<dvr::layer::diagnostics::TargetAlignmentProbe> g_alignmentProbe;
extern std::unique_ptr<dvr::layer::diagnostics::GpuFrameProbe> g_gpuFrameProbe;

} // namespace dvr::layer
