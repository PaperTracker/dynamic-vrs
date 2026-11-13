// Talks to the layer: polling status, sending edits, the A/B measurement, saving.
#include "ini_defaults.hpp"
#include "panel.hpp"
#include "panel_common.hpp"

#include <algorithm>
#include <cmath>

namespace dvr::panel {
using control::Result;

void Panel::poll() {
    if (watched_ && WaitForSingleObject(watched_, 0) == WAIT_OBJECT_0) { // the game this panel was started for exited
        close();
        return;
    }
    Status s;
    if (!endpoint_.isOpen()) {
        status(L"Waiting");
    } else if (endpoint_.read(s)) {
        latest_ = s;
        haveStatus_ = true;
        if (s.session != endpoint_.session() || s.result == static_cast<uint32_t>(Result::Closed)) {
            connect(0);
            scan();
            return;
        }
        if (s.result == static_cast<uint32_t>(Result::Rejected)) {
            waiting_ = 0;
            status(L"Rejected");
            if (pending_.empty()) sync(s.accepted);
        } else if (GetTickCount64() - s.heartbeatMs > 2000) {
            status(L"Paused");
        } else if (s.result == static_cast<uint32_t>(Result::WaitingForFrame) ||
                   (waiting_ && s.appliedSequence < waiting_)) {
            status(L"Pending");
        } else {
            waiting_ = 0;
            if (pending_.empty() && !ab_) sync(s.applied);
            if (s.applied.mode == 0)
                status(L"OffApplied");
            else
                status(L"Applied",
                       {{L"width", static_cast<double>(s.width)}, {L"height", static_cast<double>(s.height)}});
        }
        if (const double now = nowMs(); s.metrics.timingSamples && now - lastGpuSample_ >= GpuSampleMs) {
            lastGpuSample_ = now;
            gpuHistory_.push_back(s.metrics.gpuMsMedian);
            if (gpuHistory_.size() > GpuHistoryLength) gpuHistory_.erase(gpuHistory_.begin());
        }
        if (ab_) stepMeasure(s);
        if (s.width && s.height) { // match the eye viewport's aspect ratio
            const float height = std::clamp(
                layout_.view.width() * static_cast<float>(s.height) / (static_cast<float>(s.width) / 2), 170.F, 330.F);
            if (std::fabs(height - viewHeight_) >= 1) {
                viewHeight_ = std::round(height);
                layout();
                fitWindow();
                invalidate();
            }
        }
    }
    const Shown now{latest_.metrics,
                    latest_.applied,
                    settings_,
                    latest_.width,
                    latest_.height,
                    uint32_t{haveStatus_},
                    static_cast<uint32_t>(tone_)};
    if (!(now == shown_)) {
        shown_ = now;
        invalidate();
    }
}

void Panel::scan() {
    auto found = discoverApplications();
    if (found != applications_) {
        applications_ = std::move(found);
        invalidate();
    }
    const auto listed = [&](uint32_t processId) {
        return std::any_of(applications_.begin(), applications_.end(),
                           [&](const Application& item) { return item.processId == processId; });
    };
    if (endpoint_.isOpen() && listed(endpoint_.processId())) return;
    const uint32_t choice = listed(options_.processId) ? options_.processId
                            : applications_.empty()    ? 0
                                                       : applications_.front().processId;
    if (choice || endpoint_.isOpen()) connect(choice);
}

void Panel::connect(uint32_t processId) {
    if (ab_) finishMeasure(true);
    abValue_.clear();
    abDetail_.clear();
    abSaving_ = 0;
    endpoint_.close();
    haveStatus_ = false;
    gpuHistory_.clear();
    eased_.clear(); // a newly connected application's values appear without easing from the old one's
    pending_ = {};
    waiting_ = 0;
    KillTimer(window_, SendTimer);
    sendQueued_ = false;
    if (processId) {
        if (endpoint_.open(processId) && endpoint_.read(latest_)) {
            haveStatus_ = true;
            sync(latest_.accepted);
        } else {
            endpoint_.close();
            notice(L"Error",
                   {{L"detail", strings_.format(L"ErrorConnect", {{L"pid", static_cast<double>(processId)}})}});
        }
    }
    invalidate();
}

void Panel::sync(const Settings& settings) {
    const Settings previous = settings_;
    settings_ = settings;
    if (dragging_ != Control::None) // keep the thumb under the cursor
        std::copy(std::begin(previous.ringDegrees), std::end(previous.ringDegrees), settings_.ringDegrees);
    if (settings.mode) lastMode_ = settings.mode;
}

void Panel::edit(const Patch& patch) {
    if (!enabled(Control::Mode)) return;
    patch.applyTo(settings_);
    holdUntil_ = 0;
    send(patch);
    invalidate();
}

void Panel::send(const Patch& patch) {
    pending_.merge(patch);
    // Only a slider drag is coalesced; a click goes out at once.
    if (dragging_ == Control::None) {
        flush();
        return;
    }
    if (!sendQueued_) {
        SetTimer(window_, SendTimer, SendMs, nullptr);
        sendQueued_ = true;
    }
}

void Panel::flush() {
    KillTimer(window_, SendTimer);
    sendQueued_ = false;
    if (pending_.empty() || !endpoint_.isOpen()) return;
    uint64_t sequence = 0;
    switch (endpoint_.write(pending_, sequence)) {
    case WriteResult::Sent:
        waiting_ = sequence;
        pending_ = {};
        status(L"Pending");
        break;
    case WriteResult::Busy:
        status(L"Busy");
        SetTimer(window_, SendTimer, SendMs, nullptr);
        sendQueued_ = true;
        break;
    case WriteResult::Invalid:
        pending_ = {};
        notice(L"Error", {{L"detail", strings_.get(L"ErrorInvalid")}});
        break;
    case WriteResult::Failed:
        pending_ = {};
        notice(L"Error", {{L"detail", strings_.get(L"ErrorSession")}});
        break;
    }
}

void Panel::status(std::wstring_view key, std::initializer_list<Strings::Argument> arguments) {
    if (GetTickCount64() < holdUntil_) return;
    auto text = strings_.format(key, arguments);
    const Tone tone = key == L"Applied" || key == L"Saved"                          ? Tone::Good
                      : key == L"Pending" || key == L"Busy" || key == L"Paused"     ? Tone::Warn
                      : key == L"Rejected" || key == L"Error" || key == L"NoConfig" ? Tone::Bad
                                                                                    : Tone::Idle;
    if (text == message_ && tone == tone_) return;
    message_ = std::move(text);
    tone_ = tone;
    invalidate();
}

void Panel::notice(std::wstring_view key, std::initializer_list<Strings::Argument> arguments) {
    holdUntil_ = 0;
    status(key, arguments);
    holdUntil_ = GetTickCount64() + 5000;
}

void Panel::startMeasure() {
    if (!enabled(Control::Measure)) return;
    if (latest_.applied.mode == 0) {
        abDetail_ = strings_.get(L"AbNeedsOn");
        invalidate();
        return;
    }
    ab_ = AbRun{latest_.applied.mode};
    startPhase();
}

void Panel::startPhase() {
    const uint32_t mode = AbPhases[ab_->phase] ? ab_->mode : 0;
    Patch patch{Mode};
    patch.values.mode = mode;
    send(patch);
    ab_->deadline = GetTickCount64() + AbPhaseMs;
    abValue_ = printed(L"%zu/%zu", ab_->phase + 1, std::size(AbPhases));
    abDetail_ = strings_.format(L"AbRunning", {{L"state", strings_.get(mode ? L"AbOn" : L"AbOff")}});
    invalidate();
}

void Panel::stepMeasure(const Status& s) {
    if (GetTickCount64() < ab_->deadline) return;
    const uint32_t expected = AbPhases[ab_->phase] ? ab_->mode : 0;
    if (!pending_.empty() || waiting_ || s.applied.mode != expected) {
        ab_->deadline = GetTickCount64() + 1000; // not applied yet; give it another second
        return;
    }
    if (s.metrics.timingSamples >= 60) ab_->results[AbPhases[ab_->phase]].push_back(s.metrics.gpuMsMedian);
    if (++ab_->phase == std::size(AbPhases))
        finishMeasure(false);
    else
        startPhase();
}

void Panel::finishMeasure(bool cancelled) {
    const AbRun run = std::move(*ab_);
    ab_.reset();
    if (endpoint_.isOpen()) {
        Patch patch{Mode};
        patch.values.mode = run.mode;
        send(patch);
    }
    abSaving_ = 0;
    if (cancelled || run.results[0].empty() || run.results[1].empty()) {
        abValue_.clear();
        abDetail_ = strings_.get(L"AbCancelled");
    } else {
        const float off = median(run.results[0]), on = median(run.results[1]);
        abSaving_ = off - on;
        abValue_ = off >= on ? printed(L"−%.2f ms", off - on) : printed(L"+%.2f ms", on - off);
        abDetail_ = strings_.format(L"AbResult", {{L"off", off}, {L"on", on}});
    }
    invalidate();
}

void Panel::save() {
    if (!haveStatus_) return;
    if (options_.config.empty()) {
        notice(L"NoConfig");
        return;
    }
    endpoint_.read(latest_);
    std::wstring error;
    if (saveDefaults(options_.config, latest_.applied, error))
        notice(L"Saved", {{L"path", options_.config.wstring()}});
    else
        notice(L"Error", {{L"detail", error}});
}

void Panel::restore() {
    if (!haveStatus_) return;
    endpoint_.read(latest_);
    settings_ = latest_.startup;
    holdUntil_ = 0;
    send({AllFields, latest_.startup});
    flush();
    invalidate();
}

void Panel::close() {
    if (ab_) finishMeasure(true); // put the foveation mode back before leaving
    flush();
    DestroyWindow(window_);
}

} // namespace dvr::panel
