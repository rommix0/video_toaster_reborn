#include <algorithm>
#include <memory>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/background.h"
#include "vt/effect.h"
#include "vt/switcher.h"

namespace {

using vt::Bus;
using vt::Source;

const vt::Rgba8 kRed{200, 20, 20, 255}, kGreen{20, 200, 20, 255};

vt::Frame solid(vt::Rgba8 c) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), c);
    return f;
}

// A busy picture, so recolouring and wipes have something to show.
vt::Frame ramp(int seed) {
    vt::Frame f;
    for (size_t i = 0; i < f.px.size(); ++i)
        f.px[i] = {uint8_t(i * 7 + seed), uint8_t((i >> 5) + seed * 3), uint8_t((i >> 2) ^ seed), 255};
    return f;
}

std::shared_ptr<const vt::Effect> load(const char* rel) {
    std::string why;
    std::shared_ptr<const vt::Effect> e = vt::loadEffect(dataRoot() / std::filesystem::path(rel), &why);
    INFO(rel << ": " << why);
    REQUIRE(e);
    return e;
}

std::shared_ptr<const vt::Effect> pagePeel() { return load("4.2/Effects/4000FX/PagePeel/Page Peel !"); }
std::shared_ptr<const vt::Effect> dve() { return load("4.2/Effects/2000FX/Blinds/Split Corners In"); }
std::shared_ptr<const vt::Effect> fadeToNeg() { return load("4.2/Effects/ChromaFx/Fade To Neg"); }
std::shared_ptr<const vt::Effect> neonCycle() { return load("4.2/Effects/ChromaFx/Neon Cycle"); }
std::shared_ptr<const vt::Effect> dissolve() { return std::shared_ptr<const vt::Effect>(vt::makeDissolve()); }

struct Rig {
    vt::Frame in1 = solid(kRed), in2 = solid(kGreen), in3 = ramp(1), in4 = ramp(2);
    vt::SourceFrames src{{&in1, &in2, &in3, &in4}};
    vt::Switcher sw;
    vt::Frame render() const {
        vt::Frame out;
        sw.render(src, out);
        return out;
    }
};

// What the effect itself gives for these inputs.
vt::Frame direct(const vt::Effect& e, int tbar, const vt::Frame& a, const vt::Frame& b, uint32_t frameNumber = 0) {
    vt::Frame out;
    vt::RenderCtx ctx;
    ctx.frameNumber = frameNumber;
    e.render(tbar, a, b, out, ctx);
    return out;
}

// A dissolve that has a sound, for pendingSound.
class Beep final : public vt::Effect {
public:
    Beep() {
        snd_.rate = 8000;
        snd_.channels = 1;
        snd_.pcm = {1, 2, 3};
        inner_ = vt::makeDissolve();
    }
    std::string name() const override { return "Beep"; }
    vt::Timing timing() const override { return inner_->timing(); }
    int frameCount() const override { return 1; }
    void render(int t, const vt::Frame& a, const vt::Frame& b, vt::Frame& o, const vt::RenderCtx& c) const override {
        inner_->render(t, a, b, o, c);
    }
    const vt::Sound* sound() const override { return &snd_; }

private:
    vt::Sound snd_;
    std::unique_ptr<vt::Effect> inner_;
};

}  // namespace

TEST_CASE("select and take swap Main and Preview", "[switcher]") {
    Rig r;
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::DV1);
    r.sw.select(Bus::Overlay, Source::In4);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::DV1);
    REQUIRE(r.sw.selected(Bus::Overlay) == Source::In4);
    r.sw.take();
    REQUIRE(r.sw.selected(Bus::Main) == Source::DV1);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In3);
    REQUIRE(r.sw.selected(Bus::Overlay) == Source::In4);

    // Program is the Main source, plainly; the Overlay bus is ignored in version 1
    r.sw.select(Bus::Main, Source::In1);
    REQUIRE(r.render().px == r.in1.px);
    r.sw.select(Bus::Overlay, Source::In2);
    REQUIRE(r.render().px == r.in1.px);
    // DV1 and DV2 are black until framestores exist; a null input is black too
    r.sw.select(Bus::Main, Source::DV2);
    REQUIRE(r.render().px == solid({0, 0, 0, 255}).px);
    r.src.inputs[0] = nullptr;
    r.sw.select(Bus::Main, Source::In1);
    REQUIRE(r.render().px == solid({0, 0, 0, 255}).px);
}

