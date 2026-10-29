// DVE moves as an Effect: the T-bar (or RenderCtx::animFrame) picks a control frame and renderDve reads
// the moving clip through it, bit-exact. Which clip moves comes from dveInfo:
//   MovingA  A moves over B ("Out" moves)      MovingB  B moves over A ("In" moves)
//   DistortA A moved over A (Glitch effects)   Flip     A until switchFrame, then B, over black
// The art layer's palette for frame k is the most recent non-empty cmaps[j], 1 <= j <= k, else
// bm.cmap (the anim effects' rule). The "!" DVEs' tag 0x1000001B routes (dveRoutes, per frame
// from that palette) mix A, B or the generator into the picture by level. Effects with unknown key codes (24..63; none in the corpus)
// are logged once, when loaded.
#include <string>

#include "effect_detail.h"
#include "log_detail.h"
#include "vt/dve.h"
#include "vt/timing.h"

namespace vt::detail {
namespace {

static_assert(kRasterW == kDveWidth && kRasterH == 2 * kDveLines, "DVE control drives the raster 1:1 / line-doubled");

class DveEffect final : public CroutonEffect {
public:
    DveEffect(CroutonBasics b, const Anim& an, DveInfo info, const std::vector<uint8_t>& tag1b)
        : CroutonEffect(std::move(b)), info_(info) {
        long unknownCells = 0;
        const std::vector<Rgb>* palette = &an.bm.cmap;
        for (size_t k = 0; k < an.planes.size(); ++k) {
            if (k >= 1 && k < an.cmaps.size() && an.cmaps[k] && !an.cmaps[k]->empty()) palette = &*an.cmaps[k];
            frames_.push_back(decodeDve(an.planes[k]));
            palettes_.push_back(*palette);
            routes_.push_back(dveRoutes(tag1b, *palette));
            for (uint8_t v : frames_.back().key) unknownCells += v >= 24 && v < 64;
        }
        if (unknownCells > 0)
            warn(basics().name + ": " + std::to_string(unknownCells) + " DVE cells use unknown fill codes (drawn mid grey)");
        black_.px.assign(black_.px.size(), kBlack);
    }

    int frameCount() const override { return int(frames_.size()); }
    bool usesDvMemory() const override { return true; }
    DigitalMove digitalMove() const override {
        return info_.role == DveRole::MovingB ? DigitalMove::B : DigitalMove::A;
    }

    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        const int k = animFrameFor(tbar, frameCount(), ctx);
        // The moving clip comes through the DV path (delayed while live); the background is analog.
        const Frame& da = ctx.digitalA ? *ctx.digitalA : a;
        const Frame& db = ctx.digitalB ? *ctx.digitalB : b;
        const Frame* moving = &da;
        const Frame* background = &b;
        switch (info_.role) {
            case DveRole::MovingA: break;
            case DveRole::MovingB: moving = &db; background = &a; break;
            case DveRole::DistortA: background = &a; break;
            case DveRole::Flip:
                moving = k < info_.switchFrame ? &da : &db;
                background = &black_;
                break;
        }
        // the routes take the analog buses a / b as given, whichever clip moves
        const DveMix mix{&routes_[size_t(k)], &a, &b, ctx.background, ctx.generator};
        renderDve(frames_[size_t(k)], *moving, *background, palettes_[size_t(k)], out, ctx.guesses,
                  routes_[size_t(k)].empty() ? nullptr : &mix);
    }

private:
    DveInfo info_;
    std::vector<DveControl> frames_;
    std::vector<std::vector<Rgb>> palettes_;  // per frame
    std::vector<std::vector<DveRoute>> routes_;  // per frame, from tag 0x1000001B; empty = none
    Frame black_;
};

}  // namespace

std::unique_ptr<Effect> makeDveEffect(CroutonBasics b, const Anim& an, const std::vector<uint8_t>& tag1b, std::string* why) {
    if (an.planes.empty()) {
        if (why) *why = "DVE effect without frames";
        return nullptr;
    }
    for (const auto& p : an.planes)
        if (p.h != kDveLines || !isDve(p)) {
            if (why) *why = "DVE control frames are not 768x241 with the DVE signature";
            return nullptr;
        }
    const DveInfo info = dveInfo(an);
    return std::make_unique<DveEffect>(std::move(b), an, info, tag1b);
}

}  // namespace vt::detail
