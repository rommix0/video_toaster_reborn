#pragma once
// The engine's picture: one 768x482 frame of 8-bit RGBA, row-major.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vt {

inline constexpr int kRasterW = 768, kRasterH = 482;

struct Rgba8 {
    uint8_t r, g, b, a;
    bool operator==(const Rgba8&) const = default;
};

struct Frame {
    std::vector<Rgba8> px = std::vector<Rgba8>(size_t(kRasterW) * kRasterH);
    Rgba8& at(int x, int y) { return px[size_t(y) * kRasterW + size_t(x)]; }
    const Rgba8& at(int x, int y) const { return px[size_t(y) * kRasterW + size_t(x)]; }
};

}  // namespace vt
