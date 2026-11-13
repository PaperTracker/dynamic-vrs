#include "rate_pattern.hpp"

#include "eye_projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace dvr::layer {
namespace {

// The tiers one eye's pattern steps through: the rings in use, each with the tangent band its boundary is
// dithered over, then the tier beyond the last ring.
struct TierLadder {
    uint32_t count = 0;
    NvU8 tier[RingCount] = {};
    float low[RingCount] = {}, span[RingCount] = {};
    NvU8 rest = dvr::control::Tier4x4;
};

TierLadder tierLadder(float widen) {
    constexpr float radiansPerDegree = 3.14159265358979323846F / 180.0F;
    TierLadder ladder;
    const auto degrees = appliedRingDegrees(widen);
    for (uint32_t ring = 0; ring < RingCount; ++ring) {
        if (degrees[ring] <= 0.0F) continue;
        if (degrees[ring] >= EdgeDegrees) {
            ladder.rest = static_cast<NvU8>(ring);
            break;
        }
        // Dithering spreads the boundary evenly over the band on both sides of the ring.
        const float half = 0.5F * g_settings.ditherDegrees;
        const float low = std::tan(std::max(degrees[ring] - half, 0.0F) * radiansPerDegree);
        const float high = std::tan(std::min(degrees[ring] + half, EdgeDegrees - 1.0F) * radiansPerDegree);
        ladder.tier[ladder.count] = static_cast<NvU8>(ring);
        ladder.low[ladder.count] = low;
        ladder.span[ladder.count] = high - low;
        ++ladder.count;
    }
    return ladder;
}

} // namespace

void fillFoveationPattern(std::vector<NvU8>& pattern, UINT tileWidth, UINT tileHeight, bool uniform, float leftCx,
                          float leftCy, float rightCx, float rightCy, float widen, UINT renderWidth, UINT renderHeight,
                          bool rasterFlipY, EyeLayout layout, const EyeViewports* viewports) {
    const UINT layers = eyeLayoutLayers(layout);
    const std::size_t layerTiles = static_cast<std::size_t>(tileWidth) * tileHeight;
    pattern.assign(layerTiles * layers,
                   static_cast<NvU8>(dvr::control::tierOfRate(static_cast<uint32_t>(g_settings.uniformRate))));
    if (uniform) {
        return;
    }
    // 4x4 ordered dither: neighbouring boundary tiles take thresholds far apart, so a band reads as a
    // fine mix of both tiers instead of a second edge.
    static constexpr float Bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const float width =
        static_cast<float>(renderWidth ? renderWidth : tileWidth * NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
    const float height =
        static_cast<float>(renderHeight ? renderHeight : tileHeight * NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);
    const EyeViewports eyes = viewports ? *viewports : fullEyeViewports(layout, width, height);
    const TierLadder ladder = tierLadder(widen);
    const std::pair<float, float> scales[2] = {eyeTangentScale(0), eyeTangentScale(1)};
    const float centerX[2] = {leftCx, rightCx};
    const float centerY[2] = {leftCy, rightCy};
    const auto tierAt = [&](int eye, float eyeLocalX, float displayY, float threshold) {
        // Tangent distance from the center: the rings are ellipses of tan(angle) times the eye's scale.
        const float dx = (eyeLocalX - centerX[eye]) / scales[eye].first;
        const float dy = (displayY - centerY[eye]) / scales[eye].second;
        const float distance = std::sqrt(dx * dx + dy * dy);
        for (uint32_t step = 0; step < ladder.count; ++step)
            if (distance <= ladder.low[step] + threshold * ladder.span[step]) return ladder.tier[step];
        return ladder.rest;
    };
    for (UINT layer = 0; layer < layers; ++layer) {
        NvU8* const tiles = pattern.data() + layerTiles * layer;
        for (UINT y = 0; y < tileHeight; ++y) {
            const float pixelY = (static_cast<float>(y) + 0.5F) * NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
            for (UINT x = 0; x < tileWidth; ++x) {
                const float pixelX = (static_cast<float>(x) + 0.5F) * NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
                const float threshold = (Bayer[y & 3U][x & 3U] + 0.5F) / 16.0F;
                NvU8 tier = 0;
                EyeViewportPoint points[2];
                const bool inside[2] = {eyeViewportPoint(eyes[0], pixelX, pixelY, rasterFlipY, points[0]),
                                        eyeViewportPoint(eyes[1], pixelX, pixelY, rasterFlipY, points[1])};
                if (layout == EyeLayout::Array) {
                    if (inside[layer])
                        tier = tierAt(static_cast<int>(layer), points[layer].x, points[layer].y, threshold);
                } else if (inside[0] || inside[1]) {
                    const auto left =
                        inside[0] ? tierAt(0, points[0].x, points[0].y, threshold) : dvr::control::Tier4x4;
                    const auto right =
                        inside[1] ? tierAt(1, points[1].x, points[1].y, threshold) : dvr::control::Tier4x4;
                    tier = static_cast<NvU8>(std::min(left, right));
                }
                tiles[static_cast<std::size_t>(y) * tileWidth + x] = tier;
            }
        }
    }
}

} // namespace dvr::layer
