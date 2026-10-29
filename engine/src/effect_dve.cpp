// DVE moves as an Effect: the T-bar (or RenderCtx::animFrame) picks a control frame and renderDve reads
// the moving clip through it, bit-exact. Which clip moves comes from dveInfo:
//   MovingA  A moves over B ("Out" moves)      MovingB  B moves over A ("In" moves)
//   DistortA A moved over A (Glitch effects)   Flip     A until switchFrame, then B, over black
// The art layer's palette for frame k is the most recent non-empty cmaps[j], 1 <= j <= k, else
// bm.cmap (the anim effects' rule). The "!" DVEs' tag 0x1000001B routes (dveRoutes, per frame
// from that palette) mix the other bus or the generator into the picture by level; which bus
// S = 1 shows comes from the routing descriptor (tag 0x80000007, see render). Effects with unknown
// key codes (24..63; none in the corpus) are logged once, when loaded.
#include <algorithm>
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
    DveEffect(CroutonBasics b, const Anim& an, DveInfo info, const std::vector<uint8_t>& tag1b, uint32_t descriptor)
        : CroutonEffect(std::move(b)), info_(info), routesMoveA_(descriptor == 3 || descriptor == 32) {
        long unknownCells = 0;
        const std::vector<Rgb>* palette = &an.bm.cmap;
        for (size_t k = 0; k < an.planes.size(); ++k) {
            if (k >= 1 && k < an.cmaps.size() && an.cmaps[k] && !an.cmaps[k]->empty()) palette = &*an.cmaps[k];
            frames_.push_back(decodeDve(an.planes[k]));
            palettes_.push_back(*palette);
            routes_.push_back(dveRoutes(tag1b, *palette));
            // only routes and the art colour read it: 185 KB a frame
            if (routes_.back().empty() && frames_.back().art.empty()) frames_.back().index.clear();
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
        // Ruling 10 (a GUESS): S = 1 is the bus that does not move, analog; S = 3 the moving bus's
        // picture as the move moves it, the DVE picture itself (the DV path: the moving bus holds
        // 0x30|input from the take, FREEZE.md 5). The routing descriptor says which bus moves
        // (ALGO.md 7, 15.2): 3 and 32 move A, every other one B. The control frames' role does not
        // decide it.
        // Key mode: the fill replaces the key-0 background (the Flip role's black too) and S = 1.
        const Frame* s1 = routesMoveA_ ? &b : &a;
        if (ctx.keyFill) {
            std::fill(fill_.px.begin(), fill_.px.end(), *ctx.keyFill);
            background = s1 = &fill_;
        }
        const DveMix mix{&routes_[size_t(k)], s1, ctx.background, ctx.generator};
        renderDve(frames_[size_t(k)], *moving, *background, palettes_[size_t(k)], out, ctx.guesses,
                  routes_[size_t(k)].empty() ? nullptr : &mix);
    }

private:
    DveInfo info_;
    bool routesMoveA_;  // routing descriptor 3 or 32: S = 1 is B, else A
    std::vector<DveControl> frames_;
    std::vector<std::vector<Rgb>> palettes_;  // per frame
    std::vector<std::vector<DveRoute>> routes_;  // per frame, from tag 0x1000001B; empty = none
    Frame black_;
    mutable Frame fill_;  // render scratch: the keyFill colour
};

}  // namespace

std::unique_ptr<Effect> makeDveEffect(CroutonBasics b, const Anim& an, const std::vector<uint8_t>& tag1b, uint32_t descriptor,
                                      std::string* why) {
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
    return std::make_unique<DveEffect>(std::move(b), an, info, tag1b, descriptor);
}

}  // namespace vt::detail
