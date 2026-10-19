#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/assets/icons.h"
#include "vt/ilbm.h"
#include "vt/log.h"

namespace fs = std::filesystem;

namespace {

using vt::assets::Crouton;

fs::path effects(const char* rel) { return dataRoot() / "4.2/Effects" / rel; }

const Crouton* find(const std::vector<Crouton>& cs, const std::string& name) {
    const auto it = std::find_if(cs.begin(), cs.end(), [&](const Crouton& c) { return c.name == name; });
    return it == cs.end() ? nullptr : &*it;
}

// A scratch folder unique to this run, removed when the test ends.
struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("vt_icons_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
};

struct LogLines {
    std::vector<std::string> lines;
    LogLines() { vt::setLogSink([this](std::string_view m) { lines.emplace_back(m); }); }
    ~LogLines() { vt::setLogSink({}); }
};

}  // namespace

TEST_CASE("the Wipes drawer: 80x50 croutons sorted by name, decoded as the engine decodes the icon") {
    LogLines log;
    const auto cs = vt::assets::scanCroutons(effects("4000FX/Wipes"));
    REQUIRE(cs.size() == 79);
    CHECK(log.lines.empty());
    for (const auto& c : cs) {
        INFO(c.name);
        CHECK(c.image.w == 80);
        CHECK(c.image.h == 50);
        CHECK(c.image.px.size() == 80u * 50u);
        CHECK(!c.palette.empty());
        CHECK(c.effect.parent_path() == effects("4000FX/Wipes"));
    }
    CHECK(cs.front().name == "Crop To Center");
    CHECK(find(cs, "Wipe 3 Slots In") < find(cs, "WIPE 5 SLITS L-R"));  // case-insensitive order

    const Crouton* crop = find(cs, "Crop To Center");
    REQUIRE(crop);
    CHECK(crop->effect == effects("4000FX/Wipes/Crop To Center"));
    CHECK(crop->icon == effects("4000FX/Wipes/Crop To Center.i"));
    const auto bytes = readFile(crop->icon);
    const auto forms = vt::iffForms(bytes);
    REQUIRE(!forms.empty());
    const auto [bm, img] = vt::readIlbm(bytes, forms.front());
    CHECK(crop->image.px == img.px);
    REQUIRE(crop->palette.size() == bm.cmap.size());
    for (size_t i = 0; i < bm.cmap.size(); ++i) {
        CHECK(crop->palette[i] == vt::Rgba8{bm.cmap[i].r, bm.cmap[i].g, bm.cmap[i].b, 255});
    }
}

TEST_CASE("an effect file without a sibling icon is its own icon (6 planes, 64 colours)") {
    const auto withSibling = vt::assets::scanCroutons(effects("2000FX/Control"));
    const Crouton* v1 = find(withSibling, "Video Source 1");
    REQUIRE(v1);
    CHECK(v1->icon == effects("2000FX/Control/Video Source 1.I"));
    CHECK(v1->palette.size() == 64);  // 6 planes

    TempDir dir;
    fs::copy_file(v1->effect, dir.path / "Video Source 1");
    const auto alone = vt::assets::scanCroutons(dir.path);
    REQUIRE(alone.size() == 1);
    CHECK(alone[0].icon == alone[0].effect);
    CHECK(alone[0].image.px == v1->image.px);
    CHECK(alone[0].palette == v1->palette);
}

TEST_CASE("ChromaFX croutons get the placeholder picture") {
    const auto cs = vt::assets::scanCroutons(effects("ChromaFx"));
    const Crouton* band = find(cs, "Band Filter");
    REQUIRE(band);
    CHECK(band->icon == effects("ChromaFx/Band Filter.I"));
    REQUIRE(band->image.px.size() == 80u * 50u);
    CHECK(band->image.px[0] == 0);           // outline
    CHECK(band->image.px[1 * 80 + 1] == 2);  // face at (1,1)
    CHECK(band->palette.size() == 4);
}

TEST_CASE("folders, non-crouton files and icons of the wrong size are skipped") {
    TempDir dir;
    for (const char* f : {"Crop To Center", "Crop To Center.i"}) fs::copy_file(effects("4000FX/Wipes") / f, dir.path / f);
    fs::copy_file(effects("4000FX/SoftEdge/Wipe Soft Circle Out!"), dir.path / "Big");  // 768x482 leading ILBM
    std::ofstream(dir.path / "notes.txt") << "not a crouton\n";
    fs::create_directories(dir.path / "Sub");
    std::ofstream(dir.path / "Sub.i") << "drawer icon";

    LogLines log;
    const auto cs = vt::assets::scanCroutons(dir.path);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].name == "Crop To Center");
    REQUIRE(log.lines.size() == 1);
    CHECK(log.lines[0].find("Big") != std::string::npos);
    CHECK(log.lines[0].find("768x482") != std::string::npos);
}
