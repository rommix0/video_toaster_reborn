#include "vt/key.h"

#include <algorithm>

namespace vt {
namespace {

// (a * (255 - l) + b * l + 127) / 255, as effect_detail.h's mix255 (which is not public).
uint8_t mix255(int a, int b, int l) { return uint8_t((a * (255 - l) + b * l + 127) / 255); }

}  // namespace

KeyMove keyMoveFor(uint32_t d) {
    switch (d) {
        case 2: case 17: return KeyMove::FliesOn;
        case 3: case 18: return KeyMove::FliesOff;
        case 5: case 6: case 19: return KeyMove::TwoPhase;
        default: return KeyMove::Drops;
    }
}

bool keyModeLocked(uint32_t d) { return keyMoveFor(d) == KeyMove::Drops || d == 5 || d == 19; }

bool keyLevelLocked(uint32_t d) { return keyMoveFor(d) == KeyMove::Drops; }

uint8_t keyLuma(Rgba8 p) { return uint8_t((77 * p.r + 150 * p.g + 29 * p.b + 128) >> 8); }

int keyClip(bool black, int level) {
    const int l = std::clamp(level, 1, 256);
    return (black ? l : 257 - l) - 1;
}

void lumaKey(const Frame& fill, const Frame& background, const Frame& signal, int v, bool black,
             int softness, Frame& out) {
    const int s = std::clamp(softness, 0, 255);
    out.px.resize(fill.px.size());
    for (size_t i = 0; i < fill.px.size(); ++i) {
        const int y = keyLuma(signal.px[i]);
        const int d = black ? y - v : v - y;
        const int w = s == 0 ? (d >= 0 ? 255 : 0) : std::clamp(((2 * d + s) * 255) / (2 * s), 0, 255);
        const Rgba8 b = background.px[i], f = fill.px[i];
        out.px[i] = {mix255(b.r, f.r, w), mix255(b.g, f.g, w), mix255(b.b, f.b, w), 255};
    }
}

}  // namespace vt
