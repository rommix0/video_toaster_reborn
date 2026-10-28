#include "catch_amalgamated.hpp"
#include "golden.h"

#include <algorithm>
#include <map>
#include <string>

namespace {

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

}  // namespace

TEST_CASE("golden files have the expected record counts") {
    REQUIRE(golden::readLines("ilbm.jsonl").size() == 113);
    REQUIRE(golden::readLines("anims.jsonl").size() == 354);
    REQUIRE(golden::readLines("mixtables.jsonl").size() == 290);
    REQUIRE(golden::readLines("dve.jsonl").size() == 64);
    REQUIRE(golden::readLines("timing.jsonl").size() == 467);
    REQUIRE(golden::readLines("chromafx.jsonl").size() == 96 + 396);
    REQUIRE(golden::readLines("algo.jsonl").size() == 114);
}

TEST_CASE("golden records parse into all value kinds") {
    const auto ilbm = golden::readLines("ilbm.jsonl");
    for (const auto& r : ilbm) {
        REQUIRE(num(r, "w") > 0);
        REQUIRE(arr(r, "gray_lut").size() == (size_t{1} << num(r, "planes")));
        const auto bytes = readFile(golden::sourcePath(str(r, "source")));
        REQUIRE(!bytes.empty());
    }

    const auto dve = golden::readLines("dve.jsonl");
    const auto& frames = std::get<std::vector<std::map<std::string, int64_t>>>(dve.at(0).at("frames").v);
    REQUIRE(frames.size() > 1);
    REQUIRE(frames[0].count("key_crc") == 1);

    const auto chroma = golden::readLines("chromafx.jsonl");
    const auto& tags = std::get<std::map<std::string, int64_t>>(chroma.at(0).at("tags").v);
    REQUIRE(tags.at("0x80000080") == 1);  // BW Poster is a Transition-mode crouton
    REQUIRE(arr(chroma.at(0), "tables_crc").size() == 3);
    REQUIRE(str(chroma.at(0), "name") == "B&W Poster");
}

TEST_CASE("golden timing matches the crouton defaults") {
    for (const auto& r : golden::readLines("timing.jsonl")) {
        const auto& fields = arr(r, "fields");
        const auto& dur = arr(r, "duration");
        REQUIRE(dur.size() == 4);  // slow, medium, fast, variable
        const auto& sp = str(r, "speed_default");
        REQUIRE((sp == "slow" || sp == "medium" || sp == "fast" || sp == "variable"));
        if (!fields.empty()) {
            REQUIRE(fields.size() == 4);
            for (size_t i = 0; i < 4; ++i) REQUIRE(dur[i] == (fields[i] * num(r, "frames") + 1) / 2);
        }
    }
}
