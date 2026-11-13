#pragma once
#include "common/vrs_control_protocol.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dvr::panel {
using control::Settings;
using control::Status;

struct Application {
    uint32_t processId = 0;
    std::wstring name;
    bool operator==(const Application&) const = default;
};

// Processes whose layer currently publishes a live control endpoint.
std::vector<Application> discoverApplications();

// Settings fields a request changes. The other fields come from the newest unprocessed
// request, or from the accepted settings, so two controllers do not undo each other.
enum Field : uint32_t {
    Mode = 1 << 0,
    RingDegrees = 1 << 1, // all four rings at once: they are kept in order together
    DitherDegrees = 1 << 2,
    UniformRate = 1 << 3,
    Rings = 1 << 4,
    BlurMip = 1 << 5,
    AllFields = ~0U,
};
struct Patch {
    uint32_t fields = 0;
    Settings values;
    void applyTo(Settings& settings) const;
    void merge(const Patch& other);
    bool empty() const { return fields == 0; }
};

enum class WriteResult { Sent, Busy, Invalid, Failed };

class Endpoint {
public:
    Endpoint() = default;
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;
    ~Endpoint() { close(); }

    bool open(uint32_t processId);
    void close();
    bool isOpen() const { return view_ != nullptr; }
    uint32_t processId() const { return processId_; }
    uint64_t session() const { return session_; }
    // False while the layer is in the middle of publishing; the caller tries again on its next tick.
    bool read(Status& status) const;
    // Never waits for another controller: Busy means try again shortly.
    WriteResult write(const Patch& patch, uint64_t& sequence);

private:
    void* mapping_ = nullptr;
    void* writer_ = nullptr;
    control::SharedControl* view_ = nullptr;
    uint32_t processId_ = 0;
    uint64_t session_ = 0;
};
} // namespace dvr::panel
