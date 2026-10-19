#include <algorithm>
#include <initializer_list>
#include <stdexcept>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/assets/font.h"
#include "vt/crc32.h"

namespace {

using vt::assets::ColorFont;
using vt::assets::IndexedImage;

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

const char* const kCg14 = "4.2/Programs/System_Support/Fonts/CGInterface/14.4C";

vt::assets::HunkImage hunks(const std::string& rel) { return vt::assets::loadHunks(readFile(golden::sourcePath(rel))); }

const ColorFont& font(const std::string& rel) {
    static std::map<std::string, ColorFont> cache;
    auto it = cache.find(rel);
    if (it == cache.end()) it = cache.emplace(rel, vt::assets::loadColorFont(hunks(rel))).first;
    return it->second;
}

// The CRC the generator takes over the glyph tables: little-endian int16 of each table in turn.
uint32_t tablesCrc(std::initializer_list<const std::vector<int>*> tables) {
    std::vector<uint8_t> b;
    for (const auto* t : tables)
        for (int v : *t) {
            b.push_back(uint8_t(v & 0xff));
            b.push_back(uint8_t(v >> 8 & 0xff));
        }
    return vt::crc32(b);
}

// A JAM2 render of `text` into a width x ySize image, pen at (0, baseline): what the generator hashes.
IndexedImage render(const ColorFont& f, const std::string& text) {
    IndexedImage img{vt::assets::textWidth(f, text), f.ySize, {}};
    img.px.assign(size_t(img.w) * size_t(img.h), 0);
    vt::assets::drawText(img, f, 0, f.baseline, text, true);
    return img;
}

const std::vector<golden::Record>& fontsGolden() {
    static const auto g = golden::readLines("fonts.jsonl");
    return g;
}

}  // namespace

TEST_CASE("colour fonts match the reference decoder") {
    int fonts = 0;
    for (const auto& r : fontsGolden()) {
        if (str(r, "rec") != "font") continue;
        ++fonts;
        const auto& f = font(str(r, "file"));
        INFO(str(r, "file"));
        CHECK(f.ySize == num(r, "ysize"));
        CHECK(f.baseline == num(r, "baseline"));
        CHECK(f.loChar == num(r, "lo"));
        CHECK(f.hiChar == num(r, "hi"));
        CHECK(f.depth == num(r, "depth"));
        CHECK(f.proportional == (num(r, "proportional") != 0));
        const auto& words = arr(r, "colours");
        REQUIRE(f.colours.size() == words.size());
        for (size_t i = 0; i < words.size(); ++i)
            CHECK(f.colours[i] == vt::Rgba8{uint8_t((words[i] >> 8 & 15) * 17), uint8_t((words[i] >> 4 & 15) * 17),
                                            uint8_t((words[i] & 15) * 17), 255});
        CHECK(f.strike.w == num(r, "strike_w"));
        CHECK(f.strike.h == num(r, "strike_h"));
        CHECK(vt::crc32(f.strike.px) == uint32_t(num(r, "strike_crc32")));
        const size_t n = size_t(f.hiChar - f.loChar + 2);
        REQUIRE(f.charLoc.size() == n);
        REQUIRE(f.charWidth.size() == n);
        REQUIRE(f.charSpace.size() == n);
        REQUIRE(f.charKern.size() == n);
        CHECK(tablesCrc({&f.charWidth, &f.charSpace, &f.charKern}) == uint32_t(num(r, "widths_crc32")));
        CHECK(tablesCrc({&f.charLoc}) == uint32_t(num(r, "offsets_crc32")));
    }
    CHECK(fonts == 8);
}

TEST_CASE("colour font text renders match the reference") {
    int renders = 0;
    for (const auto& r : fontsGolden()) {
        if (str(r, "rec") != "render") continue;
        ++renders;
        const auto& f = font(str(r, "file"));
        INFO(str(r, "file") << " \"" << str(r, "text") << "\"");
        const auto img = render(f, str(r, "text"));
        CHECK(vt::assets::textWidth(f, str(r, "text")) == num(r, "width"));
        CHECK(img.w == num(r, "width"));
        CHECK(vt::crc32(img.px) == uint32_t(num(r, "crc32")));
    }
    CHECK(renders == 18);
}

TEST_CASE("128 in CGInterface 14 matches the golden render") {
    const auto& f = font(kCg14);
    const golden::Record* g = nullptr;
    for (const auto& r : fontsGolden())
        if (str(r, "rec") == "render" && str(r, "file") == kCg14 && str(r, "text") == "128") g = &r;
    REQUIRE(g);
    const auto img = render(f, "128");
    CHECK(vt::crc32(img.px) == uint32_t(num(*g, "crc32")));
    CHECK(vt::assets::textWidth(f, "128") == 24);  // 3 x the 8 px advance of the fixed-width font
}

