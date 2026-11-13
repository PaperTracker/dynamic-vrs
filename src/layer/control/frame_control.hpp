#pragma once
#include "common/openvr_abi.hpp"

struct ID3D11DeviceContext;

// The runtime control channel at frame granularity: applies control-panel changes and publishes metrics.

namespace dvr::layer {

// Called after each successful Submit. Returns true once both eyes of a frame were submitted.
bool finishRuntimeControlFrame(int eye, const OpenVrTexture* texture);
void finishOpenXrControlFrame(ID3D11DeviceContext* context);

} // namespace dvr::layer
