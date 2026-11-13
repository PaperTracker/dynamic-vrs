// Positions every element and maps points to controls.
#include "panel.hpp"
#include "panel_common.hpp"

#include <algorithm>
#include <cmath>

namespace dvr::panel {
using control::Result;

void Panel::layout() {
    constexpr float Width = 960, Margin = 24, Gap = 16, Pad = 18, LabelWidth = 152, Row = 32, RowGap = 8, Heading = 22;
    Layout l;
    l.width = Width;
    auto& hits = l.hits;
    l.title = {Margin, 19, Width / 2, 51};
    l.subtitle = {Margin, 53, Width / 2, 73};
    l.application = {Width - Margin - 320, 42, Width - Margin, 74};
    l.applicationLabel = {l.application.left, 20, l.application.right, 38};
    hits.push_back({l.application, Control::Application, 0});

    const float top = 98, leftX = Margin, leftRight = Margin + 544, rightX = leftRight + Gap,
                rightRight = Width - Margin;
    float y = top;
    const auto formRow = [&](Rect& label, Rect& control, float height) {
        label = {leftX + Pad, y, leftX + Pad + LabelWidth, y + height};
        const float inset = (height - Row) / 2;
        control = {leftX + Pad + LabelWidth, y + inset, leftRight - Pad, y + inset + Row};
        y += height + RowGap;
    };

    // Foveation on/off, gaze or head direction, preset.
    y += Pad;
    l.enableTitle = {leftX + Pad, y, leftRight - Pad - 60, y + Heading};
    l.enableSwitch = {leftRight - Pad - 44, y + 1, leftRight - Pad, y + Heading - 1};
    y += Heading + 14;
    formRow(l.modeLabel, l.mode, Row);
    formRow(l.presetLabel, l.preset, Row);
    y += Pad - RowGap;
    hits.push_back({l.enableSwitch, Control::Enable, 0});
    hits.push_back(
        {{l.enableTitle.left, l.enableTitle.top, l.enableTitle.left + 200, l.enableTitle.bottom}, Control::Enable, 0});
    hits.push_back({l.mode, Control::Mode, 2});
    hits.push_back({l.preset, Control::Preset, 4});

    // Region sizes and shading quality.
    const float regionsTop = y;
    y += Pad;
    l.regionsTitle = {leftX + Pad, y, leftRight - Pad, y + Heading};
    y += Heading + 14;
    const auto sliderRow = [&](Rect& label, Rect& track, Rect& value) {
        label = {leftX + Pad, y, leftX + Pad + LabelWidth, y + Row};
        track = {leftX + Pad + LabelWidth, y, leftRight - Pad - 96, y + Row};
        value = {leftRight - Pad - 90, y, leftRight - Pad, y + Row};
        y += Row + 4;
    };
    for (uint32_t ring = 0; ring < RingCount; ++ring) {
        sliderRow(l.ringLabel[ring], l.ring[ring], l.ringValue[ring]);
        hits.push_back({l.ring[ring], ringControl(ring), 0});
    }
    formRow(l.ditherLabel, l.dither, 40);
    l.effective = {leftX + Pad, y, leftRight - Pad, y + 34};
    y += 34 + Pad;
    l.regionsCard = {leftX, regionsTop, leftRight, y};
    hits.push_back({l.dither, Control::Dither, 3});

    // Diagnostics, folded away by default.
    const float diagnosticsTop = y;
    y += 12;
    l.diagnostics = {leftX + Pad, y, leftRight - Pad, y + 24};
    hits.push_back({l.diagnostics, Control::Diagnostics, 0});
    y += 24;
    if (diagnosticsOpen_) {
        y += 12;
        formRow(l.displayLabel, l.display, Row);
        formRow(l.uniformLabel, l.uniform, Row);
        y -= RowGap;
        hits.push_back({l.display, Control::Display, 3});
        hits.push_back({l.uniform, Control::Uniform, 4});
    }
    y += 12;
    l.diagnosticsCard = {leftX, diagnosticsTop, leftRight, y};
    l.settingsCard = {leftX, top, leftRight, y};
    const float leftBottom = y;

    // Live view of the left eye.
    y = top + Pad;
    l.viewTitle = {rightX + Pad, y, rightRight - Pad, y + Heading};
    y += Heading + 4;
    l.viewState = {rightX + Pad, y, rightRight - Pad, y + 22};
    y += 22 + 14;
    l.view = {rightX + Pad, y, rightRight - Pad, y + viewHeight_};
    y += viewHeight_ + 14;
    l.legendBar = {rightX + Pad, y, rightRight - Pad, y + 8};
    y += 8 + 10;
    for (auto& row : l.legend) {
        row = {rightX + Pad, y, rightRight - Pad, y + 24};
        y += 26;
    }
    y += Pad - 2;
    l.viewCard = {rightX, top, rightRight, std::max(leftBottom, y)};
    y = std::max(leftBottom, y) + Gap;

    // Metric tiles.
    const float metricsTop = y;
    y += Pad;
    l.metricsTitle = {Margin + Pad, y, Width - Margin - Pad, y + Heading};
    y += Heading + 10;
    const float tileWidth = (Width - 2 * Margin - 2 * Pad - 3 * Gap) / 4, tileHeight = 126;
    for (size_t index = 0; index < l.tiles.size(); ++index) {
        const float left = Margin + Pad + static_cast<float>(index) * (tileWidth + Gap);
        l.tiles[index] = {left, y, left + tileWidth, y + tileHeight};
    }
    const float measureWidth = std::max(renderer_.measureWidth(strings_.get(L"Measure"), Font::BodyBold),
                                        renderer_.measureWidth(strings_.get(L"MeasureCancel"), Font::BodyBold));
    l.measure = {Width - Margin - Pad - measureWidth - 28, l.metricsTitle.top - 3, Width - Margin - Pad,
                 l.metricsTitle.top + 25};
    l.metricsTitle.right = l.measure.left - Gap;
    hits.push_back({l.measure, Control::Measure, 0});
    y += tileHeight + Pad;
    l.metricsCard = {Margin, metricsTop, Width - Margin, y};

    // Footer: status on the left, actions on the right.
    y += Gap;
    const float saveWidth = renderer_.measureWidth(strings_.get(L"Save"), Font::BodyBold) + 36;
    const float restoreWidth = renderer_.measureWidth(strings_.get(L"Restore"), Font::BodyBold) + 32;
    l.save = {Width - Margin - saveWidth, y, Width - Margin, y + 34};
    l.restore = {l.save.left - 10 - restoreWidth, y, l.save.left - 10, y + 34};
    l.statusDot = {Margin + 1, y + 12, Margin + 11, y + 22};
    l.message = {Margin + 20, y - 4, l.restore.left - 16, y + 38};
    hits.push_back({l.restore, Control::Restore, 0});
    hits.push_back({l.save, Control::Save, 0});
    y += 34 + 12;
    const float noteHeight = renderer_.measureHeight(strings_.get(L"Note"), Font::Small, Width - 2 * Margin);
    l.note = {Margin, y, Width - Margin, y + noteHeight + 2};
    y += noteHeight + 20;
    l.height = y;
    layout_ = std::move(l);
}

void Panel::fitWindow() {
    RECT frame{0, 0, std::lround(layout_.width * scale()), std::lround(layout_.height * scale())};
    AdjustWindowRectExForDpi(&frame, WindowStyle, FALSE, 0, static_cast<UINT>(renderer_.dpi()));
    SetWindowPos(window_, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

std::pair<Panel::Control, int> Panel::hitTest(float x, float y) const {
    for (const auto& hit : layout_.hits) {
        if (!hit.rect.contains(x, y)) continue;
        const int index = hit.segments
                              ? std::min(hit.segments - 1, static_cast<int>((x - hit.rect.left) / hit.rect.width() *
                                                                            static_cast<float>(hit.segments)))
                              : 0;
        return {hit.control, index};
    }
    return {Control::None, -1};
}

bool Panel::enabled(Control control) const {
    switch (control) {
    case Control::Application:
    case Control::Diagnostics: return true;
    case Control::Measure: return haveStatus_ && (ab_ || latest_.metrics.timingSamples > 0);
    default: return haveStatus_ && !ab_;
    }
}

} // namespace dvr::panel
