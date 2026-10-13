#include "udp_sender.hpp"

#include <array>
#include <iostream>

UdpSender::UdpSender(const AmbientConfig& config) {
    if (!config.udpEnabled) return;

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        std::cerr << "Could not create UDP socket.\n";
        return;
    }

    destination_.sin_family = AF_INET;
    destination_.sin_port = htons(static_cast<u_short>(config.udpPort));
    if (InetPtonA(AF_INET, config.udpHost.c_str(), &destination_.sin_addr) != 1) {
        std::cerr << "Invalid udp_host IPv4 address: " << config.udpHost << '\n';
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return;
    }

    enabled_ = true;
    std::cout << "UDP RGB streaming enabled for " << config.udpHost << ':' << config.udpPort
              << " (360-byte RGB packets)\n";
}

UdpSender::~UdpSender() {
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
}

bool UdpSender::enabled() const {
    return enabled_;
}

void UdpSender::send(const AmbientColors& colors) const {
    if (!enabled_) return;

    std::array<char, kLedCount * 3> packet{};
    std::size_t offset = 0;
    const auto append = [&packet, &offset](const EdgeColors& edge, bool reverse) {
        for (std::size_t position = 0; position < kLedsPerEdge; ++position) {
            const auto& color = edge[reverse ? kLedsPerEdge - position - 1 : position];
            for (const auto channel : color) packet[offset++] = static_cast<char>(channel);
        }
    };

    append(colors.top, false);
    append(colors.right, false);
    append(colors.bottom, true);
    append(colors.left, true);
    sendto(socket_, packet.data(), static_cast<int>(packet.size()), 0,
        reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
}