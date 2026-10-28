// ALGO Switcher moves as an Effect: the crouton's program run by vt::AlgoVm (vt/algo.h; sections
// cited as "ALGO.md N" are in tools/re/ALGO.md). The original keeps the VM's H and V buffers from
// step to step within a take (ALGO.md 15.1), so each take runs one VM through the steps the
// Switcher renders, in order: a T-bar drag depends on every position rendered, Auto is
// deterministic. Pictures by kind and routing descriptor (tag 0x80000007, ALGO.md 15.2):
//   digital (0, 3)  renderDve of algoControl: desc 2 B over A, 3 A over B, 5 A over A then B over
//                   B, 6 A then B over the effect colour; a channel-23 level then fades the
//                   background toward that composite (ALGO.md 15.4, GUESS);
//   wipe (1, 4)     renderAlgoWipe (desc 4);
//   fade (2)        desc 1 A -> B by the level; desc 8 A -> matte, then matte -> B (ALGO.md 15.4,
//                   GUESS polarity).
// "Then" is the routing walker's second step, algoSecondStep. Kinds 3 and 4 play at their stored
// position, as 0 and 1 (interactive positioning is out of scope).
#include <algorithm>
#include <climits>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "effect_detail.h"
#include "vt/algo.h"
#include "vt/dve.h"

namespace vt::detail {
namespace {

constexpr uint32_t kTagProgram = 0x1000001C;
constexpr uint32_t kTagKind = 0x80000063;
constexpr uint32_t kTagDescriptor = 0x80000007;

bool isDigital(int kind) { return kind == 0 || kind == 3; }

// Threading: render is const but steps the take's VM, held in mutable members. The Switcher
// renders from one thread (vt/switcher.h), so there is no lock.
class AlgoEffect final : public CroutonEffect {
public:
    AlgoEffect(CroutonBasics b, std::vector<uint8_t> program, int kind, uint32_t desc, uint32_t flags)
        : CroutonEffect(std::move(b)), program_(std::move(program)), kind_(kind), desc_(desc), flags_(flags) {}

    int frameCount() const override { return 1; }
    bool usesDvMemory() const override { return isDigital(kind_); }

    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        tbar = clampTBar(tbar);
        // Ruling 7: in Auto, N is the running Auto's own tick count.
        const int n = ctx.autoStep >= 1 && ctx.autoSteps > 0
                          ? ctx.autoSteps
                          : basics().timing.durationTicks[size_t(ctx.speed.value_or(Speed::Medium))];
        // A new take starts from a fresh VM: install + init (ALGO.md 15.1), its colour latched.
        if (!vm_ || ctx.take != take_) {
            vm_.emplace(program_, kind_, algoColourIndex(ctx.colour));
            take_ = ctx.take;
            lastKey_ = INT_MIN;  // Ruling 2: no key equals it, so the take's first render steps
        }
        // The same step rendered again (the T-bar held while ticks render) does not step the VM.
        const int key = ctx.autoStep >= 1 ? ctx.autoStep : -1 - tbar;
        if (key != lastKey_) {
            step_ = &vm_->step(ctx.autoStep >= 1 ? algoAutoT(ctx.autoStep, n, flags_) : algoTBarT(tbar, flags_));
            lastKey_ = key;
        }
        const AlgoStep& s = *step_;
        const bool second = algoSecondStep(tbar, ctx.autoStep, n, flags_);
        auto matte = [&]() -> const Frame& {
            std::fill(matte_.px.begin(), matte_.px.end(), ctx.background);
            return matte_;
        };

        if (isDigital(kind_)) {
            DveControl control = algoControl(s);
            control.borderFill = uint8_t(algoBorderFill(algoColourIndex(ctx.colour)));  // ALGO.md 15.5
            const Frame* moving = &a;
            const Frame* background = &b;
            switch (desc_) {
                case 2: moving = &b; background = &a; break;
                case 3: break;
                case 5: moving = background = second ? &b : &a; break;
                default: moving = second ? &b : &a; background = &matte(); break;  // 6: BKG matte
            }
            renderDve(control, *moving, *background, {}, out, ctx.guesses);
            // ALGO.md 15.4 (GUESS): the level fades the key-0 background toward the composite.
            if (s.level >= 0) mixLevel(*background, out, s.level, out);
        } else if (kind_ == 2) {
            const int level = s.level < 0 ? 255 : s.level;  // mixLevel clamps to 0..255
            if (desc_ == 1) mixLevel(a, b, level, out);
            else mixLevel(second ? b : a, matte(), level, out);  // 8: Dip To Matte
        } else {
            renderAlgoWipe(s, a, b, out);
        }
    }

private:
    std::vector<uint8_t> program_;
    int kind_;
    uint32_t desc_;
    uint32_t flags_;
    mutable std::optional<AlgoVm> vm_;  // the current take's VM
    mutable uint64_t take_ = 0;
    mutable int lastKey_ = INT_MIN;     // ctx.autoStep if >= 1, else -1 - tbar
    mutable const AlgoStep* step_ = nullptr;  // vm_'s last step
    mutable Frame matte_;
};

}  // namespace

std::unique_ptr<Effect> makeAlgoEffect(CroutonBasics b, const Crouton& c, std::string* why) {
    auto fail = [&](std::string reason) -> std::unique_ptr<Effect> {
        if (why) *why = std::move(reason);
        return nullptr;
    };
    const int kind = int(c.tag(kTagKind).value_or(0));
    if (kind == 5) return fail("Art Card Key needs keys (not built yet)");
    const auto it = c.blobs.find(kTagProgram);
    if (it == c.blobs.end()) return fail("ALGO effect without a program");
    const uint32_t desc = c.tag(kTagDescriptor).value_or(0);
    const int fast = std::max(1, b.timing.durationTicks[size_t(Speed::Fast)]);
    uint32_t flags = 0;
    try {
        // Ruling 5: a probe take at load (the T-bar 0..511, then Auto at Fast), so a program the
        // VM cannot run is refused here and never throws from render.
        AlgoVm vm(it->second, kind, 0);
        flags = vm.flags();
        for (int pos = 0; pos <= 511; ++pos) vm.step(algoTBarT(pos, flags));
        AlgoVm autoVm(it->second, kind, 0);
        for (int k = 1; k <= fast; ++k) autoVm.step(algoAutoT(k, fast, flags));
    } catch (const std::exception& ex) {
        return fail(std::string("ALGO program not supported: ") + ex.what());
    }
    const bool routed = isDigital(kind) ? desc == 2 || desc == 3 || desc == 5 || desc == 6
                        : kind == 2     ? desc == 1 || desc == 8
                                        : desc == 4;
    if (!routed) return fail("ALGO routing descriptor " + std::to_string(desc) + " not supported");
    return std::make_unique<AlgoEffect>(std::move(b), it->second, kind, desc, flags);
}

}  // namespace vt::detail
