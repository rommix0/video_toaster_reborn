#include "vt/mix.h"

#include <algorithm>

namespace vt {
namespace {

// toaster_fx_convert.dominant: most frequent index, lowest wins ties.
int dominant(const IndexImage& im) {
    std::array<size_t, 256> n{};
    for (uint8_t p : im.px) ++n[p];
    int best = 0;
    for (int i = 1; i < 256; ++i)
        if (n[size_t(i)] > n[size_t(best)]) best = i;
    return best;
}

bool usable(const Anim& a) { return a.bm.planes >= 1 && a.bm.planes <= 8 && !a.frames.empty(); }

bool isVideo(const AnimPlan& p, int i) {
    switch (p.kind) {
        case AnimKind::Roles: return i == p.a || i == p.b;
        case AnimKind::Overlay: return i == p.video;
        default: return false;
    }
}

// toaster_fx_convert.overlay_colours for the indices < 1 << planes. Video indices are 0,0,0,0.
// *stoodIn (if given) says whether the colours are `bg` (an all-black palette) or the palette's.
std::vector<Rgba8> overlayColours(const Anim& a, const AnimPlan& p, const std::vector<Rgb>& cm,
                                  const std::array<bool, 256>& used, Rgba8 bg, bool* stoodIn = nullptr) {
    const int n = 1 << a.bm.planes;
    std::vector<Rgba8> rgba(size_t(n), Rgba8{0, 0, 0, 0});
    std::vector<int> over;
    for (int i = 0; i < n; ++i)
        if (!isVideo(p, i)) over.push_back(i);
    for (int i : over) rgba[size_t(i)] = Rgba8{bg.r, bg.g, bg.b, 255};
    if (stoodIn) *stoodIn = true;
    if (int(cm.size()) >= n) {
        int mx = 0;
        for (int i : over) mx = std::max({mx, int(cm[size_t(i)].r), int(cm[size_t(i)].g), int(cm[size_t(i)].b)});
        // tiny values are control codes, not colours; an all-zero set has max 0 < 16 as well
        if (mx >= 16) {
            for (int i : over) rgba[size_t(i)] = Rgba8{cm[size_t(i)].r, cm[size_t(i)].g, cm[size_t(i)].b, 255};
            if (stoodIn) *stoodIn = false;
        }
    }
    std::vector<int> lv;
    for (int i : over)
        if (used[size_t(i)]) lv.push_back(i);
    if (lv.size() >= 3) {
        int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
        for (int i : lv) {
            const int c[3] = {rgba[size_t(i)].r, rgba[size_t(i)].g, rgba[size_t(i)].b};
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], c[k]);
                hi[k] = std::max(hi[k], c[k]);
            }
        }
        if (std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) <= 6) {  // glow: index is strength
            const int top = lv.back();                                       // lv is ascending
            for (int i : lv) rgba[size_t(i)].a = uint8_t(i * 255 / top);
        }
    }
    return rgba;
}

bool anyOverlay(const std::vector<Rgba8>& rgba, const std::array<bool, 256>& used) {
    for (size_t i = 0; i < rgba.size(); ++i)
        if (used[i] && rgba[i].a != 0) return true;
    return false;
}

void applyRoutes(std::array<MixEntry, 256>& t, const AnimPlan& plan, int n, Rgba8 bg) {
    for (size_t i = 0; i < plan.routes.size() && i < size_t(n); ++i) {
        switch (plan.routes[i]) {
            case PaletteRoute::Palette: break;
            case PaletteRoute::A: t[i] = MixEntry{MixSrc::A, 0, {0, 0, 0, 0}, false}; break;
            case PaletteRoute::B: t[i] = MixEntry{MixSrc::B, 255, {0, 0, 0, 0}, false}; break;
            case PaletteRoute::Generator: t[i] = MixEntry{MixSrc::Colour, 0, {bg.r, bg.g, bg.b, 255}, true}; break;
        }
    }
}

uint16_t be16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] << 8 | b[o + 1]); }

}  // namespace

// np.unique(concat(f[::2, ::2]))
std::array<bool, 256> usedIndices(const Anim& a) {
    std::array<bool, 256> used{};
    for (const auto& f : a.frames)
        for (int y = 0; y < f.h; y += 2)
            for (int x = 0; x < f.w; x += 2) used[f.px[size_t(y) * size_t(f.w) + size_t(x)]] = true;
    return used;
}

std::vector<PaletteRoute> paletteRoutes(const std::vector<uint8_t>& tag1b, int planes) {
    return planes == 2 ? sourceRoutes(tag1b, 1 << planes) : std::vector<PaletteRoute>{};
}

