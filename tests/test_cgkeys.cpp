// ToasterCG key pages on the Switcher (docs/2026-10-30-cg-keys-design.md, tools/re/CGKEY.md): the page
// decoder and the dissolve law against tests/golden/cgkeys.jsonl (tools/make_cgkeys_golden.py from
// tools/re/cgkey_ref.py), and the Switcher's behaviour with a page loaded.
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/crc32.h"
#include "vt/effect.h"
#include "vt/framestore.h"
#include "vt/switcher.h"

namespace {

using vt::Bus;
using vt::Source;

const vt::Rgba8 kNavy{0x1B, 0x2A, 0x80, 255}, kGreen{20, 200, 20, 255}, kRed{200, 20, 20, 255};

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

uint32_t crcOf(const vt::Frame& f) {
    return vt::crc32({reinterpret_cast<const uint8_t*>(f.px.data()), f.px.size() * sizeof(vt::Rgba8)});
}

vt::Frame solid(vt::Rgba8 c) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), c);
    return f;
}

// Decoded once per page for the whole run.
std::shared_ptr<const vt::CgKeyPage> page(const std::string& name) {
    static std::map<std::string, std::shared_ptr<const vt::CgKeyPage>> cache;
    auto& p = cache[name];
    if (!p) {
        std::string why;
        auto loaded = vt::loadCgKeyPage(dataRoot() / "4.2/CG/Keys" / name, &why);
        INFO(name << ": " << why);
        REQUIRE(loaded);
        p = std::make_shared<const vt::CgKeyPage>(std::move(*loaded));
    }
    return p;
}

std::vector<int> ints(const std::vector<int64_t>& v) {
    std::vector<int> out;
    for (int64_t x : v) out.push_back(int(x));
    return out;
}

// Main = In1 navy, Preview = In2 green, In3 red; the store live; nothing on Superimpose.
struct Rig {
    vt::Frame in1 = solid(kNavy), in2 = solid(kGreen), in3 = solid(kRed), in4 = solid(kRed);
    vt::SourceFrames src{{&in1, &in2, &in3, &in4}};
    vt::Switcher sw;
    vt::Frame render() const {
        vt::Frame out;
        sw.render(src, out);
        return out;
    }
    void ticks(int n) {
        for (int i = 0; i < n; ++i) {
            render();
            sw.tick();
        }
    }
};

// The fill over Main at level L, as the golden script computes it.
vt::Frame mixed(const vt::CgKeyPage& p, const vt::Frame& main, int level) {
    vt::Frame out;
    for (size_t i = 0; i < out.px.size(); ++i) {
        const int k = vt::cgMix(p.alpha[i], level, vt::CgMixLaw::CodeAndFraction);
        const vt::Rgba8 m = main.px[i], f = p.fill.px[i];
        auto mix = [&](int a, int b) { return uint8_t((a * (255 - k) + b * k + 127) / 255); };
        out.px[i] = {mix(m.r, f.r), mix(m.g, f.g), mix(m.b, f.b), 255};
    }
    return out;
}

}  // namespace

TEST_CASE("CG key pages decode as cgkey_ref.load places them", "[cgkeys][golden]") {
    int pages = 0;
    for (const auto& r : golden::readLines("cgkeys.jsonl")) {
        if (str(r, "kind") != "page") continue;
        ++pages;
        const std::filesystem::path path = golden::sourcePath(str(r, "source"));
        INFO(str(r, "source"));
        CHECK(vt::isCgKeyPage(path));
        const auto p = page(path.filename().string());
        CHECK(crcOf(p->fill) == uint32_t(num(r, "fill_crc")));
        REQUIRE(p->alpha.size() == size_t(vt::kRasterW) * vt::kRasterH);
        CHECK(vt::crc32(p->alpha) == uint32_t(num(r, "alpha_crc")));
        CHECK(p->timing.durationTicks == std::array<int, 4>{90, 60, 30, 60});
        CHECK(p->timing.defaultSpeed == vt::Speed::Medium);

        // Program at each level (T-bar 2L) over Main = navy.
        Rig rig;
        REQUIRE(rig.sw.loadCgPage(p));
        const auto& levels = arr(r, "levels");
        const auto& crcs = arr(r, "mix_crc");
        for (size_t i = 0; i < levels.size(); ++i) {
            rig.sw.setTBar(int(2 * levels[i]));
            REQUIRE(rig.sw.cgPage().level == levels[i]);
            CHECK(crcOf(rig.render()) == uint32_t(crcs[i]));
        }
    }
    CHECK(pages == 6);
}

