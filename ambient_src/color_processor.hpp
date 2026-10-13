#pragma once

#include "ambient_config.hpp"
#include "ambient_types.hpp"

#include <cstdint>

class ColorProcessor {
public:
    explicit ColorProcessor(const AmbientConfig& config);
    AmbientColors process(const std::uint8_t* pixels, int width, int height, int rowPitch);

private:
    Rgb sample(const std::uint8_t* pixels, int width, int height, int rowPitch,
               int x0, int y0, int x1, int y1) const;
    Rgb correctAndSmooth(const Rgb& color, std::size_t index);

    AmbientConfig config_;
    AmbientColors previous_{};
    bool hasPrevious_ = false;
};