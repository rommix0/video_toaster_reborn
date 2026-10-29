#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/anim.h"
#include "vt/background.h"
#include "vt/chromafx.h"
#include "vt/crouton.h"
#include "vt/dve.h"
#include "vt/effect.h"
#include "vt/key.h"
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
    r.sw.select(Bus::Superimpose, Source::In4);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::DV1);
    REQUIRE(r.sw.selected(Bus::Superimpose) == Source::In4);
    r.sw.take();
    REQUIRE(r.sw.selected(Bus::Main) == Source::DV1);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In3);
    REQUIRE(r.sw.selected(Bus::Superimpose) == Source::None);  // Take empties it (design 2 B step 3)

    // Program is the Main source, plainly (the keyer is on, but its bus is empty)
    r.sw.select(Bus::Main, Source::In1);
    REQUIRE(r.render().px == r.in1.px);
    // a frozen DV1 or DV2 is black until written; a null input is black too
    REQUIRE(r.sw.setFrozen(true));  // writes DV1 (no bus shows DV2)
    r.sw.select(Bus::Main, Source::DV2);
    REQUIRE(r.render().px == solid({0, 0, 0, 255}).px);
    r.src.inputs[0] = nullptr;
    r.sw.select(Bus::Main, Source::In1);
    REQUIRE(r.render().px == solid({0, 0, 0, 255}).px);
}

TEST_CASE("power-up: keyer off, White, level 128, Superimpose empty", "[switcher][key]") {
    Rig r;
    CHECK(r.sw.keyMode() == vt::KeyMode::Off);
    CHECK(r.sw.keyLevel() == 128);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    r.sw.select(Bus::Superimpose, Source::In3);
    CHECK(r.sw.keyMode() == vt::KeyMode::White);  // Off kept White as the polarity
}

TEST_CASE("picking a Superimpose source turns the keyer on; OFF empties the bus", "[switcher][key]") {
    Rig r;
    CHECK(r.sw.select(Bus::Superimpose, Source::In3));
    CHECK(r.sw.keyMode() == vt::KeyMode::White);
    r.sw.setKeyMode(vt::KeyMode::Black);
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);
    CHECK(r.sw.select(Bus::Superimpose, Source::In3));  // the lit source again: nothing
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);
    r.sw.setKeyMode(vt::KeyMode::Off);
    CHECK(r.sw.keyMode() == vt::KeyMode::Off);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    r.sw.select(Bus::Superimpose, Source::In3);
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);  // the polarity was kept
    // None empties the bus and leaves the keyer alone
    CHECK(r.sw.select(Bus::Superimpose, Source::None));
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);
}

TEST_CASE("DV2 alone on Superimpose is refused", "[switcher][key]") {
    const char* const msg = "Superimpose: DV2 alone cannot be keyed";
    std::string why;
    {
        Rig r;  // frozen: the plain DV2 still
        r.sw.select(Bus::Superimpose, Source::In3);
        REQUIRE(r.sw.setFrozen(true));
        CHECK_FALSE(r.sw.select(Bus::Superimpose, Source::DV2, &why));
        CHECK(why == msg);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);
        CHECK(r.sw.select(Bus::Superimpose, Source::DV1));  // DV1 on a frozen store is fine
        CHECK(r.sw.selected(Bus::Superimpose) == Source::DV1);
    }
    {
        Rig r;  // live, an input on the bus to make live
        r.sw.select(Bus::Superimpose, Source::In4);
        CHECK(r.sw.select(Bus::Superimpose, Source::DV2));
        CHECK(r.sw.selected(Bus::Superimpose) == Source::DV2);
        CHECK(r.sw.dvInput() == 3);
    }
    {
        Rig r;  // live, live DV already on the bus: DV2 shows the same live DV input
        r.sw.select(Bus::Superimpose, Source::In4);
        REQUIRE(r.sw.select(Bus::Superimpose, Source::DV1));
        REQUIRE(r.sw.dvInput() == 3);
        CHECK(r.sw.select(Bus::Superimpose, Source::DV2));
        CHECK(r.sw.selected(Bus::Superimpose) == Source::DV2);
        CHECK(r.sw.dvInput() == 3);
        r.sw.select(Bus::Superimpose, Source::Bkg);  // BKG is no DV to make live
        CHECK_FALSE(r.sw.select(Bus::Superimpose, Source::DV2));
        CHECK(r.sw.selected(Bus::Superimpose) == Source::Bkg);
    }
    {
        Rig r;  // live, nothing on the bus
        why.clear();
        CHECK_FALSE(r.sw.select(Bus::Superimpose, Source::DV2, &why));
        CHECK(why == msg);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
        CHECK(r.sw.keyMode() == vt::KeyMode::Off);
    }
    {
        Rig r;  // Main and Preview always need a source
        why.clear();
        CHECK_FALSE(r.sw.select(Bus::Main, Source::None, &why));
        CHECK(why == "no source");
        CHECK_FALSE(r.sw.select(Bus::Preview, Source::None));
        CHECK(r.sw.selected(Bus::Main) == Source::In1);
        CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    }
}

TEST_CASE("the key picture: L 0, 1..256, 257", "[switcher][key]") {
    Rig r;
    r.sw.select(Bus::Superimpose, Source::In3);
    r.sw.setKeyMode(vt::KeyMode::Black);
    r.sw.setKeyLevel(128);
    vt::Frame expect;
    vt::lumaKey(r.in3, r.in1, r.in3, vt::keyClip(true, 128), true, 0, expect);
    CHECK(r.render().px == expect.px);
    CHECK(r.render().px != r.in1.px);
    CHECK(r.render().px != r.in3.px);
    r.sw.setKeyLevel(0);
    CHECK(r.render().px == r.in3.px);
    r.sw.setKeyLevel(257);
    CHECK(r.render().px == r.in1.px);
    r.sw.setKeyLevel(1);
    vt::lumaKey(r.in3, r.in1, r.in3, vt::keyClip(true, 1), true, 0, expect);
    CHECK(r.render().px == expect.px);
    r.sw.setKeyLevel(128);
    r.sw.setKeyMode(vt::KeyMode::White);
    vt::lumaKey(r.in3, r.in1, r.in3, vt::keyClip(false, 128), false, 0, expect);
    CHECK(r.render().px == expect.px);
    r.sw.setKeyLevel(999);
    CHECK(r.sw.keyLevel() == vt::kKeyLevelMax);
    r.sw.setKeyLevel(-5);
    CHECK(r.sw.keyLevel() == 0);
}

TEST_CASE("a key needs a Superimpose source that is not Main", "[switcher][key]") {
    Rig r;
    r.sw.select(Bus::Superimpose, Source::In1);  // the same as Main
    REQUIRE(r.sw.keyMode() == vt::KeyMode::White);
    CHECK(r.render().px == r.in1.px);
    r.sw.select(Bus::Superimpose, Source::None);
    for (auto m : {vt::KeyMode::Black, vt::KeyMode::White}) {  // lit, but nothing to key
        r.sw.setKeyMode(m);
        CHECK(r.render().px == r.in1.px);
    }
}

TEST_CASE("keying BKG Snow and a missing input", "[switcher][key]") {
    Rig r;
    r.sw.setCustomBackground(0x8000);  // bit 15: Snow
    r.sw.select(Bus::Superimpose, Source::Bkg);
    const vt::Frame first = r.render();
    r.sw.tick();
    CHECK(r.render().px != first.px);  // the noise moves, so does the key

    Rig m;
    m.src.inputs[1] = nullptr;
    m.sw.select(Bus::Superimpose, Source::In2);
    vt::Frame expect;
    vt::lumaKey(solid({0, 0, 0, 255}), m.in1, solid({0, 0, 0, 255}), vt::keyClip(false, 128), false, 0, expect);
    CHECK(m.render().px == expect.px);
}

