// Animated wipes: an ANIM whose palette-index frames say, pixel by pixel, whether the picture shows
// A, B, a soft mix of the two, or a solid colour (overlay art). The mix rules live in mix.h.
#include <algorithm>
#include <array>
#include <mutex>

#include "effect_detail.h"
#include "vt/timing.h"

namespace vt::detail {
namespace {

bool sameColour(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

class AnimEffect final : public CroutonEffect {
public:
    using Table = std::array<MixEntry, 256>;

    AnimEffect(CroutonBasics b, Anim an, AnimPlan plan, ControlMap map)
        : CroutonEffect(std::move(b)), an_(std::move(an)), plan_(std::move(plan)), map_(map),
          peak_(flashPeak(an_, plan_)) {
        // Palette segments: frame k uses the most recent non-empty cmaps[j] with 1 <= j <= k, else
        // bm.cmap (frame 0 is never a palette change). paletteFrame_[s] is that j (0 = bm.cmap).
        paletteFrame_.push_back(0);
        segOf_.resize(an_.frames.size());
        for (size_t k = 0; k < an_.frames.size(); ++k) {
            if (k >= 1 && k < an_.cmaps.size() && an_.cmaps[k] && !an_.cmaps[k]->empty())
                paletteFrame_.push_back(int(k));
            segOf_[k] = int(paletteFrame_.size()) - 1;
        }
    }

    int frameCount() const override { return int(an_.frames.size()); }

    // True if some palette of this effect draws solid colour (otherwise an Overlay plan has
    // nothing to show but the video).
    bool drawsColour() const {
        for (const auto& t : tables(Rgba8{255, 255, 255, 255})->t)
            for (const auto& e : t)
                if (e.src == MixSrc::Colour) return true;
        return false;
    }

    // Per pixel: the control image's palette index picks a MixEntry.
    //   level  = the entry's bLevel (B's share, 0..255); out = A + (B - A) * level / 255 per channel
    //            with round-half-up integer arithmetic (mix255), so level 0 is A and 255 is B exactly;
    //   Colour entries are then composited over that mix with the colour's own alpha, using the
    //   same formula: out = mix + (colour - mix) * colour.a / 255.
    // Overlay plans have no matte: a flash (flashPeak) shows A in animation frames before the peak
    // and B from it, with the overlay on top; other overlay-only effects have no peak, so their
    // clip is cut A -> B at the T-bar midpoint (tbar 256), like the cut (GUESS).
    // Video under a glow Colour entry (alpha < 255) is A (bLevel 0 for roles), not the nearest A/B
    // pixel's matte as in the converter: Ruling 14, a v1 limitation.
    // A raster line with no control line is black.
    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        const int t = clampTBar(tbar);
        const int frame = animFrameFor(t, frameCount(), ctx);
        const auto set = tables(ctx.background);
        const Table& table = set->t[size_t(segOf_[size_t(frame)])];
        const IndexImage& f = an_.frames[size_t(frame)];
        int forced = -1;  // -1: use each entry's own level
        if (plan_.kind == AnimKind::Overlay) forced = peak_ >= 0 ? (frame >= peak_ ? 255 : 0) : (t >= 256 ? 255 : 0);
        for (int y = 0; y < kRasterH; ++y) {
            Rgba8* dst = &out.at(0, y);
            const int line = map_.line(y);
            if (line < 0) {
                std::fill(dst, dst + kRasterW, kBlack);
                continue;
            }
            const uint8_t* src = &f.px[size_t(line) * size_t(f.w)];
            const Rgba8* pa = &a.at(0, y);
            const Rgba8* pb = &b.at(0, y);
            for (int x = 0; x < kRasterW; ++x) {
                const MixEntry& e = table[src[map_.x(x)]];
                Rgba8 m = mixPixel(pa[x], pb[x], forced >= 0 ? forced : e.bLevel);
                if (e.src == MixSrc::Colour) {
                    const int ca = e.colour.a;
                    m = {mix255(m.r, e.colour.r, ca), mix255(m.g, e.colour.g, ca), mix255(m.b, e.colour.b, ca), m.a};
                }
                dst[x] = m;
            }
        }
    }

private:
    struct Tables {
        Rgba8 background;
        std::vector<Table> t;  // one per palette segment
    };

    // The mix tables, built once per palette change and kept until the background colour (which
    // black palettes use) changes.
    std::shared_ptr<const Tables> tables(Rgba8 bg) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cache_ || !sameColour(cache_->background, bg)) {
            auto n = std::make_shared<Tables>();
            n->background = bg;
            for (int j : paletteFrame_) {
                const std::vector<Rgb>& cm = j == 0 ? an_.bm.cmap : *an_.cmaps[size_t(j)];
                n->t.push_back(mixTable(an_, plan_, cm, bg));
            }
            cache_ = std::move(n);
        }
        return cache_;
    }

    Anim an_;
    AnimPlan plan_;
    ControlMap map_;
    int peak_;
    std::vector<int> paletteFrame_;  // palette segment -> the frame whose CMAP it is (0 = bm.cmap)
    std::vector<int> segOf_;         // animation frame -> palette segment
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const Tables> cache_;
};

}  // namespace

std::unique_ptr<Effect> makeAnimEffect(CroutonBasics b, Anim an, AnimPlan plan, std::string* why) {
    if (an.frames.empty() || plan.kind == AnimKind::Unsupported) {
        if (why) *why = plan.reason.empty() ? "no palette-index frames" : plan.reason;
        return nullptr;
    }
    auto map = controlMap(an.frames[0].w, an.frames[0].h, why);
    if (!map) return nullptr;
    const bool overlayOnly = plan.kind == AnimKind::Overlay;
    auto e = std::make_unique<AnimEffect>(std::move(b), std::move(an), std::move(plan), *map);
    if (overlayOnly && !e->drawsColour()) {
        if (why) *why = "overlay effect with nothing but video in it";
        return nullptr;
    }
    return e;
}

}  // namespace vt::detail
