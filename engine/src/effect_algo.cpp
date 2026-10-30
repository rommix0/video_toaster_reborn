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
// "Then" is the routing walker's second step, algoSecondStep. Kinds 3 and 4 play as 0 and 1 at
// RenderCtx::position (tools/re/POSITION.md): the take's VM has its program's parameters rewritten
// in place whenever the position changes, as the original's 0xb71b2 / 0xb7054 rewrite the
// crouton's own tag, so the effect itself stays shared and unchanged.
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
    AlgoEffect(CroutonBasics b, std::vector<uint8_t> program, int kind, uint32_t desc, uint32_t flags,
               std::optional<PositionRange> range)
        : CroutonEffect(std::move(b)), program_(std::move(program)), kind_(kind), desc_(desc), flags_(flags),
          range_(range) {}

    int frameCount() const override { return 1; }
    bool usesDvMemory() const override { return isDigital(kind_); }
    DigitalMove digitalMove() const override {
        if (!isDigital(kind_)) return DigitalMove::None;
        return desc_ == 2 ? DigitalMove::B : DigitalMove::A;
    }
    std::optional<PositionRange> positionRange() const override { return range_; }

    void render(int tbar, const Frame& a, const Frame& b, Frame& out, const RenderCtx& ctx) const override {
        tbar = clampTBar(tbar);
        // Ruling 7: in Auto, N is the running Auto's own tick count.
        const int n = ctx.autoStep >= 1 && ctx.autoSteps > 0
                          ? ctx.autoSteps
                          : basics().timing.durationTicks[size_t(ctx.speed.value_or(Speed::Medium))];
        // A new take starts from a fresh VM: install + init (ALGO.md 15.1), its colour latched.
        if (!vm_ || ctx.take != take_) {
            colour_ = algoColourIndex(ctx.colour);
            vm_.emplace(program_, kind_, colour_);
            take_ = ctx.take;
            lastKey_ = INT_MIN;  // Ruling 2: no key equals it, so the take's first render steps
            applied_ = {};       // the shipped program is the centred default (POSITION.md 4)
        }
        // A new position rewrites the parameters (no allocation) and steps the T-bar position again,
        // as the mouse loop's 0xb71b2 does.
        if (range_ && !(ctx.position == applied_)) {
            algoPositionProgram(vm_->program(), ctx.position.h, ctx.position.v, ctx.position.bakedAt);
            applied_ = ctx.position;
            lastKey_ = INT_MIN;
        }
        // The same step rendered again (the T-bar held while ticks render) does not step the VM.
        const int key = ctx.autoStep >= 1 ? ctx.autoStep : -1 - tbar;
        if (key != lastKey_) {
            uint32_t t = ctx.autoStep >= 1 ? algoAutoT(ctx.autoStep, n, flags_) : algoTBarT(tbar, flags_);
            if (range_ && ctx.autoStep >= 1 && ctx.position.stop) t = std::min(t, ctx.position.stop);  // ALGO.md 15.3
            step_ = &vm_->step(t);
            lastKey_ = key;
        }
        const AlgoStep& s = *step_;
        const bool second = algoSecondStep(tbar, ctx.autoStep, n, flags_);
        auto matteOf = [&](Rgba8 c) -> const Frame& {
            std::fill(matte_.px.begin(), matte_.px.end(), c);
            return matte_;
        };
        auto matte = [&]() -> const Frame& { return matteOf(ctx.background); };

        if (isDigital(kind_)) {
            DveControl control = algoControl(s);
            // ALGO.md 15.5: the colour is latched at take init, so the top/bottom border always
            // agrees with the VM's left/right one.
            control.borderFill = uint8_t(algoBorderFill(colour_));
            // The moving picture comes through the DV path (delayed while live); the background
            // stays analog, desc 5's too.
            const Frame& da = ctx.digitalA ? *ctx.digitalA : a;
            const Frame& db = ctx.digitalB ? *ctx.digitalB : b;
            const Frame* moving = &da;
            const Frame* background = &b;
            switch (desc_) {
                case 2: moving = &db; background = &a; break;
                case 3: break;
                case 5:
                    moving = second ? &db : &da;
                    background = second ? &b : &a;
                    break;
                default: moving = second ? &db : &da; background = &matte(); break;  // 6: BKG matte
            }
            if (ctx.keyFill) background = &matteOf(*ctx.keyFill);  // key mode: the fill replaces any bus
            renderDve(control, *moving, *background, {}, out, ctx.guesses);
            // ALGO.md 15.4 (GUESS): the level fades the key-0 background toward the composite;
            // in key mode the mixer is in key mode too, so no level.
            if (s.level >= 0 && !ctx.keyFill) mixLevel(*background, out, s.level, out);
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
    std::optional<PositionRange> range_;  // kinds 3 / 4 with a usable header
    mutable std::optional<AlgoVm> vm_;  // the current take's VM
    mutable EffectPosition applied_;    // the position vm_'s program holds
    mutable uint64_t take_ = 0;
    mutable int colour_ = 0;            // the Effect Color index latched at this take's start
    mutable int lastKey_ = INT_MIN;     // ctx.autoStep if >= 1, else -1 - tbar
    mutable const AlgoStep* step_ = nullptr;  // vm_'s last step
    mutable Frame matte_;
};

// The Art Card Key crouton (kind 5): no program runs. The Switcher composes the card itself
// (vt/switcher.h); render only passes A through.
class ArtCardKeyEffect final : public CroutonEffect {
public:
    using CroutonEffect::CroutonEffect;
    int frameCount() const override { return 1; }
    bool isArtCardKey() const override { return true; }
    void render(int, const Frame& a, const Frame&, Frame& out, const RenderCtx&) const override { out = a; }
};

}  // namespace

std::unique_ptr<Effect> makeAlgoEffect(CroutonBasics b, const Crouton& c, std::string* why) {
    auto fail = [&](std::string reason) -> std::unique_ptr<Effect> {
        if (why) *why = std::move(reason);
        return nullptr;
    };
    const int kind = int(c.tag(kTagKind).value_or(0));
    if (kind == 5) return std::make_unique<ArtCardKeyEffect>(std::move(b));
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
    // Kinds 3 / 4: the probe again at the four corners of the mouse range; a program that fails
    // there plays unpositioned.
    std::optional<PositionRange> range = kind == 3 || kind == 4 ? algoPositionRange(it->second) : std::nullopt;
    if (range) {
        try {
            for (int h : {-range->halfH, range->halfH - 1})
                for (int v : {-range->halfV, range->halfV - 1}) {
                    std::vector<uint8_t> p = it->second;
                    algoPositionProgram(p, h, v, -1);
                    AlgoVm vm(std::move(p), kind, 0);
                    for (int pos = 0; pos <= 511; ++pos) vm.step(algoTBarT(pos, flags));
                }
        } catch (const std::exception&) {
            range.reset();
        }
    }
    const bool routed = isDigital(kind) ? desc == 2 || desc == 3 || desc == 5 || desc == 6
                        : kind == 2     ? desc == 1 || desc == 8
                                        : desc == 4;
    if (!routed) return fail("ALGO routing descriptor " + std::to_string(desc) + " not supported");
    return std::make_unique<AlgoEffect>(std::move(b), it->second, kind, desc, flags, range);
}

}  // namespace vt::detail
