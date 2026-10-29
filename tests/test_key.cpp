#include "catch_amalgamated.hpp"
#include "vt/key.h"

using namespace vt;

namespace {

Frame solid(Rgba8 c) {
    Frame f;
    for (auto& p : f.px) p = c;
    return f;
}

Frame grayRamp() {
    Frame f;
    for (size_t i = 0; i < f.px.size(); ++i) {
        const auto g = uint8_t(i % 256);
        f.px[i] = {g, g, g, 255};
    }
    return f;
}

constexpr Rgba8 kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255};

}  // namespace

TEST_CASE("keyClip: the clip value from the level (KEY.md §0)") {
    CHECK(keyClip(true, 1) == 0);
    CHECK(keyClip(true, 128) == 127);
    CHECK(keyClip(true, 256) == 255);
    CHECK(keyClip(false, 1) == 255);
    CHECK(keyClip(false, 128) == 128);
    CHECK(keyClip(false, 256) == 0);
}

TEST_CASE("keyLuma") {
    CHECK(keyLuma({255, 255, 255, 255}) == 255);
    CHECK(keyLuma({0, 0, 0, 255}) == 0);
    CHECK(keyLuma({100, 50, 200, 255}) == (77 * 100 + 150 * 50 + 29 * 200 + 128) >> 8);
}

TEST_CASE("lumaKey: White keys out the bright, Black the dark") {
    const Frame fill = solid(kRed), bg = solid(kGreen), sig = grayRamp();
    Frame out;
    lumaKey(fill, bg, sig, 128, false, 0, out);
    for (size_t i = 0; i < out.px.size(); ++i) {
        CHECK(out.px[i] == ((i % 256) <= 128 ? kRed : kGreen));
    }
    lumaKey(fill, bg, sig, 128, true, 0, out);
    for (size_t i = 0; i < out.px.size(); ++i) {
        CHECK(out.px[i] == ((i % 256) >= 128 ? kRed : kGreen));
    }
}

TEST_CASE("lumaKey softness") {
    Frame sig;
    auto gray = [&](uint8_t g) { for (auto& p : sig.px) p = {g, g, g, 255}; };
    const Frame fill = solid(kRed), bg = solid(kGreen);
    Frame out;
    gray(102);
    lumaKey(fill, bg, sig, 100, false, 4, out);
    CHECK(out.px[0] == kGreen);
    gray(100);
    lumaKey(fill, bg, sig, 100, false, 4, out);
    CHECK(out.px[0] == Rgba8{127, 128, 0, 255});  // w = 127: mix255(0,255,127) = 127, mix255(255,0,127) = 128
    gray(98);
    lumaKey(fill, bg, sig, 100, false, 4, out);
    CHECK(out.px[0] == kRed);

    const Frame ramp = grayRamp();
    Frame a, b;
    lumaKey(fill, bg, ramp, 100, false, -5, a);
    lumaKey(fill, bg, ramp, 100, false, 0, b);
    CHECK(a.px == b.px);
    lumaKey(fill, bg, ramp, 100, false, 1000, a);
    lumaKey(fill, bg, ramp, 100, false, 255, b);
    CHECK(a.px == b.px);
}

TEST_CASE("lumaKey may write into an input") {
    const Frame bg = solid(kGreen), sig = grayRamp();
    Frame sep;
    lumaKey(solid(kRed), bg, sig, 100, false, 8, sep);
    Frame fill = solid(kRed);
    lumaKey(fill, bg, sig, 100, false, 8, fill);
    CHECK(fill.px == sep.px);
}

TEST_CASE("keyMoveFor and the locks (key_scan.py)") {
    for (uint32_t d = 0; d <= 40; ++d) {
        KeyMove want = KeyMove::Drops;
        if (d == 2 || d == 17) want = KeyMove::FliesOn;
        else if (d == 3 || d == 18) want = KeyMove::FliesOff;
        else if (d == 5 || d == 6 || d == 19) want = KeyMove::TwoPhase;
        CHECK(keyMoveFor(d) == want);
    }
    CHECK(keyModeLocked(5));
    CHECK(keyModeLocked(19));
    CHECK(keyModeLocked(1));
    CHECK(!keyModeLocked(2));
    CHECK(!keyModeLocked(6));
    CHECK(keyLevelLocked(32));
    CHECK(!keyLevelLocked(5));
}
