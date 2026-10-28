// ALGO VM: every shipped program against the Python reference (tools/ref/algo_ref.py, golden
// algo.jsonl), the T-bar / Auto t mappings and the routing step switch.
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/algo.h"
#include "vt/crc32.h"
#include "vt/crouton.h"

namespace {

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }

// make_golden.algo_frame: the spans packed as big-endian signed 16-bit pairs.
uint32_t spansCrc(const std::vector<std::pair<int, int>>& spans) {
    std::vector<uint8_t> b;
    for (const auto& [a, e] : spans)
        for (int v : {a, e}) {
            b.push_back(uint8_t(v >> 8));
            b.push_back(uint8_t(v));
        }
    return vt::crc32(b);
}

}  // namespace

TEST_CASE("algo VM matches the reference", "[algo][golden]") {
    const auto recs = golden::readLines("algo.jsonl");
    REQUIRE(recs.size() == 114);
    for (const auto& r : recs) {
        const std::string& path = str(r, "path");
        INFO(path);
        const auto crouton = vt::loadCrouton(golden::sourcePath(path));
        REQUIRE(crouton);
        const auto& blob = crouton->blobs.at(0x1000001C);
        const int kind = int(num(r, "kind"));
        std::optional<vt::AlgoVm> vm;
        int64_t seg = -1;
        for (const auto& f : std::get<std::vector<std::map<std::string, int64_t>>>(r.at("frames").v)) {
            if (f.at("seg") != seg) {  // each segment starts from a fresh VM
                seg = f.at("seg");
                vm.emplace(blob, kind, 5);
                REQUIRE(vm->flags() == uint32_t(num(r, "flags")));
                REQUIRE(int(vm->kind()) == kind);
            }
            INFO("seg " << seg << " t " << f.at("t"));
            const vt::AlgoStep& s = vm->step(uint32_t(f.at("t")));
            if (kind == 0 || kind == 3) {
                REQUIRE(vt::crc32(s.h) == uint32_t(f.at("h")));
                REQUIRE(vt::crc32(s.v) == uint32_t(f.at("v")));
                REQUIRE(s.cellLo == f.at("c0"));
                REQUIRE(s.cellHi == f.at("c1"));
                REQUIRE(s.lineLo == f.at("l0"));
                REQUIRE(s.lineHi == f.at("l1"));
            } else if (kind == 1 || kind == 4) {
                REQUIRE(spansCrc(s.spans) == uint32_t(f.at("s")));
                REQUIRE(s.pixelLo == f.at("p0"));
                REQUIRE(s.pixelHi == f.at("p1"));
                REQUIRE(s.ink == f.at("ink"));
                REQUIRE(s.bandLo == f.at("b0"));
                REQUIRE(s.bandHi == f.at("b1"));
                REQUIRE(s.bandInk == f.at("vink"));
                REQUIRE(s.inverse == ((vm->flags() & 0x80) != 0));
            }
            REQUIRE(s.level == f.at("lv"));
        }
    }
}

TEST_CASE("algo T-bar and Auto t", "[algo]") {
    REQUIRE(vt::algoTBarT(511, 0) == 0x10000);
    REQUIRE(vt::algoTBarT(256, 0) == (256u << 16) / 511);
    REQUIRE(vt::algoTBarT(300, 8) == vt::algoTBarT(1022 - 600, 0));
    REQUIRE(vt::algoAutoT(30, 30, 0) == 30 * (0x10000 / 30));
    REQUIRE(vt::algoAutoT(0, 30, 0) == vt::algoAutoT(1, 30, 0));
    REQUIRE(vt::algoAutoT(99, 30, 0) == vt::algoAutoT(30, 30, 0));
}

TEST_CASE("algo second step", "[algo]") {
    REQUIRE(!vt::algoSecondStep(255, -1, 30, 0));
    REQUIRE(vt::algoSecondStep(256, -1, 30, 0));
    REQUIRE(!vt::algoSecondStep(0, 15, 30, 0));  // 15 * 2184 = 32760 < 0x8000
    REQUIRE(vt::algoSecondStep(0, 16, 30, 0));
    REQUIRE(!vt::algoSecondStep(0, 15, 30, 8));
    REQUIRE(vt::algoSecondStep(0, 16, 30, 8));
}

TEST_CASE("algo VM refuses what it cannot play", "[algo]") {
    const std::vector<uint8_t> empty{0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0};  // no channels: playable
    REQUIRE_NOTHROW(vt::AlgoVm(empty, 0, 0));
    REQUIRE_THROWS_AS(vt::AlgoVm(empty, 5, 0), std::runtime_error);
    std::vector<uint8_t> op15{0, 0, 0, 10, 0, 0, 0, 0, 0, 1, 0, 1, 0, 15};
    op15.resize(op15.size() + 20, 0);
    REQUIRE_THROWS_AS(vt::AlgoVm({}, 0, 0), std::runtime_error);
    REQUIRE_THROWS_AS(vt::AlgoVm(op15, 0, 0), std::runtime_error);
}
