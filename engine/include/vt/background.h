#pragma once
// The Toaster's background (BKG/matte) colour generator: tag 0x80000023 picks one of these
// (1-based), tag 0x80000024 is a custom hardware word used when 0x23 is negative.
// Values come from switcher.main; see tools/re/FINDINGS.md "Background colour generator".
#include <cstdint>
#include <optional>

#include "vt/raster.h"

namespace vt {

enum class BkgColour { Black = 1, White, Red, Green, Blue, Yellow, Magenta, Cyan, Snow };

// Solid colour for Black..Cyan (Snow is noise: use renderSnow). Decoded from the Switcher's
// DV YIQ table at 0x6d88: NTSC 75 % bars with a 100 % white. Opaque.
Rgba8 bkgColour(BkgColour);

// Tag 0x80000024. In the Switcher this is a raw hardware word (two level nibbles and a hue
// nibble), not RGB. The eight colour preset words (0x99000 + $4c; the ninth button, Snow, is
// bit 15) and the "super white" 0x0fff map to their colours exactly. Any other word has no RGB
// defined in the program; it is read as 12-bit 0x0RGB (x17), which is an approximation. Bit 15
// (snow) is the caller's to check.
Rgba8 customBkgColour(uint32_t tag0x24);

// Snow: the hardware's noise source. Grey per-pixel noise, deterministic per frame number.
void renderSnow(Frame&, uint32_t frameNumber);

// One setting of the colour generator: a preset (Snow included) or a custom generator word
// (tag 0x80000024; bit 15 = Snow). Each effect has one as its own matte colour, the manual's
// "Effect Color" (PDF 178, 181; Ruling 26); the Switcher's BKG source has another.
struct EffectColour {
    bool custom = false;
    BkgColour preset = BkgColour::Black;  // when !custom
    uint32_t word = 0;                    // when custom: the hardware word (customBkgColour)

    static EffectColour of(BkgColour c) { return {false, c, 0}; }
    static EffectColour customWord(uint32_t w) { return {true, BkgColour::Black, w}; }
    bool isSnow() const { return custom ? (word & 0x8000u) != 0 : preset == BkgColour::Snow; }
    // The solid colour: bkgColour(preset) or customBkgColour(word); black for Snow (noise has no
    // single colour; effects get black, as for a Snow BKG).
    Rgba8 rgb() const;
    bool operator==(const EffectColour&) const = default;
};

// An effect's own colour from its crouton tags, as switcher.main sets the generator when the
// effect plays (0xe723e..0xe72be; tools/re/FINDINGS.md "Which colour an effect's matte shows"):
//  * tag 0x80000024 present: that custom word (its tag handler 0xe6db4 sets 0x23 to -1);
//  * tag 0x80000023, read as a signed byte (ext.w): 1..9 picks that preset; negative means the
//    custom word in tag 0x24 (missing: 0, black);
//  * no tag 0x23 (and no 0x24), or 0x23 = 10..127: nullopt. The Switcher leaves the generator
//    as it is, so the matte shows the BKG source's colour (the handler 0xe6dac that arms the
//    colour change only runs for a crouton that has the tag; 0xe727c skips values above 9).
// GUESS: 0x23 = 0 gives Black. The Switcher uses the last Effect Color loaded or picked
// ($332(a5), 0xe69a0), which vtoast_reborn does not model; no shipped crouton has 0.
std::optional<EffectColour> effectColourFromTags(std::optional<uint32_t> tag23, std::optional<uint32_t> tag24);

}  // namespace vt
