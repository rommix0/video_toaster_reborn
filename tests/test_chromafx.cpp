#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/chromafx.h"
#include "vt/crc32.h"
#include "vt/crouton.h"
#include "vt/dv_colour.h"
#include "vt/effect.h"
#include "vt/raster.h"

namespace {

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

bool same(vt::Rgba8 a, vt::Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

uint32_t tableCrc(const std::array<vt::Rgb, vt::kChromaEntries>& t) {
    std::vector<uint8_t> b;
    for (auto c : t) b.insert(b.end(), {c.r, c.g, c.b});
    return vt::crc32(b);
}

uint32_t rowCrc(const std::array<vt::Rgba8, vt::kChromaEntries>& row) {
    std::vector<uint8_t> b;
    for (auto c : row) b.insert(b.end(), {c.r, c.g, c.b, c.a});
    return vt::crc32(b);
}

std::filesystem::path chromaDir() { return dataRoot() / "4.2" / "Effects" / "ChromaFx"; }

vt::LutRecord croutonLut(const std::string& source) {
    auto c = vt::loadCrouton(golden::sourcePath(source));
    REQUIRE(c);
    auto r = vt::lutFromCrouton(*c);
    REQUIRE(r);
    return *r;
}

// A record whose two tables are solid colours.
vt::LutRecord twoColours(vt::Rgb a, vt::Rgb b, uint8_t interp = 0) {
    vt::LutRecord r;
    r.interp = interp;
    r.tables[0].fill(a);
    r.tables[1].fill(b);
    r.tables[2].fill(a);
    return r;
}

vt::Frame solid(vt::Rgba8 c) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), c);
    return f;
}

}  // namespace

TEST_CASE("RGBYIQ and YIQQUAD reproduce the 68000 routines", "[chromafx][colour]") {
    // Values from running ChromaFX 0x4938 / 0x49c2 in a 68000 emulator (tools/chromafx_rows_ref.py).
    CHECK(vt::rgbToDvQuad({255, 0, 0}) == vt::DvQuad{32, 82, 186, 136});
    CHECK(vt::rgbToDvQuad({0, 255, 255}) == vt::DvQuad{221, 189, 83, 133});
    CHECK(vt::rgbToDvQuad({255, 255, 255}) == vt::DvQuad{200, 200, 200, 200});
    CHECK(vt::rgbToDvQuad({0, 0, 0}) == vt::DvQuad{71, 71, 71, 71});
    const vt::DvYiq red = vt::rgbToDvYiq({191, 1, 0});  // the matte table's Red comes back exactly
    CHECK(red.y == 100);
    CHECK(red.i == 58);
    CHECK(red.q == 20);
    // negative I and Q round away from zero after a floor: the matte table's Cyan (138, -58, -20)
    // comes back one lower
    const vt::DvYiq cyan = vt::rgbToDvYiq({0, 189, 192});
    CHECK(cyan.y == 138);
    CHECK(cyan.i == -59);
    CHECK(cyan.q == -21);
}

TEST_CASE("DV YIQ -> RGB inverts the matrix with DV black 71 / white 200", "[chromafx][colour]") {
    // The Background colour generator table (switcher.main 0x6d88) is the reference.
    CHECK(same(vt::dvYiqToRgb({71, 0, 0}), {0, 0, 0, 255}));
    CHECK(same(vt::dvYiqToRgb({200, 0, 0}), {255, 255, 255, 255}));
    // (literal values: bkgColour itself is computed with dvYiqToRgb)
    CHECK(same(vt::dvYiqToRgb({100, 58, 20}), {191, 1, 0, 255}));
    CHECK(same(vt::dvYiqToRgb({128, -27, -51}), {0, 192, 0, 255}));
    CHECK(same(vt::dvYiqToRgb({81, -31, 30}), {0, 0, 189, 255}));
    CHECK(same(vt::dvYiqToRgb({157, 31, -30}), {192, 192, 1, 255}));
    CHECK(same(vt::dvYiqToRgb({110, 27, 51}), {191, 0, 190, 255}));
    CHECK(same(vt::dvYiqToRgb({138, -58, -20}), {0, 189, 192, 255}));
    // a quad decodes to its mean and half-differences
    CHECK(same(vt::dvQuadToRgb({200, 200, 200, 200}), {255, 255, 255, 255}));
    CHECK(same(vt::dvQuadToRgb({100 - 58, 100 - 20, 100 + 58, 100 + 20}), {191, 1, 0, 255}));
}