TEST_CASE("Take: swap, the key source to Preview when Preview was Main, Superimpose emptied", "[switcher][key]") {
    Rig r;
    r.sw.select(Bus::Preview, Source::In1);
    r.sw.select(Bus::Superimpose, Source::In3);
    r.sw.setKeyMode(vt::KeyMode::Black);
    r.sw.take();
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In3);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);

    Rig p;  // Preview was not Main: the plain swap
    p.sw.select(Bus::Superimpose, Source::In3);
    p.sw.take();
    CHECK(p.sw.selected(Bus::Main) == Source::In2);
    CHECK(p.sw.selected(Bus::Preview) == Source::In1);
    CHECK(p.sw.selected(Bus::Superimpose) == Source::None);
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

// The colour a fresh Switcher (BKG Black) gives an effect's matte: its own, else BKG's.
vt::Rgba8 r0colour(const vt::Effect& e) { return e.effectColour().value_or(vt::EffectColour{}).rgb(); }

// The effect rendered directly with this matte colour (and a Snow generator's picture).
vt::Frame withMatte(const vt::Effect& e, int tbar, const vt::Frame& a, const vt::Frame& b, vt::Rgba8 matte,
                    const vt::Frame* generator = nullptr) {
    vt::Frame out;
    vt::RenderCtx ctx;
    ctx.background = matte;
    ctx.generator = generator;
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
    // Snow: the matte shows the generator's noise of this tick (tag 0x1B routes it to the generator)
    r.sw.setEffectColour(vt::EffectColour::of(vt::BkgColour::Snow));
    vt::Frame noise;
    vt::renderSnow(noise, 0);
    REQUIRE(r.render().px == withMatte(*fork, t, r.in3, r.in4, black, &noise).px);
    REQUIRE(r.render().px != withMatte(*fork, t, r.in3, r.in4, black).px);
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

TEST_CASE("Overlays-drawer effects are overlays, decided by the drawer (Rulings 27, 32)", "[switcher][overlay]") {
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
    // only the crouton's own folder counts (Ruling 32), in any case
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "vtoast_overlay_drawer_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "Overlays" / "sub");
    fs::create_directories(tmp / "OVERLAYS");
    const fs::path snow = dataRoot() / "4.2/Effects/4000FX/Overlays/Snowfall Loop";
    fs::copy_file(snow, tmp / "Overlays" / "sub" / "Snowfall Loop");
    fs::copy_file(snow, tmp / "OVERLAYS" / "Snowfall Loop");
    const auto nested = vt::loadEffect(tmp / "Overlays" / "sub" / "Snowfall Loop");
    const auto upper = vt::loadEffect(tmp / "OVERLAYS" / "Snowfall Loop");
    fs::remove_all(tmp);
    REQUIRE(nested);
    REQUIRE(upper);
    REQUIRE_FALSE(nested->isOverlay());
    REQUIRE(upper->isOverlay());
    REQUIRE_FALSE(pagePeel()->isOverlay());
    REQUIRE_FALSE(fadeToNeg()->isOverlay());
    REQUIRE_FALSE(dissolve()->isOverlay());
}

TEST_CASE("an effect without tag 0x23 shows the BKG colour, which it follows (switcher.main 0xe723e)", "[switcher]") {
    // Blendo !: no tag 0x23 or 0x24; its tag 0x1B routes palette indices 0 and 1 to the generator
    const auto blendo = load("4.2/Effects/4000FX/Special/Blendo !");
    REQUIRE_FALSE(blendo->effectColour());
    Rig r;
    r.sw.setBackground(vt::BkgColour::Blue);
    REQUIRE(r.sw.setEffect(blendo));
    REQUIRE_FALSE(r.sw.effectHasOwnColour());
    REQUIRE(r.sw.effectColour() == vt::EffectColour::of(vt::BkgColour::Blue));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    const vt::Rgba8 blue = vt::bkgColour(vt::BkgColour::Blue), black = vt::bkgColour(vt::BkgColour::Black);
    int t = 8;  // a T-bar position where the matte shows
    while (t < 511 && withMatte(*blendo, t, r.in3, r.in4, blue).px == withMatte(*blendo, t, r.in3, r.in4, black).px) t += 8;
    REQUIRE(t < 511);
    r.sw.setTBar(t);
    REQUIRE(r.render().px == withMatte(*blendo, t, r.in3, r.in4, blue).px);
    r.sw.setBackground(vt::BkgColour::Yellow);  // a BKG change shows at once
    REQUIRE(r.render().px == withMatte(*blendo, t, r.in3, r.in4, vt::bkgColour(vt::BkgColour::Yellow)).px);
    r.sw.setBackground(vt::BkgColour::Snow);  // and a Snow BKG fills the matte with its noise
    vt::Frame noise;
    vt::renderSnow(noise, 0);
    REQUIRE(r.render().px == withMatte(*blendo, t, r.in3, r.in4, black, &noise).px);
    // the Effect Color pop-up still sets a colour of its own
    r.sw.setEffectColour(vt::EffectColour::of(vt::BkgColour::Red));
    REQUIRE(r.sw.effectHasOwnColour());
    REQUIRE(r.render().px == withMatte(*blendo, t, r.in3, r.in4, vt::bkgColour(vt::BkgColour::Red)).px);
    REQUIRE(r.sw.setEffect(blendo));
    REQUIRE_FALSE(r.sw.effectHasOwnColour());
}

TEST_CASE("an overlay loops over Main and never swaps (Rulings 27, 32)", "[switcher][overlay]") {
    // Snowfall Loop: a soft matte, B = its effect colour (White); Glitch Roll Screen: a DVE move,
    // B = Main. Both have loop frames that differ.
    const char* src = GENERATE("4.2/Effects/4000FX/Overlays/Snowfall Loop",
                               "4.2/Effects/4000FX/Overlays/Glitch Roll Screen");
    INFO(src);
    const auto ov = load(src);
    REQUIRE(ov->isOverlay());
    const bool dve = ov->usesDvMemory();
    REQUIRE(dve == (std::string(src).find("Glitch") != std::string::npos));
    REQUIRE((dve || ov->effectColour() == vt::EffectColour::of(vt::BkgColour::White)));
    const vt::Frame colour = solid(r0colour(*ov));
    const int n = ov->frameCount();
    REQUIRE(n > 2);
    const vt::Speed speed = ov->timing().resolve(ov->timing().defaultSpeed);
    const int fpf = ov->timing().fieldsPerFrame[size_t(speed)];
    REQUIRE(fpf > 0);
    // what Program must show k ticks after engagement: the overlay over Main, frame (k / fpf) mod n
    auto expected = [&](const Rig& r, int tbar, uint32_t k) {
        vt::Frame out;
        vt::RenderCtx ctx;
        ctx.background = r0colour(*ov);
        ctx.frameNumber = k;
        ctx.speed = speed;
        ctx.animFrame = int(k / uint32_t(fpf) % uint32_t(n));
        ov->render(tbar, r.in3, dve ? r.in3 : colour, out, ctx);
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
        int changes = 0, unlikeMain = 0;
        vt::Frame prev = first;
        for (uint32_t i = 0; i < 2 * loop + 3; ++i, ++k) {
            INFO("tick " << k);
            const vt::Frame shown = r.render();
            REQUIRE(shown.px == expected(r, 511, k).px);
            if (i % loop == 0) REQUIRE(shown.px == first.px);
            changes += shown.px != prev.px;
            unlikeMain += shown.px != r.in3.px;
            prev = shown;
            r.sw.tick();
        }
        // it really is animated, and it really is laid over Main
        REQUIRE(changes >= 2);
        REQUIRE(unlikeMain == int(2 * loop + 3));
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

TEST_CASE("an animated matte overlay shows its effect colour, Snow as noise (Ruling 32)", "[switcher][overlay]") {
    const auto ov = load("4.2/Effects/4000FX/Overlays/Snowfall Loop");
    Rig r;
    REQUIRE(r.sw.setEffect(ov));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.setTBar(1);
    const vt::Frame white = r.render();
    r.sw.setEffectColour(vt::EffectColour::of(vt::BkgColour::Red));
    const vt::Frame red = r.render();
    REQUIRE(white.px != red.px);
    vt::RenderCtx ctx;
    ctx.background = vt::bkgColour(vt::BkgColour::Red);
    ctx.speed = r.sw.effect().timing().resolve(r.sw.speed());
    ctx.animFrame = 0;
    vt::Frame expect;
    ov->render(1, r.in3, solid(vt::bkgColour(vt::BkgColour::Red)), expect, ctx);
    REQUIRE(red.px == expect.px);
    // Snow: B is the noise of the current tick, so the flakes change from tick to tick
    r.sw.setEffectColour(vt::EffectColour::of(vt::BkgColour::Snow));
    vt::Frame noise;
    vt::renderSnow(noise, 0);
    ctx.background = vt::Rgba8{0, 0, 0, 255};
    ctx.generator = &noise;
    ov->render(1, r.in3, noise, expect, ctx);
    REQUIRE(r.render().px == expect.px);
}

TEST_CASE("gradient overlays are split screens with Preview, held by the T-bar (Ruling 32)", "[switcher][overlay]") {
    const auto split = load("4.2/Effects/4000FX/Overlays/Split1 !");
    REQUIRE(split->isOverlay());
    REQUIRE(split->frameCount() == 1);
    Rig r;
    REQUIRE(r.sw.setEffect(split));
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    r.sw.setTBar(256);
    REQUIRE(r.sw.overlayEngaged());
    vt::Frame expect;
    vt::RenderCtx ctx;
    ctx.background = r.sw.effectColour().rgb();
    split->render(256, r.in3, r.in4, expect, ctx);
    const vt::Frame shown = r.render();
    REQUIRE(shown.px == expect.px);
    REQUIRE(shown.px != r.in3.px);
    REQUIRE(shown.px != r.in4.px);
    for (int i = 0; i < 30; ++i) r.sw.tick();  // held
    REQUIRE(r.render().px == expect.px);
    // Auto runs to the end like a wipe, but never swaps and stays engaged
    r.sw.autoTransition(vt::Speed::Fast);
    while (r.sw.autoRunning()) r.sw.tick();
    REQUIRE(r.sw.overlayEngaged());
    REQUIRE(r.sw.tbar() == 511);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    // Take ends it without a cut
    r.sw.take();
    REQUIRE_FALSE(r.sw.overlayEngaged());
    REQUIRE(r.sw.selected(Bus::Main) == Source::In3);
    REQUIRE(r.render().px == r.in3.px);
}

TEST_CASE("every Overlays-drawer effect shows something over a textured Main within one loop (Ruling 32)",
          "[switcher][overlay][corpus]") {
    std::vector<std::string> identity;
    int checked = 0;
    for (const char* drawer : {"4.2/Effects/2000FX/OverLays", "4.2/Effects/4000FX/Overlays"}) {
        for (const auto& ent : std::filesystem::directory_iterator(dataRoot() / drawer)) {
            const auto ext = ent.path().extension().string();
            if (ext == ".i" || ext == ".I") continue;
            std::shared_ptr<const vt::Effect> e = vt::loadEffect(ent.path());
            if (!e) continue;
            ++checked;
            Rig r;  // Main = In3, Preview = In4: two different textured pictures
            REQUIRE(r.sw.setEffect(e));
            r.sw.select(Bus::Main, Source::In3);
            r.sw.select(Bus::Preview, Source::In4);
            r.sw.autoTransition(r.sw.speed());
            const vt::Timing t = e->timing();
            const int s = int(t.resolve(r.sw.speed()));
            const int ticks = std::max({1, t.durationTicks[size_t(s)], t.fieldsPerFrame[size_t(s)] * e->frameCount()});
            // one loop (or the whole Auto run) and the frame after it, where Auto has finished
            bool shows = false;
            for (int k = 0; k <= ticks && !shows; ++k) {
                shows = r.render().px != r.in3.px;
                r.sw.tick();
            }
            const auto u8 = ent.path().filename().u8string();
            if (!shows) identity.emplace_back(u8.begin(), u8.end());
        }
    }
    std::string list;
    for (const auto& n : identity) list += n + "; ";
    INFO("plain Main for the whole loop: " << list);
    REQUIRE(checked >= 40);
    REQUIRE(identity.empty());
}

TEST_CASE("renderSource: the picture of one source, no effect", "[switcher]") {
    Rig r;
    vt::Frame out;
    const vt::Frame black = solid({0, 0, 0, 255});
    r.sw.setBackground(vt::BkgColour::Green);
    r.sw.renderSource(Source::Bkg, r.src, out);
    CHECK(out.px == solid(vt::bkgColour(vt::BkgColour::Green)).px);
    r.sw.renderSource(Source::In1, r.src, out);
    CHECK(out.px == r.in1.px);
    r.sw.renderSource(Source::In4, r.src, out);
    CHECK(out.px == r.in4.px);
    r.sw.renderSource(Source::DV1, r.src, out);
    CHECK(out.px == black.px);
    r.sw.renderSource(Source::DV2, r.src, out);
    CHECK(out.px == black.px);
    // an input without a picture is black; `out` may be an input's own frame
    const vt::SourceFrames none;
    r.sw.renderSource(Source::In2, none, out);
    CHECK(out.px == black.px);
    r.sw.renderSource(Source::In3, r.src, r.in3);
    CHECK(r.in3.px == ramp(1).px);
}

TEST_CASE("renderSource ignores the effect and matches render's Snow", "[switcher]") {
    Rig r;
    REQUIRE(r.sw.setEffect(dissolve()));
    r.sw.setTBar(200);  // mid-transition: Program is a mix, the Preview source is still In2
    vt::Frame out;
    r.sw.renderSource(Source::In2, r.src, out);
    CHECK(out.px == r.in2.px);
    // Snow: the noise render puts on Program for a plain BKG Main, and it changes per tick
    Rig s;
    s.sw.setBackground(vt::BkgColour::Snow);
    s.sw.select(Bus::Main, Source::Bkg);
    vt::Frame snow, prog;
    s.sw.renderSource(Source::Bkg, s.src, snow);
    s.sw.render(s.src, prog);
    CHECK(snow.px == prog.px);
    s.sw.tick();
    vt::Frame next;
    s.sw.renderSource(Source::Bkg, s.src, next);
    CHECK(next.px != snow.px);
}

namespace {

std::shared_ptr<const vt::Effect> cropToCenter() { return load("4.2/Effects/4000FX/Wipes/Crop To Center"); }

int ticksAt(const vt::Effect& e, vt::Speed s) { return e.timing().durationTicks[static_cast<size_t>(s)]; }

}  // namespace

TEST_CASE("algo auto runs N fields and swaps", "[switcher][algo]") {
    Rig r;
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    REQUIRE(r.sw.setEffect(cropToCenter()));
    const auto ref = cropToCenter();  // a fresh take stepped through 1..k
    const int n = ticksAt(*ref, vt::Speed::Fast);
    REQUIRE(n == 30);
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Forward);
    for (int k = 1; k <= n; ++k) {
        INFO("k = " << k);
        REQUIRE(r.sw.autoRunning());
        vt::RenderCtx ctx;
        ctx.background = r.sw.effectColour().rgb();
        ctx.colour = r.sw.effectColour();
        ctx.speed = vt::Speed::Fast;
        ctx.take = 1;
        ctx.autoStep = k;
        ctx.autoSteps = n;
        vt::Frame want;
        ref->render(0, r.in3, r.in4, want, ctx);
        REQUIRE(r.render().px == want.px);
        r.sw.tick();
    }
    REQUIRE(!r.sw.autoRunning());
    REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
    REQUIRE(r.sw.selected(Bus::Preview) == Source::In3);
}

TEST_CASE("algo auto step clamps", "[switcher][algo]") {
    Rig r;
    auto rec = std::make_shared<Recorder>(cropToCenter());
    REQUIRE(r.sw.setEffect(rec));
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Forward);
    auto renderTick = [&] {
        r.render();
        const vt::RenderCtx& c = rec->lastCtx;
        REQUIRE(c.autoSteps == ticksAt(*rec, c.speed.value_or(vt::Speed::Medium)));
        REQUIRE(c.autoStep >= 1);
        REQUIRE(c.autoStep <= c.autoSteps);
        r.sw.tick();
        return c.autoStep;
    };
    for (int k = 1; k <= 10; ++k) REQUIRE(renderTick() == k);
    // Fast (30) -> Slow (90): the 10 elapsed ticks become 30, the remaining 60 run at Slow.
    r.sw.autoTransition(vt::Speed::Slow, vt::Direction::Forward);
    int ticks = 0, last = 0;
    while (r.sw.autoRunning() && ticks < 1000) {
        last = renderTick();
        ++ticks;
    }
    REQUIRE(ticks == 60);
    REQUIRE(last == 90);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
}

// Ruling 7: setSpeed mid-Auto keeps the Auto's rate, and autoSteps follows the Auto, not the
// speed button: the move reaches its end picture only on the last step.
TEST_CASE("algo auto steps follow the running Auto after setSpeed", "[switcher][algo]") {
    Rig r;
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Preview, Source::In4);
    auto rec = std::make_shared<Recorder>(cropToCenter());
    REQUIRE(r.sw.setEffect(rec));
    r.sw.autoTransition(vt::Speed::Slow, vt::Direction::Forward);
    vt::Frame end;  // the end picture: the move at its last Auto step
    {
        vt::RenderCtx ctx;
        ctx.colour = r.sw.effectColour();
        ctx.background = ctx.colour.rgb();
        ctx.take = 1;
        ctx.autoStep = ctx.autoSteps = ticksAt(*rec, vt::Speed::Slow);
        cropToCenter()->render(0, r.in3, r.in4, end, ctx);
    }
    int ticks = 0, last = 0;
    while (r.sw.autoRunning() && ticks < 1000) {
        if (ticks == 10) r.sw.setSpeed(vt::Speed::Fast);
        const vt::Frame f = r.render();
        const vt::RenderCtx& c = rec->lastCtx;
        REQUIRE(c.autoSteps == 90);
        REQUIRE(c.autoStep >= 1);
        REQUIRE(c.autoStep <= c.autoSteps);
        // With N = 30 (the speed button) the move would sit on its end picture from step 30.
        if (ticks < 60) REQUIRE(f.px != end.px);
        last = c.autoStep;
        r.sw.tick();
        ++ticks;
    }
    REQUIRE(ticks == 90);
    REQUIRE(last == 90);
    REQUIRE(r.sw.selected(Bus::Main) == Source::In4);
}

