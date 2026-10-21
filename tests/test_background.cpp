#include <cstdint>
#include <optional>

#include "catch_amalgamated.hpp"
#include "vt/background.h"
#include "vt/raster.h"

namespace {

bool same(vt::Rgba8 a, vt::Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

bool sameFrame(const vt::Frame& a, const vt::Frame& b) {
    for (size_t i = 0; i < a.px.size(); ++i)
        if (!same(a.px[i], b.px[i])) return false;
    return true;
}

}  // namespace

TEST_CASE("frame is 768x482 row-major") {
    vt::Frame f;
    REQUIRE(f.px.size() == size_t(vt::kRasterW) * vt::kRasterH);
    f.at(5, 2) = {1, 2, 3, 4};
    CHECK(same(f.px[2 * vt::kRasterW + 5], {1, 2, 3, 4}));
}

// switcher.main digital matte table 0x6d88 (DV YIQ), converted through the inverse ChromaFX
// RGBYIQ matrix with DV black 71 / white 200 -> 0 / 255 (tools/re/FINDINGS.md).
TEST_CASE("bkgColour matches the Switcher's colour table") {
    using vt::BkgColour;
    CHECK(same(vt::bkgColour(BkgColour::Black), {0, 0, 0, 255}));
    CHECK(same(vt::bkgColour(BkgColour::White), {255, 255, 255, 255}));
    CHECK(same(vt::bkgColour(BkgColour::Red), {191, 1, 0, 255}));
    CHECK(same(vt::bkgColour(BkgColour::Green), {0, 192, 0, 255}));
    CHECK(same(vt::bkgColour(BkgColour::Blue), {0, 0, 189, 255}));
    CHECK(same(vt::bkgColour(BkgColour::Yellow), {192, 192, 1, 255}));
    CHECK(same(vt::bkgColour(BkgColour::Magenta), {191, 0, 190, 255}));
    CHECK(same(vt::bkgColour(BkgColour::Cyan), {0, 189, 192, 255}));
}

TEST_CASE("customBkgColour maps the Switcher's preset hardware words") {
    using vt::BkgColour;
    CHECK(same(vt::customBkgColour(0x0000), vt::bkgColour(BkgColour::Black)));
    CHECK(same(vt::customBkgColour(0x0bb0), vt::bkgColour(BkgColour::White)));
    CHECK(same(vt::customBkgColour(0x0906), vt::bkgColour(BkgColour::Red)));
    CHECK(same(vt::customBkgColour(0x00ba), vt::bkgColour(BkgColour::Green)));
    CHECK(same(vt::customBkgColour(0x0071), vt::bkgColour(BkgColour::Blue)));
    CHECK(same(vt::customBkgColour(0x0d51), vt::bkgColour(BkgColour::Yellow)));
    CHECK(same(vt::customBkgColour(0x090a), vt::bkgColour(BkgColour::Magenta)));
    CHECK(same(vt::customBkgColour(0x02d6), vt::bkgColour(BkgColour::Cyan)));
    // The Switcher's "super white" word (0xe7276).
    CHECK(same(vt::customBkgColour(0x0fff), vt::bkgColour(BkgColour::White)));
}

TEST_CASE("customBkgColour falls back to 12-bit 0x0RGB for other words") {
    CHECK(same(vt::customBkgColour(0x0f80), {255, 136, 0, 255}));
    CHECK(same(vt::customBkgColour(0x0123), {17, 34, 51, 255}));
}

TEST_CASE("renderSnow is deterministic per frame number") {
    vt::Frame a, b, c;
    vt::renderSnow(a, 7);
    vt::renderSnow(b, 7);
    vt::renderSnow(c, 8);
    CHECK(sameFrame(a, b));
    CHECK_FALSE(sameFrame(a, c));
    // Frame 8 is fresh noise, not frame 7 shifted along by a few pixels.
    for (size_t shift = 0; shift <= 4; ++shift) {
        int match = 0;
        for (size_t i = 0; i < 1000; ++i) match += c.px[i].r == a.px[i + shift].r;
        CHECK(match < 50);  // independent bytes match ~1000/256 = 4 times
    }
    // Grey, opaque, and actually noisy.
    bool dark = false, light = false;
    for (auto& p : a.px) {
        REQUIRE((p.r == p.g && p.g == p.b && p.a == 255));
        dark |= p.r < 64;
        light |= p.r > 192;
    }
    CHECK(dark);
    CHECK(light);
}

TEST_CASE("an effect's colour comes from tags 0x23 / 0x24 as switcher.main reads them") {
    using vt::BkgColour;
    using vt::EffectColour;
    CHECK(vt::effectColourFromTags(2, std::nullopt) == EffectColour::of(BkgColour::White));
    CHECK(vt::effectColourFromTags(9, std::nullopt)->isSnow());
    // tag 0x24 present: its handler 0xe6db4 sets 0x23 to -1, so the custom word wins
    CHECK(vt::effectColourFromTags(8, 0x0906) == EffectColour::customWord(0x0906));
    CHECK(vt::effectColourFromTags(std::nullopt, 0x8000)->isSnow());
    // negative 0x23 (the low byte, ext.w): the custom word, missing = 0
    CHECK(vt::effectColourFromTags(0xffffffffu, 0x0906) == EffectColour::customWord(0x0906));
    CHECK(same(vt::effectColourFromTags(0xffffffffu, 0x0906)->rgb(), vt::bkgColour(BkgColour::Red)));
    CHECK(vt::effectColourFromTags(0xffffffffu, std::nullopt) == EffectColour::customWord(0));
    CHECK(vt::effectColourFromTags(0x80u, std::nullopt) == EffectColour::customWord(0));
    CHECK(vt::effectColourFromTags(0x102u, std::nullopt) == EffectColour::of(BkgColour::White));  // low byte 2
    // missing, or above 9: no colour of its own, the generator keeps the BKG colour (0xe723e, 0xe727c)
    CHECK_FALSE(vt::effectColourFromTags(std::nullopt, std::nullopt));
    CHECK_FALSE(vt::effectColourFromTags(10u, std::nullopt));
    CHECK_FALSE(vt::effectColourFromTags(127u, std::nullopt));
    // 0: GUESS Black (the Switcher reuses the last Effect Color, $332(a5))
    CHECK(vt::effectColourFromTags(0u, std::nullopt) == EffectColour{});
    CHECK(EffectColour{} == EffectColour::of(BkgColour::Black));
    // rgb: the generator's colours; Snow is black (noise has no single colour)
    CHECK(same(EffectColour::of(BkgColour::Blue).rgb(), vt::bkgColour(BkgColour::Blue)));
    CHECK(same(EffectColour::of(BkgColour::Snow).rgb(), {0, 0, 0, 255}));
    CHECK(same(EffectColour::customWord(0x0f80).rgb(), vt::customBkgColour(0x0f80)));
    CHECK(same(EffectColour::customWord(0x8f80).rgb(), {0, 0, 0, 255}));
}