TEST_CASE("parseLut tables match the golden CRCs for croutons and banks", "[chromafx][golden]") {
    const auto recs = golden::readLines("chromafx.jsonl");
    std::map<std::string, std::vector<vt::LutRecord>> banks;
    std::map<std::string, size_t> next;  // golden bank records are in file order
    int croutons = 0, bankRecs = 0;
    for (const auto& r : recs) {
        const std::string& src = str(r, "source");
        INFO(src);
        vt::LutRecord lut;
        const auto bracket = src.find(" [");
        if (bracket == std::string::npos) {
            lut = croutonLut(src);
            ++croutons;
        } else {
            const std::string bank = src.substr(0, bracket);
            if (!banks.count(bank)) banks[bank] = vt::readLutBank(golden::sourcePath(bank));
            const auto& list = banks[bank];
            REQUIRE(next[bank] < list.size());
            lut = list[next[bank]++];
            ++bankRecs;
        }
        REQUIRE(lut.name == str(r, "name"));
        for (size_t k = 0; k < 3; ++k) REQUIRE(tableCrc(lut.tables[k]) == uint32_t(arr(r, "tables_crc")[k]));
    }
    REQUIRE(croutons == 96);
    REQUIRE(bankRecs == 396);
    for (const auto& [name, list] : banks) {
        INFO(name);
        REQUIRE(list.size() == 99);
    }
}

TEST_CASE("parseLut reads the header and rejects other data", "[chromafx]") {
    const auto lut = croutonLut("4.2/Effects/ChromaFx/BW Poster");
    CHECK(lut.name == "B&W Poster");
    CHECK(lut.interp == 6);
    CHECK(lut.flags == 0xa1);
    CHECK(lut.tbar == 364);
    CHECK(vt::chromaInterp(lut) == vt::ChromaInterp::Poster);
    std::vector<uint8_t> bad(904, 0);
    CHECK_THROWS(vt::parseLut(bad));
    std::vector<uint8_t> shortRec{'L', 'U', 'T', 1};
    CHECK_THROWS(vt::parseLut(shortRec));
    vt::LutRecord r;
    r.interp = 9;  // SENDM2/SENDM3: 7 and above act as 0
    CHECK(vt::chromaInterp(r) == vt::ChromaInterp::Spread);
}

TEST_CASE("crouton tags and record headers agree", "[chromafx]") {
    int n = 0;
    std::set<vt::ChromaInterp> styles;
    for (const auto& r : golden::readLines("chromafx_rows.jsonl")) {
        INFO(str(r, "source"));
        auto c = vt::loadCrouton(golden::sourcePath(str(r, "source")));
        REQUIRE(c);
        const auto lut = vt::lutFromCrouton(*c);
        REQUIRE(lut);
        const auto t = vt::chromaSettings(*c, *lut);
        const auto h = vt::chromaSettings(*lut);
        CHECK(t.mode == h.mode);
        CHECK(t.cycle == h.cycle);
        CHECK(t.colour == h.colour);
        CHECK(t.tbar == h.tbar);
        CHECK((t.speed == vt::Speed::Variable ? vt::Speed::Fast : t.speed) == h.speed);
        CHECK(int(t.interp) == num(r, "interp"));
        styles.insert(t.interp);
        ++n;
    }
    REQUIRE(n == 96);
    REQUIRE(styles.size() == 7);  // every style is used by at least one crouton
}

