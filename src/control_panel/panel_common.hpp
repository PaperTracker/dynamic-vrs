#pragma once
#include "panel_model.hpp"
#include "panel_renderer.hpp"

#include <windows.h>

// Constants and small helpers shared by the Panel implementation files. Not part of the panel's interface.

namespace dvr::panel {

inline constexpr UINT_PTR PollTimer = 1, ScanTimer = 2, SendTimer = 3;
// Status is a 704-byte shared-memory read; polling it costs nothing. Edits are coalesced for SendMs.
inline constexpr UINT PollMs = 100, ScanMs = 1500, SendMs = 40;
inline constexpr DWORD WindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

// A/B measurement: foveation off and on alternately with the current settings.
inline constexpr int AbPhases[] = {0, 1, 0, 1, 0, 1};
inline constexpr uint64_t AbPhaseMs = 6000;
inline constexpr size_t GpuHistoryLength = 120;
inline constexpr double GpuSampleMs = 250;
inline constexpr float GazeStaleMs = 1000;

// Keys for Panel::ease(). Controls ease under KeyControl + their Control value.
enum Key : uint32_t {
    KeyKnob = 1,
    KeyCenterX,
    KeyCenterY,
    KeyRadius,                            // + 2 * ring + 0..1: X/Y
    KeyShare = KeyRadius + 2 * RingCount, // + tier
    KeyWork = KeyShare + TierCount,
    KeyGpu,
    KeyGpuLow,
    KeyGpuHigh,
    KeyControl = 0x100,
};

inline double nowMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(frequency.QuadPart) / 1000;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / ticksPerMs;
}

inline uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float from = static_cast<float>((a >> shift) & 0xFF), to = static_cast<float>((b >> shift) & 0xFF);
        result |= static_cast<uint32_t>(std::lround(from + (to - from) * t)) << shift;
    }
    return result;
}

inline Rect segment(const Rect& rect, size_t count, size_t index) {
    const float width = rect.width() / static_cast<float>(count);
    return {rect.left + width * static_cast<float>(index), rect.top, rect.left + width * static_cast<float>(index + 1),
            rect.bottom};
}

} // namespace dvr::panel
