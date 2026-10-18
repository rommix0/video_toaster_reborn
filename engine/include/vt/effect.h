#pragma once
// The effect interface: something that turns clip A, clip B and a T-bar position into a picture.
// Cut and dissolve are built in; everything else is read from a crouton file by loadEffect.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "vt/hardware_guesses.h"
#include "vt/ilbm.h"
#include "vt/raster.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace vt {

struct RenderCtx {
    Rgba8 background{0, 0, 0, 255};  // the BKG/matte colour (black palettes in colour effects use it)
    uint32_t frameNumber = 0;        // output frame counter (snow noise etc.)
    // The animation frame to show, or -1: derive it from the T-bar (animFrameForTBar). The
    // Switcher sets it during Auto, where the T-bar's 0..511 steps cannot say which frame is
    // due (Ruling 25). Values past the last frame show the last frame. Effects with one frame
    // ignore it.
    int animFrame = -1;
    HardwareGuesses guesses;
};

class Effect {
public:
    virtual ~Effect();
    virtual std::string name() const = 0;
    virtual Timing timing() const = 0;
    // Number of animation frames in the effect's own data. 1 for effects driven only by the T-bar
    // value (cut, dissolve, gradient wipes). The T-bar maps to a frame by animFrameForTBar,
    // unless RenderCtx::animFrame names the frame.
    virtual int frameCount() const = 0;
    // tbar is 0..511 (values outside are clamped). `a` is the outgoing picture, `b` the incoming
    // one; the result goes to `out` (all kRasterW x kRasterH pixels are written).
    virtual void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx&) const = 0;
    // The effect's 8SVX sound, or nullptr (none, or one that cannot be decoded).
    virtual const Sound* sound() const { return nullptr; }
    // True for effects that use the DV memory (DVE moves, ChromaFX): at most one can run at a time.
    virtual bool usesDvMemory() const { return false; }
    // True for ChromaFX effects. The Switcher keeps one engaged after its T-bar run instead of
    // swapping Main and Preview (Ruling 21).
    virtual bool isChromaFx() const { return false; }
    // True for a ChromaFX effect in Filter mode (its picture is meaningful at T-bar 0, so moving
    // the T-bar home does not disengage it). False for everything else.
    virtual bool isChromaFilter() const { return false; }
};

// Cut: A below tbar 256, B from 256. Dissolve: per-channel linear mix
// out = (a * (511 - tbar) + b * tbar + 255) / 511, so tbar 0 is A and tbar 511 is B exactly.
std::unique_ptr<Effect> makeCut();
std::unique_ptr<Effect> makeDissolve();

// Reads a crouton file. A CRFX crouton -> ChromaFX (vt/chromafx.h: recolours B at every T-bar
// position, uses the DV memory). Otherwise the kind is decided by its content: an ANIM form -> DVE
// move (vt::isDve, see vt/dve.h) or animated wipe, else the first non-icon ILBM -> gradient wipe.
// Returns nullptr and sets *whyNot (if given) for files that cannot be played yet: ALGO/MAIN/CTRL
// (Switcher code), bar pictures, unsupported control image sizes, undecodable files.
std::unique_ptr<Effect> loadEffect(const std::filesystem::path&, std::string* whyNot = nullptr);

// The gradient wipe's decision for one pixel: B's share (0..255) for a pixel of the given gray at
// T-bar position tbar (clamped to 0..511) with the given edge softness (gray levels, clamped to
// 0..255; 0 = hard edge). Pure; see effect_gradient.cpp for the rule.
uint8_t gradientLevel(uint8_t gray, int tbar, int softness);

// Index -> gray of a gradient's ILBM (port of toaster_fx_convert.gray_lut): 1 << planes entries.
// A palette that is a plain gray ramp (r == g == b, brightest value above 0x40) gives the gray
// directly; anything else is index * 255 / (n - 1).
std::vector<uint8_t> grayLut(const Bitmap&);

}  // namespace vt
