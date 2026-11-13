// Draws the panel.
#include "panel.hpp"
#include "panel_common.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace dvr::panel {
using control::Result;

void Panel::paint() {
    const double now = nowMs();
    frameSeconds_ = static_cast<float>(std::clamp((now - lastFrame_) / 1000, 0.0, .05));
    lastFrame_ = now;
    dirty_ = moving_ = false;
    if (!renderer_.begin()) return;
    const auto& p = palette_;
    const auto& l = layout_;
    auto& r = renderer_;
    r.clear(p.background);

    // Header.
    r.text(strings_.get(L"Title"), l.title, Font::Title, p.text);
    r.text(strings_.get(L"Subtitle"), l.subtitle, Font::Subtitle, p.muted);
    r.text(strings_.get(L"Process"), l.applicationLabel, Font::Small, p.muted);
    r.fill(l.application, hover_ == Control::Application ? p.segmentHover : p.field, 6);
    r.stroke(l.application, hover_ == Control::Application ? p.accentBorder : p.borderStrong, 6);
    std::wstring application = strings_.get(L"ProcessNone");
    for (const auto& item : applications_)
        if (endpoint_.isOpen() && item.processId == endpoint_.processId())
            application = item.name + L"  ·  " + std::to_wstring(item.processId);
    r.text(application, {l.application.left + 12, l.application.top, l.application.right - 32, l.application.bottom},
           Font::Body, endpoint_.isOpen() ? p.text : p.muted);
    drawChevron(l.application.right - 18, l.application.centerY(), false, p.muted);

    // Foveation.
    const bool editable = enabled(Control::Mode);
    drawCard(l.settingsCard);
    r.text(strings_.get(L"Enable"), l.enableTitle, Font::Heading, p.text);
    {
        const Rect& s = l.enableSwitch;
        const bool on = haveStatus_ && settings_.mode != 0;
        const float at = ease(KeyKnob, on ? 1.F : 0.F, .06F);
        const uint32_t track =
            !editable ? p.idle : mix(p.switchOff, hover_ == Control::Enable ? p.accentHover : p.accent, at);
        r.fill(s, track, s.height() / 2);
        const float knob = s.height() / 2 - 3;
        const float left = s.left + 3 + knob, right = s.right - 3 - knob;
        r.ellipse(left + (right - left) * at, s.centerY(), knob, knob, 0xFFFFFF);
    }
    r.text(strings_.get(L"Mode"), l.modeLabel, Font::Body, p.text);
    drawSegments(l.mode, {strings_.get(L"Gaze"), strings_.get(L"Fixed")}, haveStatus_ ? (lastMode_ == 2 ? 0 : 1) : -1,
                 Control::Mode, editable);
    r.text(strings_.get(L"Preset"), l.presetLabel, Font::Body, p.text);
    drawSegments(
        l.preset,
        {strings_.get(L"Quality"), strings_.get(L"Balanced"), strings_.get(L"Performance"), strings_.get(L"Custom")},
        haveStatus_ ? presetIndex(settings_) : -1, Control::Preset, editable, 3);

    // Regions.
    r.line(l.regionsCard.left + 18, l.regionsCard.top, l.regionsCard.right - 18, l.regionsCard.top, p.border, 1);
    r.text(strings_.get(L"SectionRegions"), l.regionsTitle, Font::Heading, p.text);
    for (uint32_t ring = 0; ring < RingCount; ++ring) {
        const float degrees = settings_.ringDegrees[ring];
        const std::wstring label = !haveStatus_             ? L"—"
                                   : degrees <= 0           ? strings_.get(L"RingOff")
                                   : degrees >= EdgeDegrees ? strings_.get(L"RingEdge")
                                                            : printed(L"%.1f°", degrees);
        r.text(strings_.get(RingKeys[ring]), l.ringLabel[ring], Font::Body, p.text);
        drawSlider(l.ring[ring], l.ringValue[ring], ringControl(ring), ringPosition(degrees), label, editable);
    }
    const auto detailLabel = [&](const Rect& rect, const wchar_t* key, const std::wstring& detail) {
        r.text(strings_.get(key), {rect.left, rect.top + 1, rect.right, rect.centerY() + 1}, Font::Body, p.text);
        if (haveStatus_)
            r.text(detail, {rect.left, rect.centerY() + 1, rect.right, rect.bottom - 1}, Font::Small, p.muted);
    };
    detailLabel(l.ditherLabel, L"Dither", strings_.get(L"DitherDetail"));
    drawSegments(l.dither, {strings_.get(L"DitherOff"), L"2°", L"4°"},
                 haveStatus_ ? indexOf(DitherWidths, settings_.ditherDegrees) : -1, Control::Dither, editable);
    if (haveStatus_ && latest_.applied.mode != 0 && !latest_.applied.uniformRate)
        r.text(effectiveText(), l.effective, Font::Small, p.muted, Align::Leading, true);

    // Diagnostics.
    r.line(l.diagnosticsCard.left + 18, l.diagnosticsCard.top, l.diagnosticsCard.right - 18, l.diagnosticsCard.top,
           p.border, 1);
    if (hover_ == Control::Diagnostics) r.fill(l.diagnostics.inset(-6, -2), p.segment, 6);
    const uint32_t link = p.text;
    r.text(strings_.get(diagnosticsOpen_ ? L"DiagnosticsHide" : L"DiagnosticsShow"), l.diagnostics, Font::BodyBold,
           link);
    drawChevron(l.diagnostics.right - 8, l.diagnostics.centerY(), diagnosticsOpen_, link);
    if (diagnosticsOpen_) {
        r.text(strings_.get(L"Display"), l.displayLabel, Font::Body, p.text);
        drawSegments(l.display,
                     {strings_.get(L"DisplayNone"), strings_.get(L"DisplayRings"), strings_.get(L"DisplayBlur")},
                     haveStatus_ ? (settings_.rings     ? 1
                                    : settings_.blurMip ? 2
                                                        : 0)
                                 : -1,
                     Control::Display, editable);
        r.text(strings_.get(L"Uniform"), l.uniformLabel, Font::Body, p.text);
        drawSegments(l.uniform, {strings_.get(L"Normal"), rateName(2), rateName(42), rateName(4)},
                     haveStatus_ ? indexOf(UniformRates, settings_.uniformRate) : -1, Control::Uniform, editable);
    }

    drawView();
    drawMetrics();

    // Footer.
    const uint32_t tones[] = {p.idle, p.good, p.warn, p.bad};
    r.ellipse(l.statusDot.left + 5, l.statusDot.top + 5, 5, 5, tones[static_cast<int>(tone_)]);
    const float messageHeight = std::min(l.message.height(), r.measureHeight(message_, Font::Small, l.message.width()));
    const float messageTop = l.message.centerY() - messageHeight / 2;
    r.text(message_, {l.message.left, messageTop, l.message.right, messageTop + messageHeight + 1}, Font::Small,
           p.muted, Align::Leading, true);
    drawButton(l.restore, strings_.get(L"Restore"), Control::Restore, false, enabled(Control::Restore));
    drawButton(l.save, strings_.get(L"Save"), Control::Save, true, enabled(Control::Save));
    r.text(strings_.get(L"Note"), l.note, Font::Small, p.muted, Align::Leading, true);

    if (!r.end()) invalidate(); // the target was lost and is recreated on the next paint
}