TEST_CASE("buildRows matches the golden rows of every ChromaFX crouton", "[chromafx][golden]") {
    int n = 0;
    for (const auto& r : golden::readLines("chromafx_rows.jsonl")) {
        INFO(str(r, "source"));
        const auto lut = croutonLut(str(r, "source"));
        const auto rows = vt::buildRows(lut, vt::ChromaInterp(num(r, "interp")));
        const auto& crcs = arr(r, "row_crcs");
        REQUIRE(rows.rows.size() == size_t(vt::kChromaRows));
        REQUIRE(crcs.size() == size_t(vt::kChromaRows));
        for (size_t i = 0; i < crcs.size(); ++i) {
            INFO("row " << i);
            REQUIRE(rowCrc(rows.rows[i]) == uint32_t(crcs[i]));
        }
        ++n;
    }
    REQUIRE(n == 96);
}

TEST_CASE("spread runs table 0 -> table 1 over 480 steps; row 479 is the linear ramp", "[chromafx]") {
    const auto rows = vt::buildRows(twoColours({0, 0, 0}, {255, 255, 255}), vt::ChromaInterp::Spread);
    // step 0 has one 8.8 increment: 0x0080 + (255 << 8) / 480 = 0x0100 -> 1
    CHECK(same(rows.rows[0][0], vt::dvYiqToRgb(vt::rgbToDvYiq({1, 1, 1}))));
    CHECK(same(rows.rows[478][50], vt::dvYiqToRgb(vt::rgbToDvYiq({254, 254, 254}))));
    // LUT_LinearRamp: entry k of row 479 is the grey quad 4 + 2 (16 + k)
    for (int k = 0; k < vt::kChromaEntries; ++k) {
        const auto v = uint8_t(36 + 2 * k);
        REQUIRE(same(rows.rows[479][size_t(k)], vt::dvQuadToRgb({v, v, v, v})));
    }
}

TEST_CASE("rotate and nuke move table 0 through the rows; table 1 is unused", "[chromafx]") {
    vt::LutRecord r;
    for (int k = 0; k < vt::kChromaEntries; ++k) r.tables[0][size_t(k)] = {uint8_t(k * 2), uint8_t(255 - k * 2), 0};
    vt::LutRecord r2 = r;
    r2.tables[1].fill({9, 9, 9});
    for (auto in : {vt::ChromaInterp::RotateLeft, vt::ChromaInterp::RotateRight, vt::ChromaInterp::NukeLeft,
                    vt::ChromaInterp::NukeRight}) {
        const auto a = vt::buildRows(r, in), b = vt::buildRows(r2, in);
        REQUIRE(a.rows == b.rows);
    }
    const auto rows = vt::buildRows(r, vt::ChromaInterp::RotateLeft);
    // step n rotates by floor(n * 0x3334 / 65536): step 10 by 2
    const auto t0 = vt::buildRows(r, vt::ChromaInterp::RotateLeft).rows[0];
    for (size_t i = 0; i < 96; ++i) REQUIRE(same(rows.rows[10][i], t0[(i + 2) % 96]));
}

TEST_CASE("Transition: every line of a field reads the same row pair", "[chromafx]") {
    using vt::ChromaMode;
    CHECK(vt::chromaRowStart(0) == 0);
    CHECK(vt::chromaRowStart(39) == 0);
    CHECK(vt::chromaRowStart(40) == 1);
    CHECK(vt::chromaRowStart(511) == 118);
    const auto t = vt::chromaLineTable(ChromaMode::Transition, 5);
    CHECK(t[0] == 0xf0);
    CHECK(t[1] == 13);
    CHECK(t[2] == 12);
    CHECK(t[240] == 12);
    for (int y = 2; y < vt::kRasterH; ++y) {
        REQUIRE(vt::rowForLine(ChromaMode::Transition, vt::chromaRowStart(511), y) == 476 + (y & 3));
        REQUIRE(vt::rowForLine(ChromaMode::Transition, 0, y) == 4 + (y & 3));
        REQUIRE(vt::chromaTableRow(t, y) == 4 * 5 + 4 + (y & 3));
    }
    CHECK(vt::rowForLine(ChromaMode::Transition, 0, 0) == -1);  // control line 0 holds 0xf0
    CHECK(vt::rowForLine(ChromaMode::Transition, 0, 1) == -1);
    CHECK(vt::rowForLine(ChromaMode::Transition, 0, vt::kRasterH) == -1);
    CHECK(vt::rowForLine(ChromaMode::Transition, 0, -1) == -1);
}

