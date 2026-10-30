#pragma once
// Values the Toaster's hardware or Switcher code decides but that are not (yet) known from the
// program. Each is a GUESS, kept here so it is visible, documented and easy to change in one place.
#include <array>
#include <cstdint>

namespace vt {

// How a CG key page pixel of alpha a (0..3) mixes over Main at dissolve level L (0..255), see
// HardwareGuesses::cgMixLaw and vt::cgMix.
enum class CgMixLaw {
    // 64 * code + (L & 63) with code = a + (L >> 6) - 3, clamped to 0..255; a = 0 is 0
    // (tools/re/CGKEY.md section 4 "Mix law", cgkey_ref.mix_guess). A solid pixel shows L exactly.
    CodeAndFraction,
    // round(a * L / 3): CGKEY.md's fallback, the simplest law.
    Linear,
};

struct HardwareGuesses {
    // Gradient wipes: the number of gray levels over which the wipe edge ramps from A to B.
    // 0 = a hard edge (a pixel is wholly A or wholly B). HARDWARE GUESS: the original's edge
    // softness is not confirmed in the Switcher code.
    int gradientSoftness = 0;

    // DVE: opacity of the drop shadow the digital video effects draw (0..1). HARDWARE GUESS.
    float dveShadowOpacity = 0.5f;

    // DVE: which pixel inside a source cell an output pixel reads. -1 = the same offset inside the
    // source cell as the output pixel has inside its own cell; 0-3 = always that offset.
    // HARDWARE GUESS.
    int dveCellPixel = -1;

    // DVE: source cell sc (255 - key) reads raster x 4 * (sc - dveSourceCellOffset) + inner,
    // clamped to 0..767 (ALGO.md 9; the ALGO digital moves' cell row is offset against the
    // picture the same way). Cells 12..171 of the visible area then show the picture unshifted
    // for a key of 255 - (c + 3). HARDWARE GUESS.
    int dveSourceCellOffset = 3;

    // ChromaFX: a pixel of Rec.601 luma Y (0..255) reads colour-map entry
    // floor(chromaLumaOffset + Y / 4), clamped to 0..95 (the identity table's own spacing).
    // HARDWARE GUESS: the hardware addresses the map with the DV luma sample, which the code does
    // not show. LUT_LinearRamp (written as DV line 479) would be the identity if the entry were
    // DV luma / 2 - 18, i.e. 17.5 + 0.253 Y, within one entry of this.
    float chromaLumaOffset = 17.5f;

    // "!" DVEs (tag 0x1000001B, vt/dve.h): the DVE picture's share 0..255 for a routed pixel of
    // level L = 0..3; the rest is the routed source (A, B or the colour generator). Thirds, so L = 0
    // shows the source alone and L = 3 the DVE picture alone (Photo Fly Out ! flashes white, then
    // fades the photo in). HARDWARE GUESS: FREEZE.md section 2 does not give the mixer's weights.
    std::array<uint8_t, 4> dveLevelMix{0, 85, 170, 255};

    // DV store: the ticks (fields) a live digital picture lags its input, clamped to 0..8 where
    // used; a grab waits as long. HARDWARE GUESS: the code holds no delay constant; the grab
    // waits 4 fields and the manual's 1/15 s is 4 fields (FREEZE.md section 3).
    int digitalDelay = 4;

    // Luma key: the number of gray levels over which the key edge ramps from the background to the
    // fill (vt/key.h lumaKey). 0 = a hard edge. HARDWARE GUESS: the manual calls an analog key
    // sharp; the keyer's edge is not measured (docs/2026-10-29-keys-design.md section 4).
    int keySoftness = 0;

    // Positionable moves (tools/re/POSITION.md 4): the Amiga mouse counts, the delta the original's
    // acceleration works on, per logical pixel the Position dock's trackball is rolled. HARDWARE
    // GUESS: counts per mm depend on the mouse (about 200 dpi typical, POSITION.md 11.4).
    double positionCountsPerPixel = 1.0;

    // CG key pages: how the page's 2-bit alpha and the dissolve level mix the page over Main in the
    // mixer's mode 4. HARDWARE GUESS: the program forces bits 6-7 of the mix byte and picks the
    // palette (pixel code) from L >> 6; what the hardware makes of them is not in the code
    // (tools/re/CGKEY.md section 4 and section 7 unknown 1).
    CgMixLaw cgMixLaw = CgMixLaw::CodeAndFraction;

    // CG key pages: the fields (ticks) each dissolve step is held, clamped to 1 or more where used.
    // HARDWARE GUESS: each step waits at least one field; 4 makes a dissolve last about $1f0 fields,
    // the ALGO meaning of the speed value (tools/re/CGKEY.md section 2 "Duration", unknown 2).
    int cgFieldsPerStep = 4;
};

}  // namespace vt
