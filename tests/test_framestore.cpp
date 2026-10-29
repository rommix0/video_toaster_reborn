#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/crouton.h"
#include "vt/effect.h"
#include "vt/framestore.h"

namespace {

std::filesystem::path framestore(const char* rel) { return dataRoot() / rel; }

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
    for (const char* dir : {"4.2/Framestore", "4.2/CG/Frames", "4.2/Programs/Switcher_Support/StartupFrames", "4.2/3rdparty"}) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(framestore(dir), ec), end; it != end; it.increment(ec)) {
            if (!it->is_regular_file()) continue;
            const auto ext = it->path().extension().string();
            if (ext == ".i" || ext == ".I") continue;  // the icon files hold no picture
            auto c = vt::loadCrouton(it->path());
            if (!c || c->type != vt::CroutonType::Fram) continue;
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
    CHECK(count >= 60);  // FREEZE.md §4: 63
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
