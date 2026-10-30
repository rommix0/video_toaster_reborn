#pragma once
// Shared by effect_*.cpp; not part of the public API.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "vt/anim.h"
#include "vt/crouton.h"
#include "vt/effect.h"
#include "vt/ilbm.h"
#include "vt/mix.h"
#include "vt/raster.h"
#include "vt/timing.h"

namespace vt::detail {

// How a control image (gradient or animation frame) is laid onto the 768x482 raster, by integer
// repetition only (no scaling, no copied lines):
//   width 768 -> control x = raster x; width 384 -> control x = raster x / 2 (x * w / kRasterW).
//   height <= 241 (one field: 240 or 241) -> control line = raster y / 2, so line k drives raster
//   lines 2k and 2k+1 (spec 5.10); height 480 or 482 -> control line = raster y.
//   A raster line with no control line renders black.
struct ControlMap {
    int w = 0, h = 0;
    bool halfLines = false;
    int x(int rx) const { return rx * w / kRasterW; }
    int line(int ry) const {  // -1: no control line
        const int k = halfLines ? ry / 2 : ry;
        return k < h ? k : -1;
    }
};

// nullopt and *why for sizes that cannot be mapped by whole-pixel repetition.
std::optional<ControlMap> controlMap(int w, int h, std::string* why);

constexpr Rgba8 kBlack{0, 0, 0, 255};

// (a * (255 - l) + b * l + 127) / 255: l = 0 gives a, l = 255 gives b exactly; round half up.
inline uint8_t mix255(int a, int b, int l) { return uint8_t((a * (255 - l) + b * l + 127) / 255); }

inline Rgba8 mixPixel(const Rgba8& a, const Rgba8& b, int l) {
    if (l <= 0) return a;
    if (l >= 255) return b;
    return {mix255(a.r, b.r, l), mix255(a.g, b.g, l), mix255(a.b, b.b, l), mix255(a.a, b.a, l)};
}

inline int clampTBar(int t) { return t < 0 ? 0 : (t > 511 ? 511 : t); }

// The animation frame (0..n-1) an effect with n frames shows: ctx.animFrame if set, else the
// T-bar's frame.
inline int animFrameFor(int tbar, int n, const RenderCtx& ctx) {
    if (ctx.animFrame >= 0) return ctx.animFrame < n ? ctx.animFrame : n - 1;
    return animFrameForTBar(clampTBar(tbar), n);
}

// The crouton's 8SVX sound; nullopt if absent or undecodable (the reason is logged once).
std::optional<Sound> loadSound(const Crouton&, const std::string& effectName);

// What every effect read from a crouton carries besides its picture data.
struct CroutonBasics {
    std::string name;
    Timing timing;
    std::optional<Sound> sound;
    std::optional<EffectColour> colour;  // tags 0x80000023/0x24 (effectColourFromTags)
    bool overlay = false;  // from an Overlays drawer (Effect::isOverlay)
    uint32_t descriptor = 1;  // routing descriptor, tag 0x80000007 (Effect::routingDescriptor)
};

// Base of the crouton effects: answers the Effect queries that come from CroutonBasics.
class CroutonEffect : public Effect {
public:
    explicit CroutonEffect(CroutonBasics b) : b_(std::move(b)) {}
    std::string name() const override { return b_.name; }
    Timing timing() const override { return b_.timing; }
    const Sound* sound() const override { return b_.sound ? &*b_.sound : nullptr; }
    std::optional<EffectColour> effectColour() const override { return b_.colour; }
    bool isOverlay() const override { return b_.overlay; }
    uint32_t routingDescriptor() const override { return b_.descriptor; }

protected:
    const CroutonBasics& basics() const { return b_; }

private:
    CroutonBasics b_;
};

// nullptr if a gradient ILBM can be played, else the reason it cannot (bar pictures, deep colour, HAM).
const char* gradientRefusal(const Bitmap&);
// A picture taller than the raster or deeper than 8 planes: with a tag 0x1000001C program it is a
// Bar crouton (makeBarEffect), else gradientRefusal says why it cannot play.
inline bool isBarPicture(const Bitmap& bm) { return bm.h > kRasterH || bm.planes > 8; }

// `why` may be null. Return nullptr and set *why when the effect cannot be built.
std::unique_ptr<Effect> makeGradient(CroutonBasics, Bitmap, IndexImage, std::string* why);
std::unique_ptr<Effect> makeAnimEffect(CroutonBasics, Anim, AnimPlan, std::string* why);
// DVE content (isDve on the first frame); every frame must be 768x241 DVE control data. The
// routing descriptor (tag 0x80000007, 0 when absent) picks the buses the tag 0x1B routes show.
std::unique_ptr<Effect> makeDveEffect(CroutonBasics, const Anim&, const std::vector<uint8_t>& tag1b, uint32_t descriptor,
                                      std::string* why);
// CRFX crouton: its ` LUT` record, settings from the tags (vt/chromafx.h).
std::unique_ptr<Effect> makeChromaEffect(CroutonBasics, const Crouton&, std::string* why);
// ALGO crouton: its program (tag 0x1000001C) run by vt::AlgoVm, kind 0x80000063, routing
// descriptor 0x80000007 (vt/algo.h).
std::unique_ptr<Effect> makeAlgoEffect(CroutonBasics, const Crouton&, std::string* why);
// Bar ILBM crouton (isBarPicture with a tag 0x1000001C program): the still `form` moved by
// channel 29, coloured by tag 0x1000001B (tools/re/BARS.md).
std::unique_ptr<Effect> makeBarEffect(CroutonBasics, const Crouton&, const Form& form, std::string* why);

}  // namespace vt::detail
