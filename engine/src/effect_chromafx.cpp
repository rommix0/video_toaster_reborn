// ChromaFX effect (Ruling 18): recolours B (the Preview source) at every T-bar position,
// including 0; A is never shown. Cycling croutons advance one cycle step per output frame
// (the Switcher calls the cycle routine once per video field; engine frames run at field rate).
//
// The colour/B&W flag (tag 0x7e) only changes which hardware muxes the Switcher sets (sw_m2ac
// also routes the source into $75a+4 when it is "colour"); what that does to the picture is in
// the hardware, so the engine recolours by luma the same way in both modes.
#include "effect_detail.h"
#include "vt/chromafx.h"

namespace vt {
namespace {

class ChromaFx final : public Effect {
public:
    ChromaFx(std::string name, Timing timing, std::optional<Sound> sound, ChromaSettings s, ChromaRows rows)
        : name_(std::move(name)), timing_(timing), sound_(std::move(sound)), s_(s), rows_(std::move(rows)),
          step_(chromaCycleStep(timing.defaultSpeed)) {}

    std::string name() const override { return name_; }
    Timing timing() const override { return timing_; }
    int frameCount() const override { return 1; }
    const Sound* sound() const override { return sound_ ? &*sound_ : nullptr; }
    bool usesDvMemory() const override { return true; }

    void render(int tbar, const Frame&, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        const int cycle = chromaCycleRow(s_.cycle, step_, ctx.frameNumber);
        applyChroma(rows_, s_.mode, detail::clampTBar(tbar), cycle, b, out, ctx.guesses.chromaLumaOffset);
    }

private:
    std::string name_;
    Timing timing_;
    std::optional<Sound> sound_;
    ChromaSettings s_;
    ChromaRows rows_;
    uint32_t step_;  // 16.16 rows per field, from the speed the Switcher selects for the crouton
};

}  // namespace

namespace detail {

std::unique_ptr<Effect> makeChromaEffect(std::string name, const Crouton& c, std::string* why) {
    const auto lut = lutFromCrouton(c);
    if (!lut) {
        if (why) *why = "ChromaFX crouton without a LUT chunk";
        return nullptr;
    }
    const ChromaSettings s = chromaSettings(c, *lut);
    const Timing timing = fixedTiming(c);
    return std::make_unique<ChromaFx>(name, timing, loadSound(c, name), s, buildRows(*lut, s.interp));
}

}  // namespace detail
}  // namespace vt
