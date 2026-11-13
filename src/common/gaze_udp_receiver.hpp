#pragma once
#include <winsock2.h>
#include <windows.h>
#include "gaze_packet.hpp"
#include <array>

namespace dvr::gaze {
// Subscriber of the PaperTracker gaze export. One owner thread; bounded, nonblocking work per poll.
//
// The receiver retains the latest accepted packet. Gaze stabilization separately applies the configured
// stale timeout to sequence-number changes. Port-unreachable feedback clears the retained packet when
// the publisher disappears between subscription renewals.
class Receiver final {
public:
    ~Receiver() { close(); }
    Receiver() = default;
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    bool open() const { return socket_ != INVALID_SOCKET; }
    // True once the publisher has answered and until it goes away.
    bool connected() const { return have_; }
    unsigned error() const { return error_; }
    void close() {
        if (socket_ != INVALID_SOCKET) {
            send(UnsubscribeMessage.data(), static_cast<int>(UnsubscribeMessage.size()));
            closesocket(socket_);
        }
        socket_ = INVALID_SOCKET;
        if (started_) WSACleanup();
        started_ = false;
        have_ = false;
        port_ = 0;
        nextOpenMs_ = 0;
        nextRenewMs_ = 0;
        error_ = 0;
        latest_ = {};
    }
    // Returns the latest packet from 127.0.0.1:port; false while there is none or the publisher is gone.
    bool poll(std::uint16_t port, Sample& result) {
        result = {};
        const auto now = GetTickCount64();
        if (port != port_) {
            close();
            port_ = port;
        }
        if (!port) return false;
        if (socket_ == INVALID_SOCKET) {
            if (now < nextOpenMs_) return false;
            if (!openSocket()) {
                nextOpenMs_ = now + 1000;
                return false;
            }
        }
        if (now >= nextRenewMs_) {
            send(SubscribeMessage.data(), static_cast<int>(SubscribeMessage.size()));
            nextRenewMs_ = now + RenewIntervalMs;
        }
        for (int count = 0; count < 128; ++count) {
            std::array<std::uint8_t, PacketSize + 1> bytes{};
            sockaddr_in peer{};
            int peerSize = sizeof(peer);
            const int size = recvfrom(socket_, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                                      reinterpret_cast<sockaddr*>(&peer), &peerSize);
            if (size == SOCKET_ERROR) {
                const auto failure = WSAGetLastError();
                if (failure == WSAEWOULDBLOCK) break;
                if (failure == WSAECONNRESET) {
                    have_ = false;
                    latest_ = {};
                    continue;
                }
                if (failure == WSAEMSGSIZE) continue;
                close();
                port_ = port;
                error_ = failure;
                nextOpenMs_ = now + 1000;
                return false;
            }
            Sample value;
            if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
                peer.sin_port != htons(port) || !decode(bytes.data(), static_cast<std::size_t>(size), value))
                continue;
            latest_ = value;
            have_ = true;
        }
        if (!have_) return false;
        result = latest_;
        return true;
    }

private:
    void send(const char* message, int size) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(port_);
        // A lost request is repeated by the next renewal.
        sendto(socket_, message, size, 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    bool openSocket() {
        WSADATA data{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup) {
            error_ = startup;
            return false;
        }
        started_ = true;
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long nonblocking = 1;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (socket_ == INVALID_SOCKET || ioctlsocket(socket_, FIONBIO, &nonblocking) != 0 ||
            bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            const auto failure = WSAGetLastError();
            const auto port = port_;
            close();
            port_ = port;
            error_ = failure;
            return false;
        }
        error_ = 0;
        nextRenewMs_ = 0;
        return true;
    }
    SOCKET socket_ = INVALID_SOCKET;
    bool started_ = false, have_ = false;
    std::uint16_t port_ = 0;
    unsigned error_ = 0;
    ULONGLONG nextOpenMs_ = 0, nextRenewMs_ = 0;
    Sample latest_;
};
} // namespace dvr::gaze
