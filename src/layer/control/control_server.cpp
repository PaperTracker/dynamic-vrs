#include "control_server.hpp"
#include <windows.h>
#include <string>

namespace dvr::control {
bool Server::start(const Settings& startup) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    state_.processStart = (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    state_.session = static_cast<uint64_t>(counter.QuadPart);
    state_.processId = GetCurrentProcessId();
    const auto name = std::wstring(MappingPrefix) + std::to_wstring(state_.processId);
    mapping_ =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedControl), name.c_str());
    if (!mapping_) return false;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    shared_ = static_cast<SharedControl*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControl)));
    if (!shared_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    Status old;
    uint64_t sequence = 0;
    if (existed && snapshot(shared_->status, old, sequence) && old.processStart == state_.processStart &&
        old.result != static_cast<uint32_t>(Result::Closed)) {
        UnmapViewOfFile(shared_);
        CloseHandle(mapping_);
        shared_ = nullptr;
        mapping_ = nullptr;
        return false; // Another loaded layer already owns this process's endpoint.
    }
    state_.accepted = state_.applied = state_.startup = startup;
    seen_ = std::atomic_ref(shared_->request.sequence).load();
    std::atomic_ref(shared_->magic).store(0);
    std::atomic_ref(shared_->version).store(Version);
    std::atomic_ref(shared_->size).store(sizeof(SharedControl));
    send();
    std::atomic_ref(shared_->magic).store(Magic);
    return true;
}
Server::~Server() { stop(); }
void Server::stop() {
    if (shared_) {
        state_.result = static_cast<uint32_t>(Result::Closed);
        send();
        UnmapViewOfFile(shared_);
    }
    if (mapping_) CloseHandle(mapping_);
    shared_ = nullptr;
    mapping_ = nullptr;
    state_ = {};
    seen_ = pending_ = 0;
}
void Server::send() {
    if (!shared_) return;
    state_.heartbeatMs = GetTickCount64();
    publish(shared_->status, state_);
}
bool Server::poll(Settings& settings) {
    if (!shared_) return false;
    const auto available = std::atomic_ref(shared_->request.sequence).load(std::memory_order_seq_cst);
    if (available == seen_ || (available & 1)) return false;
    Request request;
    uint64_t sequence = 0;
    if (!snapshot(shared_->request, request, sequence) || sequence == seen_) return false;
    seen_ = sequence;
    state_.processedSequence = sequence;
    const auto error = request.session != state_.session ? Error::WrongSession
                       : !valid(request.settings)        ? Error::InvalidSettings
                                                         : Error::None;
    if (error != Error::None) {
        state_.error = static_cast<uint32_t>(error);
        state_.result = static_cast<uint32_t>(Result::Rejected);
        send();
        return false;
    }
    settings = request.settings;
    state_.accepted = settings;
    state_.result = static_cast<uint32_t>(Result::WaitingForFrame);
    state_.error = 0;
    state_.gpuReady = 0;
    pending_ = sequence;
    send();
    return true;
}
void Server::applied(bool gpuReady, uint32_t width, uint32_t height) {
    if (!shared_) return;
    const bool rejected = state_.result == static_cast<uint32_t>(Result::Rejected);
    if (state_.result != static_cast<uint32_t>(Result::WaitingForFrame) && state_.appliedSequence == pending_ &&
        state_.width == width && state_.height == height && state_.gpuReady == uint32_t{gpuReady})
        return;
    state_.applied = state_.accepted;
    state_.appliedSequence = pending_;
    if (!rejected) state_.result = static_cast<uint32_t>(Result::Applied);
    state_.gpuReady = gpuReady;
    state_.width = width;
    state_.height = height;
    send();
}
void Server::rebase(const Settings& startup) {
    if (!shared_) return;
    state_.startup = startup;
    if (!state_.processedSequence) state_.accepted = state_.applied = startup;
    send();
}
void Server::heartbeat() {
    if (shared_ && GetTickCount64() - state_.heartbeatMs >= 250) send();
}
} // namespace dvr::control
