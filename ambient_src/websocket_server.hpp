#pragma once

#include "ambient_config.hpp"
#include "ambient_types.hpp"

#include <memory>

class WebSocketServer {
public:
    WebSocketServer(const AmbientConfig& config, std::shared_ptr<LatestFrame> latestFrame);
    void run();

private:
    AmbientConfig config_;
    std::shared_ptr<LatestFrame> latestFrame_;
};