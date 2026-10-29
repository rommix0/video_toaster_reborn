#pragma once
// DVE moves (the "hardware ramp" ANIMs): control frames that tell the Toaster's DVE which source
// pixel to show where (port of tools/toaster_dve.py decode/render and toaster_fx_convert.write_dve).
//
// Control frame: 768 pixels x 241 lines (one field), bit planes with rowbytes 96.
//   planes 0+1  per 4-pixel cell an 8-bit value (plane 1 nibble high, plane 0 nibble low),
//               192 cells per line.
//   plane 2     rows 0..10: per-line source-line table after a 2-word header; per line two
//               big-endian words, line = (second word >> 12) << 4 | (first word >> 12). This is
//               Amiga sprite 1 data (header = SPR1POS 152D, SPR1CTL 0683, attached; the Switcher
//               keeps only the words' top nibbles and fills the rest with its register stream,
//               switcher.main 0x2b5e8), so the table is read from the LAST plane that starts with
//               the signature: in the "!" effects plane 2 is a picture plane that merely repeats
//               the sprite's row 0, and the sprite is plane 4, 5, 7 or 8.
//   planes 3..  (8/9-plane "!" effects) an artwork layer: palette group per pixel, built from
//               the planes that do not start with the 15 2D 06 83 signature (the i-th such plane
//               gives bit i); any further plane that starts with it is control and skipped.
// The data is used RAW: the stale overscan margins are not filled (spec 5.6). The engine shows
// kVisibleArea by default; the margins appear only with underscan.
#include <array>
#include <cstdint>
#include <vector>

#include "vt/anim.h"
#include "vt/hardware_guesses.h"
#include "vt/ilbm.h"
#include "vt/raster.h"

