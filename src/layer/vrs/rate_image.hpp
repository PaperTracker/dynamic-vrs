#pragma once
#include "vrs_state.hpp"

// The per-render-target NVAPI shading-rate images and their binding. Called under g_vrsMutex.

namespace dvr::layer {

void releaseRtVrsEntriesLocked();
// The rate image for a render target of this size and eye layout, created on first use; at most four are kept.
RtVrsEntry* ensureRtVrsEntryLocked(ID3D11Device* device, UINT width, UINT height,
                                   bool rasterFlipY = g_settings.rasterFlipY, EyeLayout layout = EyeLayout::SideBySide);
// Re-fills and uploads the pattern when the frame's centers, widening or configuration changed.
void refreshRtVrsPatternLocked(RtVrsEntry& entry, ID3D11DeviceContext* context);
// slice selects one eye's image of an array layout; -1 binds the whole image.
bool applyRtVrsLocked(ID3D11DeviceContext* context, RtVrsEntry& entry, int slice = -1);
void disableFixedVrsLocked(ID3D11DeviceContext* context);

} // namespace dvr::layer
