#include <stdexcept>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/assets/panel.h"
#include "vt/crc32.h"

namespace {

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

std::filesystem::path switcherMain() { return dataRoot() / "4.2/Programs/switcher.main"; }

// panel.jsonl: "node" lines, "text" lines, then one "panel" line.
struct Golden {
    std::vector<golden::Record> nodes, texts;
    golden::Record panel;
};

const Golden& panelGolden() {
    static const Golden g = [] {
        Golden out;
        for (auto& r : golden::readLines("panel.jsonl")) {
            const auto& kind = str(r, "rec");
            if (kind == "node") out.nodes.push_back(std::move(r));
            else if (kind == "text") out.texts.push_back(std::move(r));
            else out.panel = std::move(r);
        }
        return out;
    }();
    return g;
}

const vt::assets::PanelLayout& panel() {
    static const auto p = vt::assets::loadPanel(vt::assets::loadHunks(readFile(switcherMain())));
    return p;
}

void checkRect(const vt::Rect& r, const std::vector<int64_t>& g) {
    REQUIRE(g.size() == 4);
    CHECK(r == vt::Rect{int(g[0]), int(g[1]), int(g[2]), int(g[3])});
}

void checkCrcs(const std::vector<vt::assets::IndexedImage>& imgs, const std::vector<int64_t>& g) {
    REQUIRE(imgs.size() == g.size());
    for (size_t i = 0; i < imgs.size(); ++i) {
        INFO("image " << i);
        CHECK(vt::crc32(imgs[i].px) == uint32_t(g[i]));
    }
}

vt::assets::Palette palette(const std::vector<int64_t>& words) {
    vt::assets::Palette p;
    for (auto w : words)
        p.push_back({uint8_t((w >> 8 & 15) * 17), uint8_t((w >> 4 & 15) * 17), uint8_t((w & 15) * 17), 255});
    return p;
}

}  // namespace

TEST_CASE("panel nodes, images and T-bar match the reference extraction") {
    const auto& p = panel();
    const auto& g = panelGolden();
    REQUIRE(g.nodes.size() == 70);
    REQUIRE(p.nodes.size() == g.nodes.size());
    for (size_t i = 0; i < p.nodes.size(); ++i) {
        const auto& n = p.nodes[i];
        const auto& r = g.nodes[i];
        INFO(str(r, "id"));
        CHECK(n.id == str(r, "id"));
        checkRect(n.rect, arr(r, "rect"));
        checkRect(n.hitRect, arr(r, "hit_rect"));
        CHECK(n.method == num(r, "method"));
        CHECK(n.list == num(r, "list"));
        CHECK(n.selectPattern == num(r, "select_pattern") * 0x101);
        CHECK(n.hiddenByDefault == (num(r, "hidden") != 0));
        CHECK(n.states.size() == size_t(num(r, "n_states")));
        CHECK(vt::crc32(n.image.px) == uint32_t(num(r, "image_crc32")));
        checkCrcs(n.states, arr(r, "state_crc32"));
        checkCrcs(n.masks, arr(r, "mask_crc32"));
        std::vector<vt::assets::IndexedImage> selected;
        for (size_t s = 0; s < n.states.size(); ++s) {
            CHECK(n.states[s].w == n.rect.w);
            CHECK(n.states[s].h == n.rect.h);
            selected.push_back(vt::assets::selectedLook(n, int(s)));
        }
        checkCrcs(selected, arr(r, "selected_crc32"));
    }

    REQUIRE(p.tbarImages.size() == 86);
    checkCrcs(p.tbarImages, arr(g.panel, "tbar_crc32"));
    checkRect(p.tbarRect, arr(g.panel, "tbar_rect"));

    CHECK(p.rgbMode == vt::assets::Palette{{0, 0, 0, 255}, {0x55, 0x55, 0x55, 255}, {0x99, 0x99, 0x99, 255},
                                           {0xDD, 0xDD, 0xDD, 255}});
    CHECK(p.rgbMode == palette(arr(g.panel, "palette_rgb_mode")));
    CHECK(p.previewMode == palette(arr(g.panel, "palette_preview_mode")));
    CHECK(p.screenW == arr(g.panel, "screen")[0]);
    CHECK(p.screenH == arr(g.panel, "screen")[1]);
    CHECK(p.clearColour == num(g.panel, "clear_colour"));
    CHECK(p.clearMarginX == num(g.panel, "clear_margin_x"));
}

TEST_CASE("panel run-time text positions match the reference") {
    const auto& p = panel();
    const auto& g = panelGolden();
    REQUIRE(p.texts.size() == g.texts.size());
    for (size_t i = 0; i < p.texts.size(); ++i) {
        const auto& t = p.texts[i];
        const auto& r = g.texts[i];
        INFO(str(r, "id"));
        CHECK(t.id == str(r, "id"));
        CHECK(t.x == num(r, "x"));
        CHECK(t.y == num(r, "y"));
        CHECK(t.baseline == (num(r, "baseline") != 0));
        CHECK(t.text == str(r, "text"));
        CHECK(t.hiddenByDefault == (num(r, "hidden") != 0));
    }
}

TEST_CASE("default screen matches render_layout.py") {
    const auto& p = panel();
    const auto scr = vt::assets::renderDefaultScreen(p);
    REQUIRE(scr.w == 768);
    REQUIRE(scr.h == 481);
    REQUIRE(scr.px.size() == size_t(768) * 481);
    CHECK(vt::crc32(scr.px) == uint32_t(num(panelGolden().panel, "default_screen_crc32")));
}

TEST_CASE("selected look rotates colours inside the mask") {
    static constexpr uint8_t kRotate[4] = {0, 2, 3, 1};
    int checked = 0;
    for (const auto& n : panel().nodes) {
        if (n.method != 4) continue;
        REQUIRE(n.masks.size() == n.states.size());
        for (size_t s = 0; s < n.states.size(); ++s) {
            const auto& img = n.states[s];
            const auto& mask = n.masks[s];
            const auto sel = vt::assets::selectedLook(n, int(s));
            REQUIRE(sel.px.size() == img.px.size());
            REQUIRE(mask.px.size() == img.px.size());
            for (size_t i = 0; i < img.px.size(); ++i) {
                const uint8_t want = mask.px[i] ? kRotate[img.px[i]] : img.px[i];
                if (sel.px[i] != want) FAIL(n.id << " state " << s << " pixel " << i);
                ++checked;
            }
        }
    }
    CHECK(checked > 100000);
    CHECK_THROWS_AS(vt::assets::selectedLook(panel().nodes.front(), 99), std::out_of_range);
}

TEST_CASE("broken panel data throws with the address") {
    auto img = vt::assets::loadHunks(readFile(switcherMain()));
    SECTION("list head outside the image") {
        img.flat[0x5d5c + 0x160] = 0x7f;
        CHECK_THROWS_AS(vt::assets::loadPanel(img), std::runtime_error);
        CHECK_THROWS_WITH(vt::assets::loadPanel(img), Catch::Matchers::ContainsSubstring("0x"));
    }
    SECTION("draw method out of range") {
        img.flat[0x37000 + 0x2d] = 0x18;  // MAIN 1: method 6
        CHECK_THROWS_AS(vt::assets::loadPanel(img), std::runtime_error);
        CHECK_THROWS_WITH(vt::assets::loadPanel(img), Catch::Matchers::ContainsSubstring("0x03702c"));
    }
}
