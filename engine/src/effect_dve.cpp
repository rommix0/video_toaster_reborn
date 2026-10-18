// DVE moves as an Effect: the T-bar (or RenderCtx::animFrame) picks a control frame and renderDve reads
// the moving clip through it, bit-exact. Which clip moves comes from dveInfo:
//   MovingA  A moves over B ("Out" moves)      MovingB  B moves over A ("In" moves)
//   DistortA A moved over A (Glitch effects)   Flip     A until switchFrame, then B, over black
// The art layer's palette for frame k is the most recent non-empty cmaps[j], 1 <= j <= k, else
// bm.cmap (the anim effects' rule). Effects with unknown key codes (24..63; none in the corpus)
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
    DveEffect(CroutonBasics b, const Anim& an, DveInfo info) : CroutonEffect(std::move(b)), info_(info) {
        long unknownCells = 0;
        const std::vector<Rgb>* palette = &an.bm.cmap;
        for (size_t k = 0; k < an.planes.size(); ++k) {
            if (k >= 1 && k < an.cmaps.size() && an.cmaps[k] && !an.cmaps[k]->empty()) palette = &*an.cmaps[k];
            frames_.push_back(decodeDve(an.planes[k]));
            palettes_.push_back(*palette);
            for (uint8_t v : frames_.back().key) unknownCells += v >= 24 && v < 64;
        }
        if (unknownCells > 0)
            warn(basics().name + ": " + std::to_string(unknownCells) + " DVE cells use unknown fill codes (drawn mid grey)");
        black_.px.assign(black_.px.size(), kBlack);
    }

    int frameCount() const override { return int(frames_.size()); }
    bool usesDvMemory() const override { return true; }

    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        const int k = animFrameFor(tbar, frameCount(), ctx);
        const Frame* moving = &a;
        const Frame* background = &b;
        switch (info_.role) {
            case DveRole::MovingA: break;
            case DveRole::MovingB: moving = &b; background = &a; break;
            case DveRole::DistortA: background = &a; break;
            case DveRole::Flip:
                moving = k < info_.switchFrame ? &a : &b;
                background = &black_;
                break;
        }
        renderDve(frames_[size_t(k)], *moving, *background, palettes_[size_t(k)], out, ctx.guesses);
    }

private:
    DveInfo info_;
    std::vector<DveControl> frames_;
    std::vector<std::vector<Rgb>> palettes_;  // per frame
    Frame black_;
};

}  // namespace

std::unique_ptr<Effect> makeDveEffect(CroutonBasics b, const Anim& an, std::string* why) {
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
    return std::make_unique<DveEffect>(std::move(b), an, info);
}

}  // namespace vt::detail