TEST_CASE("textWidth agrees with the rendered width") {
    // JAM2 paints every advance cell, so for the fixed-width fonts (no kerning) the touched span is the width.
    for (const char* rel : {kCg14, "4.2/Programs/System_Support/Fonts/NewToaster/16.16C"}) {
        const auto& f = font(rel);
        IndexedImage img{200, f.ySize, std::vector<uint8_t>(size_t(200) * size_t(f.ySize), 99)};
        const int x0 = 10;
        vt::assets::drawText(img, f, x0, f.baseline, "Video Toaster", true, 7);
        int lo = img.w, hi = -1;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                if (img.px[size_t(y) * size_t(img.w) + size_t(x)] != 99) lo = std::min(lo, x), hi = std::max(hi, x);
        INFO(rel);
        CHECK(lo == x0);
        CHECK(hi - lo + 1 == vt::assets::textWidth(f, "Video Toaster"));
    }
    CHECK(vt::assets::textWidth(font(kCg14), "") == 0);
}

TEST_CASE("JAM1 keeps colour-0 pixels, JAM2 paints them; clipping is safe") {
    const auto& f = font(kCg14);
    IndexedImage a{40, 20, std::vector<uint8_t>(800, 9)}, b = a;
    vt::assets::drawText(a, f, 2, 14, "A", false, 5);
    vt::assets::drawText(b, f, 2, 14, "A", true, 5);
    int kept = 0, painted = 0;
    const int top = 14 - f.baseline;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const size_t i = size_t(y) * size_t(a.w) + size_t(x);
            if (y < top || y >= top + f.ySize || x < 2 || x >= 2 + 8) {  // outside the 8 x ySize cell: untouched
                CHECK(a.px[i] == 9);
                CHECK(b.px[i] == 9);
            } else if (a.px[i] == 9) {                                 // transparent in JAM1, background in JAM2
                ++kept;
                CHECK(b.px[i] == 5);
            } else {
                CHECK(a.px[i] == b.px[i]);                             // glyph pixels are the same
            }
            painted += b.px[i] == 5;
        }
    CHECK(kept > 0);
    CHECK(painted > 0);
    IndexedImage c{4, 4, std::vector<uint8_t>(16, 0)};      // mostly off-image: must not write outside
    vt::assets::drawText(c, f, -20, 2, "Hello", true, 1);
    vt::assets::drawText(c, f, 3, 100, "Hello", true, 1);
    vt::assets::drawText(c, f, 2, 2, "Hello", true, 1);
}

TEST_CASE("characters outside lo..hi use the not-found glyph") {
    const auto& f = font(kCg14);
    CHECK(render(f, "\x01").px == render(f, "\xff").px);
    CHECK(render(f, "\x01").w == f.charSpace.back());
    CHECK(vt::assets::textWidth(f, "\xff") == f.charSpace.back());
    CHECK(render(f, "A").px != render(f, "\x01").px);
}

TEST_CASE("a size file with a bad DiskFontHeader throws") {
    const auto good = hunks(kCg14);
    const uint32_t base = good.hunks[0].base;
    REQUIRE_NOTHROW(vt::assets::loadColorFont(good));
    {   // dfh_FileID is not 0x0F80
        auto bad = good;
        bad.flat[base + 4 + 14] = 0x0f;
        bad.flat[base + 4 + 15] = 0x00;
        CHECK_THROWS_AS(vt::assets::loadColorFont(bad), std::runtime_error);
    }
    {   // not a colour font (FSF_COLORFONT cleared)
        auto bad = good;
        bad.flat[base + 4 + 54 + 22] &= uint8_t(~0x40);
        CHECK_THROWS_AS(vt::assets::loadColorFont(bad), std::runtime_error);
    }
    {   // tf_CharLoc points outside the image
        auto bad = good;
        for (int i = 0; i < 4; ++i) bad.flat[base + 4 + 54 + 40 + uint32_t(i)] = 0x7f;
        CHECK_THROWS_AS(vt::assets::loadColorFont(bad), std::runtime_error);
    }
    {   // no hunks at all
        vt::assets::HunkImage empty;
        CHECK_THROWS_AS(vt::assets::loadColorFont(empty), std::runtime_error);
    }
    {   // a hunk file that is not a font: switcher.main has no DiskFontHeader at its start
        CHECK_THROWS_AS(vt::assets::loadColorFont(hunks("4.2/Programs/switcher.main")), std::runtime_error);
    }
}
