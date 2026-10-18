#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/background.h"
#include "vt/chromafx.h"
#include "vt/crouton.h"
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
vt::Frame direct(const vt::Effect& e, int tbar, const vt::Frame& a, const vt::Frame& b, uint32_t frameNumber = 0,
                 int animFrame = -1) {
    vt::Frame out;
    vt::RenderCtx ctx;
    ctx.frameNumber = frameNumber;
    ctx.animFrame = animFrame;
    e.render(tbar, a, b, out, ctx);
    return out;
}

// Forwards to a real effect and records what the Switcher asked for.
class Recorder final : public vt::Effect {
public:
    explicit Recorder(std::shared_ptr<const vt::Effect> e) : e_(std::move(e)) {}
    std::string name() const override { return e_->name(); }
    vt::Timing timing() const override { return e_->timing(); }
    int frameCount() const override { return e_->frameCount(); }
    void render(int t, const vt::Frame& a, const vt::Frame& b, vt::Frame& o, const vt::RenderCtx& c) const override {
        lastTBar = t;
        lastCtx = c;
        e_->render(t, a, b, o, c);
    }
    mutable int lastTBar = -1;
    mutable vt::RenderCtx lastCtx;

private:
    std::shared_ptr<const vt::Effect> e_;
};

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

TEST_CASE("Auto Fast on Page Peel ! is one second: 60 ticks, every frame once (manual anchor)", "[switcher]") {
    // Manual, Tutorials p24: Page Peel at Fast lasts 1 second; a tick is one 59.94 fps frame.
    Rig r;
    const auto peel = pagePeel();
    REQUIRE(peel->frameCount() == 60);
    REQUIRE(r.sw.setEffect(peel));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.autoTransition(vt::Speed::Fast);
    int last = -1;
    for (int i = 0; i < 60; ++i) {  // render, then tick (switcher.h)
        INFO("tick " << i);
        REQUIRE(r.sw.autoRunning());
        REQUIRE(r.sw.tbar() > last);  // constant rate: strictly rising, from 0
        REQUIRE(r.sw.tbar() < 511);
        if (i == 0) REQUIRE(r.sw.tbar() == 0);
        last = r.sw.tbar();
        // animation frame i, exactly: none dropped, none repeated
        REQUIRE(r.render().px == direct(*peel, r.sw.tbar(), r.in3, r.in4, 0, i).px);
        REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
        r.sw.tick();
    }
    REQUIRE_FALSE(r.sw.autoRunning());
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In3);
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.render().px == r.in4.px);  // the new Main, plainly
    r.sw.tick();                         // idle ticks change nothing
    REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
}

TEST_CASE("Auto Fast on Toddler-R takes 84 ticks: Length 00:00:01:12 (manual PDF 178)", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.setEffect(load("4.2/Effects/2000FX/People/Toddler-R")));
    r.sw.autoTransition(vt::Speed::Fast);
    int n = 0;
    while (r.sw.autoRunning()) {
        r.sw.tick();
        REQUIRE(++n <= 84);
    }
    REQUIRE(n == 84);  // 42 frames at 30 fps
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
}

TEST_CASE("during Auto every animation frame is shown for exactly fieldsPerFrame ticks", "[switcher]") {
    // Forked Lightning: 25 frames, 3/2/1 fields per frame at Slow/Medium/Fast.
    const auto inner = load("4.2/Effects/2000FX/Organic/Forked Lightning");
    const int n = inner->frameCount();
    for (vt::Speed s : {vt::Speed::Slow, vt::Speed::Medium, vt::Speed::Fast}) {
        const int fpf = inner->timing().fieldsPerFrame[size_t(s)];
        INFO("speed " << int(s) << ", " << fpf << " fields per frame");
        REQUIRE(fpf == 3 - int(s));
        Rig r;
        const auto rec = std::make_shared<Recorder>(inner);
        REQUIRE(r.sw.setEffect(rec));
        r.sw.autoTransition(s);
        std::vector<int> shown;
        while (r.sw.autoRunning()) {
            (void)r.render();
            shown.push_back(rec->lastCtx.animFrame);
            r.sw.tick();
            REQUIRE(shown.size() <= size_t(n * fpf));
        }
        REQUIRE(shown.size() == size_t(n * fpf));
        for (size_t i = 0; i < shown.size(); ++i) {
            INFO("tick " << i);
            REQUIRE(shown[i] == int(i) / fpf);
        }
    }
    // a hand-driven T-bar leaves the frame to the T-bar (animFrameForTBar)
    Rig h;
    const auto rec = std::make_shared<Recorder>(inner);
    REQUIRE(h.sw.setEffect(rec));
    h.sw.setTBar(300);
    (void)h.render();
    REQUIRE(rec->lastCtx.animFrame == -1);
    REQUIRE(rec->lastTBar == 300);
}

