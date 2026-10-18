// ChromaFX effect (Ruling 18): recolours B (the Preview source) at every T-bar position,
// including 0; A is never shown. Cycling croutons advance one cycle step per output frame
// (the Switcher calls the cycle routine once per video field; engine frames run at field rate)
// and ignore the T-bar while they cycle (Ruling 20).
//
// The colour/B&W flag (tag 0x7e) only changes which hardware muxes the Switcher sets (sw_m2ac
// also routes the source into $75a+4 when it is "colour"); what that does to the picture is in
// the hardware, so the engine recolours by luma the same way in both modes.
#include "effect_detail.h"
#include "vt/chromafx.h"

namespace vt {
namespace {

class ChromaFx final : public detail::CroutonEffect {
public:
    ChromaFx(detail::CroutonBasics b, ChromaSettings s, ChromaRows rows)
        : CroutonEffect(std::move(b)), s_(s), rows_(std::move(rows)),
          step_(chromaCycleStep(basics().timing.defaultSpeed)) {}

    int frameCount() const override { return 1; }
    bool usesDvMemory() const override { return true; }
    bool isChromaFx() const override { return true; }
    bool isChromaFilter() const override { return s_.mode == ChromaMode::Filter; }

    void render(int tbar, const Frame&, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        // Ruling 20: while cycling the row position comes from the cycle alone (DummyTBar).
        const int v = chromaRowPosition(detail::clampTBar(tbar), s_.cycle, step_, ctx.frameNumber);
        applyChroma(rows_, s_.mode, v, b, out, ctx.guesses.chromaLumaOffset);
    }

private:
    ChromaSettings s_;
    ChromaRows rows_;
    uint32_t step_;  // 16.16 rows per field, from the speed the Switcher selects for the crouton
};

}  // namespace

namespace detail {

std::unique_ptr<Effect> makeChromaEffect(CroutonBasics b, const Crouton& c, std::string* why) {
    const auto lut = lutFromCrouton(c);
    if (!lut) {
        if (why) *why = "ChromaFX crouton without a LUT chunk";
        return nullptr;
    }
    const ChromaSettings s = chromaSettings(c, *lut);
    return std::make_unique<ChromaFx>(std::move(b), s, buildRows(*lut, s.interp));
}

}  // namespace detail
}  // namespace vt
