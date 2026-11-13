#include "rate_image.hpp"

#include "common/log.hpp"
#include "common/paths.hpp"
#include "gaze_tracking.hpp"
#include "rate_pattern.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace dvr::layer {
namespace {

void releaseRtVrsEntry(RtVrsEntry& entry) {
    for (auto& view : entry.sliceViews) releaseCom(view);
    releaseCom(entry.rateView);
    releaseCom(entry.rateTexture);
    releaseCom(entry.device);
    entry = RtVrsEntry{};
}

// Read the rate texture back to the CPU to confirm the tile indices the GPU holds: center 0 (1x1), corners the
// outermost tier.
void verifyRtVrsTextureLocked(RtVrsEntry& entry, ID3D11DeviceContext* context) {
    if (context == nullptr || entry.device == nullptr || entry.rateTexture == nullptr) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    entry.rateTexture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(entry.device->CreateTexture2D(&desc, nullptr, &staging))) {
        logMessage("stage=vrs-verify result=staging-create-failed render=%ux%u", entry.width, entry.height);
        return;
    }
    context->CopyResource(staging, entry.rateTexture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData != nullptr) {
        const auto* data = static_cast<const unsigned char*>(mapped.pData);
        if (!entry.dumpWritten) {
            // Dump the GPU rate image as PGM (P5) so the sharp region can be located visually.
            {
                const std::filesystem::path dumpRoot = logDirectory();
                std::error_code error;
                std::filesystem::create_directories(dumpRoot, error);
                const auto dumpPath = dumpRoot / (L"vrs-rate-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                                  std::to_wstring(entry.width) + L"x" + std::to_wstring(entry.height) +
                                                  (entry.rasterFlipY ? L"-flipY" : L"") + L".pgm");
                std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
                if (dump) {
                    dump << "P5\n" << entry.tileWidth << " " << entry.tileHeight << "\n255\n";
                    for (UINT y = 0; y < entry.tileHeight; ++y) {
                        dump.write(reinterpret_cast<const char*>(data + static_cast<std::size_t>(y) * mapped.RowPitch),
                                   entry.tileWidth);
                    }
                    dump.close();
                    entry.dumpWritten = true;
                    logMessage("stage=vrs-dump result=written render=%ux%u path=\"%s\"", entry.width, entry.height,
                               wideToUtf8(dumpPath.wstring()).c_str());
                }
            }
        }
        const auto sampleAt = [&](const UINT x, const UINT y) {
            const UINT clampedX = x < entry.tileWidth ? x : entry.tileWidth - 1;
            const UINT clampedY = y < entry.tileHeight ? y : entry.tileHeight - 1;
            return static_cast<unsigned>(data[static_cast<std::size_t>(clampedY) * mapped.RowPitch + clampedX]);
        };
        const UINT centerX = entry.tileWidth / 2;
        const UINT centerY = entry.tileHeight / 2;
        logMessage("stage=vrs-verify render=%ux%u tiles=%ux%u center=%u quarterX=%u corner=%u edgeMid=%u", entry.width,
                   entry.height, entry.tileWidth, entry.tileHeight, sampleAt(centerX, centerY),
                   sampleAt(entry.tileWidth / 4, centerY), sampleAt(0, 0), sampleAt(entry.tileWidth - 1, centerY));
        context->Unmap(staging, 0);
    } else {
        logMessage("stage=vrs-verify result=map-failed render=%ux%u", entry.width, entry.height);
    }
    staging->Release();
}

} // namespace

void releaseRtVrsEntriesLocked() {
    for (RtVrsEntry& entry : g_rtVrsEntries) {
        releaseRtVrsEntry(entry);
    }
    g_rtVrsEntries.clear();
}

