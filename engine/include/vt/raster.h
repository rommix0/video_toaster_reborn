#pragma once
// The engine's picture: one 768x482 frame of 8-bit RGBA, row-major.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vt {

inline constexpr int kRasterW = 768, kRasterH = 482;

struct Rect {
    int x, y, w, h;
    bool operator==(const Rect&) const = default;
};

// The part of the raster shown by default (spec 5.6); underscan shows the whole raster.
// This is the measured clean area of the DVE control data: cells 12..171 (pixels 48..687) and
// control lines 13..228 (raster lines 26..457). The Switcher's control-display copper lists
// (DIWSTRT 0x155C / DIWSTOP 0x06CA, DDFSTRT 0x20 / DDFSTOP 0xD4, e.g. sw_m4c2 at 0x33524) do not
// blank to this area, and no Toaster blanking setting was found (tools/re/FINDINGS.md, "DVE").
inline constexpr Rect kVisibleArea{48, 26, 640, 432};

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