TEST_CASE("Auto duration follows the speed and the constant rate", "[switcher]") {
    Rig r;  // dissolve: Slow 90, Medium 60, Fast 30, Variable 60 frames at 29.97 fps -> twice in ticks
    REQUIRE(r.sw.effect().timing().durationTicks == std::array{180, 120, 60, 120});
    int n = 0;
    r.sw.autoTransition(vt::Speed::Slow);
    REQUIRE(r.render().px == r.in1.px);  // the first frame is the T-bar at 0: all A
    while (r.sw.autoRunning()) {
        r.sw.tick();
        ++n;
        REQUIRE(n <= 180);
    }
    REQUIRE(n == 180);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);

    // from halfway: the remaining half of the trip
    Rig h;
    h.sw.setTBar(256);
    h.sw.autoTransition(vt::Speed::Fast);
    n = 0;
    while (h.sw.autoRunning()) {
        h.sw.tick();
        ++n;
        REQUIRE(n <= 60);
    }
    REQUIRE(n == 30);
    REQUIRE(h.sw.selected(Bus::Main) == Source::In2);

    // Cut is one 29.97 fps frame (two ticks) at every speed: A, then B, then the swap
    Rig c;
    REQUIRE(c.sw.setEffect(std::shared_ptr<const vt::Effect>(vt::makeCut())));
    c.sw.autoTransition(vt::Speed::Slow);
    REQUIRE(c.render().px == c.in1.px);
    c.sw.tick();
    REQUIRE(c.sw.autoRunning());
    REQUIRE(c.render().px == c.in2.px);
    c.sw.tick();
    REQUIRE_FALSE(c.sw.autoRunning());
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
    // another ChromaFX replaces the engaged one (Ruling 23): it starts disengaged, T-bar 0
    REQUIRE(r.sw.setEffect(neonCycle(), &why));
    REQUIRE(r.sw.effect().name() == neonCycle()->name());
    REQUIRE_FALSE(r.sw.chromaEngaged());
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.sw.tbar() == 0);
    r.sw.setTBar(100);
    REQUIRE(r.sw.chromaEngaged());
    // ChromaFX -> DVE stays refused
    why.clear();
    REQUIRE_FALSE(r.sw.setEffect(dve(), &why));
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

TEST_CASE("selecting a ChromaFX replaces an engaged one and resets its counter (Ruling 23)", "[switcher][chromafx]") {
    Rig r;
    const auto fx = neonCycle();
    REQUIRE(r.sw.setEffect(fx));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.setTBar(1);
    for (int i = 0; i < 40; ++i) r.sw.tick();
    REQUIRE(r.render().px == direct(*fx, 1, r.in3, r.in4, 40).px);
    REQUIRE(r.sw.setEffect(fx));  // even the same crouton again
    REQUIRE_FALSE(r.sw.chromaEngaged());
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.render().px == r.in3.px);  // plain Main
    r.sw.setTBar(1);
    REQUIRE(r.render().px == direct(*fx, 1, r.in3, r.in4, 0).px);  // counter restarted
    // a Take-less swap of Main/Preview did not happen
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
}

