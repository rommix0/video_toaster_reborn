// DVE moves: control decoding against toaster_dve.decode, bit-exact source reads, fills, roles and
// the DVE Effect.
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/anim.h"
#include "vt/crc32.h"
#include "vt/crouton.h"
#include "vt/dve.h"
#include "vt/effect.h"
#include "vt/raster.h"
#include "vt/sound.h"
#include "vt/timing.h"

namespace {

constexpr std::array<char, 4> kAnimType{'A', 'N', 'I', 'M'};
const vt::Rgba8 kBlack{0, 0, 0, 255};

const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }

struct DveFile {
    vt::Crouton crouton;
    vt::Anim anim;
};

DveFile openDve(const std::string& source) {
    auto c = vt::loadCrouton(golden::sourcePath(source));
    REQUIRE(c);
    for (const auto& f : c->forms)
        if (f.type == kAnimType) {
            vt::Anim a = vt::readAnim(c->bytes, f);
            return {std::move(*c), std::move(a)};
        }
    FAIL("no ANIM form in " << source);
    return {};
}

uint32_t crcOf(const uint8_t* p, size_t n) { return vt::crc32(std::span<const uint8_t>(p, n)); }

// Source pixel (x, y) encoded in its colour: r = x & 255, g = x >> 8 | (y & 0x3f) << 2, b = y >> 6.
// (The brief's g = x >> 8 | (y & 0x7f) << 1 cannot hold x >> 8 = 2 for x >= 512: x needs 10 bits.)
vt::Frame coordinateFrame() {
    vt::Frame f;
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x)
            f.at(x, y) = {uint8_t(x & 255), uint8_t((x >> 8) | ((y & 0x3f) << 2)), uint8_t(y >> 6), 255};
    return f;
}
int decodedX(vt::Rgba8 c) { return c.r | ((c.g & 3) << 8); }
int decodedY(vt::Rgba8 c) { return (c.g >> 2) | (c.b << 6); }

vt::Frame solid(vt::Rgba8 c) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), c);
    return f;
}

vt::Frame pattern(unsigned seed) {
    vt::Frame f;
    for (size_t i = 0; i < f.px.size(); ++i) {
        const unsigned v = unsigned(i) * 2654435761u + seed * 40503u;
        f.px[i] = {uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24), 255};
    }
    return f;
}

// A blank DVE control frame (3 planes, 241 rows of 96 bytes, signature on plane 2).
vt::Planes blankPlanes(int count) {
    vt::Planes p;
    p.count = count;
    p.h = vt::kDveLines;
    p.rowbytes = 96;
    p.bytes.assign(size_t(count) * size_t(p.h) * 96u, 0);
    const uint8_t sig[4] = {0x15, 0x2D, 0x06, 0x83};
    std::copy(sig, sig + 4, p.plane(2));
    return p;
}

// The frame's palette by the anim effects' rule: the most recent non-empty cmaps[j], 1 <= j <= k,
// else bm.cmap.
const std::vector<vt::Rgb>& paletteFor(const vt::Anim& a, int k) {
    for (int j = k; j >= 1; --j)
        if (size_t(j) < a.cmaps.size() && a.cmaps[size_t(j)] && !a.cmaps[size_t(j)]->empty())
            return *a.cmaps[size_t(j)];
    return a.bm.cmap;
}

int tbarForFrame(int f, int n) {
    for (int t = 0; t < 512; ++t)
        if (vt::animFrameForTBar(t, n) == f) return t;
    FAIL("no tbar for frame " << f << " of " << n);
    return 0;
}

}  // namespace