void Panel::drawCard(const Rect& rect) {
    renderer_.fill(rect, palette_.card, 8);
    renderer_.stroke(rect, palette_.border, 8);
}

void Panel::drawSegments(const Rect& rect, std::initializer_list<std::wstring_view> labels, int selected,
                         Control control, bool active, int inert) {
    const auto& p = palette_;
    const auto key = KeyControl + static_cast<uint32_t>(control);
    renderer_.fill(rect, p.segment, 6);
    renderer_.stroke(rect, p.border, 6);
    const float cellWidth = rect.width() / static_cast<float>(labels.size());
    for (int index = 0; index < static_cast<int>(labels.size()); ++index)
        if (index != selected && active && index != inert && hover_ == control && hoverIndex_ == index)
            renderer_.fill(segment(rect, labels.size(), static_cast<size_t>(index)).inset(3), p.segmentHover, 4);
    // A tonal selection follows the same treatment as PaperTracker's checked controls.
    float at = -1;
    if (selected >= 0) {
        at = ease(key, static_cast<float>(selected), .07F, .002F);
        const bool hot = active && hover_ == control && hoverIndex_ == selected;
        const float left = rect.left + at * cellWidth;
        const Rect selection = Rect{left, rect.top, left + cellWidth, rect.bottom}.inset(3);
        renderer_.fill(selection, !active ? p.segmentHover : hot ? p.segmentHover : p.accentSoft, 4);
        renderer_.stroke(selection, active ? p.accentBorder : p.borderStrong, 4);
    } else {
        forget(key);
    }
    int index = 0;
    for (const auto label : labels) {
        const Rect cell = segment(rect, labels.size(), static_cast<size_t>(index)).inset(6, 0);
        if (at >= 0 && std::fabs(at - static_cast<float>(index)) < .5F)
            renderer_.text(label, cell, index == selected ? Font::BodyBold : Font::Body,
                           active ? p.accentText : p.muted, Align::Center);
        else
            renderer_.text(label, cell, Font::Body, active && index != inert ? p.text : p.muted, Align::Center);
        ++index;
    }
}