TEST_CASE("select is live during a transition", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.setEffect(dissolve()));
    r.sw.select(Bus::Main, Source::In1);
    r.sw.select(Bus::Preview, Source::In2);
    r.sw.setTBar(200);
    REQUIRE(r.render().px == direct(*dissolve(), 200, r.in1, r.in2).px);
    r.sw.select(Bus::Preview, Source::In4);  // the next render already blends towards In4
    REQUIRE(r.render().px == direct(*dissolve(), 200, r.in1, r.in4).px);
    r.sw.setTBar(511);  // the 511 swap exchanges what is selected at that moment
    REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In1);
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
    const int d = fx->timing().durationTicks[size_t(fx->timing().resolve(vt::Speed::Fast))];
    REQUIRE(d == 2 * fx->timing().durationFrames[size_t(fx->timing().resolve(vt::Speed::Fast))]);
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
    REQUIRE(r.sw.pendingSound().get() == beep->sound());
    REQUIRE(r.sw.pendingSound() == nullptr);
    r.sw.setTBar(300);  // same transition
    REQUIRE(r.sw.pendingSound() == nullptr);
    r.sw.setTBar(0);
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.pendingSound().get() == beep->sound());
    r.sw.tick();
    REQUIRE(r.sw.pendingSound() == nullptr);
    while (r.sw.autoRunning()) r.sw.tick();
    REQUIRE(r.sw.pendingSound() == nullptr);
    // a cut is not a transition
    r.sw.take();
    REQUIRE(r.sw.pendingSound() == nullptr);
    // a Take drops a sound nobody collected
    r.sw.setTBar(10);
    r.sw.take();
    REQUIRE(r.sw.pendingSound() == nullptr);
    // an effect without sound gives nullptr
    REQUIRE(r.sw.setEffect(dissolve()));
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.pendingSound() == nullptr);
}

TEST_CASE("a pending sound outlives its effect (Ruling 28)", "[switcher]") {
    Rig r;
    auto beep = std::make_shared<Beep>();
    const vt::Sound* raw = beep->sound();
    REQUIRE(r.sw.setEffect(beep));
    beep.reset();  // the Switcher holds the last reference
    r.sw.setTBar(10);
    const std::shared_ptr<const vt::Sound> snd = r.sw.pendingSound();
    REQUIRE(snd.get() == raw);
    REQUIRE(r.sw.setEffect(dissolve()));  // the Switcher lets go of Beep
    REQUIRE(snd->rate == 8000);
    REQUIRE(snd->pcm == std::vector<int16_t>{1, 2, 3});
}

TEST_CASE("the speed button: the effect's default, setSpeed, autoTransition (Ruling 29)", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.speed() == vt::Speed::Medium);  // the dissolve's default
    REQUIRE(r.sw.setEffect(pagePeel()));
    REQUIRE(r.sw.speed() == vt::Speed::Fast);  // tag 0x55 default, as the Switcher loads it
    r.sw.setSpeed(vt::Speed::Medium);
    REQUIRE(r.sw.speed() == vt::Speed::Medium);
    // the 2000FX Page Peel (54 frames) has no Slow button: Auto runs at its default, Fast
    REQUIRE(r.sw.setEffect(load("4.2/Effects/2000FX/PagePeel/Page Peel")));
    r.sw.autoTransition(vt::Speed::Slow);
    REQUIRE(r.sw.speed() == vt::Speed::Slow);
    int n = 0;
    while (r.sw.autoRunning()) {
        r.sw.tick();
        REQUIRE(++n <= 54);
    }
    REQUIRE(n == 54);
    // the render context carries the resolved speed
    Rig q;
    const auto rec = std::make_shared<Recorder>(pagePeel());
    REQUIRE(q.sw.setEffect(rec));
    q.sw.setSpeed(vt::Speed::Variable);  // Page Peel ! has no Variable button: its default
    q.sw.setTBar(100);
    (void)q.render();
    REQUIRE_FALSE(pagePeel()->timing().available(vt::Speed::Variable));
    REQUIRE(rec->lastCtx.speed == vt::Speed::Fast);
    q.sw.setSpeed(vt::Speed::Medium);
    (void)q.render();
    REQUIRE(rec->lastCtx.speed == vt::Speed::Medium);
}

TEST_CASE("a cycling ChromaFX cycles at the speed button's rate (FINDINGS)", "[switcher][chromafx]") {
    // LUT_GridCycle*: Slow 0.25, Medium 0.5, Fast 1, Variable 0.5 rows per field (16.16 below)
    const auto fx = neonCycle();  // Transition, cycling forward, crouton default Slow
    const auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/ChromaFx/Neon Cycle");
    REQUIRE(c);
    const auto lut = vt::lutFromCrouton(*c);
    REQUIRE(lut);
    const auto rows = vt::buildRows(*lut, vt::chromaInterp(*lut));
    const std::pair<vt::Speed, uint32_t> rates[] = {
        {vt::Speed::Slow, 0x4000}, {vt::Speed::Medium, 0x8000}, {vt::Speed::Fast, 0x10000}, {vt::Speed::Variable, 0x8000}};
    for (const auto& [speed, step] : rates) {
        INFO("speed " << int(speed));
        Rig r;
        REQUIRE(r.sw.setEffect(fx));
        REQUIRE(fx->timing().resolve(speed) == speed);
        r.sw.setSpeed(speed);
        r.sw.select(Bus::Main, Source::In3);
        r.sw.select(Bus::Preview, Source::In4);
        r.sw.setTBar(1);  // engage
        uint32_t ticks = 0;
        for (uint32_t n : {0u, 3u, 4u, 9u, 120u, 479u}) {
            while (ticks < n) {
                r.sw.tick();
                ++ticks;
            }
            vt::Frame expect;
            vt::applyChroma(rows, vt::ChromaMode::Transition, vt::chromaCycleRow(vt::ChromaCycle::Forward, step, n),
                            r.in4, expect);
            INFO("tick " << n);
            REQUIRE(r.render().px == expect.px);
        }
    }
}

