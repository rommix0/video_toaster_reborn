// ToasterCG scroll and crawl pages on the Switcher (docs/2026-10-30-cg-scrolls-design.md, tools/re/CGSCROLL.md):
// the page renderer and the per-field motion against tests/golden/cgscrolls.jsonl
// (tools/make_cgscrolls_golden.py from tools/re/cgscroll_ref.py), and the Switcher's behaviour.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/cg_scroll.h"
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

// Decoded once per (page, speed) for the whole run.
std::shared_ptr<const vt::CgScroll> page(const std::string& rel, int speed = -1) {
    static std::map<std::pair<std::string, int>, std::shared_ptr<const vt::CgScroll>> cache;
    auto& p = cache[{rel, speed}];
    if (!p) {
        std::string why;
        auto loaded = vt::loadCgScroll(dataRoot() / "4.2/CG" / rel, &why, speed);
        INFO(rel << ": " << why);
        REQUIRE(loaded);
        p = std::make_shared<const vt::CgScroll>(std::move(*loaded));
    }
    return p;
}
const char* const kMojo = "Scrolls/MojoWorld.Scroll";
const char* const kNight = "Scrolls/NightOfTerror";
const char* const kNovel = "Crawls/Novel.Crawl";

// Main = In1 navy, Preview = In2 green, In3 / In4 red; the store live; nothing on Superimpose.
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
        for (int i = 0; i < n; ++i) sw.tick();
    }
};

// Program shows the page somewhere (some pixel is not Main's navy).
bool showsPage(const vt::Frame& f) {
    return std::any_of(f.px.begin(), f.px.end(), [](vt::Rgba8 p) { return p != kNavy; });
}

}  // namespace

TEST_CASE("CG scroll and crawl pages play as cgscroll_ref.py renders them, field by field", "[cgscrolls][golden]") {
    int records = 0;
    for (const auto& r : golden::readLines("cgscrolls.jsonl")) {
        ++records;
        const std::filesystem::path path = golden::sourcePath(str(r, "source"));
        const int speed = int(num(r, "speed"));
        INFO(str(r, "source") << " speed " << speed);
        CHECK(vt::isCgScroll(path));
        const auto p = page(path.parent_path().filename().string() + "/" + path.filename().string(), speed);
        CHECK(p->speed == speed);
        CHECK(p->endField == num(r, "end_field"));
        if (str(r, "kind") == "scroll") {
            CHECK(p->kind == vt::CgScroll::Kind::Scroll);
            CHECK(p->width == 640);
            CHECK(p->stopField == num(r, "stop_field"));
            CHECK(p->pause == (num(r, "stop_field") >= 0));
            CHECK(p->hold == num(r, "hold"));
        } else {
            CHECK(p->kind == vt::CgScroll::Kind::Crawl);
            CHECK(p->width == 768 * num(r, "windows"));
            CHECK(p->bandTop == arr(r, "band")[0]);
            CHECK(p->bandTop + p->rows == arr(r, "band")[1]);
        }

        Rig rig;
        REQUIRE(rig.sw.loadCgScroll(p, true));
        const auto& fields = arr(r, "fields");
        const auto& crcs = arr(r, "crc");
        int at = 0;
        for (size_t i = 0; i < fields.size(); ++i) {
            rig.ticks(int(fields[i]) - at);
            at = int(fields[i]);
            INFO("field " << at);
            CHECK(rig.sw.cgScroll().playing == (at < p->endField));
            CHECK(crcOf(rig.render()) == uint32_t(crcs[i]));
        }
    }
    CHECK(records == 6);
}

TEST_CASE("a CG scroll or crawl page is TYPE SCRO / CRAW; other files are refused with a reason", "[cgscrolls]") {
    CHECK_FALSE(vt::isCgScroll(dataRoot() / "4.2/CG/Keys/LowerThird1"));
    CHECK_FALSE(vt::isCgScroll(dataRoot() / "4.2/CG/no such page"));
    std::string why;
    CHECK_FALSE(vt::loadCgScroll(dataRoot() / "4.2/CG/Keys/LowerThird1", &why));
    CHECK(why.find("not a scroll or crawl page") != std::string::npos);
    why.clear();
    CHECK_FALSE(vt::loadCgScroll(dataRoot() / "4.2/CG/no such page", &why));
    CHECK_FALSE(why.empty());
    Rig r;
    CHECK_FALSE(r.sw.loadCgScroll(nullptr, true, &why));
    CHECK(why == "not a decoded CG scroll or crawl page");
    CHECK(r.sw.cgScroll() == vt::CgScrollState{});
}

