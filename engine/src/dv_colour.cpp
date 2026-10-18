#include "vt/dv_colour.h"

#include <algorithm>
#include <cstdint>

namespace vt {
namespace {

// adj(M) of the RGBYIQ matrix M = {{0x26df, 0x4c51, 0x0ed2}, {0x4d7c, -0x23c1, -0x29bb},
// {0x1b90, -0x43ff, 0x286f}} (det -9342313446888). RGB = M^-1 (Y - 71, I, Q) * 65536 / s with
// s = 129 * 65536 / (255 * 33282), the scale that maps DV 71..200 onto 0..255. With the input
// given as 4 (Y - 71), 4 I, 4 Q this is adj . v * 255 * 33282 / (4 * 129 * det); reduced by the
// common factor 99846 it is adj . v * -85 / 48280689648.
constexpr int64_t kAdj[3][3] = {{-280701684, -268269645, -173987289},
                                {-280701684, 76232337, 181564317},
                                {-280701684, 311070129, -478617435}};
constexpr int64_t kNum = -85;
constexpr int64_t kDen = 48280689648;

int64_t floorDiv(int64_t n, int64_t d) {  // d > 0
    const int64_t q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

uint8_t channel(const int64_t (&row)[3], int64_t y4, int64_t i4, int64_t q4) {
    const int64_t n = (row[0] * y4 + row[1] * i4 + row[2] * q4) * kNum;
    const int64_t v = floorDiv(2 * n + kDen, 2 * kDen);  // round half up
    return uint8_t(std::clamp<int64_t>(v, 0, 255));
}

Rgba8 fromYiq4(int64_t y4, int64_t i4, int64_t q4) {
    return {channel(kAdj[0], y4, i4, q4), channel(kAdj[1], y4, i4, q4), channel(kAdj[2], y4, i4, q4), 255};
}

uint8_t clampQuad(int v) {  // YIQQUAD: byte arithmetic, unsigned compare against 0xdd and 0x20
    const auto b = uint8_t(v);
    return b >= 0xdd ? uint8_t(0xdd) : (b < 0x20 ? uint8_t(0x20) : b);
}

}  // namespace

DvYiq rgbToDvYiq(Rgb c) {
    const int32_t r = c.r, g = c.g, b = c.b;
    const int y = int(((r * 0x26df + g * 0x4c51 + b * 0x0ed2 + 0x470000) >> 16) & 0xff);
    int32_t i = (r * 0x4d7c - g * 0x23c1 - b * 0x29bb) >> 8;  // asr.l #8
    i = i >= 0 ? i + 0x80 : i - 0x80;                         // bpl: addi.w / subi.w #$80
    const auto ib = int8_t(uint8_t(uint16_t(i) >> 8));        // clr.b: the word's high byte
    int32_t q = r * 0x1b90 - g * 0x43ff + b * 0x286f;
    q = q >= 0 ? q + 0x8000 : q - 0x8000;
    const auto qb = int8_t(uint8_t(uint32_t(q) >> 16));       // swap, andi.w #$ff
    return {y, ib, qb};
}

DvQuad dvYiqToQuad(DvYiq c) {
    return {clampQuad(c.y - c.i), clampQuad(c.y - c.q), clampQuad(c.y + c.i), clampQuad(c.y + c.q)};
}

Rgba8 dvYiqToRgb(DvYiq c) { return fromYiq4(4 * (int64_t(c.y) - 71), 4 * int64_t(c.i), 4 * int64_t(c.q)); }

Rgba8 dvQuadToRgb(const DvQuad& q) {
    const int64_t a = q[0], b = q[1], c = q[2], d = q[3];  // Y-I, Y-Q, Y+I, Y+Q
    return fromYiq4(a + b + c + d - 284, 2 * (c - a), 2 * (d - b));
}

}  // namespace vt