TEST_CASE("take ends a transition in progress", "[switcher]") {
    Rig r;
    r.sw.setTBar(200);
    r.sw.take();
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
    REQUIRE(r.render().px == r.in2.px);
}

TEST_CASE("Auto Fast on Page Peel completes in exactly 30 ticks", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.setEffect(pagePeel()));
    REQUIRE(r.sw.effect().timing().durationFrames[size_t(vt::Speed::Fast)] == 30);
    r.sw.select(Bus::Main, Source::In1);
    r.sw.select(Bus::Preview, Source::In2);
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.autoRunning());
    int last = 0;
    for (int i = 1; i <= 29; ++i) {
        r.sw.tick();
        INFO("tick " << i);
        REQUIRE(r.sw.tbar() > last);  // constant rate: strictly rising
        REQUIRE(r.sw.tbar() < 511);
        last = r.sw.tbar();
        REQUIRE(r.sw.selected(Bus::Main) == Source::In1);
        REQUIRE(r.sw.autoRunning());
    }
    r.sw.tick();  // the 30th
    REQUIRE_FALSE(r.sw.autoRunning());
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In1);
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.render().px == r.in2.px);  // the new Main, plainly
    r.sw.tick();                         // idle ticks change nothing
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
}

TEST_CASE("Auto duration follows the speed and the constant rate", "[switcher]") {
    Rig r;  // dissolve: Slow 90, Medium 60, Fast 30, Variable 60
    int n = 0;
    r.sw.autoTransition(vt::Speed::Slow);
    while (r.sw.autoRunning()) {
        r.sw.tick();
        ++n;
        REQUIRE(n <= 90);
    }
    REQUIRE(n == 90);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);

    // from halfway: about half the trip
    Rig h;
    h.sw.setTBar(256);
    h.sw.autoTransition(vt::Speed::Fast);
    n = 0;
    while (h.sw.autoRunning()) {
        h.sw.tick();
        ++n;
        REQUIRE(n <= 30);
    }
    REQUIRE(n == 15);
    REQUIRE(h.sw.selected(Bus::Main) == Source::In2);

    // Cut is one frame at every speed
    Rig c;
    REQUIRE(c.sw.setEffect(std::shared_ptr<const vt::Effect>(vt::makeCut())));
    c.sw.autoTransition(vt::Speed::Slow);
    c.sw.tick();
    REQUIRE(c.sw.selected(Bus::Main) == Source::In2);
    REQUIRE(c.sw.tbar() == 0);
}

TEST_CASE("the T-bar by hand", "[switcher]") {
    Rig r;
    r.sw.setTBar(-5);
    REQUIRE(r.sw.tbar() == 0);
    r.sw.setTBar(300);
    REQUIRE(r.sw.tbar() == 300);
    REQUIRE(r.sw.transitionActive());
    // back to 0 mid-way: nothing swaps
    r.sw.setTBar(0);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In1);
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.render().px == r.in1.px);
    // pushed to the end: swap, and the T-bar reads 0 again
    r.sw.setTBar(900);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In1);
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.render().px == r.in2.px);
    // a manual move stops Auto
    r.sw.autoTransition(vt::Speed::Fast);
    r.sw.tick();
    r.sw.setTBar(100);
    REQUIRE_FALSE(r.sw.autoRunning());
    r.sw.tick();
    REQUIRE(r.sw.tbar() == 100);
}

TEST_CASE("T-bar moved back renders what a fresh render does (Review Focus 5)", "[switcher]") {
    for (const auto& effect : {pagePeel(), dve(), dissolve()}) {
        Rig back, fresh;
        REQUIRE(back.sw.setEffect(effect));
        REQUIRE(fresh.sw.setEffect(effect));
        for (Rig* r : {&back, &fresh}) {
            r->sw.select(Bus::Main, Source::In3);
            r->sw.select(Bus::Preview, Source::In4);
        }
        back.sw.setTBar(400);
        (void)back.render();
        back.sw.tick();
        back.sw.setTBar(100);
        fresh.sw.setTBar(100);
        INFO(effect->name());
        const vt::Frame a = back.render();
        REQUIRE(a.px == fresh.render().px);
        REQUIRE(a.px == direct(*effect, 100, back.in3, back.in4).px);
        REQUIRE(a.px != back.in3.px);
    }
}