// RenderCtx::take: a new take on setEffect and on every start from idle, the same one through a
// drag and its reversal (ALGO.md 15.1).
TEST_CASE("the take number changes only when a take starts", "[switcher][algo]") {
    Rig r;
    auto rec = std::make_shared<Recorder>(cropToCenter());
    auto takeAt = [&](int tbar) {
        r.sw.setTBar(tbar);
        r.render();
        return rec->lastCtx.take;
    };
    REQUIRE(r.sw.setEffect(rec));
    const uint64_t first = takeAt(100);
    REQUIRE(takeAt(300) == first);  // drag on
    REQUIRE(takeAt(50) == first);   // and back: still the same take
    r.sw.setTBar(0);                // aborted
    const uint64_t second = takeAt(100);
    REQUIRE(second != first);
    r.sw.setTBar(0);
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Forward);  // Auto from idle
    r.render();
    const uint64_t third = rec->lastCtx.take;
    REQUIRE(third != second);
    r.sw.tick();
    r.render();
    REQUIRE(rec->lastCtx.take == third);
    r.sw.setTBar(0);  // the hand stops Auto, back at 0
    REQUIRE(r.sw.setEffect(rec));  // the same effect selected again: a new take
    const uint64_t fourth = takeAt(100);
    REQUIRE(fourth != third);
    REQUIRE(r.sw.setEffect(rec));  // setEffect mid-take: the drag after it is a new take
    REQUIRE(takeAt(100) != fourth);
}