void Panel::drawSlider(const Rect& track, const Rect& valueRect, Control control, float position,
                       const std::wstring& label, bool active) {
    const auto& p = palette_;
    // The thumb follows the cursor while dragged and eases when the value changes otherwise.
    const auto key = KeyControl + static_cast<uint32_t>(control);
    if (!haveStatus_ || dragging_ == control) forget(key);
    const float shown = haveStatus_ ? ease(key, position, .08F, .0005F) : position;
    const float left = track.left + 10, right = track.right - 10, cy = track.centerY();
    const float x = left + std::clamp(shown, 0.F, 1.F) * (right - left);
    const uint32_t fillColor = active ? p.sharp : p.idle;
    renderer_.fill({left, cy - 2, right, cy + 2}, p.trough, 2);
    if (haveStatus_) {
        renderer_.fill({left, cy - 2, x, cy + 2}, fillColor, 2);
        const bool hot = active && (hover_ == control || dragging_ == control);
        const float radius = hot ? 8.F : 7.F;
        renderer_.ellipse(x, cy, radius, radius, p.field);
        renderer_.ellipseOutline(x, cy, radius - 1, radius - 1, fillColor, 2.5F);
    }
    renderer_.text(label, valueRect, Font::BodyBold, active ? p.text : p.muted, Align::Trailing);
}

void Panel::drawButton(const Rect& rect, const std::wstring& label, Control control, bool accent, bool active) {
    const auto& p = palette_;
    const bool hot = active && hover_ == control;
    const bool down = hot && pressed_ == control;
    if (accent) {
        renderer_.fill(rect, !active ? p.segment : down ? p.accentPressed : hot ? p.accentHover : p.accent, 6);
        if (!active) renderer_.stroke(rect, p.border, 6);
        renderer_.text(label, rect, Font::BodyBold, active ? 0xFFFFFF : p.muted, Align::Center);
    } else {
        renderer_.fill(rect, !active ? p.segment : down ? p.accentSoft : hot ? p.segmentHover : p.field, 6);
        renderer_.stroke(rect, active ? p.borderStrong : p.border, 6);
        renderer_.text(label, rect, Font::BodyBold, active ? p.text : p.muted, Align::Center);
    }
}

void Panel::drawChevron(float cx, float cy, bool up, uint32_t color) {
    const float dy = up ? -2.5F : 2.5F;
    renderer_.line(cx - 4.5F, cy - dy, cx, cy + dy, color, 1.6F);
    renderer_.line(cx, cy + dy, cx + 4.5F, cy - dy, color, 1.6F);
}