TEST_CASE("DVE control decoding matches toaster_dve.decode (unfilled) for every frame", "[dve][golden]") {
    int effects = 0, frames = 0;
    for (const auto& r : golden::readLines("dve.jsonl")) {
        const std::string& src = str(r, "source");
        INFO(src);
        const auto f = openDve(src);
        const auto& want = std::get<std::vector<std::map<std::string, int64_t>>>(r.at("frames").v);
        REQUIRE(f.anim.planes.size() == want.size());
        REQUIRE(vt::isDve(f.anim.planes[0]));
        for (size_t k = 0; k < want.size(); ++k) {
            INFO("frame " << k);
            const vt::DveControl c = vt::decodeDve(f.anim.planes[k]);
            REQUIRE(crcOf(c.key.data(), c.key.size()) == uint32_t(want[k].at("key_crc")));
            REQUIRE(crcOf(c.line.data(), c.line.size()) == uint32_t(want[k].at("line_crc")));
            const int64_t art = want[k].at("art_crc");
            if (art == 0) {
                REQUIRE(c.art.empty());
            } else {
                REQUIRE(c.art.size() == size_t(vt::kDveLines) * vt::kDveWidth);
                REQUIRE(crcOf(c.art.data(), c.art.size()) == uint32_t(art));
            }
            ++frames;
        }
        ++effects;
    }
    REQUIRE(effects == 64);
    REQUIRE(frames > 1000);
}

TEST_CASE("isDve and decodeDve on hand-made planes", "[dve]") {
    vt::Planes p = blankPlanes(5);
    REQUIRE(vt::isDve(p));
    // cell nibbles: plane 1 high, plane 0 low; even cells take the byte's high nibble
    p.plane(1)[5 * 96 + 0] = 0xAB;
    p.plane(0)[5 * 96 + 0] = 0xCD;
    // line table: header words 0-1, then line k = (word 3 + 2k) >> 12 << 4 | (word 2 + 2k) >> 12,
    // read from the last plane with the signature (the sprite plane, here plane 4)
    auto putWord = [&](int pl, int w, uint16_t v) {
        p.plane(pl)[2 * w] = uint8_t(v >> 8);
        p.plane(pl)[2 * w + 1] = uint8_t(v);
    };
    putWord(4, 2, 0x5123);  // line 0 low nibble 5
    putWord(4, 3, 0x1fff);  // line 0 high nibble 1 -> 0x15 = 21
    putWord(4, 4, 0xF000);
    putWord(4, 5, 0xF000);  // line 1 = 0xFF -> clipped to 240
    putWord(2, 2, 0x7000);  // plane 2 repeats the header but is a picture plane: its words are ignored
    putWord(2, 3, 0x3000);
    // plane 3: art bit 0; plane 4 carries the signature, so it is control and not art
    p.plane(3)[7 * 96 + 1] = 0x80;  // pixel 8 of line 7
    const uint8_t sig[4] = {0x15, 0x2D, 0x06, 0x83};
    std::copy(sig, sig + 4, p.plane(4));
    p.plane(4)[7 * 96 + 2] = 0xFF;  // ignored
    const vt::DveControl c = vt::decodeDve(p);
    REQUIRE(c.key[5 * 192 + 0] == 0xAC);
    REQUIRE(c.key[5 * 192 + 1] == 0xBD);
    REQUIRE(c.line[0] == 21);
    REQUIRE(c.line[1] == 240);
    REQUIRE(c.art.size() == size_t(241) * 768);
    REQUIRE(c.art[7 * 768 + 8] == 1);
    REQUIRE(c.art[7 * 768 + 16] == 0);
    REQUIRE(std::count(c.art.begin(), c.art.end(), uint8_t(0)) == ptrdiff_t(c.art.size()) - 1);
    // no art planes -> empty art
    REQUIRE(vt::decodeDve(blankPlanes(3)).art.empty());

    // more than 8 art planes is refused before any undefined shift, even with 40 of them
    for (int count : {12, 43}) {
        vt::Planes deep = blankPlanes(count);
        for (int pl = 3; pl < count; ++pl) deep.plane(pl)[0] = 0x80;
        REQUIRE_THROWS_AS(vt::decodeDve(deep), std::runtime_error);
    }
    REQUIRE(vt::decodeDve(blankPlanes(11)).art.size() == size_t(241) * 768);  // exactly 8 is fine

    vt::Planes bad = blankPlanes(3);
    bad.plane(2)[3] = 0x03;  // 15 2D 06 03 is the other planes' header, not DVE plane 2
    REQUIRE_FALSE(vt::isDve(bad));
    REQUIRE_THROWS(vt::decodeDve(bad));
    REQUIRE_FALSE(vt::isDve(blankPlanes(2)));
    vt::Planes narrow = blankPlanes(3);
    narrow.rowbytes = 48;
    REQUIRE_FALSE(vt::isDve(narrow));
}

