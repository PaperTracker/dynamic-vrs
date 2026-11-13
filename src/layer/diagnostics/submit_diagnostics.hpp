#pragma once
#include <d3d11.h>

#include "common/openvr_abi.hpp"

// Diagnostics applied at Submit: the target alignment probe and the debug blur / radius rings overlay.

namespace dvr::layer {

// Records the currently bound eye target for the armed alignment probe. Called under g_vrsMutex.
void observeAlignmentTargetLocked(ID3D11DeviceContext* context);
// Scores the recorded targets against the submitted left eye once, then disarms the probe.
void captureAlignmentSubmission(int eye, const OpenVrTexture* texture);
// Draw into the submitted color image. The original submission structure, flags
// and auxiliary textures remain owned by the application and pass through intact.
bool drawSubmissionVisualAid(int eye, const OpenVrTexture* texture, const OpenVrTextureBounds* bounds);
bool drawOpenXrVisualAid(int eye, ID3D11Texture2D* texture);

} // namespace dvr::layer
