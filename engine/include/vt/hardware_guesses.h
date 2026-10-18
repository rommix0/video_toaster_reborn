#pragma once
// Values the Toaster's hardware or Switcher code decides but that are not (yet) known from the
// program. Each is a GUESS, kept here so it is visible, documented and easy to change in one place.
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

    // ChromaFX: offset added to the luma value (0..255 scale) before the colour tables are
    // applied. HARDWARE GUESS.
    float chromaLumaOffset = 17.5f;
};

}  // namespace vt
