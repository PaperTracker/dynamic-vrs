#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace dvr::control {
inline constexpr uint32_t Magic = 0x43565450; // PTVC
inline constexpr uint32_t Version = 3;
inline constexpr wchar_t MappingPrefix[] = L"Local\\DynamicVRS.Control.v3.";
// Held by a running control panel, so the layer does not auto-launch a second one.
inline constexpr wchar_t PanelMutexName[] = L"Local\\DynamicVRS.ControlPanel";

// Shading-rate tiers from the gaze point outward; the rate image stores these indices.
enum Tier : uint32_t { Tier1x1, Tier2x1, Tier2x2, Tier4x2, Tier4x4, TierCount };
// Every tier but the last (4x4) ends at a ring.
inline constexpr uint32_t RingCount = TierCount - 1;
// A ring at this angle reaches the edge of the view: its tier fills the rest and no coarser tier is used.
inline constexpr float EdgeDegrees = 90;

// Tier of a uniform or legacy rate code: 1 = 2x1, 2 = 2x2, 42 = 4x2, 4 = 4x4.
constexpr uint32_t tierOfRate(uint32_t rate) {
    return rate == 1 ? Tier2x1 : rate == 2 ? Tier2x2 : rate == 42 ? Tier4x2 : rate == 4 ? Tier4x4 : Tier1x1;
}

struct Settings {
    uint32_t mode = 0; // off, fixed, gaze
    // Outer radius of the 1x1, 2x1, 2x2 and 4x2 tiers in degrees from the eye axis; everything beyond the last
    // ring is 4x4. 0 skips a tier, EdgeDegrees extends it to the edge of the view.
    float ringDegrees[RingCount] = {};
    // Width in degrees of the band around each ring where tiles of the two tiers are dithered; 0 = hard edges.
    float ditherDegrees = 0;
    // Diagnostic: nonzero shades the whole target at this rate code (2, 42 or 4), leaving no sharp zone.
    uint32_t uniformRate = 0;
    uint32_t rings = 0, blurMip = 0, targetWidth = 0, targetHeight = 0;
    uint32_t combine = 0, flipX = 0, flipY = 0;
    float forwardYaw = 0, forwardPitch = 0;
    bool operator==(const Settings&) const = default;
};
// What the sharp zone is doing in gaze mode. Saccade, Stale and Lost widen every ring for a while.
enum class GazeState : uint32_t { Tracking, Saccade, Stale, Lost };
// Live measurements published with every status update. Radii are what the rate image
// actually uses, after degrees are projected per eye; they are 0 until a pattern exists.
struct Metrics {
    // Per eye and ring: X and Y radius as a fraction of the eye viewport; 0 for a skipped ring or one
    // that reaches the edge.
    float radii[2][RingCount][2] = {};
    float ringDegrees[RingCount] = {};   // rings in effect, widening included; 0 skipped, EdgeDegrees edge
    float coverage[TierCount] = {};      // tile fraction at each tier
    float shadingWork = 1;               // estimated pixel-shading work relative to full rate
    float gpuMsMedian = 0, gpuMsP95 = 0; // scene GPU time per frame (OpenVR pre-submit GPU time)
    float frameIntervalMs = 0;           // median client frame interval
    float gazeHz = 0, gazeAgeMs = 0;
    uint32_t gazeConnected = 0, gazeValid = 0, timingSamples = 0;
    float center[2] = {.5F, .5F}; // left-eye sharp-zone center, viewport coordinates with y down
    float widenDegrees = 0;       // how far every ring is currently pushed outward
    uint32_t gazeState = 0;       // GazeState
    bool operator==(const Metrics&) const = default;
};
enum class Result : uint32_t { WaitingForFrame, Applied, Rejected, Closed };
enum class Error : uint32_t { None, InvalidSettings, WrongSession };
struct Request {
    uint64_t session = 0;
    Settings settings;
};
struct Status {
    uint64_t session = 0, processStart = 0, processedSequence = 0, appliedSequence = 0, heartbeatMs = 0;
    uint32_t processId = 0, result = 0, error = 0, gpuReady = 0, width = 0, height = 0;
    Settings accepted, applied, startup;
    Metrics metrics;
};
static_assert(sizeof(Settings) == 64 && sizeof(Metrics) == 152 && sizeof(Request) == 72 && sizeof(Status) == 408);

// Windows x64 only. A single writer per mailbox publishes even sequences.
// Control clients serialize request writes with the per-PID Writer mutex.
// Atomic payload words make concurrent snapshots defined; readers never wait.
template <typename T> struct alignas(64) Mailbox {
    uint64_t sequence = 0;
    std::array<uint32_t, sizeof(T) / 4> words{};
    std::array<uint8_t, (64 - (8 + sizeof(T)) % 64) % 64> padding{};
};
struct alignas(64) SharedControl {
    uint32_t magic = 0, version = 0, size = 0, reserved = 0;
    std::array<uint8_t, 48> padding{};
    Mailbox<Request> request;
    Mailbox<Status> status;
};
static_assert(offsetof(SharedControl, request) == 64 && offsetof(SharedControl, status) == 192 &&
              sizeof(SharedControl) == 640);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free && std::atomic_ref<uint32_t>::is_always_lock_free);

template <typename T> uint64_t publish(Mailbox<T>& mailbox, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<uint32_t, sizeof(T) / 4> words{};
    std::memcpy(words.data(), &value, sizeof(T));
    auto sequence = std::atomic_ref(mailbox.sequence);
    const auto next = (sequence.load(std::memory_order_seq_cst) & ~uint64_t{1}) + 2;
    sequence.store(next - 1, std::memory_order_seq_cst);
    for (size_t i = 0; i < words.size(); ++i)
        std::atomic_ref(mailbox.words[i]).store(words[i], std::memory_order_seq_cst);
    sequence.store(next, std::memory_order_seq_cst);
    return next;
}
template <typename T> bool snapshot(const Mailbox<T>& mailbox, T& value, uint64_t& version) {
    const auto before = std::atomic_ref(mailbox.sequence).load(std::memory_order_seq_cst);
    if (!before || (before & 1)) return false;
    std::array<uint32_t, sizeof(T) / 4> words{};
    for (size_t i = 0; i < words.size(); ++i)
        words[i] = std::atomic_ref(mailbox.words[i]).load(std::memory_order_seq_cst);
    if (before != std::atomic_ref(mailbox.sequence).load(std::memory_order_seq_cst)) return false;
    std::memcpy(&value, words.data(), sizeof(T));
    version = before;
    return true;
}
inline bool valid(const Settings& s) {
    const auto range = [](float v, float a, float b) { return std::isfinite(v) && v >= a && v <= b; };
    const auto rate = [](uint32_t v) { return v == 2 || v == 4 || v == 42; };
    // Rings in use widen outward: a finer tier never ends beyond a coarser one.
    float previous = 0;
    for (const float ring : s.ringDegrees) {
        if (!range(ring, 0, EdgeDegrees) || (ring && ring < previous)) return false;
        if (ring) previous = ring;
    }
    return s.mode <= 2 && range(s.ditherDegrees, 0, 10) && (!s.uniformRate || rate(s.uniformRate)) && s.rings <= 1 &&
           s.blurMip <= 8 && s.combine <= 1 && s.flipX <= 1 && s.flipY <= 1 && s.targetWidth <= 20000 &&
           s.targetHeight <= 20000 && ((s.targetWidth == 0) == (s.targetHeight == 0)) && range(s.forwardYaw, -10, 10) &&
           range(s.forwardPitch, -10, 10);
}
} // namespace dvr::control
