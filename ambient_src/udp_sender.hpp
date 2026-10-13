#pragma once

#include "ambient_config.hpp"
#include "ambient_types.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

class UdpSender {
public:
    explicit UdpSender(const AmbientConfig& config);
    ~UdpSender();
    bool enabled() const;
    void send(const AmbientColors& colors) const;

private:
    bool enabled_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in destination_{};
};