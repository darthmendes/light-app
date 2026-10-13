#include "websocket_server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64(const std::uint8_t* bytes, std::size_t length) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    for (std::size_t index = 0; index < length; index += 3) {
        const std::uint32_t first = bytes[index];
        const std::uint32_t second = index + 1 < length ? bytes[index + 1] : 0;
        const std::uint32_t third = index + 2 < length ? bytes[index + 2] : 0;
        const std::uint32_t value = (first << 16) | (second << 8) | third;
        result.push_back(alphabet[(value >> 18) & 0x3f]);
        result.push_back(alphabet[(value >> 12) & 0x3f]);
        result.push_back(index + 1 < length ? alphabet[(value >> 6) & 0x3f] : '=');
        result.push_back(index + 2 < length ? alphabet[value & 0x3f] : '=');
    }
    return result;
}

std::string acceptKey(const std::string& key) {
    const std::string source = key + kWebSocketGuid;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Could not initialize WebSocket SHA-1 handshake");
    }

    std::vector<UCHAR> object(objectLength);
    std::array<UCHAR, 20> digest{};
    const bool hashOk = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(source.data())),
            static_cast<ULONG>(source.size()), 0) >= 0 &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashOk) throw std::runtime_error("Could not calculate WebSocket handshake hash");
    return base64(digest.data(), digest.size());
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool sendAll(SOCKET client, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const int count = ::send(client, data + sent, static_cast<int>(length - sent), 0);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool sendFrame(SOCKET client, std::uint8_t opcode, const std::string& payload) {
    std::vector<char> frame;
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        return false;
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return sendAll(client, frame.data(), frame.size());
}

std::string ambientMessage(const AmbientColors& colors) {
    const auto edgeJson = [](const EdgeColors& edge) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& color : edge) {
            result.push_back({{"r", color[0]}, {"g", color[1]}, {"b", color[2]}});
        }
        return result;
    };
    return nlohmann::json{
        {"type", "ambient_colors"},
        {"colors", {
            {"left", edgeJson(colors.left)}, {"right", edgeJson(colors.right)},
            {"top", edgeJson(colors.top)}, {"bottom", edgeJson(colors.bottom)}
        }}
    }.dump();
}

bool handshake(SOCKET client) {
    std::string request;
    std::array<char, 2048> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const int count = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0) return false;
        request.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto keyStart = lower(request).find("sec-websocket-key:");
    if (keyStart == std::string::npos) return false;
    const auto valueStart = request.find(':', keyStart) + 1;
    const auto valueEnd = request.find("\r\n", valueStart);
    const std::string key = request.substr(valueStart, valueEnd - valueStart);
    const auto first = key.find_first_not_of(" \t");
    const auto last = key.find_last_not_of(" \t");
    if (first == std::string::npos) return false;
    const std::string accept = acceptKey(key.substr(first, last - first + 1));
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return sendAll(client, response.data(), response.size());
}

bool consumeFrames(SOCKET client, std::vector<std::uint8_t>& pending) {
    while (pending.size() >= 2) {
        const std::uint8_t first = pending[0];
        const std::uint8_t second = pending[1];
        const std::uint8_t opcode = first & 0x0f;
        const bool masked = (second & 0x80) != 0;
        std::uint64_t payloadLength = second & 0x7f;
        std::size_t headerLength = 2;
        if (payloadLength == 126) {
            if (pending.size() < 4) return true;
            payloadLength = (static_cast<std::uint64_t>(pending[2]) << 8) | pending[3];
            headerLength = 4;
        } else if (payloadLength == 127) {
            if (pending.size() < 10) return true;
            payloadLength = 0;
            for (std::size_t index = 2; index < 10; ++index) payloadLength = (payloadLength << 8) | pending[index];
            headerLength = 10;
        }
        if (!masked || payloadLength > 65536) return false;
        const std::size_t maskOffset = headerLength;
        headerLength += 4;
        if (pending.size() < headerLength + payloadLength) return true;
        std::string payload(static_cast<std::size_t>(payloadLength), '\0');
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<char>(pending[headerLength + index] ^ pending[maskOffset + index % 4]);
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(headerLength + payloadLength));
        if (opcode == 0x8) return false;
        if (opcode == 0x9 && !sendFrame(client, 0xA, payload)) return false;
        if (opcode == 0x1) {
            try {
                const auto message = nlohmann::json::parse(payload);
                if (message.value("type", std::string{}) == "ping" && !sendFrame(client, 0x1, R"({"type":"pong"})")) {
                    return false;
                }
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    return true;
}

void serveClient(SOCKET client, std::shared_ptr<LatestFrame> latestFrame) {
    DWORD sendTimeoutMs = 100;
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs));
    if (!handshake(client)) {
        closesocket(client);
        return;
    }
    std::cout << "WebSocket client connected\n";
    std::vector<std::uint8_t> pending;
    std::uint64_t sentSequence = 0;
    bool connected = true;
    std::array<char, 4096> input{};

    while (connected) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(client, &readable);
        timeval wait{0, 10000};
        const int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == SOCKET_ERROR) break;
        if (ready > 0 && FD_ISSET(client, &readable)) {
            const int count = recv(client, input.data(), static_cast<int>(input.size()), 0);
            if (count <= 0) break;
            pending.insert(pending.end(), input.begin(), input.begin() + count);
            connected = consumeFrames(client, pending);
        }

        if (!connected) break;
        AmbientColors colors;
        std::uint64_t sequence = 0;
        {
            std::lock_guard lock(latestFrame->mutex);
            sequence = latestFrame->sequence;
            if (sequence != sentSequence) colors = latestFrame->colors;
        }
        if (sequence != 0 && sequence != sentSequence) {
            const auto message = ambientMessage(colors);
            if (!sendFrame(client, 0x1, message)) break;
            sentSequence = sequence;
        }
    }
    std::cout << "WebSocket client disconnected\n";
    shutdown(client, SD_BOTH);
    closesocket(client);
}
}

WebSocketServer::WebSocketServer(const AmbientConfig& config, std::shared_ptr<LatestFrame> latestFrame)
    : config_(config), latestFrame_(std::move(latestFrame)) {}

void WebSocketServer::run() {
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) throw std::runtime_error("Could not create WebSocket listener");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(config_.websocketPort));
    if (InetPtonA(AF_INET, config_.websocketHost.c_str(), &address.sin_addr) != 1 ||
        bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener);
        throw std::runtime_error("Could not bind WebSocket listener to " + config_.websocketHost + ":" +
            std::to_string(config_.websocketPort));
    }

    std::cout << "Ambient WebSocket server listening on ws://" << config_.websocketHost << ':'
              << config_.websocketPort << '\n';
    while (true) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread(serveClient, client, latestFrame_).detach();
    }
}