void Panel::drawView() {
    const auto& p = palette_;
    const auto& l = layout_;
    auto& r = renderer_;
    drawCard(l.viewCard);
    r.text(strings_.get(L"SectionView"), l.viewTitle, Font::Heading, p.text);
    const auto& m = latest_.metrics;
    const auto& applied = latest_.applied;
    const bool off = haveStatus_ && applied.mode == 0;
    const bool uniform = haveStatus_ && !off && applied.uniformRate;
    const bool gaze = haveStatus_ && !off && !uniform && applied.mode == 2;
    const bool gazeLive = gaze && m.gazeConnected && m.gazeValid && m.gazeAgeMs < GazeStaleMs;

    // A separate status row leaves room for long tracking-state descriptions.
    const wchar_t* stateKey = L"ViewFixed";
    uint32_t stateColor = p.accent;
    if (!haveStatus_ || off) {
        stateKey = !haveStatus_ ? L"ViewNone" : L"ViewOff";
        stateColor = p.idle;
    } else if (uniform) {
        stateKey = L"ViewUniform";
        stateColor = p.warn;
    } else if (gaze) {
        // The layer widens the rings during fast eye movement and while gaze is missing.
        const auto gazeState = static_cast<control::GazeState>(m.gazeState);
        stateKey = !gazeLive                                  ? L"ViewGazeLost"
                   : gazeState == control::GazeState::Saccade ? L"ViewSaccade"
                   : gazeState == control::GazeState::Stale   ? L"ViewGazeStale"
                   : gazeState == control::GazeState::Lost    ? L"ViewGazeLost"
                                                              : L"ViewLive";
        stateColor = gazeLive && (gazeState == control::GazeState::Tracking || gazeState == control::GazeState::Saccade)
                         ? p.good
                         : p.warn;
    }
    const auto& state = strings_.get(stateKey);
    const Rect& stateRow = l.viewState;
    r.ellipse(stateRow.left + 3, stateRow.centerY(), 3, 3, stateColor);
    r.text(state, {stateRow.left + 14, stateRow.top, stateRow.right, stateRow.bottom}, Font::Small, p.muted);

    // The eye viewport with each tier's shading blocks drawn to scale (one grid cell per shade): the
    // tier beyond every ring fills it, then each ring from the outside in.
    const Rect& v = l.view;
    const bool rings = haveStatus_ && !off && !uniform && m.radii[0][0][0] > 0;
    r.pushClip(v);
    if (!haveStatus_) {
        r.fill(v, p.field);
        const float textHeight = r.measureHeight(strings_.get(L"ViewWaiting"), Font::Body, v.width() - 48);
        const Rect hint{v.left + 24, v.centerY() - textHeight / 2, v.right - 24, v.centerY() + textHeight / 2 + 1};
        r.text(strings_.get(L"ViewWaiting"), hint, Font::Body, p.muted, Align::Center, true);
    } else if (off) {
        r.fill(v, p.sharp);
    } else {
        r.fill(v, tierGrid(uniform ? control::tierOfRate(applied.uniformRate) : restTier(m.ringDegrees)));
        if (rings) {
            // Gaze arrives at the game's frame rate: a short time constant only bridges frames.
            const float cx = v.left + ease(KeyCenterX, m.center[0], .025F, .0005F) * v.width();
            const float cy = v.top + ease(KeyCenterY, m.center[1], .025F, .0005F) * v.height();
            float radiusX[RingCount], radiusY[RingCount];
            for (uint32_t ring = 0; ring < RingCount; ++ring) {
                radiusX[ring] = ease(KeyRadius + 2 * ring, m.radii[0][ring][0], .08F, .0005F) * v.width();
                radiusY[ring] = ease(KeyRadius + 2 * ring + 1, m.radii[0][ring][1], .08F, .0005F) * v.height();
            }
            for (uint32_t ring = RingCount; ring-- > 0;)
                if (m.radii[0][ring][0] > 0) r.ellipse(cx, cy, radiusX[ring], radiusY[ring], tierGrid(ring));
            // Angles along the top of each ring, where there is room between them.
            float labelled = 0;
            for (uint32_t ring = 0; ring < RingCount; ++ring) {
                if (m.radii[0][ring][0] <= 0 || radiusY[ring] - labelled < 22) continue;
                const uint32_t labelColor = ring == 0 ? (dark_ ? p.background : 0xFFFFFF) : p.text;
                r.text(printed(L"%.1f°", m.ringDegrees[ring]),
                       {cx - 40, cy - radiusY[ring] + 3, cx + 40, cy - radiusY[ring] + 19}, Font::Small, labelColor,
                       Align::Center);
                labelled = radiusY[ring];
            }
            r.ellipseOutline(cx, cy, 6, 6, 0xFFFFFF, 1.5F);
            r.ellipse(cx, cy, 2, 2, 0xFFFFFF);
        }
    }
    r.popClip();
    r.maskCorners(v, 6, p.card);
    r.stroke(v, p.border, 6);
    if (!rings)
        for (uint32_t key = KeyCenterX; key < KeyShare; ++key) forget(key);

    // Share of the eye at each tier: a stacked bar, then one row per tier.
    float share[TierCount];
    for (uint32_t tier = 0; tier < TierCount; ++tier) {
        const float target = !haveStatus_ ? 0.F : off ? (tier == 0 ? 1.F : 0.F) : m.coverage[tier];
        share[tier] = ease(KeyShare + tier, target, .12F, .0005F);
    }
    const Rect& bar = l.legendBar;
    r.pushClip(bar);
    r.fill(bar, p.segment);
    float x = bar.left;
    for (uint32_t tier = 0; tier < TierCount && haveStatus_; ++tier) {
        const float width = std::clamp(share[tier], 0.F, 1.F) * bar.width();
        r.fill({x, bar.top, x + width, bar.bottom}, tierColor(tier));
        x += width;
    }
    r.popClip();
    r.maskCorners(bar, 4, p.card);
    constexpr const wchar_t* keys[TierCount] = {L"LegendFull", L"LegendHalf", L"LegendQuarter", L"LegendEighth",
                                                L"LegendSixteenth"};
    for (uint32_t tier = 0; tier < TierCount; ++tier) {
        const Rect& row = l.legend[tier];
        const Rect swatch{row.left, row.centerY() - 5, row.left + 10, row.centerY() + 5};
        r.fill(swatch, tierColor(tier), 3);
        r.stroke(swatch, p.border, 3);
        r.text(strings_.get(keys[tier]), {row.left + 20, row.top, row.right - 120, row.bottom}, Font::Body, p.text);
        r.text(TierNames[tier], {row.right - 120, row.top, row.right - 64, row.bottom}, Font::Small, p.muted);
        r.text(haveStatus_ ? printed(L"%.1f%%", share[tier] * 100) : L"—", row, Font::BodyBold,
               haveStatus_ ? p.text : p.muted, Align::Trailing);
    }
}

