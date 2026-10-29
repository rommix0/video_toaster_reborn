#pragma once
// The Switcher's luma key (tools/re KEY.md): the clip level -> clip value law, the per-pixel key,
// and which keyer controls an effect's routing descriptor locks.
#include <cstdint>

#include "vt/raster.h"

namespace vt {

enum class KeyMode { Off, Black, White };  // the lit button

// How an effect's routing descriptor moves the key (key_scan.py).
enum class KeyMove { Drops, FliesOn, FliesOff, TwoPhase };

// Clip level L runs 0..257: 0 shows the Superimpose source alone, 257 shows Main alone.
inline constexpr int kKeyLevelMax = 257;

// 2, 17 FliesOn; 3, 18 FliesOff; 5, 6, 19 TwoPhase; everything else (0, the unused values and
// unknown ones included) Drops.
KeyMove keyMoveFor(uint32_t descriptor);
// The effect holds the key mode: Drops, 5 and 19.
bool keyModeLocked(uint32_t descriptor);
// The effect holds the clip level: Drops.
bool keyLevelLocked(uint32_t descriptor);

// Y = (77 R + 150 G + 29 B + 128) >> 8.
uint8_t keyLuma(Rgba8);
// The clip value for level L (clamped to 1..256): (black ? L : 257 - L) - 1.
int keyClip(bool black, int level);

// Keys `fill` over `background` by the luma of `signal`. White: the fill shows where Y <= v; Black:
// where Y >= v. `softness` (clamped to 0..255) is the width in gray levels of the edge, centred on v;
// 0 is hard. Alpha 255. Pixel by pixel, so `out` may alias any input.
void lumaKey(const Frame& fill, const Frame& background, const Frame& signal, int v, bool black,
             int softness, Frame& out);

}  // namespace vt
