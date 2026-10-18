#pragma once
// The Toaster's background (BKG/matte) colour generator: tag 0x80000023 picks one of these
// (1-based), tag 0x80000024 is a custom hardware word used when 0x23 is negative.
// Values come from switcher.main; see tools/re/FINDINGS.md "Background colour generator".
#include <cstdint>

#include "vt/raster.h"

namespace vt {

enum class BkgColour { Black = 1, White, Red, Green, Blue, Yellow, Magenta, Cyan, Snow };

// Solid colour for Black..Cyan (Snow is noise: use renderSnow). Decoded from the Switcher's
// DV YIQ table at 0x6d88: NTSC 75 % bars with a 100 % white. Opaque.
Rgba8 bkgColour(BkgColour);

// Tag 0x80000024. In the Switcher this is a raw hardware word (two level nibbles and a hue
// nibble), not RGB. The nine preset words (0x99000 + $4c) and the "super white" 0x0fff map
// to their colours exactly. Any other word has no RGB defined in the program; it is read as
// 12-bit 0x0RGB (x17), which is an approximation. Bit 15 (snow) is the caller's to check.
Rgba8 customBkgColour(uint32_t tag0x24);

// Snow: the hardware's noise source. Grey per-pixel noise, deterministic per frame number.
void renderSnow(Frame&, uint32_t frameNumber);

}  // namespace vt
