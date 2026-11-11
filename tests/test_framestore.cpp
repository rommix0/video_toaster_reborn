#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/assets/icons.h"
#include "vt/crouton.h"
#include "vt/dv_colour.h"
#include "vt/effect.h"
#include "vt/framestore.h"
#include "vt/iff.h"

namespace {

std::filesystem::path framestore(const char* rel) { return dataRoot() / rel; }

// A still with a distinct ramp per gun inside the framestore rect and black outside.
vt::Frame ramp(int step) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), vt::Rgba8{0, 0, 0, 255});
    const vt::Rect r = vt::framestoreRect();
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            f.at(r.x + x, r.y + y) = {uint8_t(x * step), uint8_t(y * step), uint8_t((x + y) * step), 255};
    return f;
}

// A composite save keeps one colour per pixel pair (x even, x + 1): each loaded pixel is the pair's
// mean within kRoundTrip per gun, for colours a composite store can hold (design 2 C). Measured over
// all 16.7 M RGB colours: RGB -> DvQuad -> RGB moves the 99.5 % whose quad needs no clamp by at most 8
// (99.5 % of them within 6); DV luma has 130 steps, not 256. Over-saturated colours are clamped (up
// to 32), as on the Toaster. Checked inside the framestore rect.
constexpr int kRoundTrip = 8;
void requireNearPairs(const vt::Frame& back, const vt::Frame& still) {
    const vt::Rect r = vt::framestoreRect();
    int worst = 0, wx = 0, wy = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const int x0 = r.x + ((x - r.x) & ~1);
            const vt::Rgba8 a = still.at(x0, y), b = still.at(x0 + 1, y), got = back.at(x, y);
            for (auto [g, m] : {std::pair{int(got.r), (a.r + b.r + 1) / 2}, {int(got.g), (a.g + b.g + 1) / 2},
                                {int(got.b), (a.b + b.b + 1) / 2}})
                if (std::abs(g - m) > worst) worst = std::abs(g - m), wx = x, wy = y;
        }
    INFO("worst gun error " << worst << " at " << wx << "," << wy);
    CHECK(worst <= kRoundTrip);
}

// A ramp that stays inside the composite range (guns 80..175, so none is far from the others).
vt::Frame softRamp(int step) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), vt::Rgba8{0, 0, 0, 255});
    const vt::Rect r = vt::framestoreRect();
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            f.at(r.x + x, r.y + y) = {uint8_t(80 + x * step % 96), uint8_t(80 + y * step % 96),
                                      uint8_t(80 + (x + y) * step % 96), 255};
    return f;
}

struct TempDir {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "vt_framestore_save";
    TempDir() { std::filesystem::remove_all(dir); }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

}  // namespace

TEST_CASE("ColorBars decodes to the 75 % bars") {
    std::string why;
    auto f = vt::loadFramestore(framestore("4.2/Framestore/ColorBars"), &why);
    INFO(why);
    REQUIRE(f);
    REQUIRE(vt::framestoreRect() == vt::Rect{8, 1, 752, 480});

    const std::array<vt::Rgba8, 7> bars{{{191, 191, 191, 255}, {191, 191, 0, 255}, {0, 191, 191, 255},
                                         {0, 191, 0, 255},     {191, 0, 191, 255}, {191, 0, 0, 255},
                                         {0, 0, 191, 255}}};
    for (int k = 0; k < 7; ++k)
        for (int y : {121, 241}) {
            const vt::Rgba8 got = f->at(61 + 107 * k, y), want = bars[size_t(k)];
            INFO("bar " << k << " row " << y);
            CHECK(std::abs(got.r - want.r) <= 2);
            CHECK(std::abs(got.g - want.g) <= 2);
            CHECK(std::abs(got.b - want.b) <= 2);
        }
    for (auto [x, y] : {std::pair{0, 0}, {767, 481}}) CHECK(f->at(x, y) == vt::Rgba8{0, 0, 0, 255});
}

