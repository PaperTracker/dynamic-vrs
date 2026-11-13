#pragma once
#include "common/vrs_control_protocol.hpp"

namespace dvr::control {
// Called under the native layer's existing render-state mutex, never from GUI.
class Server {
public:
    ~Server();
    bool start(const Settings& startup);
    void stop();
    bool poll(Settings& settings);
    void applied(bool gpuReady, uint32_t width, uint32_t height);
    void heartbeat();
    // Replaces the startup settings once they are fully known (rings an old ini gave as viewport
    // fractions need the eye projection); also the accepted and applied ones while no request came.
    void rebase(const Settings& startup);
    // Publishes immediately; called once per completed frame.
    void publishMetrics(const Metrics& value) {
        state_.metrics = value;
        send();
    }

private:
    void send();
    void* mapping_ = nullptr;
    SharedControl* shared_ = nullptr;
    Status state_;
    uint64_t seen_ = 0, pending_ = 0;
};
} // namespace dvr::control
