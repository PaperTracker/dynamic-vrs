#include "gaze_tracking.hpp"

#include "common/log.hpp"
#include "vrs_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace dvr::layer {
namespace {

// One poll of the gaze export: which eyes have a point, and where it lands in each eye viewport, using
// the same conversion as the gaze VRS pattern.
struct GazeMeasurement {
    bool connected = false, fresh = false; // fresh: a packet not seen by an earlier poll
    bool valid[2] = {};
    EyeViewportPoint point[2];
    uint64_t sequence = 0;
};

GazeMeasurement readGazeMeasurement() {
    GazeMeasurement result;
    auto& gaze = g_vrsState.gaze;
    dvr::gaze::Sample frame;
    const bool received = gaze.receiver.poll(g_settings.gazeExportPort, frame);
    if (!g_settings.gazeExportPort && !gaze.disabledLogged) {
        logMessage("stage=gaze-export result=disabled port=0");
        gaze.disabledLogged = true;
    }
    const auto error = gaze.receiver.error();
    if (error && error != gaze.loggedError) {
        logMessage("stage=gaze-export result=socket-failed port=%u error=%u retryMs=1000", g_settings.gazeExportPort,
                   error);
        gaze.openLogged = false;
    }
    gaze.loggedError = error;
    if (gaze.receiver.open() && !gaze.openLogged) {
        logMessage("stage=gaze-export result=subscribing target=127.0.0.1:%u", g_settings.gazeExportPort);
        gaze.openLogged = true;
    }
    if (gaze.receiver.connected() != gaze.connectedLogged) {
        gaze.connectedLogged = gaze.receiver.connected();
        logMessage("stage=gaze-export result=%s target=127.0.0.1:%u",
                   gaze.connectedLogged ? "publisher-answered" : "publisher-gone", g_settings.gazeExportPort);
    }
    g_metrics.gazeValid = false;
    if (!received) return result;
    result.connected = true;
    result.sequence = frame.sequence;
    // With combine, both eyes look at the fused point; each still projects it from its own position.
    const bool fused = g_settings.gazeCombine != 0 && (frame.flags & dvr::gaze::FusedValid);
    const bool valid[2] = {fused || (frame.flags & dvr::gaze::LeftValid) != 0,
                           fused || (frame.flags & dvr::gaze::RightValid) != 0};
    if (frame.sequence != g_metrics.lastGazeSequence) {
        g_metrics.lastGazeSequence = frame.sequence;
        g_metrics.lastGazePacketMs = GetTickCount64();
        ++g_metrics.gazePackets;
        result.fresh = true;
    }
    g_metrics.gazeValid = valid[0] || valid[1];
    if (!g_metrics.gazeValid || !ensureEyeProjection() || !ensureFixedCenters()) return result;
    for (int eye = 0; eye < 2; ++eye) {
        const auto& angles = fused ? frame.fused : eye == 0 ? frame.left : frame.right;
        result.valid[eye] = valid[eye] && projectGazeToViewport(eye, angles, result.point[eye]);
    }
    return result;
}

using dvr::control::GazeState;
using TangentPoint = std::array<float, 2>;

// The stabilizer works in each eye's tangent plane, where distances are angles and do not depend on
// how the viewport stretches them.
TangentPoint viewportToTangent(int eye, const EyeViewportPoint& point) {
    const auto& e = g_eyeProjection[eye];
    return {e.tangentLeft + point.x * (e.tangentRight - e.tangentLeft),
            e.tangentTop + point.y * (e.tangentBottom - e.tangentTop)};
}

EyeViewportPoint tangentToViewport(int eye, const TangentPoint& point) {
    const auto& e = g_eyeProjection[eye];
    return {std::clamp((point[0] - e.tangentLeft) / (e.tangentRight - e.tangentLeft), 0.0F, 1.0F),
            std::clamp((point[1] - e.tangentTop) / (e.tangentBottom - e.tangentTop), 0.0F, 1.0F)};
}

float angleDegrees(const TangentPoint& a, const TangentPoint& b) {
    constexpr float degreesPerRadian = 180.0F / 3.14159265358979323846F;
    const float dot = a[0] * b[0] + a[1] * b[1] + 1.0F;
    const float norms = std::sqrt((a[0] * a[0] + a[1] * a[1] + 1.0F) * (b[0] * b[0] + b[1] * b[1] + 1.0F));
    return std::acos(std::clamp(dot / norms, -1.0F, 1.0F)) * degreesPerRadian;
}

double nowMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(frequency.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / ticksPerMs;
}

// Turns gaze packets into the centers the rate image uses. While the eye fixates, a speed-adaptive
// low-pass and a deadband keep tracker noise from moving the rings; a saccade snaps the center to the
// new point. Every ring is pushed outward during a saccade and while gaze is invalid, silent or gone,
// then eases back. Lost gaze drifts to head-forward instead of jumping there.
struct GazeStabilizer {
    struct Eye {
        bool tracked = false; // had a valid point since gaze mode started
        TangentPoint raw{}, filtered{}, shown{};
        TangentPoint velocity{}; // low-passed, tangent units per second
        double lastValidMs = 0;
    } eyes[2];
    double lastUpdateMs = 0, lastPacketMs = 0, saccadeUntilMs = 0, lostSinceMs = 0;
    float widen = 0.0F;
    GazeState state = GazeState::Tracking;
    // Rate-limited event log and a periodic summary.
    double nextEventLogMs = 0, summaryStartMs = 0, widenedMs = 0, widenSum = 0, heldMs = 0;
    unsigned saccades = 0, stale = 0, lost = 0, recovered = 0, suppressed = 0;
};
GazeStabilizer g_stabilizer;

const char* gazeStateName(GazeState state) {
    switch (state) {
    case GazeState::Saccade: return "saccade";
    case GazeState::Stale: return "stale";
    case GazeState::Lost: return "lost";
    default: return "tracking";
    }
}

// Events come in bursts (a saccade every few hundred ms, a blink every few seconds): one line per 2 s
// at most, counting what it left out, and a summary every 30 s.
void logStabilizerEvent(GazeStabilizer& g, double now, const char* event, const char* detail) {
    if (now < g.nextEventLogMs) {
        ++g.suppressed;
        return;
    }
    g.nextEventLogMs = now + 2000.0;
    logMessage("stage=gaze-stabilizer event=%s %s widen=%.1f suppressed=%u", event, detail, g.widen, g.suppressed);
    g.suppressed = 0;
}

void stabilizeGazeLocked(EyeViewportPoint centers[2], float& widen, uint64_t& sequence) {
    constexpr float twoPi = 6.28318530717958647692F, degreesPerRadian = 57.2957795F;
    auto& g = g_stabilizer;
    const auto& config = g_settings.gazeStabilizer;
    const double now = nowMs();
    const float dt = g.lastUpdateMs ? static_cast<float>(std::clamp(now - g.lastUpdateMs, 0.0, 250.0)) : 0.0F;
    g.lastUpdateMs = now;
    if (!g.summaryStartMs) g.summaryStartMs = now;

    const GazeMeasurement m = readGazeMeasurement();
    sequence = m.sequence;
    const float packetSeconds =
        g.lastPacketMs ? static_cast<float>(std::clamp(now - g.lastPacketMs, 4.0, 250.0) / 1000.0) : 0.0F;
    if (m.fresh) g.lastPacketMs = now;
    // The export sends only when the gaze changes; a valid point that stopped changing for this long
    // is treated as stale, since a living eye never holds that still.
    const bool silent = config.staleMs > 0.0F && g.lastPacketMs && now - g.lastPacketMs > config.staleMs;

    bool usable[2] = {}, snap = false;
    TangentPoint measured[2]{};
    float fastest = 0.0F;
    for (int eye = 0; eye < 2; ++eye) {
        usable[eye] = m.connected && m.valid[eye] && !silent;
        if (!usable[eye]) continue;
        measured[eye] = viewportToTangent(eye, m.point[eye]);
        const auto& e = g.eyes[eye];
        // A first point, or one after a gap, is a jump to wherever the eye went meanwhile.
        if (!e.tracked || now - e.lastValidMs > std::max(config.staleMs, 50.0F)) {
            snap = true;
        } else if (m.fresh && packetSeconds > 0.0F) {
            const float jump = angleDegrees(measured[eye], e.raw);
            const float speed = jump / packetSeconds;
            fastest = std::max(fastest, speed);
            if (config.saccadeDegreesPerSecond > 0.0F && speed >= config.saccadeDegreesPerSecond && jump >= 1.0F)
                snap = true;
        }
    }
    if (snap) {
        g.saccadeUntilMs = now + config.saccadeHoldMs;
        if (g.state == GazeState::Tracking) {
            ++g.saccades;
            char detail[64];
            std::snprintf(detail, sizeof(detail), "speed=%.0fdeg/s", fastest);
            logStabilizerEvent(g, now, "saccade", detail);
        }
    }

    const float band = std::tan(config.deadbandDegrees / degreesPerRadian);
    bool held = false;
    for (int eye = 0; eye < 2; ++eye) {
        auto& e = g.eyes[eye];
        const TangentPoint fixed = viewportToTangent(eye, g_fixedCenters[eye]);
        if (usable[eye]) {
            const auto& point = measured[eye];
            if (snap || !e.tracked || config.smoothingHz <= 0.0F) {
                e.filtered = point;
                e.velocity = {};
            } else if (m.fresh && packetSeconds > 0.0F) {
                // Speed-adaptive low-pass: smooth while still, close to the raw point while moving.
                const float velocityAlpha = 1.0F - std::exp(-twoPi * packetSeconds);
                for (int axis = 0; axis < 2; ++axis)
                    e.velocity[axis] +=
                        ((point[axis] - e.raw[axis]) / packetSeconds - e.velocity[axis]) * velocityAlpha;
                const float speed = std::hypot(e.velocity[0], e.velocity[1]) * degreesPerRadian;
                const float alpha =
                    1.0F - std::exp(-twoPi * config.smoothingHz * (1.0F + speed / 30.0F) * packetSeconds);
                for (int axis = 0; axis < 2; ++axis) e.filtered[axis] += (point[axis] - e.filtered[axis]) * alpha;
            }
            e.raw = point;
            e.tracked = true;
            e.lastValidMs = now;
            // The shown center follows the filtered one on a leash as long as the deadband.
            const float dx = e.filtered[0] - e.shown[0], dy = e.filtered[1] - e.shown[1];
            const float distance = std::hypot(dx, dy);
            if (snap || band <= 0.0F || distance > band * 4.0F) {
                e.shown = e.filtered;
            } else if (distance > band) {
                const float pull = (distance - band) / distance;
                e.shown = {e.shown[0] + dx * pull, e.shown[1] + dy * pull};
            } else if (distance > 0.0F) {
                held = true;
            }
        } else if (!e.tracked) {
            e.shown = fixed;
        } else if (now - e.lastValidMs >= config.lostMs) {
            // Lost: drift to head-forward over about a third of a second.
            const float pull = 1.0F - std::exp(-dt / 300.0F);
            e.shown = {e.shown[0] + (fixed[0] - e.shown[0]) * pull, e.shown[1] + (fixed[1] - e.shown[1]) * pull};
        }
        centers[eye] = tangentToViewport(eye, e.shown);
    }

    // Lost once no eye was valid for gazeLostMs (or ever), stale before that or while one eye is
    // missing, saccade while the hold runs.
    const GazeState previous = g.state;
    const double lastValid = std::max(g.eyes[0].lastValidMs, g.eyes[1].lastValidMs);
    if (!usable[0] && !usable[1])
        g.state = !lastValid || now - lastValid >= config.lostMs ? GazeState::Lost : GazeState::Stale;
    else if ((!usable[0] && g.eyes[0].tracked) || (!usable[1] && g.eyes[1].tracked))
        g.state = GazeState::Stale;
    else
        g.state = now < g.saccadeUntilMs ? GazeState::Saccade : GazeState::Tracking;

    if (g.state != previous) {
        const bool missing = g.state == GazeState::Stale || g.state == GazeState::Lost;
        const bool wasMissing = previous == GazeState::Stale || previous == GazeState::Lost;
        char detail[96];
        if (missing) {
            if (!wasMissing) g.lostSinceMs = now;
            ++(g.state == GazeState::Stale ? g.stale : g.lost);
            const char* reason = !m.connected                 ? "disconnected"
                                 : silent                     ? "silent"
                                 : !m.valid[0] && !m.valid[1] ? "invalid"
                                                              : "one-eye";
            std::snprintf(detail, sizeof(detail), "reason=%s from=%s", reason, gazeStateName(previous));
            logStabilizerEvent(g, now, gazeStateName(g.state), detail);
        } else if (wasMissing) {
            ++g.recovered;
            std::snprintf(detail, sizeof(detail), "from=%s afterMs=%.0f", gazeStateName(previous),
                          g.lostSinceMs ? now - g.lostSinceMs : 0.0);
            logStabilizerEvent(g, now, "recovered", detail);
        }
    }

    // Widening: up at once, back down with the release time constant.
    const bool missing = g.state == GazeState::Stale || g.state == GazeState::Lost;
    const float target = g.state == GazeState::Saccade ? config.saccadeWidenDegrees
                         : missing                     ? config.lostWidenDegrees
                                                       : 0.0F;
    if (target >= g.widen || config.releaseMs <= 0.0F)
        g.widen = target;
    else
        g.widen += (target - g.widen) * (1.0F - std::exp(-dt / config.releaseMs));
    if (g.widen < 0.05F) g.widen = 0.0F;
    widen = g.widen;

    if (g.widen > 0.0F) g.widenedMs += dt;
    g.widenSum += g.widen * dt;
    if (held) g.heldMs += dt;
    if (const double window = now - g.summaryStartMs; window >= 30000.0) {
        logMessage("stage=gaze-stabilizer summary windowS=%.0f state=%s saccades=%u stale=%u lost=%u recovered=%u "
                   "widenedPct=%.1f meanWiden=%.2f deadbandHeldPct=%.1f",
                   window / 1000.0, gazeStateName(g.state), g.saccades, g.stale, g.lost, g.recovered,
                   100.0 * g.widenedMs / window, g.widenSum / window, 100.0 * g.heldMs / window);
        g.summaryStartMs = now;
        g.widenedMs = g.widenSum = g.heldMs = 0;
        g.saccades = g.stale = g.lost = g.recovered = 0;
    }
}

void resetGazeStabilizerLocked() { g_stabilizer = {}; }

bool resolveFoveationCenters(EyeViewportPoint& left, EyeViewportPoint& right, float& widen,
                             uint64_t* sequence = nullptr) {
    if (g_settings.mode != VrsMode::Gaze) {
        g_vrsState.gaze.receiver.close();
        g_vrsState.gaze.openLogged = false;
        g_vrsState.gaze.connectedLogged = false;
        resetGazeStabilizerLocked();
    }
    if (!ensureFixedCenters()) return false;
    left = g_fixedCenters[0];
    right = g_fixedCenters[1];
    widen = 0.0F;
    if (sequence) *sequence = 0;
    if (g_settings.mode == VrsMode::Gaze) {
        EyeViewportPoint centers[2];
        uint64_t measured = 0;
        stabilizeGazeLocked(centers, widen, measured);
        left = centers[0];
        right = centers[1];
        if (sequence) *sequence = measured;
    }
    return true;
}

} // namespace