TEST_CASE("The \"!\" DVEs take the line table from the sprite plane, not plane 2", "[dve]") {
    // In these effects plane 2 holds picture bits below the control rows; read as a table it gives
    // lines up to 255 (Photo Fly Out !: line 0 for every control line from 23 on).
    const char* effects[] = {
        "4.2/Effects/4000FX/Digital/Hand Pull !",          "4.2/Effects/4000FX/Digital/Flip Chrome Edge !",
        "4.2/Effects/4000FX/Digital/Warp To Globe !",      "4.2/Effects/4000FX/Digital/Fly Over Crumpled Edge !",
        "4.2/Effects/4000FX/Trajectory/Flip Out Bottom2 !", "4.2/Effects/4000FX/Space/Infinite Video Space !",
        "4.2/Effects/4000FX/Trajectory/Photo Fly Out !",
    };
    for (const char* src : effects) {
        INFO(src);
        const auto f = openDve(src);
        for (const auto& pl : f.anim.planes) {
            const vt::DveControl c = vt::decodeDve(pl);
            REQUIRE(*std::max_element(c.line.begin(), c.line.end()) <= 239);
        }
    }
    const auto photo = openDve("4.2/Effects/4000FX/Trajectory/Photo Fly Out !");
    const vt::DveControl c = vt::decodeDve(photo.anim.planes[0]);
    for (int k = 3; k < 230; ++k) REQUIRE(c.line[size_t(k)] == k);  // frame 0: the photo is the whole picture
}

TEST_CASE("DVE reads exactly one source pixel", "[dve]") {
    const auto f = openDve("4.2/Effects/2000FX/Digital/Cube Out Top");
    REQUIRE(f.anim.planes.size() > 20);
    const vt::DveControl c = vt::decodeDve(f.anim.planes[20]);
    const vt::Frame moving = coordinateFrame();
    const vt::Frame black = solid(kBlack);
    for (int cellPixel : {-1, 0, 3}) {
        INFO("dveCellPixel " << cellPixel);
        vt::HardwareGuesses g;
        g.dveCellPixel = cellPixel;
        vt::Frame out;
        REQUIRE(vt::renderDve(c, moving, black, f.anim.bm.cmap, out, g) == 0);
        int mapped = 0, background = 0;
        for (int y = 0; y < vt::kRasterH; ++y)
            for (int x = 0; x < vt::kRasterW; ++x) {
                const int k = y / 2, p = y & 1;
                const int v = c.key[size_t(k) * 192 + size_t(x / 4)];
                const vt::Rgba8 o = out.at(x, y);
                if (v >= 64) {
                    const int sx = std::clamp((255 - v - g.dveSourceCellOffset) * 4 + (cellPixel < 0 ? x % 4 : cellPixel), 0, 767);
                    const int sy = c.line[size_t(k)] * 2 + p;
                    if (decodedX(o) != sx || decodedY(o) != sy || o.a != 255)
                        FAIL("pixel (" << x << "," << y << ") v " << v << " read (" << decodedX(o) << ","
                                       << decodedY(o) << ") want (" << sx << "," << sy << ")");
                    ++mapped;
                } else if (v == 0) {
                    if (!(o == kBlack)) FAIL("background pixel (" << x << "," << y << ") not black");
                    ++background;
                }
            }
        // the encoding is one-to-one: every source pixel decodes back to itself
        for (int y = 0; y < vt::kRasterH; y += 7)
            for (int x = 0; x < vt::kRasterW; ++x)
                if (decodedX(moving.at(x, y)) != x || decodedY(moving.at(x, y)) != y) FAIL("encoding " << x << "," << y);
        REQUIRE(mapped > 10000);
        REQUIRE(background > 10000);
    }
}