TEST_CASE("CG key page dissolve levels match cgkey_ref.ramp", "[cgkeys][golden]") {
    int ramps = 0;
    for (const auto& r : golden::readLines("cgkeys.jsonl")) {
        if (str(r, "kind") != "ramp") continue;
        ++ramps;
        INFO("speed " << num(r, "speed") << " on " << num(r, "on") << " tbar " << num(r, "tbar"));
        CHECK(vt::cgRamp(int(num(r, "speed")), num(r, "on") != 0, int(num(r, "tbar"))) == ints(arr(r, "levels")));
    }
    CHECK(ramps == 16);
}

TEST_CASE("the CG mix guess: a solid pixel follows L, final opacities 50 / 75 / 100 %", "[cgkeys]") {
    for (int L = 0; L < 256; ++L) CHECK(vt::cgMix(3, L, vt::CgMixLaw::CodeAndFraction) == L);
    CHECK(vt::cgMix(0, 255, vt::CgMixLaw::CodeAndFraction) == 0);
    CHECK(vt::cgMix(1, 255, vt::CgMixLaw::CodeAndFraction) == 127);
    CHECK(vt::cgMix(2, 255, vt::CgMixLaw::CodeAndFraction) == 191);
    CHECK(vt::cgMix(2, 100, vt::CgMixLaw::CodeAndFraction) == 36);  // max(0, L - 64)
    CHECK(vt::cgMix(1, 100, vt::CgMixLaw::CodeAndFraction) == 0);
    CHECK(vt::cgMix(2, 255, vt::CgMixLaw::Linear) == 170);
}

TEST_CASE("a CG key page loads into a DV by the framestore rule with nothing on air", "[cgkeys][switcher]") {
    const auto p = page("LowerThird1");
    Rig r;
    r.sw.setKeyLevel(100);
    REQUIRE(r.sw.loadCgPage(p));
    CHECK(r.sw.frozen());
    CHECK(r.sw.still(Source::DV2).px == p->fill.px);  // no DV on a bus: DV2
    CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, false, 0, true});
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.keyMode() == vt::KeyMode::Off);
    CHECK(r.render().px == r.in1.px);
    CHECK(r.sw.speed() == vt::Speed::Medium);

    // Main shows DV2: the page goes into DV1.
    Rig q;
    REQUIRE(q.sw.setFrozen(true));
    REQUIRE(q.sw.select(Bus::Main, Source::DV2));
    REQUIRE(q.sw.loadCgPage(p));
    CHECK(q.sw.cgPage().dv == Source::DV1);
    CHECK(q.sw.still(Source::DV1).px == p->fill.px);
    CHECK(q.sw.selected(Bus::Main) == Source::DV2);

    // A transition running: refused as a framestore load is.
    Rig t;
    t.sw.setTBar(200);
    std::string why;
    CHECK_FALSE(t.sw.loadCgPage(p, false, &why));
    CHECK(why == "a transition is running: finish it first");
    CHECK(t.sw.cgPage().dv == Source::None);
}

