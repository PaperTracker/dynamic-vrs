// Mouse input and what each control does when used.
#include "panel.hpp"
#include "panel_common.hpp"

#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace dvr::panel {
using control::Result;

void Panel::mouseMove(float x, float y) {
    mouseX_ = x;
    mouseY_ = y;
    if (!tracking_) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        tracking_ = TrackMouseEvent(&track);
    }
    if (dragging_ != Control::None) {
        dragSlider(dragging_, x);
        return;
    }
    const auto [control, index] = hitTest(x, y);
    if (control != hover_ || index != hoverIndex_) {
        hover_ = control;
        hoverIndex_ = index;
        invalidate();
    }
}

void Panel::mouseDown(float x, float y) {
    const auto [control, index] = hitTest(x, y);
    if (control == Control::None || !enabled(control)) return;
    SetCapture(window_);
    pressed_ = control;
    pressedIndex_ = index;
    if (ringOf(control) >= 0) {
        dragging_ = control;
        dragSlider(control, x);
    }
    invalidate();
}

void Panel::mouseUp() {
    const Control control = pressed_;
    const int index = pressedIndex_;
    const bool dragged = dragging_ != Control::None;
    dragging_ = pressed_ = Control::None;
    if (GetCapture() == window_) ReleaseCapture();
    invalidate();
    if (dragged) {
        flush(); // send the final position without waiting for the coalescing timer
        return;
    }
    const auto [released, releasedIndex] = hitTest(mouseX_, mouseY_);
    if (control != Control::None && released == control && releasedIndex == index) click(control, index);
}

void Panel::mouseWheel(float x, float y, int delta) {
    const auto [control, index] = hitTest(x, y);
    const int ring = ringOf(control);
    if (ring < 0 || !enabled(control)) return;
    // Half a degree per notch; past either end of the degree range it steps to off or to the edge.
    const float degrees = settings_.ringDegrees[ring], step = .5F * static_cast<float>(delta) / WHEEL_DELTA;
    const float value = degrees <= 0                   ? (step > 0 ? 1.F : 0.F)
                        : degrees >= EdgeDegrees       ? (step < 0 ? RingMaximum : EdgeDegrees)
                        : degrees + step < 1           ? 0.F
                        : degrees + step > RingMaximum ? EdgeDegrees
                                                       : degrees + step;
    setRing(static_cast<uint32_t>(ring), value);
}

void Panel::click(Control control, int index) {
    Patch patch;
    switch (control) {
    case Control::Application: showApplicationMenu(); return;
    case Control::Diagnostics:
        diagnosticsOpen_ = !diagnosticsOpen_;
        layout();
        fitWindow();
        invalidate();
        return;
    case Control::Measure:
        if (ab_)
            finishMeasure(true);
        else
            startMeasure();
        return;
    case Control::Restore: restore(); return;
    case Control::Save: save(); return;
    case Control::Enable:
        patch = {Mode};
        patch.values.mode = settings_.mode ? 0 : lastMode_;
        break;
    case Control::Mode:
        lastMode_ = index == 0 ? 2 : 1;
        invalidate();
        if (!settings_.mode) return; // remembered for when foveation is switched on
        patch = {Mode};
        patch.values.mode = lastMode_;
        break;
    case Control::Preset: {
        if (index >= static_cast<int>(std::size(Presets))) return;
        patch = {RingDegrees};
        std::copy(std::begin(Presets[index].rings), std::end(Presets[index].rings), patch.values.ringDegrees);
        break;
    }
    case Control::Dither:
        patch = {DitherDegrees};
        patch.values.ditherDegrees = DitherWidths[index];
        break;
    case Control::Uniform:
        patch = {Field::UniformRate};
        patch.values.uniformRate = UniformRates[index];
        break;
    case Control::Display:
        patch = {Field::Rings | Field::BlurMip};
        patch.values.rings = index == 1;
        patch.values.blurMip = index == 2 ? 7 : 0;
        break;
    default: return;
    }
    edit(patch);
}

void Panel::dragSlider(Control control, float x) {
    const int ring = ringOf(control);
    if (ring < 0) return;
    const Rect& track = layout_.ring[ring];
    const float left = track.left + 10, right = track.right - 10;
    setRing(static_cast<uint32_t>(ring), ringDegreesAt(std::clamp((x - left) / (right - left), 0.F, 1.F)));
}

void Panel::setRing(uint32_t ring, float degrees) {
    float rings[RingCount];
    std::copy(std::begin(settings_.ringDegrees), std::end(settings_.ringDegrees), rings);
    rings[ring] = degrees;
    // Finer tiers end inside this ring and coarser ones outside it; skipped tiers stay skipped.
    if (degrees > 0)
        for (uint32_t other = 0; other < RingCount; ++other)
            if (rings[other] > 0 && (other < ring ? rings[other] > degrees : other > ring && rings[other] < degrees))
                rings[other] = degrees;
    if (std::equal(std::begin(rings), std::end(rings), std::begin(settings_.ringDegrees))) return;
    Patch patch{RingDegrees};
    std::copy(std::begin(rings), std::end(rings), patch.values.ringDegrees);
    edit(patch);
}

void Panel::showApplicationMenu() {
    applications_ = discoverApplications();
    HMENU menu = CreatePopupMenu();
    if (applications_.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, strings_.get(L"ProcessNone").c_str());
    for (size_t index = 0; index < applications_.size(); ++index) {
        const auto& item = applications_[index];
        const bool current = endpoint_.isOpen() && item.processId == endpoint_.processId();
        const auto label = item.name + L"  ·  " + std::to_wstring(item.processId);
        AppendMenuW(menu, MF_STRING | (current ? MF_CHECKED : 0), index + 1, label.c_str());
    }
    POINT origin{std::lround(layout_.application.left * scale()),
                 std::lround((layout_.application.bottom + 4) * scale())};
    ClientToScreen(window_, &origin);
    const int choice = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, origin.x,
                                        origin.y, window_, nullptr);
    DestroyMenu(menu);
    if (choice <= 0 || static_cast<size_t>(choice) > applications_.size()) return;
    options_.processId = applications_[choice - 1].processId;
    if (!endpoint_.isOpen() || endpoint_.processId() != options_.processId) connect(options_.processId);
}

} // namespace dvr::panel