TEST_CASE("dve source cell offset", "[dve]") {
    vt::DveControl c;
    for (int k = 0; k < vt::kDveLines; ++k) {
        c.line[size_t(k)] = uint8_t(k);
        for (int cell = 0; cell < vt::kDveCells; ++cell) c.key[size_t(k) * 192 + size_t(cell)] = uint8_t(255 - (cell + 3));
    }
    const vt::Frame moving = coordinateFrame();
    const vt::Frame black = solid(kBlack);
    auto render = [&](int offset) {
        vt::HardwareGuesses g;
        g.dveSourceCellOffset = offset;
        vt::Frame out;
        vt::renderDve(c, moving, black, {}, out, g);
        return out;
    };
    const vt::Frame same = render(3), shifted = render(0);
    const vt::Rect r = vt::kVisibleArea;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            REQUIRE(same.at(x, y) == moving.at(x, y));
            REQUIRE(shifted.at(x, y) == moving.at(x + 12, y));
        }
}

TEST_CASE("dve colour line draws the border fill", "[dve]") {
    vt::DveControl c;
    c.borderFill = 21;
    c.key[5 * 192 + 40] = 200;  // one moving cell on control line 5
    c.line.fill(0xfc);
    const vt::Frame moving = pattern(3);
    const vt::Frame bg = solid({10, 20, 30, 255});
    vt::Frame out;
    vt::renderDve(c, moving, bg, {}, out, vt::HardwareGuesses{});
    for (int y : {10, 11})
        for (int x = 160; x < 164; ++x) REQUIRE(out.at(x, y) == vt::Rgba8{191, 191, 0, 255});
    REQUIRE(out.at(164, 10) == bg.at(164, 10));
    REQUIRE(out.at(159, 10) == bg.at(159, 10));
}

namespace {
// A tag 0x1000001B blob: rows 1, step 1, first index `first`, the 8 masks, then the entries.
std::vector<uint8_t> tag1b(std::array<uint32_t, 2> masks, int first, std::vector<uint32_t> entries) {
    std::vector<uint8_t> t;
    auto put16 = [&](unsigned v) { t.insert(t.end(), {uint8_t(v >> 8), uint8_t(v)}); };
    auto put32 = [&](uint32_t v) { put16(v >> 16); put16(v & 0xffff); };
    put16(1); put16(1); put16(unsigned(first)); put16(unsigned(entries.size()));
    for (int i = 0; i < 8; ++i) put32(i < 2 ? masks[size_t(i)] : 0);
    for (uint32_t e : entries) put32(e);
    return t;
}
}  // namespace

TEST_CASE("dveRoutes reads source and level through the mask", "[dve]") {
    const std::vector<uint32_t> entries = {0x00009003, 0x00001002, 0x10003001, 0x00000003};
    std::vector<vt::Rgb> cmap(2);
    cmap[0].b = 0x02;
    cmap[1].b = 0x01;
    auto r = vt::dveRoutes(tag1b({0x80000714, 0x80000717}, 0, entries), cmap);
    REQUIRE(r.size() == 4);
    REQUIRE(r[0].source == 9);
    REQUIRE(r[0].level == 2);  // mask 0 has no L bits: the CMAP's
    REQUIRE(r[1].source == 1);
    REQUIRE(r[1].level == 1);
    REQUIRE(r[2].source == 3);
    REQUIRE(r[2].level == 1);  // mask 1 has them: the entry's
    REQUIRE(r[3].source == 0);

    // a mask without the source bits routes nothing
    r = vt::dveRoutes(tag1b({0x00000113, 0x80000717}, 0, entries), cmap);
    REQUIRE(r[0].source == 0);

    // sized first + count; the indexes before first keep the CMAP's level (cmap covers 0 and 1)
    r = vt::dveRoutes(tag1b({0x80000714, 0x80000717}, 3, {0x00001000}), cmap);
    REQUIRE(r.size() == 4);
    REQUIRE(r[0].level == 2);
    REQUIRE(r[1].level == 1);
    REQUIRE(r[2].level == 0);
    REQUIRE(r[0].source == 0);

    // a table shorter than its count
    auto shortTag = tag1b({0x80000714, 0x80000717}, 0, entries);
    shortTag.resize(shortTag.size() - 1);
    REQUIRE(vt::dveRoutes(shortTag, cmap).empty());
    REQUIRE(vt::dveRoutes({}, cmap).empty());
}

