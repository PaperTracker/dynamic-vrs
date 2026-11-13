#pragma once
#include "control_endpoint.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// The panel's tables and the arithmetic behind its controls, kept apart from drawing and Win32 so that it can be
// tested on its own.

namespace dvr::panel {

using control::EdgeDegrees;
using control::RingCount;
using control::TierCount;

inline constexpr uint32_t UniformRates[] = {0, 2, 42, 4};
inline constexpr float DitherWidths[] = {0, 2, 4};

// A ring slider: the left end skips the tier, the right end reaches the edge, degrees in between.
inline constexpr float RingMaximum = 60, RingEndZone = .03F;
struct Preset {
    float rings[RingCount];
};
// Outer radius in degrees of the 1x1, 2x1, 2x2 and 4x2 tiers; 4x4 beyond the last ring.
inline constexpr Preset Presets[] = {
    {{15, 25, EdgeDegrees, 0}}, // 1x1, 2x1, then 2x2 to the edge
    {{10, 16, 24, EdgeDegrees}},
    {{6, 10, 15, 22}},
};
inline constexpr const wchar_t* TierNames[TierCount] = {L"1×1", L"2×1", L"2×2", L"4×2", L"4×4"};
inline constexpr const wchar_t* RingKeys[RingCount] = {L"Ring1x1", L"Ring2x1", L"Ring2x2", L"Ring4x2"};

const wchar_t* rateName(uint32_t rate);
// Shading block width and height in pixels for a tier.
std::pair<float, float> tierBlock(uint32_t tier);
// The tier beyond every ring: that of the first ring reaching the edge, else 4x4.
uint32_t restTier(const float (&rings)[RingCount]);
// Slider position of a ring angle and back.
float ringPosition(float degrees);
float ringDegreesAt(float position);
// The index of the preset whose rings match the settings, or std::size(Presets) for custom rings.
int presetIndex(const Settings& settings);
float median(std::vector<float> values);
// "850 ms", "12 s", "3 min", "1.5 h".
std::wstring duration(float ms);

template <typename... Values> inline std::wstring printed(const wchar_t* pattern, Values... values) {
    wchar_t buffer[128];
    std::swprintf(buffer, std::size(buffer), pattern, values...);
    return buffer;
}

template <size_t N> inline int indexOf(const uint32_t (&values)[N], uint32_t value) {
    const auto found = std::find(std::begin(values), std::end(values), value);
    return found == std::end(values) ? -1 : static_cast<int>(found - std::begin(values));
}

template <size_t N> inline int indexOf(const float (&values)[N], float value) {
    for (size_t index = 0; index < N; ++index)
        if (std::fabs(values[index] - value) < .01F) return static_cast<int>(index);
    return -1;
}

} // namespace dvr::panel