TEST_CASE("algo auto from mid T-bar uses the T-bar", "[switcher][algo]") {
    Rig r;
    auto rec = std::make_shared<Recorder>(cropToCenter());
    REQUIRE(r.sw.setEffect(rec));
    auto runOut = [&] {
        int renders = 0;
        while (r.sw.autoRunning() && renders < 1000) {
            r.render();
            REQUIRE(rec->lastCtx.autoStep == -1);
            r.sw.tick();
            ++renders;
        }
        REQUIRE(renders > 0);
    };
    r.sw.setTBar(100);
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Forward);
    runOut();
    REQUIRE(r.sw.selected(Bus::Main) == Source::In2);
    // Reverse Auto, and a forward Auto from 0 turned round.
    r.sw.setTBar(300);
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Reverse);
    runOut();
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Forward);
    r.render();
    REQUIRE(rec->lastCtx.autoStep == 1);
    r.sw.tick();
    r.sw.autoTransition(vt::Speed::Fast, vt::Direction::Reverse);
    runOut();
}

// --- the DV store (design B, FREEZE.md 3-4) ---

namespace {

const vt::Frame& blackFrame() {
    static const vt::Frame f = solid({0, 0, 0, 255});
    return f;
}

// Input picture number n (red = n), so a frame tells which tick it came from.
vt::Frame numbered(int n) { return solid({uint8_t(n), 0, 0, 255}); }

// Renders ticks [from, to) with `in` showing numbered(tick + offset), ticking after each.
void run(Rig& r, vt::Frame& in, int from, int to, int offset = 0) {
    for (int n = from; n < to; ++n) {
        in = numbered(n + offset);
        r.render();
        r.sw.tick();
    }
}

}  // namespace

TEST_CASE("live DV on a bus lags its input by 4 ticks", "[switcher][dv]") {
    Rig r;
    REQUIRE(r.sw.guesses().digitalDelay == 4);
    REQUIRE_FALSE(r.sw.frozen());
    // DV pressed while live takes the bus's input as the DV input (FREEZE.md 3)
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Main, Source::DV2);
    REQUIRE(r.sw.dvInput() == 2);
    r.sw.select(Bus::Main, Source::Bkg);
    r.sw.select(Bus::Main, Source::DV1);  // BKG is not an input: the DV input stays
    REQUIRE(r.sw.dvInput() == 2);
    r.sw.select(Bus::Main, Source::In1);
    r.sw.select(Bus::Main, Source::DV1);
    REQUIRE(r.sw.dvInput() == 0);
    for (int n = 0; n < 10; ++n) {
        r.in1 = numbered(n);
        INFO("tick " << n);
        REQUIRE(r.render().px == numbered(std::max(n - 4, 0)).px);
        r.sw.tick();
    }
    CHECK(r.sw.still(Source::DV1).px == blackFrame().px);  // never written
}