TEST_CASE("Auto dissolves a CG key page on in N + 1 steps of 4 fields, the next Auto off", "[cgkeys][switcher]") {
    const auto p = page("LowerThird1");
    Rig r;
    REQUIRE(r.sw.loadCgPage(p));
    r.sw.autoTransition(r.sw.speed());
    const std::vector<int> on = vt::cgRamp(60, false, 0);
    REQUIRE(on.size() == 16);
    for (int level : on) {
        CHECK(r.sw.autoRunning());
        CHECK(r.sw.cgPage().level == level);
        CHECK(r.sw.cgPage().up);
        CHECK(r.sw.tbar() == 0);  // the handle does not move during the Auto
        r.ticks(4);
    }
    CHECK_FALSE(r.sw.autoRunning());
    CHECK(r.sw.tbar() == 511);
    CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, true, 255, true});
    CHECK(r.sw.selected(Bus::Superimpose) == Source::DV2);
    CHECK(r.sw.keyMode() == vt::KeyMode::Off);  // the keyer is shown OFF while the page is up
    CHECK(r.render().px == mixed(*p, r.in1, 255).px);

    r.sw.autoTransition(r.sw.speed());
    for (int level : vt::cgRamp(60, true, 511)) {
        CHECK(r.sw.cgPage().level == level);
        r.ticks(4);
    }
    CHECK_FALSE(r.sw.autoRunning());
    // The end: Superimpose emptied, Main and Preview kept, the plain Fade loaded, the page gone from
    // the panel, its picture still in DV2 and the store still frozen.
    CHECK(r.sw.cgPage() == vt::CgPageState{});
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    CHECK(r.sw.effect().name() == vt::makeDissolve()->name());
    CHECK(r.sw.tbar() == 0);
    CHECK(r.sw.still(Source::DV2).px == p->fill.px);
    CHECK(r.sw.frozen());
    CHECK(r.render().px == r.in1.px);

    // With Preview == Main the cut gives Preview the page's DV.
    Rig s;
    REQUIRE(s.sw.select(Bus::Preview, Source::In1));
    REQUIRE(s.sw.loadCgPage(p));
    s.sw.autoTransition(s.sw.speed());
    s.ticks(64);
    s.sw.autoTransition(s.sw.speed());
    s.ticks(68);
    CHECK(s.sw.selected(Bus::Preview) == Source::DV2);
    CHECK(s.sw.selected(Bus::Main) == Source::In1);
}

TEST_CASE("a CG key page's Auto runs at the speed button's value", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("KeyPage2")));
    r.sw.autoTransition(vt::Speed::Fast);  // 30: N = 7, 8 writes
    r.ticks(4 * 7);
    CHECK(r.sw.autoRunning());
    CHECK(r.sw.cgPage().level == 255);
    r.ticks(4);
    CHECK_FALSE(r.sw.autoRunning());
    CHECK(r.sw.speed() == vt::Speed::Fast);

    vt::HardwareGuesses g;
    g.cgFieldsPerStep = 1;
    Rig q;
    q.sw.setGuesses(g);
    REQUIRE(q.sw.loadCgPage(page("KeyPage2")));
    q.sw.autoTransition(vt::Speed::Medium);
    q.ticks(16);
    CHECK_FALSE(q.sw.autoRunning());
}

TEST_CASE("the T-bar dissolves a CG key page by hand: L = T-bar / 2", "[cgkeys][switcher]") {
    const auto p = page("LowerThird1");
    Rig r;
    REQUIRE(r.sw.loadCgPage(p));
    r.sw.setTBar(300);
    CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, true, 150, true});
    CHECK(r.sw.tbar() == 300);
    CHECK(r.render().px == mixed(*p, r.in1, 150).px);
    r.sw.setTBar(101);
    CHECK(r.sw.cgPage().level == 50);
    r.sw.setTBar(511);
    CHECK(r.sw.cgPage().level == 255);
    // Back to 0: invisible but up; the next Auto pops it to full and dissolves it off.
    r.sw.setTBar(0);
    CHECK(r.sw.cgPage().level == 0);
    CHECK(r.sw.cgPage().up);
    CHECK(r.render().px == r.in1.px);
    r.sw.autoTransition(r.sw.speed());
    CHECK(r.sw.cgPage().level == 255);
    r.ticks(4);
    CHECK(r.sw.cgPage().level == 238);
}