TEST_CASE("the CG scroll palette is the MVPG face and edge colours cut to 12 bits", "[cgscrolls]") {
    const auto p = page(kNight);
    const auto c = [](unsigned rgb12) {
        return vt::Rgba8{uint8_t((rgb12 >> 8) * 17), uint8_t((rgb12 >> 4 & 15) * 17), uint8_t((rgb12 & 15) * 17), 255};
    };
    CHECK(p->palette == std::array<vt::Rgba8, 8>{c(0), c(0xeee), c(0xeee), c(0xeee), c(0), c(0x444), c(0x999), c(0xeee)});
    CHECK(page(kMojo)->speed == 4);
    CHECK(page(kNovel)->pxPerField == 16);
    CHECK(page(kNovel, 4)->pxPerField == 16);  // 4 = 3
}

TEST_CASE("a CG scroll click loads it with nothing on air and the Fade as the effect", "[cgscrolls][switcher]") {
    Rig r;
    REQUIRE(r.sw.select(Bus::Superimpose, Source::In3));  // a key up
    r.sw.setTBar(200);                                     // a take part-way: abandoned as an effect click does
    r.sw.setTBar(0);
    const vt::Frame before = r.render();
    REQUIRE(r.sw.loadCgScroll(page(kMojo)));
    CHECK(r.sw.cgScroll() == vt::CgScrollState{true, false});
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.selected(Bus::Preview) == Source::In2);
    CHECK(r.sw.effect().name() == vt::makeDissolve()->name());
    CHECK_FALSE(r.sw.frozen());
    r.ticks(50);
    CHECK(r.render().px == before.px);

    // Auto and Take act as the Fade.
    Rig a;
    REQUIRE(a.sw.loadCgScroll(page(kMojo)));
    a.sw.autoTransition(a.sw.speed());
    CHECK(a.sw.autoRunning());
    a.ticks(200);
    CHECK_FALSE(a.sw.autoRunning());
    CHECK(a.sw.selected(Bus::Main) == Source::In2);
    a.sw.take();
    CHECK(a.sw.selected(Bus::Main) == Source::In1);
    CHECK(a.sw.cgScroll() == vt::CgScrollState{true, false});
}

TEST_CASE("playing a CG scroll freezes, makes Main = BKG input 1 and keys the page over Main alone", "[cgscrolls][switcher]") {
    const auto p = page(kMojo);
    Rig plain;
    REQUIRE(plain.sw.loadCgScroll(p, true));

    Rig r;
    REQUIRE(r.sw.select(Bus::Main, Source::Bkg));
    REQUIRE(r.sw.select(Bus::Superimpose, Source::In3));  // a key: hidden while the page plays
    r.sw.setKeyLevel(0);
    REQUIRE(r.sw.loadCgScroll(p, true));
    CHECK(r.sw.cgScroll() == vt::CgScrollState{true, true});
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.sw.frozen());
    CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);  // not changed
    CHECK(r.sw.keyMode() == vt::KeyMode::White);
    int at = 0;
    for (int j : {0, 100, 400}) {
        r.ticks(j - at);
        plain.ticks(j - at);
        at = j;
        CHECK(r.render().px == plain.render().px);
    }
    CHECK(showsPage(r.render()));

    // The end: Main alone (a cut), FREEZE on, Main still input 1, the page still loaded. The key is not
    // changed but stays hidden until the next bus update (CGSCROLL.md 3: mixer mode 7).
    r.ticks(p->endField - 1 - at);
    CHECK(r.sw.cgScroll().playing);
    r.sw.tick();
    CHECK(r.sw.cgScroll() == vt::CgScrollState{true, false});
    CHECK(r.sw.frozen());
    CHECK(r.sw.selected(Bus::Main) == Source::In1);
    CHECK(r.render().px == r.in1.px);
    CHECK(r.sw.keyMode() == vt::KeyMode::White);
    CHECK(r.sw.selected(Bus::Superimpose) == Source::In3);
    r.sw.setKeyMode(vt::KeyMode::Off);
    CHECK(r.render().px == r.in1.px);

    // A double click plays it again from the start.
    REQUIRE(r.sw.loadCgScroll(p, true));
    CHECK(r.sw.cgScroll().playing);
    r.ticks(100);
    plain.sw.stopCgScroll();
    REQUIRE(plain.sw.loadCgScroll(p, true));
    plain.ticks(100);
    CHECK(r.render().px == plain.render().px);
}