TEST_CASE("setGuesses reaches the effects (Ruling 29)", "[switcher]") {
    Rig r;
    const auto fx = fadeToNeg();
    REQUIRE(r.sw.setEffect(fx));
    REQUIRE(r.sw.guesses().chromaLumaOffset == vt::HardwareGuesses{}.chromaLumaOffset);
    vt::HardwareGuesses g;
    g.chromaLumaOffset = 0.0f;
    r.sw.setGuesses(g);
    REQUIRE(r.sw.guesses().chromaLumaOffset == 0.0f);
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.setTBar(200);
    vt::Frame expect;
    vt::RenderCtx ctx;
    ctx.guesses = g;
    fx->render(200, r.in3, r.in4, expect, ctx);
    const vt::Frame shown = r.render();
    REQUIRE(shown.px == expect.px);
    REQUIRE(shown.px != direct(*fx, 200, r.in3, r.in4).px);  // the default guess differs
}

TEST_CASE("reverse Auto runs back to 0 at the same rate, without a swap (Ruling 30)", "[switcher]") {
    Rig r;  // dissolve, Fast = 60 ticks for 0..511
    r.sw.setTBar(256);
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Reverse);
    int n = 0, last = 256;
    while (r.sw.autoRunning()) {
        REQUIRE(r.render().px == direct(*dissolve(), r.sw.tbar(), r.in1, r.in2).px);
        r.sw.tick();
        ++n;
        REQUIRE(r.sw.tbar() < last);
        last = r.sw.tbar();
        REQUIRE(n <= 60);
    }
    REQUIRE(n == (256 * 60 + 510) / 511);  // the remaining fraction, rounded up
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In1);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In2);
    REQUIRE_FALSE(r.sw.transitionActive());
    REQUIRE(r.render().px == r.in1.px);
    // at 0 there is nothing to run back
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Reverse);
    REQUIRE_FALSE(r.sw.autoRunning());

    // turning a running Auto round: it comes back from where it was, each animation frame again
    // for exactly fieldsPerFrame ticks, and ends at 0 with no swap
    const auto inner = load("4.2/Effects/2000FX/Organic/Forked Lightning");
    const auto rec = std::make_shared<Recorder>(inner);
    Rig t;
    REQUIRE(t.sw.setEffect(rec));
    t.sw.autoTransition(vt::Speed::Medium);  // 2 fields per frame
    for (int i = 0; i < 20; ++i) t.sw.tick();
    t.sw.autoTransition(vt::Speed::Medium, vt::Direction::Reverse);
    std::vector<int> shown;
    while (t.sw.autoRunning()) {
        (void)t.render();
        shown.push_back(rec->lastCtx.animFrame);
        t.sw.tick();
        REQUIRE(shown.size() <= 20);
    }
    REQUIRE(shown.size() == 20);
    for (size_t i = 0; i < shown.size(); ++i) REQUIRE(shown[i] == (19 - int(i)) / 2);
    REQUIRE(t.sw.selected(Bus::Main) == Source::In1);
    REQUIRE_FALSE(t.sw.transitionActive());
}

