#pragma once
// Shared by effect_*.cpp; not part of the public API.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "vt/anim.h"
#include "vt/crouton.h"
#include "vt/effect.h"
#include "vt/ilbm.h"
#include "vt/mix.h"
#include "vt/raster.h"

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

// The crouton's 8SVX sound; nullopt if absent or undecodable (the reason is logged once).
std::optional<Sound> loadSound(const Crouton&, const std::string& effectName);

// nullptr if a gradient ILBM can be played, else the reason it cannot (bar pictures, deep colour, HAM).
const char* gradientRefusal(const Bitmap&);

// `why` may be null. Return nullptr and set *why when the effect cannot be built.
std::unique_ptr<Effect> makeGradient(std::string name, Timing, std::optional<Sound>, Bitmap, IndexImage,
                                     std::string* why);
std::unique_ptr<Effect> makeAnimEffect(std::string name, Timing, std::optional<Sound>, Anim, AnimPlan,
                                       std::string* why);
// DVE content (isDve on the first frame); every frame must be 768x241 DVE control data.
std::unique_ptr<Effect> makeDveEffect(std::string name, Timing, std::optional<Sound>, const Anim&, std::string* why);
// CRFX crouton: its ` LUT` record, settings from the tags (vt/chromafx.h).
std::unique_ptr<Effect> makeChromaEffect(std::string name, const Crouton&, std::string* why);

}  // namespace vt::detail
