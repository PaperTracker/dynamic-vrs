#pragma once

#include <d3d11.h>

#include <array>
#include <cmath>

namespace dvr::layer {

enum class EyeLayout { Unknown, SideBySide, Array, PerEye };
inline UINT eyeLayoutLayers(EyeLayout layout) { return layout == EyeLayout::Array ? 2U : 1U; }

struct EyeViewportPoint {
    float x = 0.5F;
    float y = 0.5F;
};

struct EyeViewportRect {
    float left = 0, top = 0, width = 0, height = 0;
    bool operator==(const EyeViewportRect&) const = default;
};
using EyeViewports = std::array<EyeViewportRect, 2>;

inline EyeViewports fullEyeViewports(EyeLayout layout, float width, float height) {
    if (layout == EyeLayout::SideBySide)
        return {{{0, 0, width * 0.5F, height}, {width * 0.5F, 0, width * 0.5F, height}}};
    return {{{0, 0, width, height}, {0, 0, width, height}}};
}

inline bool resolveEyeViewports(EyeLayout layout, UINT width, UINT height, UINT count, const D3D11_VIEWPORT* viewports,
                                EyeViewports& eyes) {
    if (layout == EyeLayout::Unknown || !width || !height || !viewports || count == 0 || count > 2) return false;
    for (UINT index = 0; index < count; ++index) {
        const auto& view = viewports[index];
        if (!std::isfinite(view.TopLeftX) || !std::isfinite(view.TopLeftY) || !std::isfinite(view.Width) ||
            !std::isfinite(view.Height) || view.Width <= 0 || view.Height <= 0)
            return false;
        eyes[index] = {view.TopLeftX, view.TopLeftY, view.Width, view.Height};
    }
    if (count == 2) return true;
    eyes[1] = eyes[0];
    // A single viewport can cover packed stereo or one eye. For an eye-sized pass,
    // retain both eyes' sharp regions, as with a separate-eye render target.
    const float eyeAspect = float(width) / (2.0F * float(height));
    if (layout == EyeLayout::SideBySide && eyes[0].width / eyes[0].height >= 1.5F * eyeAspect) {
        eyes[0].width *= 0.5F;
        eyes[1].left += eyes[0].width;
        eyes[1].width = eyes[0].width;
    }
    return true;
}

inline bool eyeViewportPoint(const EyeViewportRect& viewport, float pixelX, float pixelY, bool flipY,
                             EyeViewportPoint& point) {
    if (viewport.width <= 0 || viewport.height <= 0) return false;
    const float x = (pixelX - viewport.left) / viewport.width;
    const float y = (pixelY - viewport.top) / viewport.height;
    point = {x, flipY ? 1.0F - y : y};
    return x >= 0 && x < 1 && y >= 0 && y < 1;
}

} // namespace dvr::layer
