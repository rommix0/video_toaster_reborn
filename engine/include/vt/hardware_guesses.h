#pragma once
// Values the Toaster's hardware or Switcher code decides but that are not (yet) known from the
// program. Each is a GUESS, kept here so it is visible, documented and easy to change in one place.
#include <array>
#include <cstdint>

namespace vt {

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
};

}  // namespace vt
