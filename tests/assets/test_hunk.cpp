#include <stdexcept>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/assets/hunk.h"
#include "vt/crc32.h"

namespace {

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

}  // namespace

TEST_CASE("hunk images match the Python loader") {
    const auto recs = golden::readLines("hunk.jsonl");
    REQUIRE(recs.size() == 4);
    for (const auto& rec : recs) {
        INFO(str(rec, "file"));
        auto img = vt::assets::loadHunks(readFile(golden::sourcePath(str(rec, "file"))));
        const auto& bases = arr(rec, "bases");
        const auto& sizes = arr(rec, "sizes");
        const auto& kinds = arr(rec, "kinds");
        REQUIRE(img.hunks.size() == size_t(num(rec, "n_hunks")));
        REQUIRE(bases.size() == img.hunks.size());
        for (size_t i = 0; i < img.hunks.size(); ++i) {
            CHECK(img.hunks[i].base == uint32_t(bases[i]));
            CHECK(img.hunks[i].size == uint32_t(sizes[i]));
            CHECK(int(img.hunks[i].kind) == kinds[i]);
        }
        CHECK(vt::crc32(img.flat) == uint32_t(num(rec, "flat_crc32")));
    }
}

TEST_CASE("a small hunk file: layout, memory flags, BSS and relocations") {
    // Hunk 0: CODE (chip) holding a pointer into hunk 1; hunk 1: BSS 8 bytes; hunk 2: DATA (fast).
    std::vector<uint8_t> f;
    for (uint32_t w : {0x3F3u, 0u, 3u, 0u, 2u, 0x40000002u, 2u, 0x80000001u})
        put32(f, w);
    for (uint32_t w : {0x3E9u, 2u, 0u, 0x10u,                     // CODE: word 0 = 0, word 1 = 0x10
                       0x3ECu, 1u, 2u, 4u, 0u,                    // RELOC32: word at +4 += base of hunk 2
                       0x3F2u, 0x3EBu, 2u, 0x3F2u,                // END, BSS (2 words), END
                       0x3EAu, 1u, 0xAABBCCDDu, 0x3F0u, 1u, 0x41424344u, 0u, 0u, 0x3F2u})  // DATA + SYMBOL + END
        put32(f, w);
    auto img = vt::assets::loadHunks(f);
    REQUIRE(img.hunks.size() == 3);
    CHECK(img.hunks[0].base == 0x1000);
    CHECK(img.hunks[0].size == 8);
    CHECK(img.hunks[0].kind == vt::assets::HunkImage::Hunk::Code);
    CHECK(img.hunks[0].chip);
    CHECK(img.hunks[1].base == 0x3000);
    CHECK(img.hunks[1].kind == vt::assets::HunkImage::Hunk::Bss);
    CHECK_FALSE(img.hunks[1].chip);
    CHECK(img.hunks[2].base == 0x5000);
    CHECK(img.hunks[2].kind == vt::assets::HunkImage::Hunk::Data);
    CHECK_FALSE(img.hunks[2].chip);
    CHECK(img.flat.size() == 0x7000);
    CHECK(img.be32(0x1004) == 0x10 + 0x5000);
    CHECK(img.be32(0x5000) == 0xAABBCCDD);
    CHECK(img.be16(0x5002) == 0xCCDD);
}

TEST_CASE("truncated and non-hunk files throw") {
    const auto exe = readFile(dataRoot() / "4.2/Programs/switcher.main");
    REQUIRE(exe.size() > 1000);
    CHECK_THROWS_AS(vt::assets::loadHunks(std::span<const uint8_t>(exe.data(), 1000)), std::runtime_error);
    CHECK_THROWS_AS(vt::assets::loadHunks(std::span<const uint8_t>(exe.data(), exe.size() / 2)), std::runtime_error);
    CHECK_THROWS_AS(vt::assets::loadHunks({}), std::runtime_error);

    const auto ilbm = readFile(dataRoot() / "4.2/Effects/2000FX/Border/Wipe Border Diamonds");
    CHECK_THROWS_AS(vt::assets::loadHunks(ilbm), std::runtime_error);

    // A relocation that points outside the image is rejected, not applied.
    std::vector<uint8_t> f;
    for (uint32_t w : {0x3F3u, 0u, 1u, 0u, 0u, 1u, 0x3E9u, 1u, 0u, 0x3ECu, 1u, 7u, 0u, 0u})
        put32(f, w);
    CHECK_THROWS_AS(vt::assets::loadHunks(f), std::runtime_error);
}

TEST_CASE("be32 and be16 are bounds-checked") {
    const auto img = vt::assets::loadHunks(readFile(dataRoot() / "4.2/Programs/System_Support/Fonts/CGInterface/14.4C"));
    const auto n = uint32_t(img.flat.size());
    CHECK_NOTHROW(img.be32(n - 4));
    CHECK_THROWS_AS(img.be32(n - 3), std::out_of_range);
    CHECK_THROWS_AS(img.be32(n), std::out_of_range);
    CHECK_THROWS_AS(img.be32(0xFFFFFFFFu), std::out_of_range);
    CHECK_NOTHROW(img.be16(n - 2));
    CHECK_THROWS_AS(img.be16(n - 1), std::out_of_range);
}
