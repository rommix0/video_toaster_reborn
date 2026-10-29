#pragma once
// The effect interface: something that turns clip A, clip B and a T-bar position into a picture.
// Cut and dissolve are built in; everything else is read from a crouton file by loadEffect.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vt/background.h"
#include "vt/hardware_guesses.h"
#include "vt/ilbm.h"
#include "vt/raster.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace vt {

struct RenderCtx {
    // The colour generator's colour while the effect runs, which an animated wipe's matte areas
    // show (its tag 0x1000001B colour entries, or a black palette). The Switcher passes its
    // effect colour (Switcher::effectColour(): the effect's own unless overridden, else the BKG
    // source's colour); black for Snow.
    Rgba8 background{0, 0, 0, 255};
    // When the generator is in Snow: its noise picture (kRasterW x kRasterH), which those matte
    // areas show instead of `background`. Null otherwise.
    const Frame* generator = nullptr;
    // Switcher ticks since the current ChromaFX or overlay was engaged, 0 otherwise. Cycling
    // ChromaFX take their cycle phase from it; other effects ignore it.
    uint32_t frameNumber = 0;
    // The animation frame to show, or -1: derive it from the T-bar (animFrameForTBar). The
    // Switcher sets it during Auto, where the T-bar's 0..511 steps cannot say which frame is
    // due (Ruling 25). Values past the last frame show the last frame. Effects with one frame
    // ignore it.
    int animFrame = -1;
    // The speed button the effect runs at, already resolved against its timing (the Switcher
    // passes timing().resolve(Switcher::speed())); nullopt = the effect's default speed.
    // Cycling ChromaFX take their cycle rate from it.
    std::optional<Speed> speed;
    HardwareGuesses guesses;
    // The Switcher's take number: it changes when a transition starts from idle and when the
    // effect is replaced. ALGO moves keep state within a take (ALGO.md 15.1) and start afresh
    // when it changes; other effects ignore it.
    uint64_t take = 0;
    // Forward Auto started at T-bar 0: ticks elapsed + 1, in 1..autoSteps; else -1. ALGO
    // moves step through Auto's t values by it (ALGO.md 15.1); other effects ignore it.
    int autoStep = -1;
    // With autoStep: the running Auto's total ticks, the N autoStep counts to (Ruling 7; it can
    // differ from durationTicks[speed] after a setSpeed mid-Auto). 0 = durationTicks[speed].
    int autoSteps = 0;
    // The effect colour in force (Switcher::effectColour()): an ALGO move's border colour.
    EffectColour colour;
    // The DV path's picture of A and B: the bus's input delayed by HardwareGuesses::digitalDelay
    // while the store is live and the bus shows an input; null otherwise (use a / b: a frozen DV
    // still, BKG, live DV). Digital moves move these; their key-0 background stays a / b
    // (analog). In the "!" DVEs' tag 0x1B routes S = 3 is the moving bus's picture (this one) and
    // S = 1 the other bus, analog (vt::DveMix).
    const Frame* digitalA = nullptr;
    const Frame* digitalB = nullptr;
};

// Which bus a digital move moves first (FREEZE.md 5): None for everything but the DVE moves
// (role MovingB -> B, else A) and ALGO kinds 0/3 (descriptor 2 -> B, else A). ChromaFX: None.
enum class DigitalMove { None, A, B };

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
    // one; the result goes to `out` (all kRasterW x kRasterH pixels are written). `out` must not
    // alias `a` or `b` (effects read inputs after writing output pixels); `a` and `b` may be the
    // same frame.
    virtual void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx&) const = 0;
    // The effect's 8SVX sound, or nullptr (none, or one that cannot be decoded).
    virtual const Sound* sound() const { return nullptr; }
    // The effect's own matte colour, its "Effect Color" (crouton tags 0x80000023/0x24, see
    // effectColourFromTags). nullopt when it has none (the built-in effects, and croutons without
    // the tags): the generator then keeps the BKG source's colour.
    virtual std::optional<EffectColour> effectColour() const { return std::nullopt; }
    // True for effects that use the DV memory (DVE moves, ChromaFX): at most one can run at a time.
    virtual bool usesDvMemory() const { return false; }
    // The Switcher routes this bus's input through the DV store at take start (FREEZE.md 5) and
    // passes the delayed pictures as RenderCtx::digitalA / digitalB.
    virtual DigitalMove digitalMove() const { return DigitalMove::None; }
    // True for ChromaFX effects. The Switcher keeps one engaged after its T-bar run instead of
    // swapping Main and Preview (Ruling 21).
    virtual bool isChromaFx() const { return false; }
    // True for a ChromaFX effect in Filter mode (its picture is meaningful at T-bar 0, so moving
    // the T-bar home does not disengage it). False for everything else.
    virtual bool isChromaFilter() const { return false; }
    // True for the Overlays-drawer effects (manual PDF 181: a still or looping graphic laid over
    // the current video, ended by hand). The Switcher runs them over Main instead of running a
    // transition (Rulings 27, 32). No crouton tag tells them apart, so loadEffect decides from
    // the path: the file's own folder is named "Overlays" (any case).
    virtual bool isOverlay() const { return false; }
};

// Cut: A below tbar 256, B from 256. Dissolve: per-channel linear mix
// out = (a * (511 - tbar) + b * tbar + 255) / 511, so tbar 0 is A and tbar 511 is B exactly.
std::unique_ptr<Effect> makeCut();
std::unique_ptr<Effect> makeDissolve();

// Reads a crouton file. A CRFX crouton -> ChromaFX (vt/chromafx.h: recolours B at every T-bar
// position, uses the DV memory). An ALGO crouton -> Switcher move (vt/algo.h: digital moves, which
// use the DV memory, wipes and fades). Otherwise the kind is decided by its content: an ANIM form
// -> DVE move (vt::isDve, see vt/dve.h) or animated wipe, else the first non-icon ILBM -> gradient
// wipe. Returns nullptr and sets *whyNot (if given) for files that cannot be played yet: MAIN/CTRL
// (Switcher code), Art Card Key and unsupported ALGO programs, bar pictures, unsupported control
// image sizes, undecodable files.
// An existing, readable file with no CrUD whose sibling icon (<name>.i or .I) is an ALGO crouton plays that crouton's
// program (the original reads the CrUD from the icon file; the two Smooth Fade files); the
// effect keeps the original file name.
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