TEST_CASE("reverse Auto turns a CG key page round only with the T-bar part-way", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
    r.sw.autoTransition(r.sw.speed(), vt::Direction::Reverse);  // at 0: the same as Auto (on)
    CHECK(r.sw.cgPage().level == 0);
    r.ticks(64);
    CHECK(r.sw.cgPage().level == 255);

    Rig q;
    REQUIRE(q.sw.loadCgPage(page("LowerThird1")));
    q.sw.setTBar(300);  // going on, part-way: reverse takes it off from there
    q.sw.autoTransition(q.sw.speed(), vt::Direction::Reverse);
    const std::vector<int> off = vt::cgRamp(60, true, 300);
    for (int level : off) {
        CHECK(q.sw.cgPage().level == level);
        q.ticks(4);
    }
    CHECK(q.sw.cgPage().dv == Source::None);  // the off-dissolve's end clears it

    Rig f;
    REQUIRE(f.sw.loadCgPage(page("LowerThird1")));
    f.sw.setTBar(300);
    f.sw.autoTransition(f.sw.speed());  // forward from part-way: on, with the original's step count
    CHECK(f.sw.cgPage().level == vt::cgRamp(60, false, 300).front());
    f.ticks(4 * int(vt::cgRamp(60, false, 300).size()));
    CHECK(f.sw.cgPage().level == 255);
    CHECK(f.sw.tbar() == 511);
}

TEST_CASE("an Auto that divides by zero on the Amiga runs one step to the end", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
    r.sw.setTBar(34);  // <= ceil(511 / 15): no steps
    r.sw.autoTransition(r.sw.speed());
    CHECK(r.sw.cgPage().level == 255);
    r.ticks(4);
    CHECK_FALSE(r.sw.autoRunning());
    CHECK(r.sw.cgPage().up);
    r.sw.setTBar(20);
    r.sw.autoTransition(r.sw.speed());  // off from part-way, the same trap
    CHECK(r.sw.cgPage().level == 0);
    r.ticks(4);
    CHECK(r.sw.cgPage().dv == Source::None);
}

TEST_CASE("the keyer buttons cut a CG key page off; Auto then does not bring it back", "[cgkeys][switcher]") {
    for (vt::KeyMode m : {vt::KeyMode::Off, vt::KeyMode::Black, vt::KeyMode::White}) {
        Rig r;
        REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
        r.sw.setTBar(511);
        r.sw.setKeyMode(m);
        CHECK(r.sw.keyMode() == vt::KeyMode::Off);
        CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, false, 255, true});  // still loaded, mark kept
        CHECK(r.render().px == r.in1.px);
        r.sw.autoTransition(r.sw.speed());  // the quirk: Main over Main
        r.ticks(64);
        CHECK_FALSE(r.sw.cgPage().up);
        CHECK(r.render().px == r.in1.px);
        // Its DV on Superimpose cuts it on.
        REQUIRE(r.sw.select(Bus::Superimpose, Source::DV2));
        CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, true, 255, true});
        CHECK(r.sw.tbar() == 511);
    }
}

TEST_CASE("the clip level leaves a CG key page alone", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
    r.sw.setTBar(511);
    r.sw.setKeyLevel(40);
    CHECK(r.sw.keyLevel() == 40);
    CHECK(r.sw.cgPage().up);
}

TEST_CASE("another Superimpose source cuts a CG key page off and leaves the bus empty", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
    r.sw.setTBar(511);
    REQUIRE(r.sw.select(Bus::Superimpose, Source::In3));
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.keyMode() == vt::KeyMode::Off);
    CHECK(r.sw.cgPage().dv == Source::DV2);
    CHECK_FALSE(r.sw.cgPage().up);
    CHECK(r.render().px == r.in1.px);
    // The page's own DV cuts it on, even DV2 with the store frozen.
    REQUIRE(r.sw.select(Bus::Superimpose, Source::DV2));
    CHECK(r.sw.cgPage().up);
}

