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
struct MixEntry {
    MixSrc src = MixSrc::A;
    uint8_t bLevel = 0;
    Rgba8 colour{0, 0, 0, 0};
};

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
};

// `mode` is crouton tag 0x80000022 when present (3.1 hunk files have none).
AnimPlan planAnim(const Anim&, std::optional<uint32_t> mode);

// The table for palette `cmap` (pass a.bm.cmap, or rebuild with a frame's new CMAP; an empty or
// short palette means no colours). Entries 0 .. (1 << planes) - 1 are filled; the rest (and
// everything for an Unsupported plan) is MixSrc::A, bLevel 0, transparent: harmless.
// Colour entries exist only when some used non-video index is drawn, as in the converter. Where
// the converter would use its white stand-in for an all-black palette (< 16 everywhere), the
// entries use `background` (alpha 255, or the glow level) instead.
std::array<MixEntry, 256> mixTable(const Anim&, const AnimPlan&, const std::vector<Rgb>& cmap,
                                   Rgba8 background);

// Overlay-only effects (no matte) whose overlay covers >= 97 % of the frame at its peak are a
// transition: the Toaster cuts from A to B under the peak. Returns that frame's index into
// Anim::frames (reltimes are ignored: the engine's timing comes from the crouton tags), else -1.
// The converter's golden flash_peak counts frames repeated by reltime; the two agree while every
// reltime is 1. Always -1 for other kinds.
int flashPeak(const Anim&, const AnimPlan&);

}  // namespace vt