RtVrsEntry* ensureRtVrsEntryLocked(ID3D11Device* device, UINT width, UINT height, bool rasterFlipY, EyeLayout layout) {
    if (device == nullptr || width == 0 || height == 0) {
        return nullptr;
    }
    for (RtVrsEntry& entry : g_rtVrsEntries) {
        if (entry.device == device && entry.width == width && entry.height == height &&
            entry.rasterFlipY == rasterFlipY && entry.layout == layout) {
            return &entry;
        }
    }
    if (g_rtVrsEntries.size() >= 4U) {
        releaseRtVrsEntry(g_rtVrsEntries.front());
        g_rtVrsEntries.erase(g_rtVrsEntries.begin());
    }
    const UINT tileWidth = (width + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
    const UINT tileHeight =
        (height + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
    if (tileWidth == 0 || tileHeight == 0) {
        return nullptr;
    }

    RtVrsEntry entry;
    entry.device = device;
    entry.width = width;
    entry.height = height;
    entry.rasterFlipY = rasterFlipY;
    entry.layout = layout;
    entry.viewports = fullEyeViewports(layout, float(width), float(height));
    entry.tileWidth = tileWidth;
    entry.tileHeight = tileHeight;
    const UINT layers = eyeLayoutLayers(layout);
    entry.pattern.assign(static_cast<std::size_t>(tileWidth) * tileHeight * layers, 2);

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = tileWidth;
    textureDesc.Height = tileHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = layers;
    textureDesc.Format = DXGI_FORMAT_R8_UINT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, &entry.rateTexture))) {
        return nullptr;
    }
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    viewDesc.Format = DXGI_FORMAT_R8_UINT;
    bool created = false;
    if (layers == 1) {
        viewDesc.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
        created = NvAPI_D3D11_CreateShadingRateResourceView(device, entry.rateTexture, &viewDesc, &entry.rateView) ==
                  NVAPI_OK;
    } else {
        // The driver picks the slice of the rate image by the array slice a primitive is rendered to.
        viewDesc.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.MipSlice = 0;
        viewDesc.Texture2DArray.FirstArraySlice = 0;
        viewDesc.Texture2DArray.ArraySize = layers;
        created = NvAPI_D3D11_CreateShadingRateResourceView(device, entry.rateTexture, &viewDesc, &entry.rateView) ==
                  NVAPI_OK;
        viewDesc.Texture2DArray.ArraySize = 1;
        for (UINT slice = 0; created && slice < layers; ++slice) {
            viewDesc.Texture2DArray.FirstArraySlice = slice;
            created = NvAPI_D3D11_CreateShadingRateResourceView(device, entry.rateTexture, &viewDesc,
                                                                &entry.sliceViews[slice]) == NVAPI_OK;
        }
    }
    if (!created) {
        logMessage("stage=vrs-rt result=view-create-failed render=%ux%u layers=%u", width, height, layers);
        for (auto& view : entry.sliceViews) releaseCom(view);
        releaseCom(entry.rateView);
        releaseCom(entry.rateTexture);
        return nullptr;
    }
    device->AddRef();
    g_rtVrsEntries.push_back(std::move(entry));
    logMessage("stage=vrs-rt result=created render=%ux%u tiles=%ux%u layers=%u rasterFlipY=%d entries=%zu", width,
               height, tileWidth, tileHeight, layers, rasterFlipY ? 1 : 0, g_rtVrsEntries.size());
    return &g_rtVrsEntries.back();
}

void refreshRtVrsPatternLocked(RtVrsEntry& entry, ID3D11DeviceContext* context) {
    const bool uniform = g_settings.uniformRate != 0;
    EyeViewportPoint left;
    EyeViewportPoint right;
    uint64_t sequence = 0;
    float widen = 0.0F;
    if (!uniform && !frameFoveationCenters(left, right, &sequence, &widen)) return;
    const float leftX = left.x;
    const float leftY = left.y;
    const float rightX = right.x;
    const float rightY = right.y;
    const bool changed = !entry.patternValid || entry.appliedConfigurationRevision != g_configurationRevision ||
                         leftX != entry.appliedLeft.x || leftY != entry.appliedLeft.y ||
                         rightX != entry.appliedRight.x || rightY != entry.appliedRight.y ||
                         widen != entry.appliedWiden;
    if (!changed) return;
    entry.lastGazeSequence = sequence;
    entry.appliedLeft = left;
    entry.appliedRight = right;
    entry.appliedWiden = widen;
    fillFoveationPattern(entry.pattern, entry.tileWidth, entry.tileHeight, uniform, leftX, leftY, rightX, rightY, widen,
                         entry.width, entry.height, entry.rasterFlipY, entry.layout, &entry.viewports);
    // Tile share per tier: 0 = 1x1 sharp, 1 = 2x1, 2 = 2x2, 3 = 4x2, 4 = 4x4.
    std::size_t counts[TierCount]{};
    for (const NvU8 value : entry.pattern) ++counts[std::min<std::size_t>(value, TierCount - 1)];
    const std::size_t totalTiles = entry.pattern.size();
    if (totalTiles != 0U)
        for (uint32_t tier = 0; tier < TierCount; ++tier)
            g_metrics.coverage[tier] = static_cast<float>(counts[tier]) / static_cast<float>(totalTiles);
    const EyeRings rings = eyeRings(0, widen);
    if (entry.patternLogs < 6U) {
        ++entry.patternLogs;
        logMessage("stage=vrs-radii render=%ux%u left=(%.3f,%.3f) right=(%.3f,%.3f) widen=%.2f "
                   "rX=%.4f,%.4f,%.4f,%.4f rY=%.4f,%.4f,%.4f,%.4f",
                   entry.width, entry.height, leftX, leftY, rightX, rightY, widen, rings.radiusX(0), rings.radiusX(1),
                   rings.radiusX(2), rings.radiusX(3), rings.radiusY(0), rings.radiusY(1), rings.radiusY(2),
                   rings.radiusY(3));
    }
    if (context != nullptr && entry.rateTexture != nullptr) {
        const std::size_t layerTiles = static_cast<std::size_t>(entry.tileWidth) * entry.tileHeight;
        for (UINT layer = 0; layer < eyeLayoutLayers(entry.layout); ++layer)
            context->UpdateSubresource(entry.rateTexture, layer, nullptr, entry.pattern.data() + layerTiles * layer,
                                       entry.tileWidth, 0);
        if (entry.verifications < 2U) {
            ++entry.verifications;
            verifyRtVrsTextureLocked(entry, context);
        }
    }
    entry.patternValid = true;
    entry.appliedConfigurationRevision = g_configurationRevision;
    ++entry.patternUpdates;
    if (entry.patternUpdates <= 8U || (entry.patternUpdates % 600U) == 0U) {
        // Shows whether the image is really sharp in the center and cheaper toward the edge.
        double percent[TierCount]{};
        for (uint32_t tier = 0; tier < TierCount && totalTiles; ++tier)
            percent[tier] = 100.0 * static_cast<double>(counts[tier]) / static_cast<double>(totalTiles);
        logMessage("stage=vrs-rt-gaze result=updated render=%ux%u rasterFlipY=%d updates=%u seq=%llu left=%.3f,%.3f "
                   "right=%.3f,%.3f uniform=%d widen=%.2f tiers=%.1f/%.1f/%.1f/%.1f/%.1f%% tiles=%ux%u r=(%.4f,%.4f) "
                   "tileC=(%.1f,%.1f) tileR=(%.1f,%.1f)",
                   entry.width, entry.height, entry.rasterFlipY ? 1 : 0, entry.patternUpdates,
                   static_cast<unsigned long long>(entry.lastGazeSequence), leftX, leftY, rightX, rightY,
                   g_settings.uniformRate, widen, percent[0], percent[1], percent[2], percent[3], percent[4],
                   entry.tileWidth, entry.tileHeight, rings.radiusX(0), rings.radiusY(0),
                   leftX * 0.5F * static_cast<float>(entry.tileWidth),
                   (entry.rasterFlipY ? 1.0F - leftY : leftY) * static_cast<float>(entry.tileHeight),
                   rings.radiusX(0) * 0.5F * static_cast<float>(entry.tileWidth),
                   rings.radiusY(0) * static_cast<float>(entry.tileHeight));
    }
}