TEST_CASE("renderDve mixes a routed pixel by its level", "[dve]") {
    REQUIRE(vt::HardwareGuesses{}.dveLevelMix == std::array<uint8_t, 4>{0, 85, 170, 255});
    vt::DveControl c;
    for (int k = 0; k < vt::kDveLines; ++k) {
        c.line[size_t(k)] = uint8_t(k);
        for (int cell = 0; cell < vt::kDveCells; ++cell) c.key[size_t(k) * 192 + size_t(cell)] = uint8_t(255 - (cell + 3));
    }
    c.index.assign(size_t(vt::kDveLines) * vt::kDveWidth, 0);
    c.index[100 * 768 + 300] = 1;
    c.index[110 * 768 + 300] = 2;
    c.index[120 * 768 + 300] = 3;
    const std::vector<vt::DveRoute> routes = {{1, 0}, {9, 2}, {3, 0}, {3, 2}};
    const vt::Frame moving = solid({200, 100, 0, 255}), a = solid({10, 20, 30, 255});
    const vt::DveMix mix{&routes, &a, {255, 255, 255, 255}, nullptr};
    vt::Frame plain, mixed;
    vt::renderDve(c, moving, a, {}, plain, vt::HardwareGuesses{});
    vt::renderDve(c, moving, a, {}, mixed, vt::HardwareGuesses{}, &mix);
    REQUIRE(mixed.at(100, 50) == a.at(100, 50));
    // S = 3 is the DVE picture itself, at any level: the moved pixel, not the bus frame at (x, y)
    REQUIRE(mixed.at(300, 220) == plain.at(300, 220));
    REQUIRE(mixed.at(300, 240) == plain.at(300, 240));
    REQUIRE(mixed.at(300, 220) == moving.at(0, 0));
    const vt::Rgba8 d = plain.at(300, 200), m = mixed.at(300, 200);
    auto want = [](int dc) { return uint8_t((255 * (255 - 170) + dc * 170 + 127) / 255); };
    REQUIRE(m == vt::Rgba8{want(d.r), want(d.g), want(d.b), 255});
}

TEST_CASE("decodeDve fills the display index only for \"!\" DVEs", "[dve]") {
    REQUIRE(vt::decodeDve(blankPlanes(3)).index.empty());
    vt::Planes p = blankPlanes(6);
    const uint8_t sig[4] = {0x15, 0x2D, 0x06, 0x83};
    std::copy(sig, sig + 4, p.plane(5));
    p.plane(2)[50 * 96] = 0x80;       // pixel (0, 50), plane 2
    p.plane(4)[50 * 96 + 1] = 0x80;   // pixel (8, 50), plane 4
    p.plane(5)[50 * 96 + 2] = 0x80;   // pixel (16, 50), the sprite: not part of the index
    const vt::DveControl c = vt::decodeDve(p);
    REQUIRE(c.index.size() == size_t(241) * 768);
    REQUIRE(c.index[50 * 768 + 0] == 4);
    REQUIRE(c.index[50 * 768 + 8] == 16);
    REQUIRE(c.index[50 * 768 + 16] == 0);
}

