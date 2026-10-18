#include <algorithm>
#include <array>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/crouton.h"
#include "vt/timing.h"

namespace {

bool hasAnim(const vt::Crouton& c) {
    return std::ranges::any_of(c.forms, [](auto& f) { return f.type == std::array{'A', 'N', 'I', 'M'}; });
}

vt::Speed speedByName(const std::string& n) {
    if (n == "slow") return vt::Speed::Slow;
    if (n == "medium") return vt::Speed::Medium;
    if (n == "fast") return vt::Speed::Fast;
    REQUIRE(n == "variable");
    return vt::Speed::Variable;
}

vt::Timing timingOf(const char* rel, int frames) {
    auto c = vt::loadCrouton(dataRoot() / rel);
    REQUIRE(c);
    return vt::animTiming(*c, frames);
}

}  // namespace

TEST_CASE("timing matches golden for every effect") {
    for (const auto& r : golden::readLines("timing.jsonl")) {
        const auto& src = std::get<std::string>(r.at("source").v);
        INFO(src);
        auto c = vt::loadCrouton(golden::sourcePath(src));
        REQUIRE(c);
        const auto frames = static_cast<int>(std::get<int64_t>(r.at("frames").v));
        const auto& fields = std::get<std::vector<int64_t>>(r.at("fields").v);
        const auto& dur = std::get<std::vector<int64_t>>(r.at("duration").v);
        const vt::Timing t = hasAnim(*c) ? vt::animTiming(*c, frames) : vt::fixedTiming(*c);
        REQUIRE(hasAnim(*c) == !fields.empty());
        REQUIRE(dur.size() == 4);
        for (size_t i = 0; i < 4; ++i) {
            REQUIRE(t.durationFrames[i] == dur[i]);
            if (!fields.empty()) REQUIRE(t.fieldsPerFrame[i] == fields[i]);
            else REQUIRE(t.fieldsPerFrame[i] == 0);
        }
        REQUIRE(t.defaultSpeed == speedByName(std::get<std::string>(r.at("speed_default").v)));
    }
}

TEST_CASE("Page Peel ! fast is one second") {
    auto t = timingOf("4.2/Effects/4000FX/PagePeel/Page Peel !", 60);
    REQUIRE(t.durationFrames[static_cast<size_t>(vt::Speed::Fast)] == 30);
    REQUIRE(t.defaultSpeed == vt::Speed::Fast);  // tag 0x55 absent: ANIM default 1 = Fast
}

TEST_CASE("disabled speed falls back") {  // Review Focus 2
    auto t = timingOf("4.2/Effects/2000FX/PagePeel/Page Peel", 54);
    REQUIRE_FALSE(t.available(vt::Speed::Slow));
    REQUIRE_FALSE(t.available(vt::Speed::Variable));
    REQUIRE(t.available(vt::Speed::Medium));
    REQUIRE(t.available(vt::Speed::Fast));
    REQUIRE(t.defaultSpeed == vt::Speed::Fast);  // tag 0x55 = 1
    REQUIRE(t.resolve(vt::Speed::Slow) == vt::Speed::Fast);
    REQUIRE(t.resolve(vt::Speed::Medium) == vt::Speed::Medium);
}

TEST_CASE("speed tag decodes through the switcher table") {
    REQUIRE(vt::decodeSpeedTag(0) == vt::Speed::Medium);
    REQUIRE(vt::decodeSpeedTag(1) == vt::Speed::Fast);
    REQUIRE(vt::decodeSpeedTag(2) == vt::Speed::Variable);
    REQUIRE(vt::decodeSpeedTag(3) == vt::Speed::Slow);
    REQUIRE(vt::decodeSpeedTag(4) == vt::Speed::Medium);  // masked & 3
}

TEST_CASE("speed tag 0 is Medium") {
    auto t = timingOf("4.2/Effects/2000FX/OverLays/DropOuts", 60);
    REQUIRE(t.defaultSpeed == vt::Speed::Medium);
}

TEST_CASE("gradient with speed tag 2 is Variable") {
    auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/2000FX/Border/Wipe Border Diamonds");
    REQUIRE(c);
    REQUIRE_FALSE(hasAnim(*c));
    auto t = vt::fixedTiming(*c);
    REQUIRE(t.defaultSpeed == vt::Speed::Variable);
    REQUIRE(t.durationFrames[static_cast<size_t>(vt::Speed::Variable)] == 40);
    REQUIRE(t.available(vt::Speed::Variable));
    REQUIRE(t.resolve(vt::Speed::Slow) == vt::Speed::Slow);
}

TEST_CASE("resolve prefers the default, then Fast, Medium, Slow, Variable") {
    vt::Timing t;
    t.defaultSpeed = vt::Speed::Medium;
    t.durationFrames = {0, 0, 0, 0};
    REQUIRE(t.resolve(vt::Speed::Slow) == vt::Speed::Medium);  // nothing available: the default
    t.durationFrames = {10, 0, 0, 5};
    REQUIRE(t.resolve(vt::Speed::Fast) == vt::Speed::Slow);  // default unavailable: Slow before Variable
    t.durationFrames = {10, 20, 0, 5};
    REQUIRE(t.resolve(vt::Speed::Fast) == vt::Speed::Medium);  // the default
    t.defaultSpeed = vt::Speed::Fast;
    t.durationFrames = {10, 20, 0, 5};
    REQUIRE(t.resolve(vt::Speed::Fast) == vt::Speed::Medium);  // Fast unavailable too: Medium
}

TEST_CASE("3.1 hunk effect gets the ANIM defaults") {
    auto c = vt::loadCrouton(dataRoot() / "3.1/ToasterAll/EFFECTS/FlyIn");
    REQUIRE(c);
    REQUIRE(c->tags.empty());
    auto t = vt::animTiming(*c, 30);
    REQUIRE(t.fieldsPerFrame == std::array{3, 2, 1, 0});
    REQUIRE(t.durationFrames == std::array{45, 30, 15, 0});
    REQUIRE(t.defaultSpeed == vt::Speed::Fast);
}

TEST_CASE("T-bar maps to frames") {
    REQUIRE(vt::animFrameForTBar(0, 60) == 0);
    REQUIRE(vt::animFrameForTBar(511, 60) == 59);
    REQUIRE(vt::animFrameForTBar(256, 60) == 30);
    REQUIRE(vt::animFrameForTBar(511, 1) == 0);
    REQUIRE(vt::animFrameForTBar(600, 60) == 59);  // clamped
}
