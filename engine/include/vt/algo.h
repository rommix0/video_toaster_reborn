#pragma once
// ALGO croutons: the Switcher's animation VM (switcher.main 4.2), a literal port of
// tools/ref/algo_ref.py (verified bit-exact against the 68000 code by algo_check.py). Sections
// cited as "ALGO.md N" are in tools/re/ALGO.md.
//
// Payload (tag 0x1000001C, ALGO.md 4): long progOff, long flags, word nChannels (not in kind 2),
// then { word channel; { word opcode; params } ... word 0 } ... word 0. Slot k of a channel
// writes the channel's word at byte offset 2k. Each step evaluates the slot animators at t
// (16.16, 0..1), then draws:
//   kinds 0 / 3 (digital, ALGO.md 5)  the H cell row (channels 1..n), the V line table (channel 10);
//   kinds 1 / 4 (wipes, ALGO.md 6)    one span per channel 13.., the band of channel 22;
//   kind 2 (level fades, ALGO.md 8)   channel 23 only.
// Kinds 3 / 4 play as 0 / 1 (interactive positioning is out of scope).
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "vt/background.h"
#include "vt/dve.h"
#include "vt/raster.h"

namespace vt {

enum class AlgoKind { Digital = 0, Wipe = 1, Fade = 2, PosDigital = 3, PosWipe = 4, ArtCardKey = 5 };

struct AlgoStep {
    // digital (kinds 0, 3)
    std::array<uint8_t, 0x170> h{};  // H row; cells 0..191 used
    std::array<uint8_t, 0xf4> v{};   // V table; control lines 0..240 used
    int cellLo = 0, cellHi = 0;      // clip range of channels 1..n (step()['cells'], Ruling 4)
    int lineLo = -1, lineHi = -1;    // -1: no line shows the row
    // wipes (kinds 1, 4)
    std::vector<std::pair<int, int>> spans;  // per channel ($14, $16); -1 = none
    int pixelLo = 0, pixelHi = 0, ink = 0, bandLo = -1, bandHi = -1, bandInk = 0;
    bool inverse = false;            // flag 0x80 (ALGO.md 6)
    // all kinds
    int level = -1;                  // channel 23 word 0 with flag 0x10, else -1
};

class AlgoVm {
public:
    // payload = tag 0x1000001C bytes, kind = tag 0x80000063 (0..4), colour = digital Effect
    // Color index 0..7 (ALGO.md 15.5). Throws std::runtime_error for kind 5, a malformed
    // payload, opcodes 15/16/18+, or flag 0x80 on a digital kind.
    AlgoVm(std::vector<uint8_t> payload, int kind, int colour);
    uint32_t flags() const { return flags_; }
    AlgoKind kind() const { return kind_; }
    // t in 16.16, 0..0x10000 (larger values act as 0x10000); state carries over (ALGO.md 15.1).
    // Throws std::runtime_error if the program indexes past a table (no shipped one does).
    const AlgoStep& step(uint32_t t);

private:
    struct Slot {
        int off, op;
        size_t prm, len;  // parameter bytes in payload_
    };
    void install();
    void eval(int n, uint32_t t);
    void digital(uint32_t t);
    void wipe(uint32_t t);

    std::vector<uint8_t> payload_;
    AlgoKind kind_;
    int render_;  // 0 digital, 1 wipe, 2 level fade
    uint32_t flags_;
    int nch_;
    std::array<std::array<uint8_t, 0x36>, 26> ch_{};  // channel word blocks 1..25 (ALGO.md 5.1, 6)
    std::array<std::vector<Slot>, 26> anim_;
    AlgoStep step_;  // h / v are the persistent H and V buffers
};

uint32_t algoTBarT(int pos, uint32_t flags);       // tbar_t; pos clamped to 0..511
uint32_t algoAutoT(int k, int n, uint32_t flags);  // auto_ts(n, flags)[k - 1]; k clamped to 1..n
// The routing walker's step for these effects (ALGO.md 15.2): T-bar >= 256, or in Auto the
// unfolded k * floor((flags & 8 ? 0x20000 : 0x10000) / n) >= (flags & 8 ? 0x10000 : 0x8000).
bool algoSecondStep(int tbar, int autoStep, int n, uint32_t flags);

// --- pictures from steps ---------------------------------------------------------------------

// ALGO.md 5.3: control line k shows the H row only when lineLo >= 0 and lineLo <= k <= lineHi, in
// cells cellLo..cellHi (the clip range, Ruling 4): key = 255 - h[c]. Every other key is 0.
// line[k] = v[k] raw for k 0..240 (0xfc = kDveColourLine and other values above 240 included).
// art stays empty; borderFill keeps its default (the caller sets it from algoBorderFill).
DveControl algoControl(const AlgoStep&);
// ALGO.md 15.5: border fill code 23 - colour index for 0..7; 23 (Black) otherwise.
int algoBorderFill(int colourIndex);
// Effect Color preset Black..Cyan -> 0..7; a custom word or Snow -> 0 (Black).
int algoColourIndex(const EffectColour&);
// ALGO.md 6: one bit per pixel picks B (1) or A (0). Span row at raster x: 1 inside a span with
// ink 1, 0 inside a span with ink 0, else ink ^ 1 within pixelLo..pixelHi, else 0. Raster line y
// takes the span row inside the band (bandLo >= 0, bandLo <= y <= bandHi) and the constant row
// bandInk outside (row 0 all 1, row 1 all 0); flag 0x80 (step.inverse) swaps them: the constant
// row bandInk ^ 1 inside the band, the span row outside. GUESS: the polarity (1 = B) is not
// confirmed against the hardware.
void renderAlgoWipe(const AlgoStep&, const Frame& a, const Frame& b, Frame& out);
// Per channel (in0 * (255 - L) + in1 * L + 127) / 255, L clamped to 0..255; alpha 255. Works
// pixel by pixel, so `out` may alias `in0` or `in1` (Ruling 1).
void mixLevel(const Frame& in0, const Frame& in1, int level, Frame& out);

}  // namespace vt
