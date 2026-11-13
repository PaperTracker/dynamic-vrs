#pragma once
#include <windows.h>
#include <d3d11.h>

#include <nvapi.h>

#include "eye_viewport.hpp"
#include "layer/config/layer_config.hpp"

#include <vector>

namespace dvr::layer {

// Fills one tile per entry with the tier index of each tile of an eye target: concentric rings around each
// eye's center, dithered at their boundaries; a uniform pattern holds g_settings.uniformRate. An array layout
// gets one image per eye, one after the other. A per-eye target cannot be told apart by eye, so its image
// keeps whatever either eye needs sharp.
void fillFoveationPattern(std::vector<NvU8>& pattern, UINT tileWidth, UINT tileHeight, bool uniform, float leftCx,
                          float leftCy, float rightCx, float rightCy, float widen, UINT renderWidth = 0,
                          UINT renderHeight = 0, bool rasterFlipY = g_settings.rasterFlipY,
                          EyeLayout layout = EyeLayout::SideBySide, const EyeViewports* viewports = nullptr);

} // namespace dvr::layer