TEST_CASE("freeze copies the delayed picture into DV1, or DV2 when a bus shows DV2", "[switcher][dv]") {
    Rig r;
    r.sw.select(Bus::Preview, Source::DV2);  // live: Preview's In2 through DV
    REQUIRE(r.sw.dvInput() == 1);
    run(r, r.in2, 0, 6);
    REQUIRE(r.sw.setFrozen(true));
    REQUIRE(r.sw.frozen());
    CHECK(r.sw.still(Source::DV2).px == numbered(2).px);
    CHECK(r.sw.still(Source::DV1).px == blackFrame().px);
    // frozen, the bus shows the still whatever the input does
    r.in2 = numbered(50);
    vt::Frame out;
    r.sw.renderSource(Source::DV2, r.src, out);
    CHECK(out.px == numbered(2).px);
    // a frozen DV does not take the bus's input
    r.sw.select(Bus::Main, Source::In4);
    r.sw.select(Bus::Main, Source::DV1);
    CHECK(r.sw.dvInput() == 1);

    Rig q;  // no DV on a bus: DV1
    run(q, q.in1, 0, 6);
    REQUIRE(q.sw.setFrozen(true));
    CHECK(q.sw.still(Source::DV1).px == numbered(2).px);
    CHECK(q.sw.still(Source::DV2).px == blackFrame().px);
}

TEST_CASE("live then freeze keeps the other still", "[switcher][dv]") {
    Rig r;
    run(r, r.in1, 0, 6);
    REQUIRE(r.sw.setFrozen(true));  // DV1 = input 1 at tick 2
    REQUIRE(r.sw.setFrozen(false));
    REQUIRE_FALSE(r.sw.frozen());
    CHECK(r.sw.still(Source::DV1).px == numbered(2).px);  // LIVE keeps the stills
    r.sw.select(Bus::Preview, Source::DV2);              // live: input 2
    run(r, r.in2, 6, 12, 50);
    REQUIRE(r.sw.setFrozen(true));  // now 12: input 2 at tick 8
    CHECK(r.sw.still(Source::DV2).px == numbered(58).px);
    CHECK(r.sw.still(Source::DV1).px == numbered(2).px);
}

TEST_CASE("switching the DV input back does not show its old frames (Ruling 4)", "[switcher][dv]") {
    auto setUp = [](Rig& r) {
        r.sw.select(Bus::Main, Source::DV1);  // live: input 1
        run(r, r.in1, 0, 11);                 // input 1 recorded at ticks 0..10
        r.sw.select(Bus::Preview, Source::DV2);  // the DV input becomes input 2 (both buses show it)
        REQUIRE(r.sw.dvInput() == 1);
        run(r, r.in2, 11, 20, 100);
        r.sw.select(Bus::Main, Source::In1);
        r.sw.select(Bus::Main, Source::DV1);  // back to input 1 at tick 20
        REQUIRE(r.sw.dvInput() == 0);
    };
    Rig r;
    setUp(r);
    r.in1 = numbered(20);
    CHECK(r.render().px == numbered(20).px);  // the current frame, not tick 10's
    r.sw.tick();
    REQUIRE(r.sw.setFrozen(true));  // Preview shows DV2: DV2
    CHECK(r.sw.still(Source::DV2).px == numbered(20).px);

    Rig q;  // a freeze before the switch-back's first render has no current history: black
    setUp(q);
    REQUIRE(q.sw.setFrozen(true));
    CHECK(q.sw.still(Source::DV2).px == blackFrame().px);
}

TEST_CASE("with digitalDelay 0 a freeze keeps the last frame shown", "[switcher][dv]") {
    Rig r;
    vt::HardwareGuesses g;
    g.digitalDelay = 0;
    r.sw.setGuesses(g);
    r.sw.select(Bus::Main, Source::DV1);  // live: input 1, no lag
    run(r, r.in1, 0, 6);                  // the last render showed tick 5's frame
    REQUIRE(r.sw.setFrozen(true));
    CHECK(r.sw.still(Source::DV1).px == numbered(5).px);
}

TEST_CASE("freeze, live and grab are refused during a transition", "[switcher][dv]") {
    Rig r;
    r.sw.setTBar(100);
    std::string why;
    REQUIRE_FALSE(r.sw.setFrozen(true, &why));
    CHECK(why == "finish the effect first");
    REQUIRE_FALSE(r.sw.frozen());
    why.clear();
    REQUIRE_FALSE(r.sw.setFrozen(false, &why));
    CHECK(why == "finish the effect first");
    why.clear();
    REQUIRE_FALSE(r.sw.grab(0, &why));
    CHECK(why == "finish the effect first");
    CHECK_FALSE(r.sw.grabPending());
    CHECK_FALSE(r.sw.frozen());
    // an engaged ChromaFX too
    Rig c;
    REQUIRE(c.sw.setEffect(fadeToNeg()));
    c.sw.setTBar(511);
    REQUIRE(c.sw.chromaEngaged());
    why.clear();
    REQUIRE_FALSE(c.sw.setFrozen(true, &why));
    CHECK(why == "finish the effect first");
}

TEST_CASE("grab 3 writes input 3 after 4 ticks, freezes and selects DV1 on Main", "[switcher][dv]") {
    Rig r;
    run(r, r.in1, 0, 6);  // the DV input is input 1
    REQUIRE(r.sw.grab(2));
    REQUIRE(r.sw.frozen());  // frozen first: DV1 = input 1 at tick 2
    REQUIRE(r.sw.grabPending());
    const vt::Frame frozen = r.sw.still(Source::DV1);
    CHECK(frozen.px == numbered(2).px);
    for (int t = 6; t < 10; ++t) {
        INFO("tick " << t);
        r.in3 = numbered(100 + t);
        CHECK(r.sw.selected(Bus::Main) == Source::In1);
        r.render();
        CHECK(r.sw.still(Source::DV1).px == frozen.px);
        CHECK(r.sw.grabPending());
        r.sw.tick();
    }
    CHECK(r.sw.selected(Bus::Main) == Source::DV1);
    r.in3 = numbered(110);
    CHECK(r.render().px == numbered(110).px);  // Main shows the grab at once
    CHECK(r.sw.still(Source::DV1).px == numbered(110).px);
    CHECK_FALSE(r.sw.grabPending());
    CHECK(r.sw.frozen());
}

TEST_CASE("grab of a missing input gives black", "[switcher][dv]") {
    Rig r;
    r.src.inputs[3] = nullptr;
    run(r, r.in1, 0, 1);
    REQUIRE(r.sw.grab(3));
    CHECK(r.sw.still(Source::DV1).px == numbered(0).px);  // the freeze's picture
    for (int t = 0; t < 4; ++t) {
        r.render();
        r.sw.tick();
    }
    r.render();
    CHECK_FALSE(r.sw.grabPending());
    CHECK(r.sw.still(Source::DV1).px == blackFrame().px);
}

TEST_CASE("freeze before any render gives black", "[switcher][dv]") {
    Rig r;
    REQUIRE(r.sw.setFrozen(true));
    CHECK(r.sw.still(Source::DV1).px == blackFrame().px);
    Rig g;
    REQUIRE(g.sw.grab(1));
    CHECK(g.sw.still(Source::DV1).px == blackFrame().px);
    // a live DV with no history yet is black too
    Rig l;
    l.sw.select(Bus::Preview, Source::DV1);
    vt::Frame out;
    l.sw.renderSource(Source::DV1, l.src, out);
    CHECK(out.px == blackFrame().px);
}

