#pragma once
// The Toaster's digital colour space (DV memory). ChromaFX's RGBYIQ (0x4938) turns RGB into
// Y, I, Q bytes (DV black 71, white 200) and YIQQUAD (0x49c2) into a "quad": one colour
// subcarrier cycle of composite samples. switcher.main's matte table (0x6d88) holds the same
// YIQ. See tools/re/FINDINGS.md ("Background colour generator", "ChromaFX row builder").
#include <array>
#include <cstdint>

#include "vt/ilbm.h"
#include "vt/raster.h"

namespace vt {

// Y 0..255 (DV black 71, white 200); I and Q signed (-128..127).
struct DvYiq {
    int y, i, q;
};

// Y-I, Y-Q, Y+I, Y+Q, each clamped to 0x20..0xdd.
using DvQuad = std::array<uint8_t, 4>;

// RGBYIQ, bit-exact: Y = (0x26df R + 0x4c51 G + 0x0ed2 B) >> 16 + 71; I and Q are the
// 0x4d7c/-0x23c1/-0x29bb and 0x1b90/-0x43ff/0x286f rows rounded away from zero.
DvYiq rgbToDvYiq(Rgb);
// YIQQUAD, bit-exact (byte arithmetic, unsigned clamp to 0x20..0xdd).
DvQuad dvYiqToQuad(DvYiq);
inline DvQuad rgbToDvQuad(Rgb c) { return dvYiqToQuad(rgbToDvYiq(c)); }

// The inverse of the RGBYIQ matrix, scaled so that its Y row maps DV 71..200 exactly onto
// 0..255; exact integer arithmetic, rounded half up, clamped. Opaque.
Rgba8 dvYiqToRgb(DvYiq);
// A quad decoded as composite video: Y = mean of the four samples, I = ((Y+I) - (Y-I)) / 2,
// Q = ((Y+Q) - (Y-Q)) / 2, then as dvYiqToRgb (no rounding before the matrix).
Rgba8 dvQuadToRgb(const DvQuad&);

}  // namespace vt