TEST_CASE("setEffect and the DV memory (Review Focus 4)", "[switcher]") {
    // a DVE move is refused while ChromaFX is engaged
    Rig r;
    REQUIRE(r.sw.setEffect(fadeToNeg()));
    r.sw.setTBar(100);
    REQUIRE(r.sw.chromaEngaged());
    std::string why;
    REQUIRE_FALSE(r.sw.setEffect(dve(), &why));
    REQUIRE_FALSE(why.empty());
    REQUIRE(r.sw.effect().name() == fadeToNeg()->name());
    REQUIRE(r.sw.chromaEngaged());
    // another ChromaFX is a DV-memory effect too
    why.clear();
    REQUIRE_FALSE(r.sw.setEffect(neonCycle(), &why));
    REQUIRE_FALSE(why.empty());
    // a null pointer is refused as well; a dissolve is not
    why.clear();
    REQUIRE_FALSE(r.sw.setEffect(nullptr, &why));
    REQUIRE_FALSE(why.empty());
    REQUIRE(r.sw.setEffect(dissolve()));
    REQUIRE_FALSE(r.sw.chromaEngaged());  // selecting another effect disengages
    REQUIRE(r.sw.tbar() == 0);
    // once disengaged the DVE is fine
    REQUIRE(r.sw.setEffect(dve()));

    // ChromaFX selected but not engaged does not block a DVE
    Rig idle;
    REQUIRE(idle.sw.setEffect(fadeToNeg()));
    REQUIRE(idle.sw.setEffect(dve()));

    // a DVE mid-transition refuses another DV-memory effect (and keeps running)
    Rig m;
    REQUIRE(m.sw.setEffect(dve()));
    m.sw.setTBar(200);
    REQUIRE_FALSE(m.sw.setEffect(fadeToNeg(), &why));
    REQUIRE(m.sw.effect().name() == dve()->name());
    REQUIRE(m.sw.tbar() == 200);
    why.clear();
    REQUIRE_FALSE(m.sw.setEffect(dve(), &why));
    REQUIRE_FALSE(why.empty());
    // ... but plain effects replace it, abandoning the move without a swap
    REQUIRE(m.sw.setEffect(pagePeel()));
    REQUIRE(m.sw.tbar() == 0);
    REQUIRE(m.sw.selected(Bus::Main) == Source::In1);
    // a DVE that finished is not mid-transition
    Rig f;
    REQUIRE(f.sw.setEffect(dve()));
    f.sw.setTBar(511);
    REQUIRE(f.sw.setEffect(fadeToNeg()));
}

TEST_CASE("ChromaFX stays engaged after Auto, without a swap (Ruling 21)", "[switcher][chromafx]") {
    Rig r;
    const auto fx = fadeToNeg();
    REQUIRE(r.sw.setEffect(fx));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    REQUIRE(r.render().px == r.in3.px);  // not engaged: plain Main
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.chromaEngaged());
    const int d = fx->timing().durationFrames[size_t(fx->timing().resolve(vt::Speed::Fast))];
    for (int i = 0; i < d; ++i) {
        REQUIRE(r.sw.autoRunning());
        r.sw.tick();
    }
    REQUIRE_FALSE(r.sw.autoRunning());
    REQUIRE(r.sw.chromaEngaged());
    REQUIRE(r.sw.tbar() == 511);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In4);
    // Program shows the recoloured Preview source (the effect's render takes B)
    const vt::Frame shown = r.render();
    REQUIRE(shown.px == direct(*fx, 511, r.in3, r.in4, uint32_t(d)).px);
    REQUIRE(shown.px != r.in4.px);
    REQUIRE(shown.px != r.in3.px);
    // further ticks leave it engaged
    r.sw.tick();
    REQUIRE(r.sw.chromaEngaged());
    // Take disengages (and cuts)
    r.sw.take();
    REQUIRE_FALSE(r.sw.chromaEngaged());
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
    REQUIRE(r.render().px == r.in4.px);
}

TEST_CASE("a Transition ChromaFX disengages when the T-bar returns to 0", "[switcher][chromafx]") {
    Rig r;
    REQUIRE(r.sw.setEffect(fadeToNeg()));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.setTBar(300);
    REQUIRE(r.sw.chromaEngaged());
    REQUIRE(r.render().px == direct(*fadeToNeg(), 300, r.in3, r.in4).px);
    r.sw.setTBar(511);  // by hand to the end: still no swap
    REQUIRE(r.sw.chromaEngaged());
    REQUIRE(r.sw.tbar() == 511);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    r.sw.setTBar(0);
    REQUIRE_FALSE(r.sw.chromaEngaged());
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    REQUIRE(r.render().px == r.in3.px);
}