TEST_CASE("Take with a CG key page up is a cut that drops it; with it down it cuts it on too", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
    r.sw.setTBar(511);
    r.sw.take();
    CHECK(r.sw.selected(Bus::Main) == Source::In2);
    CHECK(r.sw.selected(Bus::Preview) == Source::In1);
    CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, false, 255, false});  // loaded, the mark reset
    CHECK(r.render().px == r.in2.px);

    r.sw.take();  // not up: Preview to Main, and the page cuts on
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, true, 255, false});
    CHECK(r.sw.tbar() == 511);

    // Main changes under it: the page stays over the new Main.
    REQUIRE(r.sw.select(Bus::Main, Source::In3));
    CHECK(r.sw.cgPage().up);
    CHECK(r.render().px == mixed(*page("LowerThird1"), r.in3, 255).px);
}

TEST_CASE("clicking the loaded CG key page again puts its DV on Preview; a double click cuts it on", "[cgkeys][switcher]") {
    const auto p = page("LowerThird1");
    Rig r;
    REQUIRE(r.sw.loadCgPage(p));
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));  // the same path
    CHECK(r.sw.selected(Bus::Preview) == Source::DV2);
    CHECK_FALSE(r.sw.cgPage().up);

    Rig d;  // press, then double click: loaded once, cut on, Preview untouched
    REQUIRE(d.sw.loadCgPage(p));
    REQUIRE(d.sw.loadCgPage(p, true));
    CHECK(d.sw.selected(Bus::Preview) == Source::In2);
    CHECK(d.sw.cgPage() == vt::CgPageState{Source::DV2, true, 255, true});
    Rig e;  // a double click alone loads and cuts on
    REQUIRE(e.sw.loadCgPage(p, true));
    CHECK(e.sw.cgPage().up);
}

TEST_CASE("another crouton clears the CG key page", "[cgkeys][switcher]") {
    const auto p = page("LowerThird1");
    {
        Rig r;  // an effect
        REQUIRE(r.sw.select(Bus::Preview, Source::In1));
        REQUIRE(r.sw.loadCgPage(p));
        r.sw.setTBar(511);
        REQUIRE(r.sw.setEffect(std::shared_ptr<const vt::Effect>(vt::makeDissolve())));
        CHECK(r.sw.cgPage() == vt::CgPageState{});
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
        CHECK(r.sw.selected(Bus::Preview) == Source::DV2);  // Preview was Main
        CHECK(r.sw.tbar() == 0);
        CHECK(r.render().px == r.in1.px);
    }
    {
        Rig r;  // a framestore
        REQUIRE(r.sw.loadCgPage(p));
        r.sw.setTBar(511);
        REQUIRE(r.sw.loadStill(r.in3));
        CHECK(r.sw.cgPage().dv == Source::None);
        CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    }
    {
        Rig r;  // a control crouton (the app calls clearCgPage first), another page
        REQUIRE(r.sw.loadCgPage(p));
        r.sw.clearCgPage();
        CHECK(r.sw.cgPage().dv == Source::None);
        REQUIRE(r.sw.loadCgPage(p));
        REQUIRE(r.sw.loadCgPage(page("KeyPage1")));
        CHECK(r.sw.cgPage() == vt::CgPageState{Source::DV2, false, 0, true});
        CHECK(r.sw.still(Source::DV2).px == page("KeyPage1")->fill.px);
    }
}

TEST_CASE("f / l are refused while a CG key page is loaded; the LIVE button is not", "[cgkeys][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgPage(page("LowerThird1")));
    std::string why;
    CHECK_FALSE(r.sw.freezeKey(false, &why));
    CHECK(why == "f / l: a CG key page is loaded");
    CHECK(r.sw.frozen());
    r.sw.setTBar(511);
    REQUIRE(r.sw.setFrozen(false));  // LIVE: the page's DV shows the live input, the alpha stays
    r.ticks(8);
    CHECK(r.sw.cgPage().up);
    CHECK(r.render().px == r.in1.px);  // live DV2 = In1 (navy) keyed over Main = In1
    r.sw.clearCgPage();
    CHECK(r.sw.freezeKey(true));
    CHECK(r.sw.frozen());
}