TEST_CASE("reverse Auto disengages a Transition ChromaFX, not a Filter one (Ruling 30)", "[switcher][chromafx]") {
    for (const char* name : {"Fade To Neg", "Mars Filter"}) {
        INFO(name);
        const auto fx = load((std::string("4.2/Effects/ChromaFx/") + name).c_str());
        const bool filter = fx->isChromaFilter();
        REQUIRE(filter == (std::string(name) == "Mars Filter"));
        Rig r;
        REQUIRE(r.sw.setEffect(fx));
        r.sw.select(Bus::Main, Source::In3);
        r.sw.select(Bus::Preview, Source::In4);
        r.sw.autoTransition(vt::Speed::Fast);
        while (r.sw.autoRunning()) r.sw.tick();
        REQUIRE(r.sw.chromaEngaged());
        REQUIRE(r.sw.tbar() == 511);
        r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Reverse);
        int n = 0;
        while (r.sw.autoRunning()) {
            r.sw.tick();
            REQUIRE(++n <= fx->timing().durationTicks[size_t(vt::Speed::Fast)]);
        }
        REQUIRE(n == fx->timing().durationTicks[size_t(vt::Speed::Fast)]);
        REQUIRE(r.sw.tbar() == 0);
        REQUIRE(r.sw.chromaEngaged() == filter);
        REQUIRE(r.sw.selected(Bus::Main) == Source::In3);  // never a swap
        if (filter)
            REQUIRE(r.render().px == direct(*fx, 0, r.in3, r.in4).px);  // still recolouring, at T-bar 0
        else
            REQUIRE(r.render().px == r.in3.px);
    }
}

TEST_CASE("a Filter ChromaFX stays engaged at T-bar 0", "[switcher][chromafx]") {
    Rig r;
    const auto fx = load("4.2/Effects/ChromaFx/Mars Filter");
    REQUIRE(fx->isChromaFilter());
    REQUIRE(r.sw.setEffect(fx));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.setTBar(300);
    REQUIRE(r.sw.chromaEngaged());
    r.sw.setTBar(0);
    REQUIRE(r.sw.chromaEngaged());
    REQUIRE(r.sw.transitionActive());
    const vt::Frame at0 = r.render();
    REQUIRE(at0.px == direct(*fx, 0, r.in3, r.in4).px);
    REQUIRE(at0.px != r.in3.px);
    // Take turns it off (and cuts)
    r.sw.take();
    REQUIRE_FALSE(r.sw.chromaEngaged());
    REQUIRE(r.render().px == r.in4.px);
}

TEST_CASE("Take during Auto cuts once and stops Auto", "[switcher]") {
    Rig r;
    r.sw.autoTransition(vt::Speed::Fast);
    for (int i = 0; i < 10; ++i) r.sw.tick();
    REQUIRE(r.sw.tbar() > 0);
    r.sw.take();
    REQUIRE_FALSE(r.sw.autoRunning());
    REQUIRE(r.sw.tbar() == 0);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
    for (int i = 0; i < 100; ++i) r.sw.tick();  // no second swap when Auto would have ended
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
    REQUIRE(r.render().px == r.in2.px);
}

TEST_CASE("pressing Auto again while it runs changes nothing", "[switcher]") {
    Rig r;
    r.sw.autoTransition(vt::Speed::Fast);
    int n = 0;
    while (r.sw.autoRunning()) {
        if (n % 7 == 3) r.sw.autoTransition(vt::Speed::Fast);
        r.sw.tick();
        REQUIRE(++n <= 60);
    }
    REQUIRE(n == 60);  // still exactly the full trip
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);  // swapped once
    // after it finished, Auto starts a new transition
    r.sw.autoTransition(vt::Speed::Fast);
    REQUIRE(r.sw.autoRunning());
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

// The effect rendered directly with this matte colour.
vt::Frame withMatte(const vt::Effect& e, int tbar, const vt::Frame& a, const vt::Frame& b, vt::Rgba8 matte) {
    vt::Frame out;
    vt::RenderCtx ctx;
    ctx.background = matte;
    e.render(tbar, a, b, out, ctx);
    return out;
}