bool latchFoveationFrameLocked() {
    g_foveationFrame.valid = resolveFoveationCenters(g_foveationFrame.left, g_foveationFrame.right,
                                                     g_foveationFrame.widen, &g_foveationFrame.sequence);
    g_foveationFrame.configurationRevision = g_configurationRevision;
    return g_foveationFrame.valid;
}

bool frameFoveationCenters(EyeViewportPoint& left, EyeViewportPoint& right, uint64_t* sequence, float* widen) {
    if ((!g_foveationFrame.valid || g_foveationFrame.configurationRevision != g_configurationRevision) &&
        !latchFoveationFrameLocked())
        return false;
    left = g_foveationFrame.left;
    right = g_foveationFrame.right;
    if (sequence) *sequence = g_foveationFrame.sequence;
    if (widen) *widen = g_foveationFrame.widen;
    return true;
}

void updateGazeSampleLocked() {
    if (g_settings.uniformRate != 0) return;
    auto& gaze = g_vrsState.gaze;
    EyeViewportPoint left, right;
    uint64_t sequence = 0;
    if (!frameFoveationCenters(left, right, &sequence)) return;
    if (sequence == gaze.lastSequence && left.x == gaze.leftX && left.y == gaze.leftY && right.x == gaze.rightX &&
        right.y == gaze.rightY)
        return;
    gaze.lastSequence = sequence;
    gaze.leftX = left.x;
    gaze.leftY = left.y;
    gaze.rightX = right.x;
    gaze.rightY = right.y;
    ++gaze.updates;
    if (gaze.updates <= 8U || (gaze.updates % 60U) == 0U)
        logMessage("stage=vrs-gaze result=updated updates=%u seq=%llu left=%.3f,%.3f right=%.3f,%.3f", gaze.updates,
                   static_cast<unsigned long long>(sequence), left.x, left.y, right.x, right.y);
}

GazeState gazeStabilizerStateLocked() { return g_stabilizer.state; }

void resetGazeStateLocked() { resetGazeStabilizerLocked(); }

} // namespace dvr::layer