std::vector<PaletteRoute> sourceRoutes(const std::vector<uint8_t>& tag1b, int n) {
    // 0xe80e4: word rows, word step, word first index, word count, 8 mask longs, then rows x
    // count longs. Every shipped table has one row; more rows (picked by the T-bar) are not
    // modelled.
    if (tag1b.size() < 40) return {};
    const int rows = be16(tag1b, 0), first = be16(tag1b, 4), count = be16(tag1b, 6);
    if (rows != 1 || first + count > n || tag1b.size() < 40 + size_t(count) * 4) return {};
    std::vector<PaletteRoute> r(size_t(n), PaletteRoute::Palette);
    for (int k = 0; k < count; ++k) {
        const size_t o = 40 + size_t(k) * 4;
        const uint32_t e = uint32_t(be16(tag1b, o)) << 16 | be16(tag1b, o + 2);
        if (e & ~0x0000f000u) continue;  // other bits (levels): not a plain source select
        switch (e >> 12) {
            case 1: r[size_t(first + k)] = PaletteRoute::A; break;
            case 3: r[size_t(first + k)] = PaletteRoute::B; break;
            case 9: r[size_t(first + k)] = PaletteRoute::Generator; break;
            default: break;
        }
    }
    return r;
}

AnimPlan planAnim(const Anim& an, std::optional<uint32_t> mode) {
    AnimPlan plan;
    if (!usable(an)) {
        plan.reason = "no palette-index frames";
        return plan;
    }
    const Bitmap& bm = an.bm;
    const int top = (1 << bm.planes) - 1;
    const int A = dominant(an.frames.front()), B = dominant(an.frames.back());
    plan.a = A;
    plan.b = B;
    if (bm.w >= 768 && bm.planes == 3 && (bm.camg & 0x8000) && (!mode || *mode == 0 || *mode == 2)) {
        plan.reason = "DVE data without the usual control table";
        return plan;
    }
    const bool m45 = mode && (*mode == 4 || *mode == 5);
    if (bm.planes <= 4 && (m45 || (!mode && ((A == 0 && B == top) || (A == top && B == 0))))) {
        if (A == top && B == 0) {
            plan.kind = AnimKind::Soft;
            plan.inverted = true;
            return plan;
        }
        if (A == 0) {
            plan.kind = AnimKind::Soft;
            return plan;
        }
        // otherwise it is A/B plus artwork (e.g. Count Down, Sparks): fall through
    }
    if (!mode || *mode == 4 || *mode == 5 || *mode == 7) {
        if (A == B) {
            plan.kind = AnimKind::Overlay;
            plan.video = A;
        } else {
            plan.kind = AnimKind::Roles;
        }
        return plan;
    }
    plan.reason = "unknown mode " + std::to_string(*mode);
    return plan;
}

std::array<MixEntry, 256> mixTable(const Anim& an, const AnimPlan& plan, const std::vector<Rgb>& cmap,
                                   Rgba8 background) {
    return mixTable(an, plan, cmap, background, usedIndices(an));
}

std::array<MixEntry, 256> mixTable(const Anim& an, const AnimPlan& plan, const std::vector<Rgb>& cmap,
                                   Rgba8 background, const std::array<bool, 256>& used) {
    std::array<MixEntry, 256> t{};
    if (plan.kind == AnimKind::Unsupported || !usable(an)) return t;
    const int n = 1 << an.bm.planes, top = n - 1;
    // the matte (the converter's lut); Overlay has none
    for (int i = 0; i < n; ++i) {
        int v = 0;
        if (plan.kind == AnimKind::Soft) v = (plan.inverted ? top - i : i) * 255 / top;
        else if (plan.kind == AnimKind::Roles) v = i == plan.b ? 255 : 0;
        t[size_t(i)].bLevel = uint8_t(v);
        t[size_t(i)].src = v == 255 ? MixSrc::B : MixSrc::A;
    }
    if (plan.kind != AnimKind::Soft) {
        bool stoodIn = false;
        const auto rgba = overlayColours(an, plan, cmap, used, background, &stoodIn);
        if (anyOverlay(rgba, used))
            for (int i = 0; i < n; ++i)
                if (!isVideo(plan, i)) {
                    t[size_t(i)].src = MixSrc::Colour;
                    t[size_t(i)].colour = rgba[size_t(i)];
                    t[size_t(i)].generator = stoodIn;
                }
    }
    applyRoutes(t, plan, n, background);
    return t;
}

int flashPeak(const Anim& an, const AnimPlan& plan) {
    if (plan.kind != AnimKind::Overlay || !usable(an)) return -1;
    const auto used = usedIndices(an);
    const Rgba8 white{255, 255, 255, 255};  // alpha does not depend on the stand-in colour
    auto rgba = overlayColours(an, plan, an.bm.cmap, used, white);
    if (!anyOverlay(rgba, used)) return -1;
    double best = -1;
    int peak = 0;
    for (size_t k = 0; k < an.frames.size(); ++k) {
        if (k > 0 && k < an.cmaps.size() && an.cmaps[k])  // the palette changes mid-effect
            rgba = overlayColours(an, plan, *an.cmaps[k], used, white);
        const IndexImage& f = an.frames[k];
        double sum = 0;
        size_t cnt = 0;
        for (int y = 0; y < f.h; y += 4)
            for (int x = 0; x < f.w; x += 4, ++cnt) {
                const size_t v = f.px[size_t(y) * size_t(f.w) + size_t(x)];
                if (v < rgba.size()) sum += rgba[v].a;  // an index past the palette covers nothing
            }
        const double cover = cnt ? sum / double(cnt) : 0.0;
        if (cover > best) {  // first maximum wins, as np.argmax
            best = cover;
            peak = int(k);
        }
    }
    return best >= 0.97 * 255 ? peak : -1;
}

}  // namespace vt