TEST_CASE("effects get their own Effect Color, not the BKG colour (Ruling 26)", "[switcher]") {
    const vt::Rgba8 white = vt::bkgColour(vt::BkgColour::White), black = vt::bkgColour(vt::BkgColour::Black);
    // Forked Lightning: tag 0x23 = 2 (White) over a black palette
    const auto fork = load("4.2/Effects/2000FX/Organic/Forked Lightning");
    REQUIRE(fork->effectColour() == vt::EffectColour::of(vt::BkgColour::White));
    Rig r;
    REQUIRE(r.sw.setEffect(fork));
    REQUIRE(r.sw.effectColour() == fork->effectColour());
    REQUIRE(r.sw.background() == vt::EffectColour::of(vt::BkgColour::Black));  // BKG left Black
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    int t = 8;  // a T-bar position where the matte is on screen
    while (t < 511 && withMatte(*fork, t, r.in3, r.in4, white).px == withMatte(*fork, t, r.in3, r.in4, black).px) t += 8;
    REQUIRE(t < 511);
    r.sw.setTBar(t);
    REQUIRE(r.render().px == withMatte(*fork, t, r.in3, r.in4, white).px);  // a white matte
    // the Effect Color pop-up overrides it until another setEffect
    r.sw.setEffectColour(vt::EffectColour::of(vt::BkgColour::Red));
    REQUIRE(r.render().px == withMatte(*fork, t, r.in3, r.in4, vt::bkgColour(vt::BkgColour::Red)).px);
    r.sw.setEffectColour(vt::EffectColour::of(vt::BkgColour::Snow));  // Snow: black, as for BKG
    REQUIRE(r.render().px == withMatte(*fork, t, r.in3, r.in4, black).px);
    REQUIRE(r.sw.setEffect(fork));
    REQUIRE(r.sw.effectColour() == vt::EffectColour::of(vt::BkgColour::White));

    // Bear-L: tag 0x23 = 1 (Black) stays black whatever BKG is
    const auto bear = load("4.2/Effects/2000FX/Animals/Bear-L");
    REQUIRE(bear->effectColour() == vt::EffectColour::of(vt::BkgColour::Black));
    Rig b;
    b.sw.setBackground(vt::BkgColour::Blue);
    REQUIRE(b.sw.setEffect(bear));
    const vt::Rgba8 blue = vt::bkgColour(vt::BkgColour::Blue);
    t = 8;  // a T-bar position where the matte shows (a blue matte would differ)
    while (t < 511 && withMatte(*bear, t, b.in3, b.in4, blue).px == withMatte(*bear, t, b.in3, b.in4, black).px) t += 8;
    REQUIRE(t < 511);
    b.sw.select(Bus::Main, Source::In3);
    b.sw.select(Bus::Preview, Source::In4);
    b.sw.setTBar(t);
    REQUIRE(b.render().px == withMatte(*bear, t, b.in3, b.in4, black).px);
    // and BKG on a bus still shows blue
    b.sw.setTBar(0);
    b.sw.select(Bus::Main, Source::Bkg);
    REQUIRE(b.render().px == solid(vt::bkgColour(vt::BkgColour::Blue)).px);
}

TEST_CASE("render may write into an input frame", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.setEffect(dve()));  // DVE render must not alias its inputs
    r.sw.setTBar(150);
    const vt::Frame expect = r.render();
    r.sw.render(r.src, r.in1);  // Program lands in the A source's own frame
    REQUIRE(r.in1.px == expect.px);
}

TEST_CASE("Overlays-drawer effects are overlays, decided by the drawer (Ruling 27)", "[switcher][overlay]") {
    int loaded = 0;
    for (const char* drawer : {"4.2/Effects/2000FX/OverLays", "4.2/Effects/4000FX/Overlays"}) {
        for (const auto& ent : std::filesystem::directory_iterator(dataRoot() / drawer)) {
            const auto ext = ent.path().extension().string();
            if (ext == ".i" || ext == ".I") continue;  // icons
            auto e = vt::loadEffect(ent.path());
            if (!e) continue;  // bar pictures and the like
            INFO(ent.path().string());
            REQUIRE(e->isOverlay());
            ++loaded;
        }
    }
    REQUIRE(loaded >= 30);
    // the same bytes outside the drawer are an ordinary effect
    REQUIRE_FALSE(load("4.2/Effects/4000FX/Weddings/Wedding Bells !")->isOverlay());
    REQUIRE(load("4.2/Effects/4000FX/Overlays/Wedding Bells !")->isOverlay());
    REQUIRE_FALSE(pagePeel()->isOverlay());
    REQUIRE_FALSE(fadeToNeg()->isOverlay());
    REQUIRE_FALSE(dissolve()->isOverlay());
}