TEST_CASE("Filter: consecutive lines read consecutive rows and wrap", "[chromafx]") {
    using vt::ChromaMode;
    const auto t = vt::chromaLineTable(ChromaMode::Filter, 0);
    CHECK(t[0] == 0xf0);
    for (int k = 1; k <= 238; ++k) REQUIRE(t[size_t(k)] == k);
    CHECK(t[239] == 1);
    CHECK(t[240] == 2);
    // one row per raster line from 4v + 2
    for (int y = 2; y < 2 * 239; ++y) REQUIRE(vt::rowForLine(ChromaMode::Filter, 0, y) == y);
    const int v = 50, tbar = (v + 9) * 4;
    REQUIRE(vt::chromaRowStart(tbar) == v);
    const auto f = vt::chromaLineTable(ChromaMode::Filter, v);
    CHECK(f[1] == 101);
    // pairs run up to 238, then restart at 1, 2
    const auto it = std::find(f.begin() + 1, f.end(), uint8_t(238));
    REQUIRE(it != f.end());
    CHECK(*(it + 1) == 1);
    CHECK(*(it + 2) == 2);
    for (int y = 2; y < 2 * 238 - 4 * v; ++y) REQUIRE(vt::rowForLine(ChromaMode::Filter, v, y) == 4 * v + y);
    // row positions wrap at 0x77
    CHECK(vt::rowForLine(ChromaMode::Filter, 119, 10) == vt::rowForLine(ChromaMode::Filter, 0, 10));
    CHECK(vt::rowForLine(ChromaMode::Filter, -1, 10) == vt::rowForLine(ChromaMode::Filter, 118, 10));
    // Ruling 20: while cycling the T-bar is ignored; otherwise the T-bar alone decides
    for (int tb : {0, 100, 400, 511}) {
        REQUIRE(vt::chromaRowPosition(tb, vt::ChromaCycle::None, 0x10000, 7) == vt::chromaRowStart(tb));
        for (auto c : {vt::ChromaCycle::Forward, vt::ChromaCycle::Backward, vt::ChromaCycle::Both})
            REQUIRE(vt::chromaRowPosition(tb, c, 0x10000, 7) == vt::chromaCycleRow(c, 0x10000, 7));
    }
}

TEST_CASE("cycle speed and direction follow the cycle routines", "[chromafx]") {
    using vt::ChromaCycle;
    CHECK(vt::chromaCycleStep(vt::Speed::Slow) == 0x4000u);
    CHECK(vt::chromaCycleStep(vt::Speed::Medium) == 0x8000u);
    CHECK(vt::chromaCycleStep(vt::Speed::Fast) == 0x10000u);
    CHECK(vt::chromaCycleStep(vt::Speed::Variable) == 0x8000u);
    CHECK(vt::chromaCycleRow(ChromaCycle::None, 0x10000, 50) == 0);
    CHECK(vt::chromaCycleRow(ChromaCycle::Forward, 0x10000, 0) == 0);
    CHECK(vt::chromaCycleRow(ChromaCycle::Forward, 0x10000, 118) == 118);
    CHECK(vt::chromaCycleRow(ChromaCycle::Forward, 0x10000, 119) == 0);
    CHECK(vt::chromaCycleRow(ChromaCycle::Forward, 0x4000, 7) == 1);
    CHECK(vt::chromaCycleRow(ChromaCycle::Backward, 0x10000, 1) == 118);
    CHECK(vt::chromaCycleRow(ChromaCycle::Backward, 0x4000, 1) == 118);
    CHECK(vt::chromaCycleRow(ChromaCycle::Backward, 0x4000, 5) == 117);
    CHECK(vt::chromaCycleRow(ChromaCycle::Both, 0x10000, 118) == 118);
    CHECK(vt::chromaCycleRow(ChromaCycle::Both, 0x10000, 119) == 118);
    CHECK(vt::chromaCycleRow(ChromaCycle::Both, 0x10000, 237) == 0);
    CHECK(vt::chromaCycleRow(ChromaCycle::Both, 0x10000, 238) == 0);
    CHECK(vt::chromaCycleRow(ChromaCycle::Both, 0x10000, 239) == 1);
    CHECK(vt::chromaCycleRow(ChromaCycle::Forward, 0x8000, 119ull * 2 * 1000003 + 3) == 1);
}