TEST_CASE("every shipped framestore loads") {
    int count = 0, bad = 0;
    for (const char* dir : {"4.2/Framestore", "4.2/CG/Frames", "4.2/Programs/Switcher_Support/StartupFrames"}) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(framestore(dir), ec), end; it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const auto ext = it->path().extension().string();
            if (ext == ".i" || ext == ".I") continue;  // the icon files hold no picture
            if (!vt::isFramestore(it->path())) continue;
            ++count;
            std::string why;
            if (!vt::loadFramestore(it->path(), &why)) {
                ++bad;
                std::cout << "refused " << it->path().string() << ": " << why << "\n";
            }
        }
    }
    CAPTURE(count, bad);
    CHECK(bad == 0);
    CHECK(count == 74);  // FREEZE.md §4: 75 = 63 with CrUD FRAM + 12 LightWaveDemo without (Ruling 9), less 1 in 3rdparty (not in the repository)
}

TEST_CASE("framestores without a CrUD of their own load (Ruling 9)") {
    // ApeBot: FRAM only in ApeBot.I; Theater: no CrUD in the file or in Theater.i
    for (const char* rel : {"4.2/Framestore/LightWaveDemo/ApeBot", "4.2/Framestore/LightWaveDemo/Theater"}) {
        INFO(rel);
        const auto p = framestore(rel);
        const auto c = vt::loadCrouton(p);
        REQUIRE(c);
        CHECK(c->type == vt::CroutonType::None);
        CHECK(vt::isFramestore(p));
        std::string why;
        CHECK(vt::loadFramestore(p, &why));
        CHECK(!vt::loadEffect(p, &why));
        CHECK(why == "a framestore (load it into DV1/DV2), not an effect");
    }
    // a CrUD-less effect whose icon carries its program (Plan 4 Ruling 9) and an effect are not
    CHECK(!vt::isFramestore(framestore("4.2/Effects/2000FX/Smooth Fade")));
    CHECK(!vt::isFramestore(framestore("4.2/Effects/4000FX/Digital/Hand Pull !")));
    CHECK(!vt::isFramestore(framestore("4.2/Framestore/nothing")));
}

TEST_CASE("an unreadable icon counts as none; the decided picture is handed back") {
    TempDir t;
    std::filesystem::create_directories(t.dir);
    const auto pic = t.dir / "Pic";
    std::filesystem::copy_file(framestore("4.2/Framestore/LightWaveDemo/Theater"), pic);
    std::ofstream(t.dir / "Pic.i", std::ios::binary) << "FORM\xff\xff\xff\xff" "CrUD not an icon";  // cut short
    REQUIRE(vt::loadCrouton(t.dir / "Pic.i"));
    CHECK(vt::loadCrouton(t.dir / "Pic.i")->type == vt::CroutonType::None);
    std::optional<vt::Frame> still;
    CHECK(vt::isFramestore(pic, &still));
    REQUIRE(still);
    CHECK(still->px == vt::loadFramestore(pic)->px);
    // decided from a CrUD: nothing decoded
    std::optional<vt::Frame> none;
    CHECK(vt::isFramestore(framestore("4.2/Framestore/ColorBars"), &none));
    CHECK(!none);
}

TEST_CASE("framestore refuses bad files") {
    const auto bars = framestore("4.2/Framestore/ColorBars");
    const auto cut = std::filesystem::temp_directory_path() / "vt_framestore_cut";
    {
        const auto bytes = readFile(bars);
        std::ofstream(cut, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), 100000);
    }
    for (const auto& p : {cut, framestore("4.2/Effects/4000FX/Digital/Hand Pull !"), framestore("4.2/Framestore/nothing")}) {
        std::string why;
        INFO(p.string());
        CHECK(!vt::loadFramestore(p, &why));
        CHECK(!why.empty());
    }
    std::filesystem::remove(cut);
}

TEST_CASE("loadEffect refuses a framestore") {
    std::string why;
    CHECK(!vt::loadEffect(framestore("4.2/Framestore/ColorBars"), &why));
    CHECK(why == "a framestore (load it into DV1/DV2), not an effect");
}