TEST_CASE("an overlay loops over Main and never swaps (Ruling 27)", "[switcher][overlay]") {
    const auto ov = load("4.2/Effects/4000FX/Overlays/Snowfall Loop");
    REQUIRE(ov->isOverlay());
    const int n = ov->frameCount();
    REQUIRE(n > 2);
    const vt::Speed speed = ov->timing().resolve(ov->timing().defaultSpeed);
    const int fpf = ov->timing().fieldsPerFrame[size_t(speed)];
    REQUIRE(fpf > 0);
    // what Program must show k ticks after engagement: the overlay over Main, frame (k / fpf) mod n
    auto expected = [&](const Rig& r, int tbar, uint32_t k) {
        vt::Frame out;
        vt::RenderCtx ctx;
        ctx.background = ov->effectColour().rgb();
        ctx.frameNumber = k;
        ctx.speed = speed;
        ctx.animFrame = int(k / uint32_t(fpf) % uint32_t(n));
        ov->render(tbar, r.in3, r.in3, out, ctx);
        return out;
    };

    SECTION("Auto engages it, runs the T-bar to 511 and leaves it running; T-bar 0 ends it") {
        Rig r;
        REQUIRE(r.sw.setEffect(ov));
        r.sw.select(Bus::Main, Source::In3);
        r.sw.select(Bus::Preview, Source::In4);
        r.sw.autoTransition(speed);
        REQUIRE(r.sw.overlayEngaged());
        uint32_t k = 0;
        for (; r.sw.autoRunning(); ++k) {
            REQUIRE(r.render().px == expected(r, r.sw.tbar(), k).px);
            r.sw.tick();
        }
        REQUIRE(k == uint32_t(ov->timing().durationTicks[size_t(speed)]));
        REQUIRE(r.sw.overlayEngaged());
        REQUIRE(r.sw.tbar() == 511);
        REQUIRE(r.sw.selected(Bus::Main) == Source::In3);  // no swap
        REQUIRE(r.sw.selected(Bus::Preview) == Source::In4);
        // it loops: the frame wraps after n * fpf ticks
        const vt::Frame first = expected(r, 511, 0);
        const uint32_t loop = uint32_t(n * fpf);
        for (; k % loop != 0; ++k) r.sw.tick();
        for (uint32_t i = 0; i < 2 * loop + 3; ++i, ++k) {
            INFO("tick " << k);
            const vt::Frame shown = r.render();
            REQUIRE(shown.px == expected(r, 511, k).px);
            if (i % loop == 0) REQUIRE(shown.px == first.px);
            r.sw.tick();
        }
        // T-bar 0 ends it, still without a swap
        r.sw.setTBar(0);
        REQUIRE_FALSE(r.sw.overlayEngaged());
        REQUIRE_FALSE(r.sw.transitionActive());
        REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
        REQUIRE(r.render().px == r.in3.px);
    }

    SECTION("a T-bar run by hand engages it; 511 does not swap") {
        Rig r;
        REQUIRE(r.sw.setEffect(ov));
        r.sw.select(Bus::Main, Source::In3);
        r.sw.select(Bus::Preview, Source::In4);
        r.sw.setTBar(100);
        REQUIRE(r.sw.overlayEngaged());
        REQUIRE(r.render().px == expected(r, 100, 0).px);
        r.sw.tick();
        r.sw.setTBar(511);
        REQUIRE(r.sw.overlayEngaged());
        REQUIRE(r.sw.tbar() == 511);
        REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
        REQUIRE(r.render().px == expected(r, 511, 1).px);
        // Take only ends it: no cut
        r.sw.take();
        REQUIRE_FALSE(r.sw.overlayEngaged());
        REQUIRE(r.sw.tbar() == 0);
        REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
        REQUIRE(r.sw.selected(Bus::Preview) == Source::In4);
        REQUIRE(r.render().px == r.in3.px);
        // with nothing engaged, Take is the ordinary cut again
        r.sw.take();
        REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
    }

    SECTION("reverse Auto and setEffect end it") {
        Rig r;
        REQUIRE(r.sw.setEffect(ov));
        r.sw.autoTransition(speed);
        for (int i = 0; i < 5; ++i) r.sw.tick();
        r.sw.autoTransition(speed, vt::Direction::Reverse);
        while (r.sw.autoRunning()) r.sw.tick();
        REQUIRE_FALSE(r.sw.overlayEngaged());
        REQUIRE(r.sw.selected(Bus::Main) == Source::In1);
        r.sw.setTBar(50);
        REQUIRE(r.sw.overlayEngaged());
        REQUIRE(r.sw.setEffect(dissolve()));
        REQUIRE_FALSE(r.sw.transitionActive());
        REQUIRE(r.sw.selected(Bus::Main) == Source::In1);
    }
}
