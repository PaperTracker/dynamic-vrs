#include "vrs_state.hpp"

namespace dvr::layer {

std::vector<RtVrsEntry> g_rtVrsEntries;
std::mutex g_vrsMutex;
FixedVrsState g_vrsState;
dvr::control::Server g_runtimeControl;
uint64_t g_configurationRevision = 1;
uint64_t g_boundConfigurationRevision = 0;
std::atomic<bool> g_alignmentArmed = false;
std::unique_ptr<dvr::layer::diagnostics::TargetAlignmentProbe> g_alignmentProbe;
std::unique_ptr<dvr::layer::diagnostics::GpuFrameProbe> g_gpuFrameProbe;
FoveationFrameSample g_foveationFrame;
LiveMetrics g_metrics;
bool g_openXrActive = false;

} // namespace dvr::layer