TEST_CASE("framestore 4 Fields save then load keeps each pixel pair's colour") {
    TempDir t;
    const vt::Frame still = softRamp(3);
    std::string why;
    const auto file = t.dir / "Ramp";
    const bool saved = vt::saveFramestore(file, still, vt::SaveFields::Four, &why);
    INFO(why);  // INFO copies `why` now: after the call, before the REQUIRE that needs it
    REQUIRE(saved);

    const auto back = vt::loadFramestore(file, &why);
    INFO(why);
    REQUIRE(back);
    requireNearPairs(*back, still);
    const auto c = vt::loadCrouton(file);
    REQUIRE(c);
    CHECK(c->type == vt::CroutonType::Fram);

    REQUIRE(std::filesystem::exists(t.dir / "Ramp.I"));
    CHECK(!std::filesystem::exists(t.dir / "Ramp.tmp"));
    CHECK(!std::filesystem::exists(t.dir / "Ramp.I.tmp"));
    const auto icons = vt::assets::scanCroutons(t.dir);
    REQUIRE(icons.size() == 1);
    CHECK(icons[0].image.w == 80);
    CHECK(icons[0].image.h == 50);
}

TEST_CASE("saved file has the original's layout") {
    TempDir t;
    REQUIRE(vt::saveFramestore(t.dir / "Ramp", ramp(3), vt::SaveFields::Four));
    const auto bytes = readFile(t.dir / "Ramp");
    const auto forms = vt::iffForms(bytes);
    const std::array<std::string, 7> want{"ILBM", "ILBM", " PUS", "ILBM", " PUS", "CrUD", " PUS"};
    REQUIRE(forms.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK(std::string(forms[i].type.data(), 4) == want[i]);

    const auto chunks = vt::formChunks(bytes, forms[0]);
    REQUIRE(chunks.size() == 3);
    CHECK(std::string(chunks[0].id.data(), 4) == "BMHD");
    // 16 planes, PLTP lanes 6 then 7, MSB first: switcher.main 0x11ba66 flags 3 (FREEZE.md §3 SAVE)
    const std::vector<uint8_t> bmhd{0x02, 0xF0, 0x01, 0xE0, 0, 0, 0, 0, 0x10, 0, 0x01, 0, 0, 0, 0x0A, 0x0B, 0x02, 0xF0, 0x01, 0xE0};
    CHECK(std::vector<uint8_t>(bytes.begin() + long(chunks[0].data), bytes.begin() + long(chunks[0].data + chunks[0].size)) == bmhd);
    CHECK(std::string(chunks[1].id.data(), 4) == "PLTP");
    CHECK(chunks[1].size == 32);
    for (size_t n = 0; n < 16; ++n) {
        CHECK(bytes[chunks[1].data + 2 * n] == 6 + n / 8);
        CHECK(bytes[chunks[1].data + 2 * n + 1] == 7 - n % 8);
    }

    // The tail is ColorBars.I's layout: same size, and the CrUD forms are byte-identical.
    const auto icons = readFile(t.dir / "Ramp.I");
    const auto orig = readFile(framestore("4.2/Framestore/ColorBars.I"));
    REQUIRE(icons.size() == orig.size());
    CHECK(std::equal(icons.end() - 88, icons.end(), orig.end() - 88));
    CHECK(std::equal(icons.begin(), icons.begin() + 8, orig.begin()));  // FORM, 3240
    CHECK(std::equal(bytes.end() - long(icons.size()), bytes.end(), icons.begin()));
}

TEST_CASE("framestore save creates the folder and handles unicode names") {
    TempDir t;
    const auto file = t.dir / "new folder" / std::filesystem::path(u8"Bärs ✓");
    const vt::Frame still = softRamp(1);
    std::string why;
    const bool saved = vt::saveFramestore(file, still, vt::SaveFields::Four, &why);
    INFO(why);
    REQUIRE(saved);
    const auto back = vt::loadFramestore(file, &why);
    INFO(why);
    REQUIRE(back);
    requireNearPairs(*back, still);
    CHECK(std::filesystem::exists(std::filesystem::path(file) += ".I"));

    // a refusal names the file in UTF-8 (path::string() is the ANSI code page: "?" for ✓, or a throw)
    const std::u8string missing = u8"Fehlt ✓";
    CHECK(!vt::loadFramestore(t.dir / missing, &why));
    CHECK(why.starts_with("cannot read "));
    CHECK(why.ends_with(std::string(missing.begin(), missing.end())));
}

TEST_CASE("framestore SAVE over an existing file replaces it") {
    TempDir t;
    const auto file = t.dir / "Twice";
    std::string why;
    REQUIRE(vt::saveFramestore(file, softRamp(1), vt::SaveFields::Four, &why));
    const bool again = vt::saveFramestore(file, softRamp(3), vt::SaveFields::Four, &why);
    INFO(why);
    REQUIRE(again);
    const auto back = vt::loadFramestore(file, &why);
    INFO(why);
    REQUIRE(back);
    requireNearPairs(*back, softRamp(3));
}

TEST_CASE("framestore save fails cleanly") {
    TempDir t;
    std::filesystem::create_directories(t.dir);
    { std::ofstream(t.dir / "plain") << "x"; }
    std::string why;
    // the parent "folder" is a file
    CHECK(!vt::saveFramestore(t.dir / "plain" / "Ramp", ramp(3), vt::SaveFields::Four, &why));
    CHECK(!why.empty());
    CHECK(std::distance(std::filesystem::directory_iterator(t.dir), {}) == 1);
}

TEST_CASE("framestore save keeps ColorBars and the colours a composite store holds") {
    TempDir t;
    std::string why;
    const auto bars = vt::loadFramestore(framestore("4.2/Framestore/ColorBars"), &why);
    INFO(why);
    REQUIRE(bars);
    // Flat 16 x 16 blocks of decoded quads: Y over the DV range, I and Q up to +-60 (legal samples).
    vt::Frame sweep;
    std::fill(sweep.px.begin(), sweep.px.end(), vt::Rgba8{0, 0, 0, 255});
    const vt::Rect r = vt::framestoreRect();
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const int k = (y / 16) * (r.w / 16) + x / 16;
            const int Y = 100 + k % 71, i = (k * 7) % 61 - 30, q = (k * 13) % 61 - 30;
            sweep.at(r.x + x, r.y + y) =
                vt::dvQuadToRgb({uint8_t(Y - i), uint8_t(Y - q), uint8_t(Y + i), uint8_t(Y + q)});
        }
    for (const vt::Frame* still : std::initializer_list<const vt::Frame*>{&*bars, &sweep}) {
        const auto file = t.dir / "Sweep";
        const bool saved = vt::saveFramestore(file, *still, vt::SaveFields::Four, &why);
        INFO(why);
        REQUIRE(saved);
        const auto back = vt::loadFramestore(file, &why);
        INFO(why);
        REQUIRE(back);
        requireNearPairs(*back, *still);
    }
}