TEST_CASE("DVE art: colour from the display index, mixed toward the moving picture by its level", "[dve]") {
    vt::DveControl c;
    c.key[3 * 192 + 1] = 0;    // pixels 4..7: background
    c.key[3 * 192 + 2] = 255;  // pixels 8..11: moving
    c.art.assign(size_t(241) * 768, 0);
    c.index.assign(size_t(241) * 768, 0);
    auto put = [&](int x, uint8_t idx) {
        c.index[3 * 768 + size_t(x)] = idx;
        c.art[3 * 768 + size_t(x)] = uint8_t(idx >> 3);
    };
    put(4, 61);  // plane 2 set: cmap[60], not cmap[56]; over the background the level does not matter
    put(8, 61);  // cmap[60], L 3: the moving picture
    put(9, 58);  // cmap[56], L 2
    put(10, 49);  // cmap[48], L 0: the art
    put(11, 5);   // art group 0: no art
    std::vector<vt::Rgb> cmap(64);
    for (int i = 0; i < 64; ++i) cmap[size_t(i)] = {uint8_t(4 * i), uint8_t(100 + i), uint8_t(i >> 2)};  // L = b & 3
    const vt::Frame moving = solid({200, 100, 0, 255}), bg = solid({10, 20, 30, 255});
    auto art = [&](int i) { return vt::Rgba8{cmap[size_t(i)].r, cmap[size_t(i)].g, cmap[size_t(i)].b, 255}; };
    auto mixed = [](vt::Rgba8 a, vt::Rgba8 d, int w) {
        auto m = [w](int ac, int dc) { return uint8_t((ac * (255 - w) + dc * w + 127) / 255); };
        return vt::Rgba8{m(a.r, d.r), m(a.g, d.g), m(a.b, d.b), 255};
    };
    const vt::Rgba8 mv{200, 100, 0, 255};
    vt::Frame out;
    vt::renderDve(c, moving, bg, cmap, out, vt::HardwareGuesses{});
    for (int y : {6, 7}) {
        REQUIRE(out.at(4, y) == art(60));
        REQUIRE(out.at(8, y) == mv);
        REQUIRE(out.at(9, y) == mixed(art(56), mv, 170));
        REQUIRE(out.at(10, y) == art(48));
        REQUIRE(out.at(11, y) == mv);
    }
    // a route table's level wins over the palette's for the indexes it covers
    std::vector<vt::DveRoute> routes(57);
    routes[56].level = 1;
    const vt::DveMix mix{&routes, &bg, {0, 0, 0, 255}, nullptr};
    vt::renderDve(c, moving, bg, cmap, out, vt::HardwareGuesses{}, &mix);
    REQUIRE(out.at(9, 6) == mixed(art(56), mv, 85));
    REQUIRE(out.at(8, 6) == mv);  // index 60 is past the table: the palette's L 3
}

