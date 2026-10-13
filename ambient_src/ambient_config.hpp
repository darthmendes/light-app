#pragma once

#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

struct AmbientConfig {
    int fps = 60;
    double edgeThicknessPercent = 0.08;
    double edgeInsetPercent = 0.10;
    double smoothingAlpha = 0.35;
    std::array<double, 3> gains{1.0, 1.0, 1.0};
    std::string websocketHost = "127.0.0.1";
    int websocketPort = 8765;
    bool udpEnabled = false;
    std::string udpHost = "192.168.4.1";
    int udpPort = 4210;

    static AmbientConfig load(const std::string& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot open config: " + path);

        const auto json = nlohmann::json::parse(input);
        AmbientConfig config;
        config.fps = json.value("fps", config.fps);
        config.edgeThicknessPercent = json.value("edge_thickness_percent", config.edgeThicknessPercent);
        config.edgeInsetPercent = json.value("edge_inset_percent", config.edgeInsetPercent);
        config.smoothingAlpha = json.value("smoothing_alpha", config.smoothingAlpha);
        config.websocketHost = json.value("websocket_host", config.websocketHost);
        config.websocketPort = json.value("websocket_port", config.websocketPort);
        config.udpEnabled = json.value("udp_enabled", config.udpEnabled);
        config.udpHost = json.value("udp_host", config.udpHost);
        config.udpPort = json.value("udp_port", config.udpPort);

        if (json.contains("color_gains")) {
            const auto& values = json.at("color_gains");
            if (!values.is_array() || values.size() != 3) {
                throw std::runtime_error("color_gains must contain exactly three numbers");
            }
            for (std::size_t channel = 0; channel < 3; ++channel) {
                config.gains[channel] = values.at(channel).get<double>();
            }
        }

        if (config.fps < 1 || config.fps > 240 ||
            config.edgeThicknessPercent <= 0.0 || config.edgeThicknessPercent > 0.5 ||
            config.edgeInsetPercent < 0.0 || config.edgeInsetPercent >= 0.5 ||
            config.smoothingAlpha < 0.0 || config.smoothingAlpha > 1.0 ||
            config.websocketPort < 1 || config.websocketPort > 65535 ||
            config.udpPort < 1 || config.udpPort > 65535) {
            throw std::runtime_error("Config values are outside supported ranges");
        }
        for (const double gain : config.gains) {
            if (!std::isfinite(gain) || gain < 0.0 || gain > 8.0) {
                throw std::runtime_error("color_gains values must be between 0 and 8");
            }
        }
        return config;
    }
};