TEST_CASE("load target follows the loader rule", "[switcher][dv]") {
    const vt::Frame still = numbered(77);
    struct Case {
        const char* name;
        bool frozen;
        Bus bus;
        Source dv;
        Source target;
    };
    // FREEZE.md 4 step 2
    for (const Case& c : {Case{"Main DV1, Superimpose None", true, Bus::Main, Source::DV1, Source::DV2},
                          Case{"Superimpose DV1", true, Bus::Superimpose, Source::DV1, Source::DV2},
                          Case{"Preview DV2", true, Bus::Preview, Source::DV2, Source::DV1},
                          Case{"nothing", false, Bus::Superimpose, Source::Bkg, Source::DV2},
                          Case{"Main live DV1", false, Bus::Main, Source::DV1, Source::DV2}}) {
        INFO(c.name);
        Rig r;
        if (c.frozen) REQUIRE(r.sw.setFrozen(true));
        r.sw.select(c.bus, c.dv);
        REQUIRE(r.sw.loadStill(still));
        CHECK(r.sw.frozen());
        CHECK(r.sw.selected(Bus::Preview) == c.target);
        CHECK(r.sw.still(c.target).px == still.px);
        if (!c.frozen && c.dv == Source::DV1) CHECK(r.sw.selected(c.bus) == Source::In1);  // stripped
    }
    // Superimpose DV2 only exists made live first (a frozen store refuses it), then frozen
    Rig q;
    q.sw.select(Bus::Superimpose, Source::In4);
    REQUIRE(q.sw.select(Bus::Superimpose, Source::DV2));
    REQUIRE(q.sw.setFrozen(true));
    REQUIRE(q.sw.loadStill(still));
    CHECK(q.sw.selected(Bus::Preview) == Source::DV1);
    // refused while a transition runs
    Rig t;
    t.sw.setTBar(100);
    std::string why;
    REQUIRE_FALSE(t.sw.loadStill(still, &why));
    CHECK(why == "a transition is running: finish it first");
    CHECK_FALSE(t.sw.frozen());
}

// The original waits out the grab before it reads another button; here a take or a transition
// that starts in the wait keeps its buses (and a digital move's live store), and the grabbed frame
// still goes into DV1.
TEST_CASE("a take or a transition in a grab's wait keeps its buses", "[switcher][dv]") {
    Rig t;
    run(t, t.in1, 0, 6);
    REQUIRE(t.sw.grab(2));
    run(t, t.in3, 6, 7);  // one tick into the wait
    t.sw.take();
    run(t, t.in3, 7, 11);  // due at tick 10's render
    CHECK_FALSE(t.sw.grabPending());
    CHECK(t.sw.selected(Bus::Main) == Source::In2);
    CHECK(t.sw.selected(Bus::Preview) == Source::In1);
    CHECK(t.sw.still(Source::DV1).px == numbered(10).px);

    Rig m;
    run(m, m.in1, 0, 6);
    REQUIRE(m.sw.grab(2));
    run(m, m.in3, 6, 7);
    m.sw.setTBar(100);  // the default effect starts
    run(m, m.in3, 7, 11);
    CHECK_FALSE(m.sw.grabPending());
    CHECK(m.sw.selected(Bus::Main) == Source::In1);
    CHECK(m.sw.tbar() == 100);
    CHECK(m.sw.still(Source::DV1).px == numbered(10).px);

    Rig g;  // with neither, the grab selects DV1 as before
    run(g, g.in1, 0, 6);
    REQUIRE(g.sw.grab(2));
    run(g, g.in3, 6, 11);
    CHECK(g.sw.selected(Bus::Main) == Source::DV1);
}

TEST_CASE("loading a framestore cancels a pending grab (Ruling 5)", "[switcher][dv]") {
    Rig r;
    run(r, r.in1, 0, 6);
    REQUIRE(r.sw.grab(2));
    run(r, r.in3, 6, 7);  // one tick into the wait
    REQUIRE(r.sw.grabPending());
    r.sw.select(Bus::Preview, Source::DV2);  // frozen: the loader's target is DV1, the grab's DV
    REQUIRE(r.sw.loadStill(numbered(77)));
    CHECK_FALSE(r.sw.grabPending());
    run(r, r.in3, 7, 12);
    CHECK(r.sw.still(Source::DV1).px == numbered(77).px);
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::DV1);
}

TEST_CASE("loading a framestore disengages ChromaFX", "[switcher][dv][chromafx]") {
    Rig r;
    REQUIRE(r.sw.setEffect(fadeToNeg()));
    r.sw.setTBar(511);
    REQUIRE(r.sw.chromaEngaged());
    REQUIRE(r.sw.loadStill(numbered(5)));
    CHECK_FALSE(r.sw.chromaEngaged());
    CHECK_FALSE(r.sw.transitionActive());
    CHECK(r.sw.selected(Bus::Preview) == Source::DV2);
}

TEST_CASE("saveSource order", "[switcher][dv]") {
    Rig r;
    std::string why;
    CHECK_FALSE(r.sw.saveSource(&why));
    CHECK(why == "SAVE: no still in DV1/DV2");
    REQUIRE(r.sw.loadStill(numbered(5)));  // DV2, on Preview
    CHECK(r.sw.saveSource() == Source::DV2);
    r.sw.select(Bus::Preview, Source::In2);
    r.sw.select(Bus::Main, Source::DV1);
    CHECK(r.sw.saveSource() == Source::DV1);
    r.sw.select(Bus::Main, Source::DV2);  // Main is read, not just the DV1 fallback
    CHECK(r.sw.saveSource() == Source::DV2);
    r.sw.select(Bus::Main, Source::In1);
    CHECK(r.sw.saveSource() == Source::DV1);
}

// --- digital moves through the DV store (design B, FREEZE.md 5) ---

namespace {

const char* const kBlockOut = "4.2/Effects/2000FX/Digital/Block Out";  // a plain DVE ANIM

// A T-bar position of Block Out and two visible pixels of its control frame there: one the moving
// clip draws (key >= 64, no art) and one the background shows (key 0). The first position from 1
// whose frame has both (Ruling 2).
struct MoveProbe {
    int tbar, mx, my, bx, by;
};
MoveProbe blockOutProbe() {
    auto c = vt::loadCrouton(dataRoot() / kBlockOut);
    REQUIRE(c);
    const auto f = std::find_if(c->forms.begin(), c->forms.end(),
                                [](const vt::Form& f) { return f.type == std::array<char, 4>{'A', 'N', 'I', 'M'}; });
    REQUIRE(f != c->forms.end());
    const vt::Anim an = vt::readAnim(c->bytes, *f);
    REQUIRE(vt::dveInfo(an).role == vt::DveRole::MovingA);  // A moves
    const vt::Rect r = vt::kVisibleArea;
    for (int t = 1; t < 511; ++t) {
        const vt::DveControl dc = vt::decodeDve(an.planes[size_t(vt::animFrameForTBar(t, int(an.planes.size())))]);
        MoveProbe p{t, -1, -1, -1, -1};
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) {
                const uint8_t v = dc.key[size_t(y / 2) * vt::kDveCells + size_t(x / 4)];
                if (!dc.art.empty() && dc.art[size_t(y / 2) * vt::kDveWidth + size_t(x)] != 0) continue;
                if (v >= 64 && p.mx < 0) p.mx = x, p.my = y;
                if (v == 0 && p.bx < 0) p.bx = x, p.by = y;
            }
        if (p.mx >= 0 && p.bx >= 0) return p;
    }
    FAIL("Block Out has no frame with both a moving and a background pixel");
    return {};
}

const vt::Rgba8 kNine{9, 9, 9, 255};
bool same(vt::Rgba8 a, vt::Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

}  // namespace

