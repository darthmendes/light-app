#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <mutex>

inline constexpr std::size_t kLedsPerEdge = 30;
inline constexpr std::size_t kLedCount = kLedsPerEdge * 4;

using Rgb = std::array<std::uint8_t, 3>;
using EdgeColors = std::array<Rgb, kLedsPerEdge>;

struct AmbientColors {
    EdgeColors top{};
    EdgeColors right{};
    EdgeColors bottom{};
    EdgeColors left{};
};

struct LatestFrame {
    std::mutex mutex;
    AmbientColors colors{};
    std::uint64_t sequence = 0;
};