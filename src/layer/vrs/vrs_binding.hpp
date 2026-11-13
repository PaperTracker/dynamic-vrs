#pragma once
#include <d3d11.h>

#include "common/openvr_abi.hpp"
#include "rate_pattern.hpp"

// Chooses the render targets that receive the eye rate image and drives the per-frame VRS state from Submit.

namespace dvr::layer {

// The submitted eye texture's size (or the configured vrsTargetWidth/Height), with a view that fits the
// submitted eye layout: one layer, or both slices of an eye array.
bool matchesVrsTarget(const D3D11_TEXTURE2D_DESC& target, const D3D11_RENDER_TARGET_VIEW_DESC& view);
// Called from the OMSetRenderTargets hook under g_vrsMutex: binds the rate image to an eye target, or unbinds.
void applyVrsToTargetsLocked(ID3D11DeviceContext* context, UINT renderTargetViewCount,
                             ID3D11RenderTargetView* const* renderTargetViews);
// Called after each successful Submit; completedFrame starts the next frame's gaze sample.
void enableFixedVrsForNextFrame(int eye, const OpenVrTexture* texture, const OpenVrTextureBounds* bounds,
                                bool completedFrame = true);
// OpenXR supplies the layout from projection subimages and calls this before rendering a waited image.
void prepareOpenXrVrs(ID3D11Texture2D* texture, EyeLayout layout, bool completedFrame = false);
void releaseD3d11Vrs(ID3D11DeviceContext* context);

} // namespace dvr::layer
