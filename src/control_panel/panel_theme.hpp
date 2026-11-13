#pragma once
#include <cstdint>

namespace dvr::panel {

// Every color the panel draws with, as 0xRRGGBB.
struct Palette {
    uint32_t background, card, border, text, muted, accent, accentHover, accentPressed, trough;
    uint32_t segment, segmentHover, switchOff, sharp, transition, transitionGrid, outer, outerGrid;
    uint32_t good, warn, idle, bad;
    uint32_t field, borderStrong, accentSoft, accentBorder, accentText;
};

// Whether Windows is set to the dark app theme.
bool systemDark();
const Palette& paletteFor(bool dark);

} // namespace dvr::panel