// Design 2 E (save_ref.py, verified): file rows 1, 3 .. 477 (raster 2, 4 .. 478) kept, rows 2 .. 476
// the floor mean of their neighbours, rows 0, 478, 479 (raster 1, 479, 480) black.
TEST_CASE("framestore 1 Field save keeps the even field, line-doubled") {
    TempDir t;
    vt::Frame still;
    std::fill(still.px.begin(), still.px.end(), vt::Rgba8{0, 0, 0, 255});
    const vt::Rect r = vt::framestoreRect();
    auto lineColour = [](int y) { return vt::Rgba8{uint8_t(y * 7 % 256), uint8_t(y * 13 % 256), uint8_t(255 - y % 256), 255}; };
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) still.at(x, y) = lineColour(y);
    const auto file = t.dir / "Field";
    std::string why;
    const bool saved = vt::saveFramestore(file, still, vt::SaveFields::One, &why);
    INFO(why);
    REQUIRE(saved);
    const auto back = vt::loadFramestore(file, &why);
    INFO(why);
    REQUIRE(back);

    vt::Frame want = still;
    for (int y = r.y; y < r.y + r.h; ++y) {
        const int row = y - r.y;
        for (int x = r.x; x < r.x + r.w; ++x) {
            vt::Rgba8& w = want.at(x, y);
            if (row == 0 || row >= 478) {
                w = {0, 0, 0, 255};
            } else if (row % 2 == 0) {
                const vt::Rgba8 a = still.at(x, y - 1), b = still.at(x, y + 1);
                w = {uint8_t((a.r + b.r) >> 1), uint8_t((a.g + b.g) >> 1), uint8_t((a.b + b.b) >> 1), 255};
            }
        }
    }
    requireNearPairs(*back, want);
    for (int x = r.x; x < r.x + r.w; ++x)
        for (int y : {r.y, r.y + 478, r.y + 479}) CHECK(back->at(x, y) == vt::Rgba8{0, 0, 0, 255});
}