uint32_t Panel::tierColor(uint32_t tier) const {
    return tier == 0 ? palette_.sharp
                     : mix(palette_.transition, palette_.outer, static_cast<float>(tier - 1) / (TierCount - 2));
}

Grid Panel::tierGrid(uint32_t tier) const {
    // 3.5 DIPs per pixel keeps 2×1 visible and 4×4 clearly coarse; one grid cell per shade.
    const auto [width, height] = tierBlock(tier);
    const float coarse = static_cast<float>(std::max(tier, 1U) - 1) / (TierCount - 2);
    const uint32_t line = tier == 0 ? palette_.sharp : mix(palette_.transitionGrid, palette_.outerGrid, coarse);
    return {width * 3.5F, height * 3.5F, tierColor(tier), line, layout_.view.left, layout_.view.top};
}

// The applied rings in words, with the temporary widening when there is one.
std::wstring Panel::effectiveText() const {
    const auto& m = latest_.metrics;
    std::wstring rings;
    for (uint32_t ring = 0; ring < RingCount; ++ring) {
        const float degrees = m.ringDegrees[ring];
        if (degrees <= 0) continue;
        if (!rings.empty()) rings += L", ";
        rings += degrees >= EdgeDegrees
                     ? strings_.format(L"EffectiveEdge", {{L"tier", std::wstring(TierNames[ring])}})
                     : strings_.format(L"EffectiveRing", {{L"tier", std::wstring(TierNames[ring])}, {L"deg", degrees}});
        if (degrees >= EdgeDegrees) break;
    }
    if (restTier(m.ringDegrees) == control::Tier4x4) {
        if (!rings.empty()) rings += L", ";
        rings += strings_.format(L"EffectiveRest", {{L"tier", std::wstring(TierNames[control::Tier4x4])}});
    }
    std::wstring text = strings_.format(L"Effective", {{L"rings", rings}});
    if (m.widenDegrees > .05F) {
        const auto state = static_cast<control::GazeState>(m.gazeState);
        text += L" " +
                strings_.format(
                    L"Widened",
                    {{L"deg", m.widenDegrees},
                     {L"reason", strings_.get(state == control::GazeState::Saccade ? L"WidenSaccade" : L"WidenGaze")}});
    }
    return text;
}