TEST_CASE("a cycling ChromaFX advances every tick from engagement", "[switcher][chromafx]") {
    Rig r;
    const auto fx = neonCycle();
    REQUIRE(r.sw.setEffect(fx));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.setTBar(1);  // engage
    REQUIRE(r.sw.chromaEngaged());
    REQUIRE(r.render().px == direct(*fx, 1, r.in3, r.in4, 0).px);
    vt::Frame prev = r.render();
    int changes = 0;
    for (uint32_t n = 1; n <= 600; ++n) {
        r.sw.tick();
        const vt::Frame now = r.render();
        INFO("tick " << n);
        REQUIRE(now.px == direct(*fx, 1, r.in3, r.in4, n).px);
        if (now.px != prev.px) ++changes;
        prev = now;
    }
    REQUIRE(changes > 10);  // the picture moves with time even though the T-bar does not
    // the counter resets on re-engagement
    r.sw.setTBar(0);
    REQUIRE_FALSE(r.sw.chromaEngaged());
    r.sw.setTBar(1);
    REQUIRE(r.render().px == direct(*fx, 1, r.in3, r.in4, 0).px);
    // idle (not engaged) ticks do not count
    Rig q;
    REQUIRE(q.sw.setEffect(fx));
    for (int i = 0; i < 50; ++i) q.sw.tick();
    q.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(q.render().px == direct(*fx, 0, q.in1, q.in2, 0).px);
}

TEST_CASE("pendingSound hands the sound over once per transition", "[switcher]") {
    Rig r;
    const auto beep = std::make_shared<Beep>();
    REQUIRE(r.sw.setEffect(beep));
    REQUIRE(r.sw.pendingSound() == nullptr);
    r.sw.setTBar(10);
    REQUIRE(r.sw.pendingSound() == beep->sound());
    REQUIRE(r.sw.pendingSound() == nullptr);
    r.sw.setTBar(300);  // same transition
    REQUIRE(r.sw.pendingSound() == nullptr);
    r.sw.setTBar(0);
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.pendingSound() == beep->sound());
    r.sw.tick();
    REQUIRE(r.sw.pendingSound() == nullptr);
    while (r.sw.autoRunning()) r.sw.tick();
    REQUIRE(r.sw.pendingSound() == nullptr);
    // a cut is not a transition
    r.sw.take();
    REQUIRE(r.sw.pendingSound() == nullptr);
    // an effect without sound gives nullptr
    REQUIRE(r.sw.setEffect(dissolve()));
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.pendingSound() == nullptr);
}

TEST_CASE("BKG renders the background colour", "[switcher]") {
    Rig r;
    r.sw.select(Bus::Main, Source::Bkg);
    REQUIRE(r.render().px == solid(vt::bkgColour(vt::BkgColour::Black)).px);
    for (auto c : {vt::BkgColour::Red, vt::BkgColour::White, vt::BkgColour::Cyan}) {
        r.sw.setBackground(c);
        REQUIRE(r.render().px == solid(vt::bkgColour(c)).px);
    }
    r.sw.setCustomBackground(0x0f80);  // 12-bit 0RGB approximation, no snow bit
    REQUIRE(r.render().px == solid(vt::customBkgColour(0x0f80)).px);
    // Snow is per-tick noise
    r.sw.setBackground(vt::BkgColour::Snow);
    vt::Frame expect;
    vt::renderSnow(expect, 0);
    REQUIRE(r.render().px == expect.px);
    r.sw.tick();
    vt::renderSnow(expect, 1);
    REQUIRE(r.render().px == expect.px);
    r.sw.setCustomBackground(0x8000);  // bit 15: snow
    REQUIRE(r.render().px == expect.px);
}

TEST_CASE("effects see the background colour", "[switcher]") {
    Rig r;
    r.sw.setBackground(vt::BkgColour::Blue);
    REQUIRE(r.sw.setEffect(pagePeel()));
    r.sw.setTBar(250);
    vt::Frame expect;
    vt::RenderCtx ctx;
    ctx.background = vt::bkgColour(vt::BkgColour::Blue);
    pagePeel()->render(250, r.in1, r.in2, expect, ctx);
    REQUIRE(r.render().px == expect.px);
}

TEST_CASE("render may write into an input frame", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.setEffect(dve()));  // DVE render must not alias its inputs
    r.sw.setTBar(150);
    const vt::Frame expect = r.render();
    r.sw.render(r.src, r.in1);  // Program lands in the A source's own frame
    REQUIRE(r.in1.px == expect.px);
}