TEST_CASE("a digital move's moving picture lags 4 ticks", "[switcher][dv]") {
    const MoveProbe p = blockOutProbe();
    Rig r;
    // The DV input starts as input 3, so input 1's history comes from recording Main.
    r.sw.select(Bus::Main, Source::In3);
    r.sw.select(Bus::Main, Source::DV1);
    r.sw.select(Bus::Main, Source::In1);
    REQUIRE(r.sw.dvInput() == 2);
    r.in2 = solid(kNine);
    REQUIRE(r.sw.setEffect(load(kBlockOut)));
    run(r, r.in1, 0, 10);
    r.in1 = numbered(10);
    r.sw.setTBar(p.tbar);
    CHECK(r.sw.dvInput() == 0);  // the moving bus's input now goes through DV
    const vt::Frame out = r.render();
    INFO("T-bar " << p.tbar);
    CHECK(int(out.at(p.mx, p.my).r) == 10 - 4);
    CHECK(same(out.at(p.bx, p.by), kNine));  // the key-0 background is analog, current
}

TEST_CASE("a frozen DV on the moving bus moves the still without lag", "[switcher][dv]") {
    const MoveProbe p = blockOutProbe();
    Rig r;
    run(r, r.in1, 0, 6);
    REQUIRE(r.sw.setFrozen(true));  // DV1 = input 1 at tick 2
    r.sw.select(Bus::Main, Source::DV1);
    r.in2 = solid(kNine);
    REQUIRE(r.sw.setEffect(load(kBlockOut)));
    run(r, r.in1, 6, 10);
    r.sw.setTBar(p.tbar);
    CHECK(r.sw.frozen());  // a DV on a bus keeps the freeze
    const vt::Frame out = r.render();  // the take's first frame
    CHECK(same(out.at(p.mx, p.my), numbered(2).at(0, 0)));
    CHECK(same(out.at(p.bx, p.by), kNine));
}

TEST_CASE("a digital move taken with a frozen store and no DV on a bus goes live and keeps the stills", "[switcher][dv]") {
    Rig r;
    run(r, r.in1, 0, 6);
    REQUIRE(r.sw.setFrozen(true));          // DV1 = input 1 at tick 2
    REQUIRE(r.sw.loadStill(numbered(77)));  // DV2, on Preview
    r.sw.select(Bus::Preview, Source::In2);
    // a cut and a dissolve leave the store frozen
    r.sw.take();
    CHECK(r.sw.frozen());
    r.sw.take();
    for (auto e : {std::shared_ptr<const vt::Effect>(vt::makeCut()), dissolve()}) {
        REQUIRE(r.sw.setEffect(e));
        r.sw.setTBar(100);
        CHECK(r.sw.frozen());
        r.sw.setTBar(0);
    }
    REQUIRE(r.sw.setEffect(load(kBlockOut)));
    r.sw.select(Bus::Main, Source::In4);
    r.sw.setTBar(100);
    CHECK_FALSE(r.sw.frozen());
    CHECK(r.sw.dvInput() == 3);  // Main's input goes through DV
    CHECK(r.sw.still(Source::DV1).px == numbered(2).px);
    CHECK(r.sw.still(Source::DV2).px == numbered(77).px);
}

TEST_CASE("ChromaFX has no lag", "[switcher][dv][chromafx]") {
    // Preview's input changes every tick in r and holds this tick's frame in q: the same picture.
    Rig r, q;
    for (Rig* x : {&r, &q}) {
        REQUIRE(x->sw.setEffect(fadeToNeg()));
        x->sw.setTBar(511);
        REQUIRE(x->sw.chromaEngaged());
    }
    q.in2 = numbered(10);
    for (int n = 0; n <= 10; ++n) {
        r.in2 = numbered(n);
        const vt::Frame got = r.render(), want = q.render();
        INFO("tick " << n);
        if (n == 10) CHECK(got.px == want.px);
        r.sw.tick();
        q.sw.tick();
    }
}

// --- Take, Auto and the T-bar with a key up (design 2 B) ---

namespace {

const char* const kPushOn = "4.2/Effects/4000FX/Push-Pull/Push On L-R";     // ALGO, fly on (2)
const char* const kSplitIn = "4.2/Effects/2000FX/Blinds/Split Corners In";  // DVE, fly on (17)
const char* const kBlinds3 = "4.2/Effects/2000FX/Blinds/Blinds 3 Shrink";   // fly off (3)
const char* const kTiles = "4.2/Effects/2000FX/Digital/Infinite Tiles";     // two-phase (5)
const char* const kSmoothFade = "4.2/Effects/2000FX/Smooth Fade";           // drops

std::shared_ptr<const vt::Effect> loadDesc(const char* rel, uint32_t desc) {
    auto e = load(rel);
    REQUIRE(e->routingDescriptor() == desc);
    return e;
}

// Program's pixels by where their colour comes from: x's colours, y's colours (not x's), neither.
struct Mix {
    int x = 0, y = 0, other = 0;
};
Mix mixOf(const vt::Frame& out, const vt::Frame& x, const vt::Frame& y) {
    auto rgb = [](vt::Rgba8 c) { return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16; };
    std::unordered_set<uint32_t> xs, ys;
    for (vt::Rgba8 c : x.px) xs.insert(rgb(c));
    for (vt::Rgba8 c : y.px) ys.insert(rgb(c));
    Mix m;
    for (vt::Rgba8 c : out.px) {
        if (xs.count(rgb(c))) ++m.x;
        else if (ys.count(rgb(c))) ++m.y;
        else ++m.other;
    }
    return m;
}

// Runs Auto to the end on r and q side by side (render, then tick); f(r's Program, q's Program, T-bar).
template <class F>
void autoBoth(Rig& r, Rig& q, F f) {
    r.sw.autoTransition(r.sw.speed());
    q.sw.autoTransition(q.sw.speed());
    for (int n = 0; r.sw.autoRunning(); ++n) {
        REQUIRE(n < 10000);
        const int tbar = r.sw.tbar();
        const vt::Frame got = r.render(), plain = q.render();
        f(got, plain, tbar);
        r.sw.tick();
        q.sw.tick();
    }
}

vt::HardwareGuesses noDelay() {
    vt::HardwareGuesses g;
    g.digitalDelay = 0;
    return g;
}

}  // namespace

TEST_CASE("a dissolve with a key up drops the key from the first field", "[switcher][key]") {
    for (auto e : {dissolve(), loadDesc(kSmoothFade, 1)}) {
        INFO(e->name());
        const bool isDissolve = e->name() == dissolve()->name();
        Rig r, q;  // q: the same take without a key
        for (Rig* x : {&r, &q}) REQUIRE(x->sw.setEffect(e));
        r.sw.select(Bus::Superimpose, Source::In3);
        r.sw.setKeyMode(vt::KeyMode::Black);
        r.sw.setKeyLevel(128);
        int n = 0;
        autoBoth(r, q, [&](const vt::Frame& got, const vt::Frame& plain, int tbar) {
            if (n++ == 0 && isDissolve) CHECK(got.px == direct(*e, tbar, r.in1, r.in2).px);
            CHECK(r.sw.keyMode() == vt::KeyMode::Off);
            CHECK(got.px == plain.px);
        });
        CHECK(r.sw.selected(Bus::Main) == Source::In2);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
        CHECK(r.sw.keyMode() == vt::KeyMode::Off);
    }
}