TEST_CASE("DVE shadows, colour bars, unknown codes and art", "[dve]") {
    vt::DveControl c;
    // control line 3 (raster lines 6-7): cell v = its index for 0..63, art on cells 70.. below
    for (int cell = 0; cell < 64; ++cell) c.key[3 * 192 + size_t(cell)] = uint8_t(cell);
    c.key[3 * 192 + 64] = 255;  // moving, source cell 0
    c.art.assign(size_t(241) * 768, 0);
    c.art[3 * 768 + 4 * 64 + 1] = 2;  // over the moving cell: cmap[16]
    c.art[3 * 768 + 4 * 17] = 1;      // over a colour bar: cmap[8]
    c.art[3 * 768 + 4 * 1] = 9;       // cmap[72] is past the palette: not drawn
    std::vector<vt::Rgb> cmap(64);
    for (int i = 0; i < 64; ++i) cmap[size_t(i)] = {uint8_t(i), uint8_t(100 + i), uint8_t(200 - i)};
    const vt::Rgba8 bgc{200, 150, 100, 255};
    const vt::Frame bg = solid(bgc);
    const vt::Frame moving = pattern(7);

    auto shadowAlpha = [](int v, double opacity) { return int(std::min(v, 7) * (255.0 * opacity) / 7.0); };
    auto darken = [](int ch, int a) { return uint8_t((ch * (255 - a) + 127) / 255); };
    const std::map<int, vt::Rgba8> bars = {
        {16, {0, 0, 191, 255}},   {17, {191, 0, 0, 255}},   {18, {191, 0, 191, 255}}, {19, {0, 191, 0, 255}},
        {20, {0, 191, 191, 255}}, {21, {191, 191, 0, 255}}, {22, {191, 191, 191, 255}}, {23, {0, 0, 0, 255}},
    };
    // the converter's alphas for a full shadow of 0.5: (k * 127.5 / 7) as uint8
    const int pyAlpha[8] = {0, 18, 36, 54, 72, 91, 109, 127};
    for (int v = 1; v <= 15; ++v) REQUIRE(shadowAlpha(v, 0.5) == pyAlpha[std::min(v, 7)]);

    for (float opacity : {0.5f, 1.0f, 0.0f}) {
        INFO("opacity " << opacity);
        vt::HardwareGuesses g;
        g.dveShadowOpacity = opacity;
        vt::Frame out;
        const int unknown = vt::renderDve(c, moving, bg, cmap, out, g);
        REQUIRE(unknown == (63 - 24 + 1) * 4 * 2);  // 40 cells x 4 pixels x 2 raster lines
        for (int y : {6, 7})
            for (int x = 0; x < 4 * 65; ++x) {
                INFO("x " << x << " y " << y);
                const int v = x / 4;
                const vt::Rgba8 o = out.at(x, y);
                vt::Rgba8 want;
                if (x == 4 * 17) {
                    want = {cmap[8].r, cmap[8].g, cmap[8].b, 255};
                } else if (x == 4 * 64 + 1) {
                    want = {cmap[16].r, cmap[16].g, cmap[16].b, 255};
                } else if (v == 64) {
                    want = moving.at(0, (c.line[3] * 2) + (y & 1));  // source cell 0 reads x clamp(4 * (0 - 3) + inner) = 0
                } else if (v == 0) {
                    want = bgc;
                } else if (v <= 15) {
                    const int a = shadowAlpha(v, opacity);
                    want = {darken(bgc.r, a), darken(bgc.g, a), darken(bgc.b, a), bgc.a};
                } else if (v <= 23) {
                    want = bars.at(v);
                } else {
                    want = {128, 128, 128, 255};
                }
                REQUIRE(o == want);
            }
        // a full-strength shadow at opacity 1 is black; at 0 it is the background
        if (opacity == 1.0f) REQUIRE(out.at(4 * 7, 6) == vt::Rgba8{0, 0, 0, 255});
        if (opacity == 0.0f) REQUIRE(out.at(4 * 7, 6) == bgc);
        // the rest of the picture is background (key 0, art 0)
        REQUIRE(out.at(700, 6) == bgc);
        REQUIRE(out.at(5, 300) == bgc);
    }
}

TEST_CASE("DVE roles match the converter's write_dve", "[dve][golden]") {
    struct Want {
        const char* source;
        vt::DveRole role;
        int switchFrame;
    };
    const Want wants[] = {
        {"4.2/Effects/2000FX/Digital/Cube Out Top", vt::DveRole::MovingA, -1},
        {"4.2/Effects/2000FX/Trajectory/Fly In Left", vt::DveRole::MovingB, -1},
        {"4.2/Effects/2000FX/Digital/Flying Blocks", vt::DveRole::MovingB, -1},
        {"4.2/Effects/2000FX/OverLays/Glitch Bottom Skew", vt::DveRole::DistortA, -1},
        {"4.2/Effects/2000FX/OverLays/Glitch Roll Screen", vt::DveRole::DistortA, -1},
        {"4.2/Effects/4000FX/Digital/Flip Chrome Edge !", vt::DveRole::Flip, 39},
        // raw coverage would give 31 (stale margin cells); the converter's filled rule gives 30
        {"4.2/Effects/4000FX/Space/Infinite Video Space !", vt::DveRole::Flip, 30},
        {"4.2/Effects/4000FX/Digital/Warp To Globe !", vt::DveRole::MovingA, -1},
        {"3.1/ToasterAll/EFFECTS/FlyInTop", vt::DveRole::MovingB, -1},
    };
    for (const auto& w : wants) {
        INFO(w.source);
        const auto f = openDve(w.source);
        const vt::DveInfo i = vt::dveInfo(f.anim);
        REQUIRE(i.role == w.role);
        REQUIRE(i.switchFrame == w.switchFrame);
    }
    // the whole corpus: 35 MovingA, 25 MovingB, 2 Flip, 2 DistortA (converter run, Task 9 report)
    std::map<vt::DveRole, int> n;
    for (const auto& r : golden::readLines("dve.jsonl")) ++n[vt::dveInfo(openDve(str(r, "source")).anim).role];
    REQUIRE(n[vt::DveRole::MovingA] == 35);
    REQUIRE(n[vt::DveRole::MovingB] == 25);
    REQUIRE(n[vt::DveRole::Flip] == 2);
    REQUIRE(n[vt::DveRole::DistortA] == 2);
}