TEST_CASE("applyChroma looks up each pixel's luma in its line's row", "[chromafx]") {
    const auto rows = vt::buildRows(twoColours({255, 0, 0}, {0, 0, 255}), vt::ChromaInterp::Spread);
    const vt::Frame in = solid({128, 128, 128, 255});
    vt::Frame out;
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, out);
    // raster lines 0 and 1 have no row
    CHECK(same(out.at(5, 0), {0, 0, 0, 255}));
    CHECK(same(out.at(5, 1), {0, 0, 0, 255}));
    // luma 128 -> entry floor(17.5 + 32) = 49
    for (int y = 2; y < 10; ++y) CHECK(same(out.at(7, y), rows.rows[size_t(4 + (y & 3))][49]));
    vt::applyChroma(rows, vt::ChromaMode::Transition, vt::chromaRowStart(511), in, out);
    CHECK(same(out.at(7, 4), rows.rows[476][49]));
    // the offset is adjustable; entries clamp to 0..95
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, out, 100.0f);
    CHECK(same(out.at(7, 4), rows.rows[4][95]));
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, out, -100.0f);
    CHECK(same(out.at(7, 4), rows.rows[4][0]));
    // a non-finite offset counts as 0: luma 128 -> entry 32
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, out, std::numeric_limits<float>::quiet_NaN());
    CHECK(same(out.at(7, 4), rows.rows[4][32]));
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, out, std::numeric_limits<float>::infinity());
    CHECK(same(out.at(7, 4), rows.rows[4][32]));
    // the default offset is the hardware guess
    vt::Frame d1, d2;
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, d1);
    vt::applyChroma(rows, vt::ChromaMode::Transition, 0, in, d2, vt::HardwareGuesses{}.chromaLumaOffset);
    CHECK(d1.px == d2.px);
    // Rec.601 luma: (299 R + 587 G + 114 B) / 1000; pure green 255 -> 149.685 -> entry 54
    const vt::Frame g = solid({0, 255, 0, 255});
    vt::applyChroma(rows, vt::ChromaMode::Filter, 0, g, out);
    CHECK(same(out.at(3, 20), rows.rows[20][54]));
}

TEST_CASE("ChromaFX croutons load as effects that recolour B", "[chromafx][effect]") {
    int n = 0;
    for (const auto& ent : std::filesystem::directory_iterator(chromaDir())) {
        if (!ent.is_regular_file()) continue;
        const auto ext = ent.path().extension().string();
        if (ext == ".i" || ext == ".I") continue;  // crouton icons
        // the folder also holds three framestore stills (BluePoster, Gamma, Zebra Stripe)
        const auto c = vt::loadCrouton(ent.path());
        if (!c || c->type != vt::CroutonType::Crfx) continue;
        std::string why;
        INFO(ent.path().string());
        auto e = vt::loadEffect(ent.path(), &why);
        REQUIRE(e);
        REQUIRE(why.empty());
        REQUIRE(e->usesDvMemory());
        REQUIRE(e->frameCount() == 1);
        ++n;
    }
    REQUIRE(n == 96);

    // "Red" is a Transition crouton (no cycling): out = applyChroma(rows, Transition, v(tbar), b)
    auto e = vt::loadEffect(chromaDir() / "Red");
    REQUIRE(e);
    const auto c = vt::loadCrouton(chromaDir() / "Red");
    const auto lut = vt::lutFromCrouton(*c);
    const auto rows = vt::buildRows(*lut, vt::chromaInterp(*lut));
    vt::Frame a = solid({10, 200, 30, 255}), b = solid({90, 90, 200, 255}), out, expect;
    vt::RenderCtx ctx;
    for (int tbar : {0, 200, 511}) {
        e->render(tbar, a, b, out, ctx);
        vt::applyChroma(rows, vt::ChromaMode::Transition, vt::chromaRowStart(tbar), b, expect);
        REQUIRE(out.px == expect.px);
    }
    // A is never shown
    vt::Frame a2 = solid({250, 1, 1, 255}), out2;
    e->render(511, a2, b, out2, ctx);  // `out` is the tbar 511 render
    REQUIRE(out2.px == out.px);
    // the luma offset comes from the hardware guesses
    ctx.guesses.chromaLumaOffset = 0.0f;
    e->render(200, a, b, out2, ctx);
    vt::applyChroma(rows, vt::ChromaMode::Transition, vt::chromaRowStart(200), b, expect, 0.0f);
    REQUIRE(out2.px == expect.px);
}