namespace vt {

inline constexpr int kDveLines = 241, kDveCells = 192, kDveWidth = 768;
// ALGO.md 15.5: a control line value of 0xfc (above 240) is not a source line; it marks the border
// colour (DveControl::borderFill). decodeDve never produces it; algoControl copies the V table raw.
inline constexpr uint8_t kDveColourLine = 0xfc;

struct DveControl {
    std::array<uint8_t, kDveLines * kDveCells> key{};  // cell value per control line
    std::array<uint8_t, kDveLines> line{};              // source line per control line, 0..240
    std::vector<uint8_t> art;                           // 241 x 768 palette groups (line-major); empty if none
    // 241 x 768 display-bitmap palette index (line-major): every plane except the line table's
    // sprite plane, bit i from the i-th of them (FREEZE.md section 1 item 6). Filled only when the
    // frame has more than 3 planes; empty otherwise.
    std::vector<uint8_t> index;
    uint8_t borderFill = 23;  // fill code (16..23) drawn where a moving cell's line is kDveColourLine
};

// DVE content: >= 3 planes, at least one row, rowbytes 96, plane 2 row 0 starts 15 2D 06 83.
bool isDve(const Planes&);

// One control frame, raw (no overscan fill; the line table clipped to 0..240). Throws
// std::runtime_error unless isDve and h == 241.
DveControl decodeDve(const Planes&);

// Which clip moves (port of write_dve; coverage = fraction of key cells >= 64):
//   first and last frame coverage > 0.9: Flip if the minimum coverage over all frames is < 0.5
//     (A until the first minimum-coverage frame, B from it, over black; switchFrame = that
//     animation frame index, reltimes ignored), else DistortA (A moved over A, the Glitch effects);
//   else first >= last: MovingA (A over B, "Out" moves); else MovingB (B over A, "In" moves).
// Coverage for this decision only is taken with the converter's overscan fill
// (toaster_dve.coverage), so roles and switch frames match the converter exactly; with the raw
// data "Infinite Video Space !" would switch at frame 31 instead of 30 (stale margin cells).
enum class DveRole { MovingA, MovingB, Flip, DistortA };
struct DveInfo {
    DveRole role = DveRole::MovingA;
    int switchFrame = -1;  // Flip only, else -1
};
DveInfo dveInfo(const Anim&);  // uses Anim::planes (the 9-plane effect has no index frames)

// Tag 0x1000001B for one frame of a "!" DVE (the layout of vt::paletteRoutes' comment, row 0 only;
// FREEZE.md 2): per palette index of the display bitmap, source 0 = none (the art rule), 1 and 3
// = a bus (DveMix), 9 = the colour generator; level L 0..3. Entry i is merged with its CMAP
// entry as the Switcher does, (cmapBits & ~mask) | (e & mask), mask = masks[(e >> 28) & 7]: the
// source is bits 12-15 of e when the mask has all of 0x604, else none; L is the merged low two
// bits, with cmap[i].b standing for the CMAP's bits. Entries below the table's first index are
// not in the table: source 0, level cmap[i].b & 3. Empty when the tag is short.
struct DveRoute {
    uint8_t source = 0, level = 0;
};
std::vector<DveRoute> dveRoutes(const std::vector<uint8_t>& tag1b, const std::vector<Rgb>& cmap);

// The buses a route shows (Ruling 10, a GUESS): S = 1 the bus that does not move (analog; which
// one depends on the move, DveEffect, from the routing descriptor); S = 3 the moving bus's
// picture as the move moves it, which is the DVE picture itself, so an S = 3 route leaves the
// pixel as drawn whatever its level.
struct DveMix {
    const std::vector<DveRoute>* routes = nullptr;
    const Frame* s1 = nullptr;         // S = 1
    Rgba8 colour{0, 0, 0, 255};        // S = 9
    const Frame* generator = nullptr;  // S = 9 with a Snow generator
};

// Render one control frame. Per output pixel (x, y): k = y / 2 (control line, spec 5.10),
// p = y & 1, c = x / 4, v = key[k * 192 + c]:
//   v >= 64     out = moving.at(clamp(4 * (sc - dveSourceCellOffset) + inner, 0, 767), line[k] * 2 + p),
//               sc = 255 - v, inner = dveCellPixel < 0 ? x % 4 : dveCellPixel: one read, no
//               filtering (dveCellPixel above 3 acts as 3, line[k] above 240 as 240;
//               decodeDve never produces such lines). A line of kDveColourLine instead draws the
//               fill code borderFill (as 16..23 below).
//   v == 0      out = background.at(x, y).
//   1..15       shadow: background darkened by s = min(v, 7). With
//               a = floor(s * (255 * dveShadowOpacity) / 7) (opacity clamped to 0..1; the same
//               alpha toaster_dve.render gives its black overlay), each colour channel is
//               (bg * (255 - a) + 127) / 255 (integer, round half up); alpha stays the background's.
//   16..23      colour-bar fills (75 % = 191): blue, red, magenta, green, cyan, yellow, white, black.
//   24..63      unknown code: mid grey (128, 128, 128); counted in the return value.
//   art group g > 0 (when present): the art colour is cmap[ci], ci = the display index with its
//               key-stream bits (planes 0-1) cleared, index & 0xFC (cmap[g * 8] when
//               DveControl::index is empty); when ci is past the end of cmap the art pixel is not
//               drawn. Where v < 64 the art colour replaces the result. Over the moving picture
//               (v >= 64) the index's level L (routes[ci].level when the mix's routes cover ci, else
//               cmap[ci].b & 3) mixes the art toward it, w = dveLevelMix[L] as below: L = 0 the art,
//               L = 3 the picture (FREEZE.md 2; Infinite Video Space ! keeps its video under L = 3
//               art and draws its tiles with L = 0 over the background).
//   routed pixel (mix given, and DveControl::index non-empty): i = index at the pixel; when
//               routes[i].source is 1 or 9 the pixel is s = s1 / the generator (or colour) at (x, y)
//               mixed with d, the pixel as drawn above (before the art layer), by
//               w = dveLevelMix[L]: (s * (255 - w) + d * w + 127) / 255 per channel, alpha 255.
//               S = 3 is d itself (DveMix). The art rule does not apply to a routed pixel. A route
//               whose bus frame is null is not routed.
// Every output pixel gets alpha 255 except the moving/background/shadow cases, which keep the
// source pixel's alpha. Returns the number of output pixels drawn as unknown-code grey.
int renderDve(const DveControl&, const Frame& moving, const Frame& background, const std::vector<Rgb>& cmap,
              Frame& out, const HardwareGuesses&, const DveMix* mix = nullptr);

}  // namespace vt