TEST_CASE("every DVE loads as an effect and renders its role", "[dve][effect]") {
    const vt::Frame a = pattern(1), b = pattern(2), black = solid(kBlack);
    int loaded = 0, sounds = 0;
    for (const auto& r : golden::readLines("dve.jsonl")) {
        const std::string& src = str(r, "source");
        INFO(src);
        std::string why;
        auto e = vt::loadEffect(golden::sourcePath(src), &why);
        INFO(why);
        REQUIRE(e);
        ++loaded;
        const auto f = openDve(src);
        const int n = int(f.anim.planes.size());
        REQUIRE(e->usesDvMemory());
        REQUIRE(e->frameCount() == n);
        REQUIRE(e->timing().durationFrames == vt::animTiming(f.crouton, n).durationFrames);
        REQUIRE(e->timing().fieldsPerFrame == vt::animTiming(f.crouton, n).fieldsPerFrame);
        std::optional<vt::Sound> s;
        try {
            s = vt::readSound(f.crouton.bytes);
        } catch (const std::exception&) {
        }
        REQUIRE((e->sound() != nullptr) == s.has_value());
        if (e->sound()) {
            ++sounds;
            REQUIRE(e->sound()->pcm == s->pcm);
        }
        const vt::DveInfo info = vt::dveInfo(f.anim);
        std::vector<int> frames = {0, n / 2, n - 1};
        if (info.switchFrame >= 0) frames.insert(frames.end(), {info.switchFrame - 1, info.switchFrame});
        for (int k : frames) {
            INFO("frame " << k);
            const vt::Frame* moving = &a;
            const vt::Frame* background = &b;
            switch (info.role) {
                case vt::DveRole::MovingA: break;
                case vt::DveRole::MovingB: moving = &b; background = &a; break;
                case vt::DveRole::DistortA: background = &a; break;
                case vt::DveRole::Flip:
                    moving = k < info.switchFrame ? &a : &b;
                    background = &black;
                    break;
            }
            vt::Frame want, got;
            vt::RenderCtx ctx;
            // the "!" DVEs carry tag 0x1000001B routes: A, B and the generator mix in by level.
            // Ruling 10: routing descriptors 3 and 32 move A, so S = 1 is B and S = 3 is A; any
            // other descriptor S = 1 is A and S = 3 is B.
            const auto tag = f.crouton.blobs.find(0x1000001B);
            const auto routes = vt::dveRoutes(tag != f.crouton.blobs.end() ? tag->second : std::vector<uint8_t>{},
                                              paletteFor(f.anim, k));
            const uint32_t desc = f.crouton.tag(0x80000007).value_or(0);
            const bool aMoves = desc == 3 || desc == 32;
            const vt::DveMix mix{&routes, aMoves ? &b : &a, ctx.background, nullptr};
            vt::renderDve(vt::decodeDve(f.anim.planes[size_t(k)]), *moving, *background, paletteFor(f.anim, k), want,
                          ctx.guesses, routes.empty() ? nullptr : &mix);
            e->render(tbarForFrame(k, n), a, b, got, ctx);
            REQUIRE(got.px == want.px);
        }
    }
    REQUIRE(loaded == 64);
    INFO("DVEs with sound: " << sounds);
}

TEST_CASE("the visible area is the DVE data's clean area", "[dve]") {
    // spec 5.6: cells 12..171 and control lines 13..228 (see tools/re/FINDINGS.md, DVE)
    REQUIRE(vt::kVisibleArea.x == 12 * 4);
    REQUIRE(vt::kVisibleArea.w == 160 * 4);
    REQUIRE(vt::kVisibleArea.y == 13 * 2);
    REQUIRE(vt::kVisibleArea.h == (229 - 13) * 2);
}