TEST_CASE("Push On with a key up flies Preview on as a key (design 7)", "[switcher][key]") {
    for (auto [path, desc] : {std::pair{kPushOn, 2u}, std::pair{kSplitIn, 17u}}) {
        INFO(path);
        Rig r, q;  // q: the plain move
        for (Rig* x : {&r, &q}) {
            x->sw.setGuesses(noDelay());
            REQUIRE(x->sw.setEffect(loadDesc(path, desc)));
            x->sw.select(Bus::Preview, Source::In3);
        }
        r.sw.setKeyMode(vt::KeyMode::Black);  // the bus empty
        r.sw.setKeyLevel(128);
        bool mid = false;
        autoBoth(r, q, [&](const vt::Frame& got, const vt::Frame& plain, int tbar) {
            if (mid || tbar < 256) return;
            mid = true;
            CHECK(got.px != r.in1.px);
            CHECK(got.px != plain.px);
            const Mix m = mixOf(got, r.in1, r.in3);
            CHECK(m.other == 0);
            CHECK(m.y > 0);
        });
        CHECK(mid);
        CHECK(r.sw.selected(Bus::Main) == Source::In1);
        CHECK(r.sw.selected(Bus::Preview) == Source::In3);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);
        CHECK(r.sw.keyMode() == vt::KeyMode::Black);
    }
}

TEST_CASE("fly on with Preview = Main shows Main and still keys Preview at the end", "[switcher][key]") {
    Rig r, q;
    for (Rig* x : {&r, &q}) {
        x->sw.setGuesses(noDelay());
        REQUIRE(x->sw.setEffect(loadDesc(kPushOn, 2)));
    }
    r.sw.select(Bus::Preview, Source::In1);
    r.sw.setKeyMode(vt::KeyMode::Black);
    autoBoth(r, q, [&](const vt::Frame& got, const vt::Frame&, int) { CHECK(got.px == r.in1.px); });
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In1);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::In1);
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);
}

TEST_CASE("fly off with a key up: the key source lands on Preview", "[switcher][key]") {
    Rig r, q;
    for (Rig* x : {&r, &q}) REQUIRE(x->sw.setEffect(loadDesc(kBlinds3, 3)));
    r.sw.select(Bus::Superimpose, Source::In3);
    r.sw.setKeyMode(vt::KeyMode::Black);
    autoBoth(r, q, [](const vt::Frame&, const vt::Frame&, int) {});
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In3);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.keyMode() == vt::KeyMode::Black);
}

TEST_CASE("fly off with an empty Superimpose runs as a normal move", "[switcher][key]") {
    Rig r, q;
    for (Rig* x : {&r, &q}) REQUIRE(x->sw.setEffect(loadDesc(kBlinds3, 3)));
    r.sw.setKeyMode(vt::KeyMode::Black);  // on, nothing to fly
    autoBoth(r, q, [](const vt::Frame& got, const vt::Frame& plain, int) { CHECK(got.px == plain.px); });
    CHECK(r.sw.selected(Bus::Main) == Source::In2);
    CHECK(r.sw.selected(Bus::Preview) == Source::In1);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
}

TEST_CASE("a two-phase move keys Superimpose over Main, then Main over Superimpose, and ends as it began",
          "[switcher][key]") {
    Rig r;
    r.sw.setGuesses(noDelay());
    REQUIRE(r.sw.setEffect(loadDesc(kTiles, 5)));
    r.sw.select(Bus::Superimpose, Source::In3);  // keyer on, White
    for (int t : {128, 300}) {
        INFO("T-bar " << t);
        r.sw.setTBar(t);
        const Mix m = mixOf(r.render(), r.in1, r.in3);
        CHECK(m.other == 0);
        CHECK(m.x > 0);
        CHECK(m.y > 0);
        r.sw.tick();
    }
    r.sw.setTBar(511);
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);
    CHECK(r.sw.keyMode() == vt::KeyMode::White);
}

TEST_CASE("back to T-bar 0", "[switcher][key]") {
    {
        Rig r;  // fly on: Superimpose emptied, Preview kept
        REQUIRE(r.sw.setEffect(loadDesc(kPushOn, 2)));
        r.sw.select(Bus::Preview, Source::In3);
        r.sw.select(Bus::Superimpose, Source::In4);
        r.sw.setTBar(200);
        r.sw.setTBar(0);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
        CHECK(r.sw.selected(Bus::Preview) == Source::In3);
        CHECK(r.sw.selected(Bus::Main) == Source::In1);
        CHECK(r.sw.keyMode() == vt::KeyMode::White);
    }
    {
        Rig r;  // fly off: the key source to Preview
        REQUIRE(r.sw.setEffect(loadDesc(kBlinds3, 3)));
        r.sw.select(Bus::Superimpose, Source::In3);
        r.sw.setTBar(200);
        r.sw.setTBar(0);
        CHECK(r.sw.selected(Bus::Preview) == Source::In3);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
        CHECK(r.sw.selected(Bus::Main) == Source::In1);
        CHECK(r.sw.keyMode() == vt::KeyMode::White);
    }
    {
        Rig r;  // drops: the keyer stays off
        r.sw.select(Bus::Superimpose, Source::In3);
        r.sw.setTBar(200);
        r.sw.setTBar(0);
        CHECK(r.sw.keyMode() == vt::KeyMode::Off);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
        CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    }
}

TEST_CASE("locks during a take", "[switcher][key]") {
    {
        Rig r;  // a dissolve holds the mode and the level
        r.sw.select(Bus::Superimpose, Source::In3);
        r.sw.setTBar(200);
        REQUIRE(r.sw.keyMode() == vt::KeyMode::Off);  // dropped at take start
        r.sw.setKeyLevel(40);
        r.sw.setKeyMode(vt::KeyMode::White);
        CHECK(r.sw.keyLevel() == 128);
        CHECK(r.sw.keyMode() == vt::KeyMode::Off);
        r.sw.setTBar(511);
        CHECK(r.sw.keyLevel() == 128);
        r.sw.setKeyLevel(40);  // no take open: applies
        CHECK(r.sw.keyLevel() == 40);
    }
    {
        Rig r;  // descriptor 5 holds the mode, not the level
        REQUIRE(r.sw.setEffect(loadDesc(kTiles, 5)));
        r.sw.select(Bus::Superimpose, Source::In3);
        r.sw.setTBar(200);
        r.sw.setKeyMode(vt::KeyMode::Black);
        CHECK(r.sw.keyMode() == vt::KeyMode::White);
        r.sw.setKeyMode(vt::KeyMode::Off);
        CHECK(r.sw.keyMode() == vt::KeyMode::White);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);
        r.sw.setKeyLevel(40);
        CHECK(r.sw.keyLevel() == 40);
    }
    {
        Rig r, q;  // fly on: OFF applies, and the move finishes as a plain Push On
        for (Rig* x : {&r, &q}) {
            REQUIRE(x->sw.setEffect(loadDesc(kPushOn, 2)));
            x->sw.select(Bus::Preview, Source::In3);
        }
        r.sw.setKeyMode(vt::KeyMode::Black);
        for (Rig* x : {&r, &q}) {
            x->sw.setTBar(200);
            x->render();
        }
        r.sw.setKeyMode(vt::KeyMode::Off);
        CHECK(r.sw.keyMode() == vt::KeyMode::Off);
        for (Rig* x : {&r, &q}) x->sw.setTBar(300);
        CHECK(r.render().px == q.render().px);
        r.sw.setTBar(511);
        CHECK(r.sw.selected(Bus::Main) == Source::In3);
        CHECK(r.sw.selected(Bus::Preview) == Source::In1);
    }
}

TEST_CASE("no key while ChromaFX or an overlay is engaged", "[switcher][key]") {
    const char* src = GENERATE("4.2/Effects/ChromaFx/Fade To Neg", "4.2/Effects/4000FX/Overlays/Snowfall Loop");
    INFO(src);
    Rig r, q;  // q: no Superimpose source
    for (Rig* x : {&r, &q}) REQUIRE(x->sw.setEffect(load(src)));
    r.sw.select(Bus::Superimpose, Source::In3);
    for (Rig* x : {&r, &q}) x->sw.setTBar(511);
    REQUIRE((r.sw.chromaEngaged() || r.sw.overlayEngaged()));
    CHECK(r.render().px == q.render().px);
}
