#include "panel_model.hpp"

#include <algorithm>
#include <cmath>

namespace dvr::panel {

const wchar_t* rateName(uint32_t rate) {
    switch (rate) {
    case 1: return L"2×1";
    case 2: return L"2×2";
    case 42: return L"4×2";
    case 4: return L"4×4";
    default: return L"—";
    }
}

std::pair<float, float> tierBlock(uint32_t tier) {
    constexpr std::pair<float, float> Blocks[TierCount] = {{1.F, 1.F}, {2.F, 1.F}, {2.F, 2.F}, {4.F, 2.F}, {4.F, 4.F}};
    return Blocks[std::min(tier, TierCount - 1)];
}

uint32_t restTier(const float (&rings)[RingCount]) {
    for (uint32_t ring = 0; ring < RingCount; ++ring)
        if (rings[ring] >= EdgeDegrees) return ring;
    return control::Tier4x4;
}

float ringPosition(float degrees) {
    if (degrees <= 0) return 0;
    if (degrees >= EdgeDegrees) return 1;
    return RingEndZone + (1 - 2 * RingEndZone) * (std::clamp(degrees, 1.F, RingMaximum) - 1) / (RingMaximum - 1);
}

float ringDegreesAt(float position) {
    if (position < RingEndZone / 2) return 0;
    if (position > 1 - RingEndZone / 2) return EdgeDegrees;
    const float degrees = 1 + (position - RingEndZone) / (1 - 2 * RingEndZone) * (RingMaximum - 1);
    return std::clamp(std::round(degrees * 2) / 2, 1.F, RingMaximum);
}

std::wstring duration(float ms) {
    if (ms < 1000) return printed(L"%.0f ms", ms);
    if (ms < 60000) return printed(L"%.0f s", ms / 1000);
    if (ms < 3600000) return printed(L"%.0f min", ms / 60000);
    return printed(L"%.1f h", ms / 3600000);
}

int presetIndex(const Settings& s) {
    for (int index = 0; index < static_cast<int>(std::size(Presets)); ++index)
        if (std::equal(std::begin(s.ringDegrees), std::end(s.ringDegrees), std::begin(Presets[index].rings),
                       [](float a, float b) { return std::fabs(a - b) < .01F; }))
            return index;
    return static_cast<int>(std::size(Presets)); // Custom
}

float median(std::vector<float> values) {
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

} // namespace dvr::panel
