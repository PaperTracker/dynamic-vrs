#include "panel_theme.hpp"

#include <windows.h>

namespace dvr::panel {
namespace {
// PaperTracker's default indigo, cool neutral surfaces and shared component proportions.
// Text and primary-button roles stay distinct so both themes retain readable labels.
constexpr Palette LightPalette = {
    .background = 0xEEF0FA,
    .card = 0xFBFCFF,
    .border = 0xD9DCE7,
    .text = 0x262935,
    .muted = 0x575D70,
    .accent = 0x5266CF,
    .accentHover = 0x5A6ED8,
    .accentPressed = 0x4356B9,
    .trough = 0xBEC3D2,
    .segment = 0xF0F2F9,
    .segmentHover = 0xE8EBF6,
    .switchOff = 0x858B9F,
    .sharp = 0x5266CF,
    .transition = 0xA3B0E4,
    .transitionGrid = 0x8798D6,
    .outer = 0xE8ECF8,
    .outerGrid = 0xC5CEE9,
    .good = 0x287546,
    .warn = 0x8D5D16,
    .idle = 0x73798C,
    .bad = 0xB53E40,
    .field = 0xFBFCFF,
    .borderStrong = 0xB8BDCF,
    .accentSoft = 0xE8ECFC,
    .accentBorder = 0xAFBCEC,
    .accentText = 0x4356B9,
};
constexpr Palette DarkPalette = {
    .background = 0x141620,
    .card = 0x1D202C,
    .border = 0x303443,
    .text = 0xE6E8F2,
    .muted = 0xA4AABE,
    .accent = 0x5266CF,
    .accentHover = 0x5A6ED8,
    .accentPressed = 0x4356B9,
    .trough = 0x484E63,
    .segment = 0x242837,
    .segmentHover = 0x2D3244,
    .switchOff = 0x636A81,
    .sharp = 0x8399F0,
    .transition = 0x4C5D9A,
    .transitionGrid = 0x6678B7,
    .outer = 0x242D49,
    .outerGrid = 0x374566,
    .good = 0x86C69A,
    .warn = 0xD9B876,
    .idle = 0x8B92A8,
    .bad = 0xF49C9D,
    .field = 0x141722,
    .borderStrong = 0x484E63,
    .accentSoft = 0x2A3358,
    .accentBorder = 0x5669AB,
    .accentText = 0xA9BAFF,
};
} // namespace

bool systemDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

const Palette& paletteFor(bool dark) { return dark ? DarkPalette : LightPalette; }

} // namespace dvr::panel
