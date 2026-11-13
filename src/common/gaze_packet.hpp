#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

// PaperTracker gaze export, version 1: OSC 1.0 messages over loopback UDP.
// Wire format and publisher contract: docs/provider-layout.md.
namespace dvr::gaze {
inline constexpr std::uint16_t DefaultPort = 9945;
inline constexpr std::uint64_t RenewIntervalMs = 1000;
// The left/right points lie on the publisher's calibration plane z = -PlaneDistanceMeters in head space.
inline constexpr float PlaneDistanceMeters = 1.6F;
inline constexpr std::size_t PacketSize = 72;

enum Flag : std::uint32_t {
    LeftValid = 1U << 0,
    RightValid = 1U << 1,
    FusedValid = 1U << 2,
    LeftMeasured = 1U << 3,
    RightMeasured = 1U << 4,
    FusedMeasured = 1U << 5,
};

// Head-origin tangent-plane angles in degrees: the point is (tan(yaw), tan(pitch), -1), yaw right and pitch up.
struct Angles {
    float yawDegrees = 0, pitchDegrees = 0;
};
struct Sample {
    std::uint32_t sequence = 0, flags = 0;
    std::int32_t sampleAgeUs = -1;
    Angles left, right, fused;
};

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

// OSC strings end with at least one zero byte and are padded with zeros to a multiple of four.
template <std::size_t N> constexpr std::size_t oscStringSize() { return (N + 4) / 4 * 4; }
template <std::size_t N> constexpr auto oscMessage(const char (&address)[N]) {
    std::array<char, oscStringSize<N - 1>() + 4> message{};
    for (std::size_t i = 0; i + 1 < N; ++i) message[i] = address[i];
    message[oscStringSize<N - 1>()] = ',';
    return message;
}
inline constexpr auto SubscribeMessage = oscMessage("/papertracker/gaze/v1/subscribe");
inline constexpr auto UnsubscribeMessage = oscMessage("/papertracker/gaze/v1/unsubscribe");
// Address and type tags of every data packet: 24 + 12 bytes, followed by 9 big-endian 4-byte arguments.
inline constexpr std::string_view DataHeader{"/papertracker/gaze/v1\0\0\0,iiiffffff\0\0", 36};
static_assert(DataHeader.size() + 9 * 4 == PacketSize);

inline std::uint32_t getWord(const std::uint8_t* bytes) {
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8) |
           bytes[3];
}
inline bool decode(const std::uint8_t* bytes, std::size_t length, Sample& result) {
    if (length != PacketSize || std::memcmp(bytes, DataHeader.data(), DataHeader.size()) != 0) return false;
    const std::uint8_t* arguments = bytes + DataHeader.size();
    Sample sample;
    sample.sequence = getWord(arguments);
    sample.flags = getWord(arguments + 4);
    sample.sampleAgeUs = static_cast<std::int32_t>(getWord(arguments + 8));
    float* angles[] = {&sample.left.yawDegrees,    &sample.left.pitchDegrees, &sample.right.yawDegrees,
                       &sample.right.pitchDegrees, &sample.fused.yawDegrees,  &sample.fused.pitchDegrees};
    for (std::size_t i = 0; i < 6; ++i) {
        const std::uint32_t bits = getWord(arguments + 12 + i * 4);
        std::memcpy(angles[i], &bits, 4);
        if (!std::isfinite(*angles[i]) || std::abs(*angles[i]) >= 90.0F) return false;
    }
    result = sample;
    return true;
}
} // namespace dvr::gaze