TEST_CASE("the right button cuts a CG scroll off at once, but not in a pause scroll's hold and fade", "[cgscrolls][switcher]") {
    Rig r;
    REQUIRE(r.sw.loadCgScroll(page(kNovel), true));
    r.ticks(300);
    REQUIRE(showsPage(r.render()));
    r.sw.stopCgScroll();
    CHECK(r.sw.cgScroll() == vt::CgScrollState{true, false});
    CHECK(r.render().px == r.in1.px);
    r.sw.stopCgScroll();  // not playing: nothing
    CHECK(r.sw.cgScroll() == vt::CgScrollState{true, false});

    const auto night = page(kNight);
    REQUIRE(night->pause);
    Rig n;
    REQUIRE(n.sw.loadCgScroll(night, true));
    n.ticks(night->stopField - 1);
    n.sw.stopCgScroll();  // still moving: stops
    CHECK_FALSE(n.sw.cgScroll().playing);

    REQUIRE(n.sw.loadCgScroll(night, true));
    n.ticks(night->stopField);
    n.sw.stopCgScroll();  // the hold
    CHECK(n.sw.cgScroll().playing);
    n.ticks(night->hold + 5);
    n.sw.stopCgScroll();  // the fade
    CHECK(n.sw.cgScroll().playing);
    n.ticks(night->endField - night->stopField - night->hold - 5);
    CHECK_FALSE(n.sw.cgScroll().playing);
    CHECK(n.render().px == n.in1.px);
}

TEST_CASE("another crouton drops a CG scroll page, also while it plays", "[cgscrolls][switcher]") {
    const auto p = page(kNovel);
    for (int playing = 0; playing < 2; ++playing) {
        INFO("playing " << playing);
        auto loaded = [&](Rig& r) {
            REQUIRE(r.sw.loadCgScroll(p, playing != 0));
            r.ticks(300);
        };
        Rig e;  // an effect
        loaded(e);
        REQUIRE(e.sw.setEffect(std::shared_ptr<const vt::Effect>(vt::makeDissolve())));
        CHECK(e.sw.cgScroll() == vt::CgScrollState{});
        CHECK(e.render().px == e.in1.px);
        Rig f;  // a framestore
        loaded(f);
        REQUIRE(f.sw.loadStill(f.in3));
        CHECK(f.sw.cgScroll() == vt::CgScrollState{});
        Rig k;  // a CG key page
        loaded(k);
        std::string why;
        auto key = vt::loadCgKeyPage(dataRoot() / "4.2/CG/Keys/LowerThird1", &why);
        INFO(why);
        REQUIRE(key);
        REQUIRE(k.sw.loadCgPage(std::make_shared<const vt::CgKeyPage>(std::move(*key))));
        CHECK(k.sw.cgScroll() == vt::CgScrollState{});
        Rig c;  // a control crouton
        loaded(c);
        c.sw.clearCgScroll();
        CHECK(c.sw.cgScroll() == vt::CgScrollState{});
        CHECK(c.render().px == c.in1.px);
        Rig s;  // another scroll
        loaded(s);
        REQUIRE(s.sw.loadCgScroll(page(kMojo)));
        CHECK(s.sw.cgScroll() == vt::CgScrollState{true, false});
    }
}

TEST_CASE("playing a CG scroll while a CG key page is loaded clears the key page", "[cgscrolls][switcher]") {
    std::string why;
    auto key = vt::loadCgKeyPage(dataRoot() / "4.2/CG/Keys/LowerThird1", &why);
    INFO(why);
    REQUIRE(key);
    Rig r;
    REQUIRE(r.sw.loadCgPage(std::make_shared<const vt::CgKeyPage>(std::move(*key)), true));  // up
    REQUIRE(r.sw.cgPage().up);
    REQUIRE(r.sw.loadCgScroll(page(kNovel), true));
    CHECK(r.sw.cgPage() == vt::CgPageState{});
    CHECK(r.sw.selected(Bus::Superimpose) == Source::None);
    CHECK(r.sw.cgScroll() == vt::CgScrollState{true, true});
    CHECK(r.render().px == r.in1.px);  // field 0: nothing of the crawl yet
}

