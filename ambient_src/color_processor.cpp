#include "color_processor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

ColorProcessor::ColorProcessor(const AmbientConfig& config) : config_(config) {}

Rgb ColorProcessor::sample(const std::uint8_t* pixels, int width, int height, int rowPitch,
                           int x0, int y0, int x1, int y1) const {
    x0 = std::clamp(x0, 0, width - 1);
    y0 = std::clamp(y0, 0, height - 1);
    x1 = std::clamp(x1, x0 + 1, width);
    y1 = std::clamp(y1, y0 + 1, height);

    std::array<std::uint64_t, 3> sums{};
    const auto pixelCount = static_cast<std::uint64_t>(x1 - x0) * (y1 - y0);
    for (int y = y0; y < y1; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * rowPitch;
        for (int x = x0; x < x1; ++x) {
            const auto* pixel = row + static_cast<std::size_t>(x) * 4;
            sums[0] += pixel[2];
            sums[1] += pixel[1];
            sums[2] += pixel[0];
        }
    }

    return {
        static_cast<std::uint8_t>(sums[0] / pixelCount),
        static_cast<std::uint8_t>(sums[1] / pixelCount),
        static_cast<std::uint8_t>(sums[2] / pixelCount)
    };
}

Rgb ColorProcessor::correctAndSmooth(const Rgb& color, std::size_t index) {
    Rgb result{};
    Rgb* previousColor = nullptr;
    if (index < kLedsPerEdge) previousColor = &previous_.top[index];
    else if (index < kLedsPerEdge * 2) previousColor = &previous_.right[index - kLedsPerEdge];
    else if (index < kLedsPerEdge * 3) previousColor = &previous_.bottom[index - kLedsPerEdge * 2];
    else previousColor = &previous_.left[index - kLedsPerEdge * 3];

    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto corrected = std::clamp(std::lround(color[channel] * config_.gains[channel]), 0L, 255L);
        const auto smoothed = hasPrevious_
            ? std::lround((*previousColor)[channel] * (1.0 - config_.smoothingAlpha) + corrected * config_.smoothingAlpha)
            : corrected;
        result[channel] = static_cast<std::uint8_t>(std::clamp(smoothed, 0L, 255L));
    }
    return result;
}

AmbientColors ColorProcessor::process(const std::uint8_t* pixels, int width, int height, int rowPitch) {
    AmbientColors colors;
    const int insetX = static_cast<int>(width * config_.edgeInsetPercent);
    const int insetY = static_cast<int>(height * config_.edgeInsetPercent);
    const int edgeWidth = std::max(1, static_cast<int>(width * config_.edgeThicknessPercent));
    const int edgeHeight = std::max(1, static_cast<int>(height * config_.edgeThicknessPercent));
    const int horizontalSpan = std::max(1, width - insetX * 2);
    const int verticalSpan = std::max(1, height - insetY * 2);

    for (std::size_t segment = 0; segment < kLedsPerEdge; ++segment) {
        const int topX0 = insetX + static_cast<int>(segment * horizontalSpan / kLedsPerEdge);
        const int topX1 = insetX + static_cast<int>((segment + 1) * horizontalSpan / kLedsPerEdge);
        const int sideY0 = insetY + static_cast<int>(segment * verticalSpan / kLedsPerEdge);
        const int sideY1 = insetY + static_cast<int>((segment + 1) * verticalSpan / kLedsPerEdge);

        colors.top[segment] = correctAndSmooth(sample(pixels, width, height, rowPitch,
            topX0, 0, std::max(topX0 + 1, topX1), edgeHeight), segment);
        colors.right[segment] = correctAndSmooth(sample(pixels, width, height, rowPitch,
            width - edgeWidth, sideY0, width, std::max(sideY0 + 1, sideY1)), kLedsPerEdge + segment);
        colors.bottom[segment] = correctAndSmooth(sample(pixels, width, height, rowPitch,
            topX0, height - edgeHeight, std::max(topX0 + 1, topX1), height), kLedsPerEdge * 2 + segment);
        colors.left[segment] = correctAndSmooth(sample(pixels, width, height, rowPitch,
            0, sideY0, edgeWidth, std::max(sideY0 + 1, sideY1)), kLedsPerEdge * 3 + segment);
    }

    previous_ = colors;
    hasPrevious_ = true;
    return colors;
}