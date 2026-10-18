#include <array>
#include <stdexcept>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/anim.h"
#include "vt/background.h"
#include "vt/crouton.h"
#include "vt/mix.h"

namespace {

constexpr std::array<char, 4> kAnim{'A', 'N', 'I', 'M'};
constexpr uint32_t kTagMode = 0x80000022;

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

bool same(vt::Rgba8 a, vt::Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

struct Loaded {
    vt::Anim anim;
    std::optional<uint32_t> mode;
};

// Same choice as tools/make_golden.py: the first FORM ANIM; mode from tag 0x80000022 (hunk files have none).
Loaded open(const std::string& source) {
    auto c = vt::loadCrouton(golden::sourcePath(source));
    REQUIRE(c);
    for (auto& f : c->forms)
        if (f.type == kAnim) return {vt::readAnim(c->bytes, f), c->tag(kTagMode)};
    FAIL("no FORM ANIM in " << source);
    throw std::logic_error("unreachable");
}

}  // namespace

TEST_CASE("mix tables match the Python converter", "[mix][golden]") {
    const auto recs = golden::readLines("mixtables.jsonl");
    REQUIRE(!recs.empty());
    const vt::Rgba8 white = vt::bkgColour(vt::BkgColour::White);
    const vt::Rgba8 red = vt::bkgColour(vt::BkgColour::Red);
    int soft = 0, roles = 0, overlay = 0, flashes = 0, stood_in = 0;

    for (const auto& r : recs) {
        const std::string& src = str(r, "source");
        INFO(src);
        auto [a, mode] = open(src);
        const auto plan = vt::planAnim(a, mode);
        const std::string& kind = str(r, "kind");
        const int n = 1 << a.bm.planes;

        if (kind == "soft") {
            ++soft;
            REQUIRE(plan.kind == vt::AnimKind::Soft);
            REQUIRE(plan.inverted == (str(r, "detail") == "inverted"));
        } else if (kind == "roles") {
            ++roles;
            REQUIRE(plan.kind == vt::AnimKind::Roles);
            const auto& d = arr(r, "detail");
            REQUIRE(d.size() == 2);
            REQUIRE(plan.a == d[0]);
            REQUIRE(plan.b == d[1]);
        } else {
            ++overlay;
            REQUIRE(kind == "overlay");
            REQUIRE(plan.kind == vt::AnimKind::Overlay);
            REQUIRE(plan.video == num(r, "detail"));
        }

        const auto tw = vt::mixTable(a, plan, a.bm.cmap, white);
        const auto tr = vt::mixTable(a, plan, a.bm.cmap, red);

        // matte
        const auto& lut = arr(r, "lut");
        REQUIRE(lut.size() == (kind == "overlay" ? 0u : size_t(n)));
        for (size_t i = 0; i < lut.size(); ++i) {
            INFO("index " << i);
            if (tw[i].src != vt::MixSrc::Colour) {
                REQUIRE(tw[i].bLevel == lut[i]);
                REQUIRE((tw[i].src == vt::MixSrc::B) == (lut[i] == 255));
            }
        }

        // overlay colours
        const auto& over = arr(r, "over_rgba");
        if (over.empty()) {
            for (int i = 0; i < n; ++i) REQUIRE(tw[size_t(i)].src != vt::MixSrc::Colour);
        } else {
            REQUIRE(over.size() == size_t(n) * 4);
            bool anyDiffers = false, allBackground = true;
            int colours = 0;
            for (int i = 0; i < n; ++i) {
                INFO("index " << i);
                const vt::Rgba8 g{uint8_t(over[size_t(i) * 4]), uint8_t(over[size_t(i) * 4 + 1]),
                                  uint8_t(over[size_t(i) * 4 + 2]), uint8_t(over[size_t(i) * 4 + 3])};
                if (g.a == 0 && g.r == 0 && g.g == 0 && g.b == 0) {  // a video index
                    REQUIRE(tw[size_t(i)].src != vt::MixSrc::Colour);
                    continue;
                }
                REQUIRE(tw[size_t(i)].src == vt::MixSrc::Colour);
                REQUIRE(tr[size_t(i)].src == vt::MixSrc::Colour);
                ++colours;
                // with a white background every entry equals the golden RGBA exactly
                REQUIRE(same(tw[size_t(i)].colour, g));
                // with another background: palette colours are unchanged, the white stand-in is replaced
                const auto c = tr[size_t(i)].colour;
                REQUIRE(c.a == g.a);
                if (!(c.r == g.r && c.g == g.g && c.b == g.b)) {
                    anyDiffers = true;
                    REQUIRE(g.r == 255);
                    REQUIRE(g.g == 255);
                    REQUIRE(g.b == 255);
                }
                if (!(c.r == red.r && c.g == red.g && c.b == red.b)) allBackground = false;
            }
            REQUIRE(colours > 0);
            // the stand-in is all or nothing: the palette rule covers the whole set of overlay indices
            if (anyDiffers) {
                ++stood_in;
                REQUIRE(allBackground);
            }
        }

        // flash
        const int peak = vt::flashPeak(a, plan);
        REQUIRE(peak == num(r, "flash_peak"));
        if (peak >= 0) ++flashes;
    }
    CHECK(soft > 0);
    CHECK(roles > 0);
    CHECK(overlay > 0);
    INFO("soft " << soft << " roles " << roles << " overlay " << overlay << " flashes " << flashes
                 << " background stand-ins " << stood_in);
    CHECK(flashes == 7);
    CHECK(stood_in > 0);
}

TEST_CASE("mix table: entries past the palette are harmless", "[mix]") {
    vt::Anim a;
    a.bm.w = 4;
    a.bm.h = 4;
    a.bm.planes = 2;
    vt::IndexImage im;
    im.w = 4;
    im.h = 4;
    im.px.assign(16, 0);
    a.frames = {im, im};
    a.reltimes = {0, 1};
    a.cmaps = {std::nullopt, std::nullopt};
    const auto plan = vt::planAnim(a, 4u);
    REQUIRE(plan.kind == vt::AnimKind::Soft);
    const auto t = vt::mixTable(a, plan, {}, vt::Rgba8{255, 255, 255, 255});
    REQUIRE(t[3].bLevel == 255);
    REQUIRE(t[3].src == vt::MixSrc::B);
    REQUIRE(t[1].bLevel == 85);
    for (int i = 4; i < 256; ++i) {
        REQUIRE(t[size_t(i)].src == vt::MixSrc::A);
        REQUIRE(t[size_t(i)].bLevel == 0);
    }
}

namespace {
// A 16x16 Overlay anim: index 0 is the clip, 1..3 solid colour (a palette colour each).
vt::Anim syntheticOverlay(std::initializer_list<int> fills) {
    vt::Anim a;
    a.bm.w = 16;
    a.bm.h = 16;
    a.bm.planes = 2;
    a.bm.cmap = {{0, 0, 0}, {200, 10, 10}, {20, 200, 20}, {20, 20, 200}};
    for (int f : fills) {
        vt::IndexImage im;
        im.w = 16;
        im.h = 16;
        im.px.assign(256, uint8_t(f));
        a.frames.push_back(im);
        a.cmaps.push_back(std::nullopt);
    }
    return a;
}
}  // namespace

TEST_CASE("flash peak is a frame index, not reltime-expanded", "[mix]") {
    // frames: clip, clip, solid cover, clip; reltimes 0, 3, 1, 1 -> the expanded index would be 4
    auto a = syntheticOverlay({0, 0, 1, 0});
    a.reltimes = {0, 3, 1, 1};
    const auto plan = vt::planAnim(a, 7u);
    REQUIRE(plan.kind == vt::AnimKind::Overlay);
    REQUIRE(plan.video == 0);
    REQUIRE(vt::flashPeak(a, plan) == 2);
}

TEST_CASE("flash peak ignores pixels past the palette", "[mix]") {
    auto a = syntheticOverlay({0, 1, 0});
    a.frames[1].px[5] = 200;  // not a palette index: must not read out of bounds
    a.reltimes = {0, 1, 1};
    const auto plan = vt::planAnim(a, 7u);
    REQUIRE(plan.kind == vt::AnimKind::Overlay);
    REQUIRE(vt::flashPeak(a, plan) == 1);
}

TEST_CASE("mix table follows the palette it is given", "[mix]") {
    auto a = syntheticOverlay({0, 1, 2, 3, 0});
    a.reltimes = {0, 1, 1, 1, 1};
    const auto plan = vt::planAnim(a, 7u);
    REQUIRE(plan.kind == vt::AnimKind::Overlay);
    const vt::Rgba8 bg{255, 255, 255, 255};
    const auto t1 = vt::mixTable(a, plan, a.bm.cmap, bg);
    REQUIRE(t1[1].src == vt::MixSrc::Colour);
    REQUIRE(same(t1[1].colour, vt::Rgba8{200, 10, 10, 255}));
    const std::vector<vt::Rgb> second{{0, 0, 0}, {10, 10, 90}, {90, 10, 10}, {10, 90, 10}};
    const auto t2 = vt::mixTable(a, plan, second, bg);
    REQUIRE(same(t2[1].colour, vt::Rgba8{10, 10, 90, 255}));
    REQUIRE(same(t2[3].colour, vt::Rgba8{10, 90, 10, 255}));
    REQUIRE(t2[0].src == vt::MixSrc::A);
    // an all-black palette falls back to the background
    const auto t3 = vt::mixTable(a, plan, std::vector<vt::Rgb>(4, vt::Rgb{0, 0, 0}), vt::Rgba8{191, 1, 0, 255});
    // three used indices sharing one colour make a glow: the index is the alpha
    REQUIRE(same(t3[2].colour, vt::Rgba8{191, 1, 0, 170}));
}
