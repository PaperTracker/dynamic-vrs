#include "layer_config.hpp"

#include "common/ini_file.hpp"
#include "common/log.hpp"
#include "common/paths.hpp"

#ifdef DVR_HAS_NVAPI
#include "layer/vrs/vrs_state.hpp"
#endif

#include <algorithm>
#include <string>

namespace dvr::layer {

LayerSettings g_settings;
std::filesystem::path g_configPath;

std::filesystem::path layerConfigPath() {
    return g_configPath.empty() ? thisModuleDirectory().parent_path() / L"dynamic-vrs.ini" : g_configPath;
}

namespace {

VrsMode parseMode(const std::wstring& value) {
    if (_wcsicmp(value.c_str(), L"gaze") == 0) return VrsMode::Gaze;
    if (_wcsicmp(value.c_str(), L"fixed") == 0) return VrsMode::FixedCenter;
    return VrsMode::Off;
}

// The tier ladder, or the three regions of older ini files: 1x1 inside the inner radius, vrsMiddleRate up to
// the middle radius, then vrsOuterRate to the edge. Returns true when the ini uses the ladder keys.
bool readRings(const IniFile& ini, LayerSettings& settings) {
    static constexpr const wchar_t* RingKeys[RingCount] = {L"vrsRing1x1Degrees", L"vrsRing2x1Degrees",
                                                           L"vrsRing2x2Degrees", L"vrsRing4x2Degrees"};
    auto& rings = settings.ringDegrees;
    bool ringKeys = false;
    for (uint32_t ring = 0; ring < RingCount; ++ring) {
        const double degrees = ini.number(RingKeys[ring], -1.0, -1.0, EdgeDegrees);
        rings[ring] = degrees < 0.0 ? 0.0F : static_cast<float>(degrees);
        ringKeys |= degrees >= 0.0;
    }
    settings.legacyRingFraction = {};
    if (ringKeys) return true;

    const int middleRate = static_cast<int>(ini.number(L"vrsMiddleRate", 1.0, 1.0, 42.0));
    const int outerRate = static_cast<int>(ini.number(L"vrsOuterRate", 2.0, 0.0, 100.0));
    const uint32_t middleTier = dvr::control::tierOfRate(
        middleRate == 2 || middleRate == 4 || middleRate == 42 ? static_cast<uint32_t>(middleRate) : 1U);
    const uint32_t outerTier =
        dvr::control::tierOfRate(outerRate == 4 || outerRate == 42 ? static_cast<uint32_t>(outerRate) : 2U);
    const auto legacyRing = [&](uint32_t ring, const wchar_t* degreesKey, double maximum, const wchar_t* xKey, double x,
                                const wchar_t* yKey, double y) {
        rings[ring] = static_cast<float>(ini.number(degreesKey, 0.0, 0.0, maximum));
        if (rings[ring] == 0.0F)
            settings.legacyRingFraction[ring] = {static_cast<float>(ini.number(xKey, x, 0.05, 0.95)),
                                                 static_cast<float>(ini.number(yKey, y, 0.05, 0.95))};
    };
    rings = {};
    legacyRing(dvr::control::Tier1x1, L"vrsInnerRadiusDegrees", 45.0, L"vrsInnerRadiusX", 0.28, L"vrsInnerRadiusY",
               0.32);
    // A middle region as coarse as the outer one is just part of it.
    if (middleTier < outerTier) {
        legacyRing(middleTier, L"vrsMiddleRadiusDegrees", 60.0, L"vrsMiddleRadiusX", 0.44, L"vrsMiddleRadiusY", 0.48);
        if (rings[middleTier] > 0.0F) rings[middleTier] = std::max(rings[middleTier], rings[0]);
    }
    if (outerTier < dvr::control::Tier4x4) rings[outerTier] = EdgeDegrees;
    return false;
}

GazeStabilizerConfig readGazeStabilizer(const IniFile& ini) {
    GazeStabilizerConfig stabilizer;
    const auto read = [&ini](float& value, const wchar_t* key, double maximum) {
        value = static_cast<float>(ini.number(key, value, 0.0, maximum));
    };
    read(stabilizer.deadbandDegrees, L"gazeDeadbandDegrees", 5.0);
    read(stabilizer.smoothingHz, L"gazeSmoothingHz", 100.0);
    read(stabilizer.saccadeDegreesPerSecond, L"gazeSaccadeDegreesPerSecond", 2000.0);
    read(stabilizer.saccadeWidenDegrees, L"gazeSaccadeWidenDegrees", 30.0);
    read(stabilizer.saccadeHoldMs, L"gazeSaccadeHoldMs", 2000.0);
    read(stabilizer.lostWidenDegrees, L"gazeLostWidenDegrees", 30.0);
    read(stabilizer.staleMs, L"gazeStaleMs", 10000.0);
    read(stabilizer.lostMs, L"gazeLostMs", 60000.0);
    read(stabilizer.releaseMs, L"gazeWidenReleaseMs", 5000.0);
    return stabilizer;
}

} // namespace

void loadLayerSettings() {
    const IniFile ini(layerConfigPath());
    LayerSettings& s = g_settings;

    const bool ringKeys = readRings(ini, s);
    s.ditherDegrees = static_cast<float>(ini.number(L"vrsRingDitherDegrees", 0.0, 0.0, 10.0));
    s.gazeStabilizer = readGazeStabilizer(ini);
    const int uniformRate = static_cast<int>(ini.number(L"vrsUniformRate", 0.0, 0.0, 100.0));
    s.uniformRate = (uniformRate == 2 || uniformRate == 4 || uniformRate == 42) ? uniformRate : 0;
    s.targetWidth = static_cast<UINT>(ini.number(L"vrsTargetWidth", 0.0, 0.0, 20000.0));
    s.targetHeight = static_cast<UINT>(ini.number(L"vrsTargetHeight", 0.0, 0.0, 20000.0));
    s.gazeCombine = static_cast<int>(ini.number(L"vrsGazeCombine", 0.0, 0.0, 1.0)) != 0 ? 1 : 0;
    s.gazeForwardYawDegrees = static_cast<float>(ini.number(L"gazeForwardYawDegrees", 0.0, -10.0, 10.0));
    s.gazeForwardPitchDegrees = static_cast<float>(ini.number(L"gazeForwardPitchDegrees", 0.0, -10.0, 10.0));
    s.debugBlurMip = static_cast<int>(ini.number(L"debugBlurOutsideMip", 0.0, 0.0, 8.0));
    s.debugRadiusRings = ini.flag(L"debugRadiusRings");
    s.debugTargetAlignment = ini.flag(L"debugTargetAlignment");
#ifdef DVR_HAS_NVAPI
    if (ini.flag(L"debugGpuFrameStats"))
        g_gpuFrameProbe = std::make_unique<dvr::layer::diagnostics::GpuFrameProbe>(&logMessage);
#endif
    if (s.debugTargetAlignment) logMessage("stage=alignment-probe result=enabled trigger=radius-rings-off-on");
    logMessage("stage=vrs-geometry rings=%.2f,%.2f,%.2f,%.2f source=%s dither=%.2f debugRings=%d "
               "targetMode=%s",
               s.ringDegrees[0], s.ringDegrees[1], s.ringDegrees[2], s.ringDegrees[3],
               ringKeys ? "rings" : "legacy-regions", s.ditherDegrees, s.debugRadiusRings ? 1 : 0,
               s.targetWidth && s.targetHeight ? "explicit-size" : "submitted-size");
    const auto& stabilizer = s.gazeStabilizer;
    logMessage("stage=gaze-stabilizer result=configured deadband=%.2f smoothingHz=%.2f saccade=%.0fdeg/s "
               "saccadeWiden=%.1f holdMs=%.0f lostWiden=%.1f staleMs=%.0f lostMs=%.0f releaseMs=%.0f",
               stabilizer.deadbandDegrees, stabilizer.smoothingHz, stabilizer.saccadeDegreesPerSecond,
               stabilizer.saccadeWidenDegrees, stabilizer.saccadeHoldMs, stabilizer.lostWidenDegrees,
               stabilizer.staleMs, stabilizer.lostMs, stabilizer.releaseMs);
    if (s.debugBlurMip != 0) {
        logMessage("stage=debug-blur result=configured mip=%d purpose=boundary-visualization-only", s.debugBlurMip);
    }

    s.gazeFlipX = ini.flag(L"gazeFlipX");
    s.gazeFlipY = ini.flag(L"gazeFlipY");
    const auto gazePort = static_cast<int>(ini.number(L"gazeExportPort", dvr::gaze::DefaultPort, 0, 65535));
    s.gazeExportPort =
        static_cast<std::uint16_t>(gazePort == 0 || gazePort >= 1024 ? gazePort : dvr::gaze::DefaultPort);
    s.rasterFlipY = ini.flag(L"vrsRasterFlipY");
    s.rasterFlipYFormat = static_cast<DXGI_FORMAT>(static_cast<int>(ini.number(L"vrsRasterFlipYFormat", 0, 0, 191)));
    const auto flipSamples = static_cast<UINT>(ini.number(L"vrsRasterFlipYSamples", 1, 0, 4));
    s.rasterFlipYSamples = flipSamples == 0 || flipSamples == 2 || flipSamples == 4 ? flipSamples : 1;
    logMessage("stage=vrs-raster-transform flipY=%d flipFormat=%u flipSamples=%u", s.rasterFlipY ? 1 : 0,
               static_cast<unsigned int>(s.rasterFlipYFormat), s.rasterFlipYSamples);

    s.mode = parseMode(ini.text(L"nativeVrsMode", L"off"));
    const std::string configPath = wideToUtf8(ini.path().wstring());
    switch (s.mode) {
    case VrsMode::Gaze:
        logMessage("stage=config nativeVrsMode=gaze flipX=%d flipY=%d uniform=%d filter=%ux%u exportPort=%u "
                   "config=\"%s\"",
                   s.gazeFlipX ? 1 : 0, s.gazeFlipY ? 1 : 0, s.uniformRate, s.targetWidth, s.targetHeight,
                   static_cast<unsigned>(s.gazeExportPort), configPath.c_str());
        break;
    case VrsMode::FixedCenter: logMessage("stage=config nativeVrsMode=fixed config=\"%s\"", configPath.c_str()); break;
    case VrsMode::Off: logMessage("stage=config nativeVrsMode=off config=\"%s\"", configPath.c_str()); break;
    }
}

dvr::control::Settings runtimeSettingsLocked() {
    const LayerSettings& s = g_settings;
    dvr::control::Settings settings;
    settings.mode = static_cast<uint32_t>(s.mode);
    std::copy(s.ringDegrees.begin(), s.ringDegrees.end(), settings.ringDegrees);
    settings.ditherDegrees = s.ditherDegrees;
    settings.uniformRate = s.uniformRate;
    settings.rings = s.debugRadiusRings;
    settings.blurMip = s.debugBlurMip;
    settings.targetWidth = s.targetWidth;
    settings.targetHeight = s.targetHeight;
    settings.combine = s.gazeCombine;
    settings.flipX = s.gazeFlipX;
    settings.flipY = s.gazeFlipY;
    settings.forwardYaw = s.gazeForwardYawDegrees;
    settings.forwardPitch = s.gazeForwardPitchDegrees;
    return settings;
}

} // namespace dvr::layer