void Panel::drawMetrics() {
    const auto& p = palette_;
    const auto& l = layout_;
    auto& r = renderer_;
    drawCard(l.metricsCard);
    r.text(strings_.get(L"SectionMetrics"), l.metricsTitle, Font::Heading, p.text);
    const auto& m = latest_.metrics;
    const auto& applied = latest_.applied;
    // Every tile: caption, value, an optional picture (bar or trend), detail.
    struct Tile {
        Rect caption, value, picture, detail;
    };
    const auto parts = [&](size_t index, bool picture) {
        const Rect& t = l.tiles[index];
        if (index) r.line(t.left - 8.5F, t.top + 2, t.left - 8.5F, t.bottom - 2, p.border, 1);
        const float detailTop = picture ? t.top + 92 : t.top + 60;
        return Tile{{t.left, t.top, t.right, t.top + 18},
                    {t.left, t.top + 20, t.right, t.top + 54},
                    {t.left, t.top + 62, t.right - 14, t.top + 86},
                    {t.left, detailTop, t.right - 6, detailTop + 34}};
    };
    const auto meter = [&](const Rect& area, float fraction, uint32_t color) {
        const Rect track{area.left, area.centerY() - 3, area.right, area.centerY() + 3};
        r.fill(track, p.trough, 3);
        if (fraction > 0)
            r.fill({track.left, track.top, track.left + std::clamp(fraction, 0.F, 1.F) * track.width(), track.bottom},
                   color, 3);
    };
    const std::wstring none = L"—";

    // Shading work: estimate from the screen area at each rate.
    {
        const Tile tile = parts(0, haveStatus_);
        r.text(strings_.get(L"MetricWork"), tile.caption, Font::Small, p.muted);
        const float work = ease(KeyWork, m.shadingWork, .15F, .0005F);
        r.text(haveStatus_ ? printed(L"%.0f%%", work * 100) : none, tile.value, Font::Big,
               haveStatus_ ? p.text : p.idle);
        if (haveStatus_) {
            meter(tile.picture, work, p.accent);
            r.text(strings_.format(L"WorkDetail", {{L"saved", (1 - m.shadingWork) * 100.0}}), tile.detail, Font::Small,
                   p.muted, Align::Leading, true);
        }
    }

    // Scene GPU time with its trend over the last 30 s, so a settings change shows as a step.
    {
        const bool timed = haveStatus_ && m.timingSamples;
        const Tile tile = parts(1, timed);
        r.text(strings_.get(L"MetricGpu"), tile.caption, Font::Small, p.muted);
        if (!timed) forget(KeyGpu);
        const float gpu = timed ? ease(KeyGpu, m.gpuMsMedian, .2F, .005F) : 0;
        r.text(timed ? printed(L"%.2f ms", gpu) : none, tile.value, Font::Big, timed ? p.text : p.idle);
        if (timed && gpuHistory_.size() >= 2) {
            // The vertical range eases too, so a rescale glides instead of jumping.
            const Rect& area = tile.picture;
            const auto [lowest, highest] = std::minmax_element(gpuHistory_.begin(), gpuHistory_.end());
            const float low = ease(KeyGpuLow, *lowest, .3F, .005F), high = ease(KeyGpuHigh, *highest, .3F, .005F);
            const float span = std::max(high - low, .5F), base = low - span * .1F, range = span * 1.2F;
            // Samples arrive every GpuSampleMs; between them the line scrolls with time and its right
            // end follows the live value, so it moves continuously instead of stepping once per sample.
            const float stepX = area.width() / static_cast<float>(GpuHistoryLength - 1);
            const float scroll =
                std::clamp(static_cast<float>((nowMs() - lastGpuSample_) / GpuSampleMs), 0.F, 1.F) * stepX;
            const auto y = [&](float ms) { return area.bottom - (ms - base) / range * area.height(); };
            r.line(area.left, area.bottom, area.right, area.bottom, p.border, 1);
            std::vector<D2D1_POINT_2F> points;
            points.reserve(gpuHistory_.size() + 1);
            float x = area.right - scroll - stepX * static_cast<float>(gpuHistory_.size() - 1);
            for (size_t index = 0; index <= gpuHistory_.size(); ++index, x += stepX) {
                const bool head = index == gpuHistory_.size();
                const D2D1_POINT_2F point{head ? area.right : x, y(head ? gpu : gpuHistory_[index])};
                if (point.x <= area.left) { // the oldest segment slides out at the left edge
                    points.assign(1, point);
                    continue;
                }
                if (points.size() == 1 && points[0].x < area.left) {
                    const auto& old = points[0];
                    points[0] = {area.left, old.y + (point.y - old.y) * (area.left - old.x) / (point.x - old.x)};
                }
                points.push_back(point);
            }
            r.polyline(points, p.accent, 1.6F);
            moving_ = true;
        }
        if (haveStatus_)
            r.text(timed ? strings_.format(L"GpuDetail",
                                           {{L"p95", m.gpuMsP95},
                                            {L"fps", m.frameIntervalMs > 0 ? 1000.0 / m.frameIntervalMs : 0.0}})
                         : strings_.get(L"GpuNone"),
                   tile.detail, Font::Small, p.muted, Align::Leading, true);
    }

    // Eye tracking: packet rate, or how long the export has been silent.
    {
        const Tile tile = parts(2, false);
        std::wstring value = none, detail;
        uint32_t dot = p.idle, color = p.idle;
        if (!haveStatus_) {
            // nothing to show
        } else if (applied.mode != 2) {
            detail = strings_.get(L"GazeNotUsed");
        } else if (!m.gazeConnected) {
            value = strings_.get(L"GazeNoSignal");
            detail = strings_.get(L"GazeNone");
            dot = color = p.bad;
        } else if (m.gazeAgeMs >= GazeStaleMs) {
            value = strings_.get(L"GazeLost");
            detail = strings_.format(L"GazeStale", {{L"age", duration(m.gazeAgeMs)}});
            dot = color = p.bad;
        } else {
            value = printed(L"%.0f Hz", m.gazeHz);
            detail =
                strings_.format(L"GazeDetail", {{L"state", strings_.get(m.gazeValid ? L"GazeValid" : L"GazeInvalid")},
                                                {L"age", duration(m.gazeAgeMs)}});
            dot = m.gazeValid ? p.good : p.warn;
            color = p.text;
        }
        r.ellipse(tile.caption.left + 4, tile.caption.centerY(), 4, 4, dot);
        r.text(strings_.get(L"MetricGaze"),
               {tile.caption.left + 14, tile.caption.top, tile.caption.right, tile.caption.bottom}, Font::Small,
               p.muted);
        r.text(value, tile.value, Font::Big, color);
        r.text(detail, tile.detail, Font::Small, p.muted, Align::Leading, true);
    }

    // A/B measurement: progress while it runs, then the measured difference.
    {
        const Tile tile = parts(3, ab_.has_value());
        r.text(strings_.get(L"MetricAb"), tile.caption, Font::Small, p.muted);
        if (ab_) {
            const auto left = static_cast<float>(static_cast<int64_t>(ab_->deadline - GetTickCount64()));
            const float phase = 1 - std::clamp(left / static_cast<float>(AbPhaseMs), 0.F, 1.F);
            r.text(abValue_, tile.value, Font::Big, p.text);
            meter(tile.picture, (static_cast<float>(ab_->phase) + phase) / static_cast<float>(std::size(AbPhases)),
                  p.accent);
        } else {
            r.text(abValue_.empty() ? none : abValue_, tile.value, Font::Big,
                   abValue_.empty() ? p.idle
                   : abSaving_ > 0  ? p.good
                                    : p.text);
        }
        const auto& detail = haveStatus_ && !enabled(Control::Measure) ? strings_.get(L"AbUnavailable")
                             : abDetail_.empty()                       ? strings_.get(L"AbIdle")
                                                                       : abDetail_;
        r.text(detail, tile.detail, Font::Small, p.muted, Align::Leading, true);
        drawButton(l.measure, strings_.get(ab_ ? L"MeasureCancel" : L"Measure"), Control::Measure, false,
                   enabled(Control::Measure));
    }
}

} // namespace dvr::panel