bool applyRtVrsLocked(ID3D11DeviceContext* context, RtVrsEntry& entry, int slice) {
    if (!entry.patternValid || entry.appliedConfigurationRevision != g_configurationRevision) {
        disableFixedVrsLocked(context);
        return false;
    }
    constexpr UINT viewportCount = 2;
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewportRates[viewportCount]{};
    for (auto& viewport : viewportRates) {
        viewport.enableVariablePixelShadingRate = true;
        for (auto& rate : viewport.shadingRateTable) {
            rate = NV_PIXEL_X1_PER_RASTER_PIXEL;
        }
        // Indexed by tier; a uniform pattern holds the tier of its rate.
        viewport.shadingRateTable[dvr::control::Tier1x1] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        viewport.shadingRateTable[dvr::control::Tier2x1] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
        viewport.shadingRateTable[dvr::control::Tier2x2] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
        viewport.shadingRateTable[dvr::control::Tier4x2] = NV_PIXEL_X1_PER_4X2_RASTER_PIXELS;
        viewport.shadingRateTable[dvr::control::Tier4x4] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
    }
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC shadingRateDesc{};
    shadingRateDesc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    shadingRateDesc.numViewports = viewportCount;
    shadingRateDesc.pViewports = viewportRates;
    ID3D11NvShadingRateResourceView* const view =
        (slice == 0 || slice == 1) && entry.sliceViews[slice] ? entry.sliceViews[slice] : entry.rateView;
    const NvAPI_Status viewStatus = NvAPI_D3D11_RSSetShadingRateResourceView(context, view);
    const NvAPI_Status ratesStatus =
        viewStatus == NVAPI_OK ? NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &shadingRateDesc) : viewStatus;
    if (viewStatus == NVAPI_OK && ratesStatus == NVAPI_OK) {
        g_vrsState.active = true;
        g_boundConfigurationRevision = g_configurationRevision;
        g_runtimeControl.applied(true, entry.width, entry.height);
        ++entry.binds;
        if (entry.binds <= 3U) {
            logMessage("stage=vrs-rt result=active render=%ux%u slice=%d count=%u uniform=%d", entry.width,
                       entry.height, slice, entry.binds, g_settings.uniformRate);
        }
        return true;
    }
    logMessage("stage=vrs-rt result=failed view=%d rates=%d render=%ux%u", viewStatus, ratesStatus, entry.width,
               entry.height);
    disableFixedVrsLocked(context);
    return false;
}

void disableFixedVrsLocked(ID3D11DeviceContext* context) {
    if (!g_vrsState.active) {
        return;
    }

    constexpr UINT viewportCount = 2;
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewportRates[viewportCount]{};
    for (auto& viewport : viewportRates) {
        viewport.enableVariablePixelShadingRate = false;
    }
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC shadingRateDesc{};
    shadingRateDesc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    shadingRateDesc.numViewports = viewportCount;
    shadingRateDesc.pViewports = viewportRates;
    NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &shadingRateDesc);
    g_vrsState.active = false;
}

} // namespace dvr::layer