TEST_CASE("a key up before a CG scroll stays hidden after its end or stop until the next bus update", "[cgscrolls][switcher]") {
    const auto p = page(kNovel);
    // The key alone shows (level 0: the Superimpose source, red) once it is back.
    auto played = [&](Rig& r, bool toTheEnd) {
        REQUIRE(r.sw.select(Bus::Superimpose, Source::In3));
        r.sw.setKeyLevel(0);
        REQUIRE(r.render().px == r.in3.px);
        REQUIRE(r.sw.loadCgScroll(p, true));
        if (toTheEnd) {
            r.ticks(p->endField);
        } else {
            r.ticks(300);
            r.sw.stopCgScroll();
        }
        REQUIRE_FALSE(r.sw.cgScroll().playing);
        CHECK(r.render().px == r.in1.px);  // Main alone
        r.ticks(20);
        CHECK(r.render().px == r.in1.px);
        r.sw.stopCgScroll();      // not playing: changes nothing
        REQUIRE(r.sw.loadCgScroll(p));  // clicking the page again (message 2) sends nothing either
        CHECK(r.render().px == r.in1.px);
    };
    const std::vector<std::pair<const char*, std::function<void(Rig&)>>> updates{
        {"select", [](Rig& r) { REQUIRE(r.sw.select(Bus::Preview, Source::In4)); }},
        {"setKeyMode", [](Rig& r) { r.sw.setKeyMode(vt::KeyMode::White); }},
        {"setKeyLevel", [](Rig& r) { r.sw.setKeyLevel(0); }},
        {"setEffect", [](Rig& r) { REQUIRE(r.sw.setEffect(std::shared_ptr<const vt::Effect>(vt::makeDissolve()))); }},
        {"loadStill", [](Rig& r) { REQUIRE(r.sw.loadStill(r.in2)); }},
    };
    for (bool end : {true, false})
        for (const auto& [name, update] : updates) {
            INFO((end ? "end, then " : "stop, then ") << name);
            Rig r;
            played(r, end);
            update(r);
            CHECK(r.render().px == r.in3.px);
        }
    // A Take empties Superimpose (KEY.md 4); a key chosen again shows.
    Rig t;
    played(t, false);
    t.sw.take();
    REQUIRE(t.sw.select(Bus::Superimpose, Source::In4));
    CHECK(t.render().px == t.in4.px);
}

TEST_CASE("a corrupt CG scroll or crawl page is refused before its pre-render grows past 64 MB", "[cgscrolls]") {
    // A copy of a shipped page with its MVPG header or placements patched, in a temporary file.
    auto patched = [](const char* rel, const char* name, const std::function<void(std::vector<uint8_t>&, size_t)>& patch) {
        std::vector<uint8_t> d = readFile(dataRoot() / "4.2/CG" / rel);
        const std::string tag = "MVPG";
        const auto at = std::search(d.begin(), d.end(), tag.begin(), tag.end());
        REQUIRE(at != d.end());
        patch(d, size_t(at - d.begin()) + 4);  // the MVPG header
        const auto path = std::filesystem::temp_directory_path() / name;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
        return path;
    };
    auto put16 = [](std::vector<uint8_t>& d, size_t o, unsigned v) {
        d[o] = uint8_t(v >> 8);
        d[o + 1] = uint8_t(v);
    };
    auto get32 = [](const std::vector<uint8_t>& d, size_t o) {
        return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
    };
    // NightOfTerror's blank lines feed max height + 4 rows: 0xFFFF makes each one 42 MB.
    const auto tall = patched(kNight, "vt_cgscroll_tall", [&](std::vector<uint8_t>& d, size_t h) { put16(d, h + 0x0e, 0xffff); });
    // Every crawl glyph followed by a gap of 800 columns: one glyph per window, drawn again in the next.
    const auto wide = patched(kNovel, "vt_cgcrawl_wide", [&](std::vector<uint8_t>& d, size_t h) {
        const uint32_t nplace = get32(d, h), nglyph = get32(d, h + 4);
        for (uint32_t i = 0; i < nplace; ++i) put16(d, h + 0x34 + 12 * nglyph + 8 * i + 2, 800);
    });
    for (const auto& path : {tall, wide}) {
        INFO(path.filename().string());
        const auto t0 = std::chrono::steady_clock::now();
        std::string why;
        CHECK_FALSE(vt::loadCgScroll(path, &why));
        CHECK(why.find("too large") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
        std::filesystem::remove(path);
    }
}
