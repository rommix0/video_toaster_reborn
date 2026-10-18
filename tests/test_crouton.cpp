#include <algorithm>
#include <array>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/crouton.h"
#include "vt/iff.h"

TEST_CASE("Bear-L crouton tags") {
    auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/2000FX/Animals/Bear-L");
    REQUIRE(c);
    REQUIRE(c->type == vt::CroutonType::Anim);
    REQUIRE(c->tagOr(0x80000022) == 7);  // mode 7: colour roles
    REQUIRE(c->tagOr(0x80000058) == 2);  // medium = 2 fields per frame
}

TEST_CASE("missing tags use crouton.library ANIM defaults") {
    auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/2000FX/Animals/Falling Sheep");
    REQUIRE(c);
    REQUIRE(!c->tag(0x80000057));
    REQUIRE(c->tagOr(0x80000055) == 1);
    REQUIRE(c->tagOr(0x80000057) == 3);
    REQUIRE(c->tagOr(0x80000058) == 2);
    REQUIRE(c->tagOr(0x80000059) == 1);
}

TEST_CASE("ChromaFX crouton is CRFX in Transition mode") {
    auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/ChromaFx/Fade To Neg");
    REQUIRE(c);
    REQUIRE(c->type == vt::CroutonType::Crfx);
    REQUIRE(c->tagOr(0x80000080) == 1);
}

TEST_CASE("3.1 hunk effect unwraps to an ANIM form") {
    auto bytes = readFile(dataRoot() / "3.1/ToasterAll/EFFECTS/FlyIn");
    auto forms = vt::unwrapHunk(bytes);
    REQUIRE(std::ranges::any_of(forms, [](auto& f) { return f.type == std::array{'A', 'N', 'I', 'M'}; }));
}

TEST_CASE("crouton exposes its FORMs and chunks; missing file is nullopt") {
    auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/2000FX/Animals/Bear-L");
    REQUIRE(c);
    REQUIRE(std::ranges::any_of(c->forms, [](auto& f) { return f.type == std::array{'A', 'N', 'I', 'M'}; }));
    REQUIRE(std::ranges::any_of(c->forms, [](auto& f) { return f.type == std::array{'C', 'r', 'U', 'D'}; }));
    for (auto& f : c->forms) {
        if (f.type != std::array{'C', 'r', 'U', 'D'}) continue;
        auto ch = vt::formChunks(c->bytes, f);
        REQUIRE(std::ranges::any_of(ch, [](auto& k) { return k.id == std::array{'T', 'A', 'G', 'S'}; }));
    }
    REQUIRE(!vt::loadCrouton(dataRoot() / "4.2/does-not-exist"));
}

TEST_CASE("iffForms pads chunks to even length and tolerates truncation") {
    // FORM(4+1 pad? no) : a 3-byte JUNK chunk (padded) followed by a FORM TEST.
    std::vector<uint8_t> d = {'J', 'U', 'N', 'K', 0, 0, 0, 3, 1, 2, 3, 0,
                              'F', 'O', 'R', 'M', 0, 0, 0, 4, 'T', 'E', 'S', 'T'};
    auto f = vt::iffForms(d);
    REQUIRE(f.size() == 1);
    REQUIRE(f[0].offset == 12);
    REQUIRE(f[0].size == 4);
    REQUIRE(f[0].type == std::array{'T', 'E', 'S', 'T'});
    d.resize(18);  // truncated header: must not crash or invent forms
    REQUIRE(vt::iffForms(d).empty());
    REQUIRE(vt::unwrapHunk(d).empty());
}
