#pragma once
// Mix tables for animated wipes: for each palette index of an ANIM effect, does the pixel show
// clip A, clip B (or a soft mix of the two), or a solid colour (overlay art, borders, glows)?
// Port of tools/toaster_fx_convert.py: plan_anim, the lut/overlay_colours logic at the top of
// render_anim, and its flash rule.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vt/anim.h"
#include "vt/ilbm.h"
#include "vt/raster.h"

namespace vt {

enum class MixSrc : uint8_t { A, B, Colour };

// bLevel is B's share 0-255 (soft mattes: the grey level; roles: 255 for B's index, 0 for the
// rest). For A and B entries it is authoritative: src is B exactly when bLevel == 255, else A (a
// soft mix of A and B has src A). For Colour entries `colour` is the solid colour and colour.a its
// opacity over the A/B mix (glow levels, else 255); bLevel is the underlying matte (0 for roles).
// `generator` marks Colour entries that show the colour generator (the `background` given to
// mixTable): with a Snow generator the renderer takes each pixel from RenderCtx::generator.
struct MixEntry {
    MixSrc src = MixSrc::A;
    uint8_t bLevel = 0;
    Rgba8 colour{0, 0, 0, 0};
    bool generator = false;
};

// Tag 0x1000001B: the Switcher's own hardware word for each palette index, which it writes
// into the frame's colour table every field (0xe7a9a -> 0xe80e4; tools/re/FINDINGS.md "Which
// colour an effect's matte shows"). Bits 12-15 (S) pick the mixer source for that index through
// 0xe7ebc: 1 and 3 the two buses (A and B), 9 mux source 6, the BKG colour generator
// (sw_m420 0xae050), 0 none (the palette's own value stays).
// Palette: no route, the converter's palette rules decide.
enum class PaletteRoute : uint8_t { Palette, A, B, Generator };

// The routes of an effect with `planes` bit planes from the tag's bytes (Crouton::blobs, after
// the length). Only 2-plane effects get routes: in them the routed indices show the source alone
// (the table replaces the whole colour, mask 0x8FFF0FFF, or leaves only palette bits that show
// nothing: their A and B indices carry palette colours and still show pure video). Deeper
// ("!") effects keep their palette art and get none. Empty when there are none or the bytes
// are short; otherwise one entry per palette index (1 << planes).
std::vector<PaletteRoute> paletteRoutes(const std::vector<uint8_t>& tag1b, int planes);
// The same reading for a palette of n indices whatever the depth (the Bar ILBMs, effect_bar.cpp):
// their art words carry a level too (0x9003), so they read as Palette.
std::vector<PaletteRoute> sourceRoutes(const std::vector<uint8_t>& tag1b, int n);

enum class AnimKind { Soft, Roles, Overlay, Unsupported };

// Soft: index/max is the matte (inverted: reversed). Roles: index a shows A, index b shows B, the
// rest is colour. Overlay: index `video` shows the clip, everything else is colour.
// Unsupported: `reason` says why (the converter's "defer:" text).
struct AnimPlan {
    AnimKind kind = AnimKind::Unsupported;
    int a = -1, b = -1;  // dominant index of the first / last frame
    bool inverted = false;
    int video = -1;  // Overlay only
    std::string reason;
    // Per palette index (paletteRoutes; empty = none). planAnim leaves it empty: loadEffect
    // fills it from tag 0x1000001B. mixTable applies it over the palette rules.
    std::vector<PaletteRoute> routes;
};

// `mode` is crouton tag 0x80000022 when present.
AnimPlan planAnim(const Anim&, std::optional<uint32_t> mode);

// The table for palette `cmap` (pass a.bm.cmap, or rebuild with a frame's new CMAP; an empty or
// short palette means no colours). Entries 0 .. (1 << planes) - 1 are filled; the rest (and
// everything for an Unsupported plan) is MixSrc::A, bLevel 0, transparent: harmless.
// Colour entries exist only when some used non-video index is drawn, as in the converter. Where
// the converter would use its white stand-in for an all-black palette (< 16 everywhere), the
// entries use `background` (alpha 255, or the glow level) instead, marked `generator`.
// Then plan.routes, where present, override their indices: A -> A (bLevel 0), B -> B (255),
// Generator -> `background`, opaque, marked `generator`.
std::array<MixEntry, 256> mixTable(const Anim&, const AnimPlan&, const std::vector<Rgb>& cmap,
                                   Rgba8 background);
// The same, with the palette indices the frames use (usedIndices) found once by the caller: that
// scan of every frame is the costly part, and does not depend on the palette or background.
std::array<MixEntry, 256> mixTable(const Anim&, const AnimPlan&, const std::vector<Rgb>& cmap,
                                   Rgba8 background, const std::array<bool, 256>& used);
// Which palette indices the frames use (sampled every other pixel and line, as the converter does).
std::array<bool, 256> usedIndices(const Anim&);

// Overlay-only effects (no matte) whose overlay covers >= 97 % of the frame at its peak are a
// transition: the Toaster cuts from A to B under the peak. Returns that frame's index into
// Anim::frames (reltimes are ignored: the engine's timing comes from the crouton tags), else -1.
// The converter's golden flash_peak counts frames repeated by reltime; the two agree while every
// reltime is 1. Always -1 for other kinds.
int flashPeak(const Anim&, const AnimPlan&);

}  // namespace vt