TEST_CASE("cycling ChromaFX advances with the frame number", "[chromafx][effect]") {
    // "Neon Cycle": Transition, cycle 1 (forward), speed tag 3 (Slow, 0.25 rows per field)
    auto e = vt::loadEffect(chromaDir() / "Neon Cycle");
    REQUIRE(e);
    const auto c = vt::loadCrouton(chromaDir() / "Neon Cycle");
    const auto lut = vt::lutFromCrouton(*c);
    const auto s = vt::chromaSettings(*c, *lut);
    REQUIRE(s.cycle == vt::ChromaCycle::Forward);
    REQUIRE(s.mode == vt::ChromaMode::Transition);
    REQUIRE(s.speed == vt::Speed::Slow);
    const auto rows = vt::buildRows(*lut, s.interp);
    vt::Frame b;
    for (size_t i = 0; i < b.px.size(); ++i) b.px[i] = {uint8_t(i), uint8_t(i >> 8), uint8_t(i >> 3), 255};
    vt::Frame out, expect;
    vt::RenderCtx ctx;
    for (uint32_t fn : {0u, 4u, 40u, 476u, 1000u}) {
        ctx.frameNumber = fn;
        e->render(0, b, b, out, ctx);
        vt::applyChroma(rows, vt::ChromaMode::Transition, vt::chromaCycleRow(s.cycle, 0x4000, fn), b, expect);
        INFO(fn);
        REQUIRE(out.px == expect.px);
    }
    // a non-cycling crouton ignores the frame number
    auto red = vt::loadEffect(chromaDir() / "Red");
    vt::Frame o1, o2;
    ctx.frameNumber = 0;
    red->render(100, b, b, o1, ctx);
    ctx.frameNumber = 77;
    red->render(100, b, b, o2, ctx);
    REQUIRE(o1.px == o2.px);
}

TEST_CASE("a cycling ChromaFX ignores the T-bar (Ruling 20)", "[chromafx][effect]") {
    // The Toaster runs DummyTBar while cycling: the row comes from the cycle accumulator alone.
    vt::Frame b;
    for (size_t i = 0; i < b.px.size(); ++i) b.px[i] = {uint8_t(i * 7), uint8_t(i >> 5), uint8_t(i >> 2), 255};
    vt::RenderCtx ctx;
    auto neon = vt::loadEffect(chromaDir() / "Neon Cycle");  // Transition, cycling forward
    REQUIRE(neon);
    vt::Frame o0, o400;
    for (uint32_t fn : {0u, 37u, 500u}) {
        ctx.frameNumber = fn;
        neon->render(0, b, b, o0, ctx);
        neon->render(400, b, b, o400, ctx);
        INFO(fn);
        REQUIRE(o0.px == o400.px);
    }
    // a non-cycling Transition crouton follows the T-bar
    auto red = vt::loadEffect(chromaDir() / "Red");
    REQUIRE(red);
    red->render(0, b, b, o0, ctx);
    red->render(400, b, b, o400, ctx);
    REQUIRE(o0.px != o400.px);
}
