#include <winsock2.h>
#include <windows.h>

#include "ambient_config.hpp"
#include "ambient_types.hpp"
#include "screen_capture.hpp"
#include "udp_sender.hpp"
#include "websocket_server.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) throw std::runtime_error("Cannot locate ambient.exe");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}
}

int main(int argc, char** argv) {
    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
        std::cerr << "Winsock initialization failed.\n";
        return 1;
    }

    try {
        const auto configPath = argc > 1
            ? std::filesystem::path(argv[1])
            : executableDirectory() / "ambient_config.json";
        const auto config = AmbientConfig::load(configPath.string());
        auto latestFrame = std::make_shared<LatestFrame>();
        UdpSender udp(config);
        ScreenCapture capture(config);
        std::thread captureThread([&capture, &latestFrame, &udp]() {
            capture.run([&latestFrame, &udp](const AmbientColors& colors) {
                {
                    std::lock_guard lock(latestFrame->mutex);
                    latestFrame->colors = colors;
                    ++latestFrame->sequence;
                }
                udp.send(colors);
            });
        });

        WebSocketServer server(config, latestFrame);
        server.run();
        captureThread.join();
    } catch (const std::exception& error) {
        std::cerr << "Ambient engine error: " << error.what() << '\n';
        WSACleanup();
        return 1;
    }

    WSACleanup();
    return 0;
}