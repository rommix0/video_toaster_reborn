// ALGO VM: every shipped program against the Python reference (tools/ref/algo_ref.py, golden
// algo.jsonl), the T-bar / Auto t mappings and the routing step switch.
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/algo.h"
#include "vt/background.h"
#include "vt/crc32.h"
#include "vt/crouton.h"
#include "vt/dve.h"
#include "vt/effect.h"
#include "vt/switcher.h"

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
        size_t pos = 0, k = 0;  // frame counters within the segment
        for (const auto& f : std::get<std::vector<std::map<std::string, int64_t>>>(r.at("frames").v)) {
            if (f.at("seg") != seg) {  // each segment starts from a fresh VM
                seg = f.at("seg");
                pos = k = 0;
                vm.emplace(blob, kind, 5);
                REQUIRE(vm->flags() == uint32_t(num(r, "flags")));
                REQUIRE(int(vm->kind()) == kind);
            }
            INFO("seg " << seg << " t " << f.at("t"));
            // make_golden: segment 0 is the T-bar run 7, 14 .. 511 then 500, 489 .. 5; segment 1 is Auto at 30 ticks
            if (seg == 0) {
                const int tbar = pos < 73 ? int(7 * (pos + 1)) : int(500 - 11 * (pos - 73));
                REQUIRE(uint32_t(f.at("t")) == vt::algoTBarT(tbar, vm->flags()));
            } else {
                REQUIRE(uint32_t(f.at("t")) == vt::algoAutoT(int(k + 1), 30, vm->flags()));
            }
            ++pos;
            ++k;
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

TEST_CASE("Bar programs: channel 29 words give bars_ref.py's picture offsets", "[algo][bar][golden]") {
    for (const auto& r : golden::readLines("bars.jsonl")) {
        INFO(str(r, "source"));
        const auto c = vt::loadCrouton(golden::sourcePath(str(r, "source")));
        REQUIRE(c);
        const auto& prog = c->blobs.at(0x1000001C);
        const auto& tbars = std::get<std::vector<int64_t>>(r.at("tbar").v);
        for (size_t i = 0; i < tbars.size(); ++i) {
            const int pos = int(tbars[i]);
            const uint32_t t = pos >= 511 ? 0xffff : pos == 0 ? 1 : uint32_t(pos) << 7;  // BARS.md 5.1
            const auto w = vt::algoChannelWords(prog, 29, t);
            REQUIRE(w);
            REQUIRE(int16_t((0x17e - (*w)[0]) * 2 + (*w)[2]) == std::get<std::vector<int64_t>>(r.at("x").v)[i]);
            REQUIRE(int16_t(0xf1 - (*w)[1] + (*w)[3]) == std::get<std::vector<int64_t>>(r.at("y").v)[i]);
        }
        REQUIRE_FALSE(vt::algoChannelWords(prog, 28, 0));
    }
    const std::vector<uint8_t> op15{0, 29, 0, 15, 0, 0, 0, 0};
    REQUIRE_THROWS_AS(vt::algoChannelWords(op15, 29, 0), std::runtime_error);
    REQUIRE_THROWS_AS(vt::algoChannelWords(std::vector<uint8_t>{0, 29, 0, 17}, 29, 0), std::runtime_error);
}

namespace {
vt::Frame solidFrame(vt::Rgba8 c) {
    vt::Frame f;
    std::fill(f.px.begin(), f.px.end(), c);
    return f;
}
const vt::Rgba8 kA{10, 20, 30, 255}, kB{200, 150, 100, 255};
}  // namespace

TEST_CASE("algo control follows the ranges", "[algo]") {
    vt::AlgoStep s;
    s.h.fill(0x50);
    for (size_t k = 0; k < s.v.size(); ++k) s.v[k] = uint8_t(k * 3 + 1);
    s.v[7] = 0xfc;
    s.cellLo = 10;
    s.cellHi = 20;
    s.lineLo = 5;
    s.lineHi = 6;
    const vt::DveControl c = vt::algoControl(s);
    for (int k = 0; k < vt::kDveLines; ++k) {
        REQUIRE(c.line[size_t(k)] == s.v[size_t(k)]);
        for (int cell = 0; cell < vt::kDveCells; ++cell)
            REQUIRE(c.key[size_t(k) * 192 + size_t(cell)] == ((k == 5 || k == 6) && cell >= 10 && cell <= 20 ? 255 - 0x50 : 0));
    }
    REQUIRE(c.line[7] == vt::kDveColourLine);
    REQUIRE(c.art.empty());
    s.lineLo = -1;  // no line shows the row
    for (uint8_t k : vt::algoControl(s).key) REQUIRE(k == 0);
}

TEST_CASE("algo wipe rows", "[algo]") {
    const vt::Frame a = solidFrame(kA), b = solidFrame(kB);
    vt::AlgoStep s;
    s.spans = {{100, 199}};
    s.ink = 1;
    s.pixelLo = 0;
    s.pixelHi = 735;
    s.bandLo = 50;
    s.bandHi = 60;
    s.bandInk = 1;
    vt::Frame out;
    vt::renderAlgoWipe(s, a, b, out);
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x)
            REQUIRE(out.at(x, y) == (y >= 50 && y <= 60 && x >= 100 && x <= 199 ? kB : kA));
    s.ink = 0;
    vt::renderAlgoWipe(s, a, b, out);
    for (int y = 0; y < vt::kRasterH; ++y)
        for (int x = 0; x < vt::kRasterW; ++x)
            REQUIRE(out.at(x, y) == (y >= 50 && y <= 60 && x <= 735 && !(x >= 100 && x <= 199) ? kB : kA));
    // flag 0x80: the constant row bandInk ^ 1 inside the band (row 0 = all B for bandInk 1), the span row outside
    s.ink = 1;
    s.inverse = true;
    vt::renderAlgoWipe(s, a, b, out);
    for (int y : {0, 55, 481}) {
        REQUIRE(out.at(150, y) == kB);
        REQUIRE(out.at(50, y) == (y == 55 ? kB : kA));
    }
    s.bandInk = 0;  // row 1 = all A inside the band
    vt::renderAlgoWipe(s, a, b, out);
    REQUIRE(out.at(150, 55) == kA);
    REQUIRE(out.at(150, 0) == kB);
}

TEST_CASE("mix level", "[algo]") {
    const vt::Rgba8 p{0, 100, 200, 7}, q{255, 50, 10, 9};
    const vt::Frame in0 = solidFrame(p), in1 = solidFrame(q);
    vt::Frame out;
    vt::mixLevel(in0, in1, 0, out);
    REQUIRE(out.at(5, 5) == vt::Rgba8{0, 100, 200, 255});
    vt::mixLevel(in0, in1, 255, out);
    REQUIRE(out.at(5, 5) == vt::Rgba8{255, 50, 10, 255});
    vt::mixLevel(in0, in1, 128, out);
    REQUIRE(out.at(5, 5) == vt::Rgba8{128, 75, 105, 255});
    vt::mixLevel(in0, in1, 999, out);  // clamped
    REQUIRE(out.at(5, 5) == vt::Rgba8{255, 50, 10, 255});
    // Ruling 1: out may alias either input
    vt::Frame x0 = in0, x1 = in1, ref;
    vt::mixLevel(in0, in1, 77, ref);
    vt::mixLevel(x0, in1, 77, x0);
    REQUIRE(x0.px == ref.px);
    vt::mixLevel(in0, x1, 77, x1);
    REQUIRE(x1.px == ref.px);
}

TEST_CASE("border fill for custom and snow colours", "[algo]") {
    REQUIRE(vt::algoColourIndex(vt::EffectColour::of(vt::BkgColour::Yellow)) == 5);
    REQUIRE(vt::algoColourIndex(vt::EffectColour::of(vt::BkgColour::Black)) == 0);
    REQUIRE(vt::algoColourIndex(vt::EffectColour::of(vt::BkgColour::Cyan)) == 7);
    REQUIRE(vt::algoColourIndex(vt::EffectColour::customWord(0x1234)) == 0);
    REQUIRE(vt::algoColourIndex(vt::EffectColour::of(vt::BkgColour::Snow)) == 0);
    REQUIRE(vt::algoBorderFill(5) == 21);  // Yellow: 255 - YIQ byte 0xea (ALGO.md 5.4)
    REQUIRE(vt::algoBorderFill(0) == 23);
    REQUIRE(vt::algoBorderFill(-1) == 23);
    REQUIRE(vt::algoBorderFill(8) == 23);
}

namespace {
// Every distinct loadable digital program of the golden list (one record per program).
struct DigitalProgram {
    std::string path;
    std::vector<uint8_t> blob;
    int kind;
};
std::vector<DigitalProgram> digitalPrograms() {
    std::vector<DigitalProgram> out;
    for (const auto& r : golden::readLines("algo.jsonl")) {
        const int kind = int(num(r, "kind"));
        if (kind != 0 && kind != 3) continue;
        const auto c = vt::loadCrouton(golden::sourcePath(str(r, "path")));
        REQUIRE(c);
        out.push_back({str(r, "path"), c->blobs.at(0x1000001C), kind});
    }
    return out;
}
}  // namespace

TEST_CASE("border fill matches the border cells the VM draws", "[algo]") {
    // Push On Border L-R (tag 0x80000064): its left/right border cells carry the Effect Color
    // (c1[0x16] = kYiq0[i]); the top/bottom border fill must be the same key.
    const auto path = dataRoot() / "4.2/Effects/4000FX/Push-Pull/Push On Border L-R";
    const auto c = vt::loadCrouton(path);
    REQUIRE(c);
    REQUIRE(c->tag(0x80000064).has_value());
    for (int i = 0; i < 8; ++i) {
        INFO("colour " << i);
        vt::AlgoVm vm(c->blobs.at(0x1000001C), int(c->tag(0x80000063).value_or(0)), i);
        std::set<int> keys;
        for (int pos = 0; pos <= 255; pos += 5) {
            const vt::DveControl ctl = vt::algoControl(vm.step(vt::algoTBarT(pos, vm.flags())));
            for (uint8_t k : ctl.key)
                if (k >= 16 && k <= 23) keys.insert(k);
        }
        REQUIRE(keys.count(vt::algoBorderFill(i)) == 1);
    }
    REQUIRE(vt::algoBorderFill(5) == 21);
}

TEST_CASE("algo digital library renders with no unknown codes", "[algo]") {
    const vt::Frame a = solidFrame(kA), b = solidFrame(kB);
    vt::Frame out;
    for (const auto& p : digitalPrograms()) {
        INFO(p.path);
        for (int pos : {0, 256, 511}) {
            vt::AlgoVm vm(p.blob, p.kind, 5);  // a fresh take each
            vt::DveControl ctl = vt::algoControl(vm.step(vt::algoTBarT(pos, vm.flags())));
            ctl.borderFill = uint8_t(vt::algoBorderFill(5));
            INFO("tbar " << pos);
            REQUIRE(vt::renderDve(ctl, a, b, {}, out, {}) == 0);
        }
    }
}

// --- the Positionable moves (tools/re/POSITION.md) ----------------------------------------------

namespace {
const char* const kPositionable[] = {"Positionable Expand", "Positionable Shrink", "Positionable WIPE IN",
                                     "Positionable WIPE OUT"};
std::vector<uint8_t> positionablePayload(const char* name) {
    const auto c = vt::loadCrouton(dataRoot() / "4.2/Effects/4000FX/Special" / name);
    REQUIRE(c);
    return c->blobs.at(0x1000001C);
}
}  // namespace

TEST_CASE("Positionable header and mouse steps (0x5e2e8)", "[algo][position]") {
    for (const char* name : kPositionable) {
        INFO(name);
        const auto r = vt::algoPositionRange(positionablePayload(name));
        REQUIRE(r);
        const bool wipe = std::string(name).find("WIPE") != std::string::npos;
        REQUIRE(r->accel == (wipe ? 4096 : -16384));
        REQUIRE(r->halfH == (wipe ? 736 : 185));
        REQUIRE(r->halfV == (wipe ? 481 : 237));
        REQUIRE(r->flags == (std::string(name).find("Expand") != std::string::npos ||
                                     std::string(name).find("OUT") != std::string::npos
                                 ? 4u
                                 : 0u));
    }
    REQUIRE_FALSE(vt::algoPositionRange(std::vector<uint8_t>(87)));  // no room for the header
    auto bad = positionablePayload("Positionable Shrink");
    bad[0x10] = 0x7f;  // a parameter pointer past the payload
    REQUIRE_FALSE(vt::algoPositionRange(bad));
    // position_ref.py check(): kind 3 half a unit per count, kind 4 accelerating
    const std::pair<int, int> half[] = {{1, 1}, {2, 1}, {3, 2}, {8, 4}, {-1, -1}, {-3, -2}, {0, 0}};
    for (auto [d, want] : half) REQUIRE(vt::algoMouseStep(d, -16384) == want);
    const std::pair<int, int> accel[] = {{1, 1}, {2, 2}, {3, 4}, {4, 6}, {8, 16}, {16, 48}, {-3, -4}, {0, 0}};
    for (auto [d, want] : accel) REQUIRE(vt::algoMouseStep(d, 4096) == want);
    REQUIRE(vt::algoMouseStep(-7, -32768) == -7);
}

TEST_CASE("Positionable moves match position_ref.py (position.jsonl)", "[algo][position][golden]") {
    const vt::Frame a = solidFrame({0x1B, 0x2A, 0x80, 255}), b = solidFrame({0xB5, 0x12, 0x2E, 255});
    auto crc = [](const vt::Frame& f) {
        return vt::crc32({reinterpret_cast<const uint8_t*>(f.px.data()), f.px.size() * sizeof(vt::Rgba8)});
    };
    const auto recs = golden::readLines("position.jsonl");
    REQUIRE(recs.size() == 32);
    uint64_t take = 0;
    for (const auto& r : recs) {
        const auto& moves = std::get<std::vector<int64_t>>(r.at("moves").v);
        const int tbar = int(num(r, "tbar")), kind = int(num(r, "kind"));
        const bool bake = num(r, "bake") != 0;
        INFO(str(r, "source") << ", " << moves.size() / 2 << " moves, drop " << tbar << ", bake " << bake);
        std::string why;
        const std::shared_ptr<const vt::Effect> e = vt::loadEffect(golden::sourcePath(str(r, "source")), &why);
        INFO(why);
        REQUIRE(e);
        REQUIRE(e->positionRange());
        // The Switcher: a drag to the drop point, the mouse loop from the centre, Space.
        vt::Switcher sw;
        REQUIRE(sw.setEffect(e));
        sw.setTBar(tbar);
        sw.releaseTBar();
        REQUIRE(sw.positioning());
        for (size_t i = 0; i + 1 < moves.size(); i += 2) sw.movePosition(int(moves[i]), int(moves[i + 1]));
        REQUIRE(sw.position().h == num(r, "h"));
        REQUIRE(sw.position().v == num(r, "v"));
        if (bake) {
            sw.endPositioning(vt::PositionEnd::KeepAndStop);
            REQUIRE(sw.position().bakedAt == tbar);
            REQUIRE(sw.position().stop == uint32_t(num(r, "stop")));
            REQUIRE(sw.tbar() == std::clamp(int(num(r, "ec")), 1, 510));  // a stop at an end stays in the take
        }
        auto payload = vt::loadCrouton(golden::sourcePath(str(r, "source")))->blobs.at(0x1000001C);
        vt::algoPositionProgram(payload, sw.position().h, sw.position().v, sw.position().bakedAt);
        REQUIRE(vt::crc32(payload) == uint32_t(num(r, "payload")));
        for (const auto& f : std::get<std::vector<std::map<std::string, int64_t>>>(r.at("frames").v)) {
            const int pos = int(f.at("tbar"));
            INFO("T-bar " << pos);
            vt::AlgoVm vm(payload, kind, 0);  // a fresh take, one step
            const vt::AlgoStep& s = vm.step(vt::algoTBarT(pos, vm.flags()));
            if (kind == 3) {
                REQUIRE(vt::crc32(s.h) == uint32_t(f.at("h")));
                REQUIRE(vt::crc32(s.v) == uint32_t(f.at("v")));
                REQUIRE(s.cellLo == f.at("c0"));
                REQUIRE(s.cellHi == f.at("c1"));
                REQUIRE(s.lineLo == f.at("l0"));
                REQUIRE(s.lineHi == f.at("l1"));
            } else {
                REQUIRE(spansCrc(s.spans) == uint32_t(f.at("s")));
                REQUIRE(s.pixelLo == f.at("p0"));
                REQUIRE(s.pixelHi == f.at("p1"));
                REQUIRE(s.ink == f.at("ink"));
                REQUIRE(s.bandLo == f.at("b0"));
                REQUIRE(s.bandHi == f.at("b1"));
                REQUIRE(s.bandInk == f.at("vink"));
                // The frame the effect renders, positioned through RenderCtx.
                vt::RenderCtx ctx;
                ctx.take = ++take;
                ctx.position = sw.position();
                vt::Frame out;
                e->render(pos, a, b, out, ctx);
                REQUIRE(crc(out) == uint32_t(f.at("frame")));
            }
        }
    }
}

TEST_CASE("Positionable programs step at any anchor, baked or not", "[algo][position]") {
    for (const char* name : kPositionable) {
        INFO(name);
        const auto shipped = positionablePayload(name);
        const auto r = vt::algoPositionRange(shipped);
        REQUIRE(r);
        const int kind = std::string(name).find("WIPE") != std::string::npos ? 4 : 3;
        for (int h : {-r->halfH, -r->halfH / 2, 0, r->halfH / 3, r->halfH - 1})
            for (int v : {-r->halfV, -1, r->halfV / 2, r->halfV - 1})
                for (int bakedAt : {-1, 1, 200, 510}) {
                    INFO(h << ", " << v << " baked at " << bakedAt);
                    auto p = shipped;
                    vt::algoPositionProgram(p, h, v, bakedAt);
                    vt::AlgoVm vm(p, kind, 0);
                    for (int pos = 0; pos <= 511; pos += 7) REQUIRE_NOTHROW(vm.step(vt::algoTBarT(pos, vm.flags())));
                }
        auto p = shipped;  // the centred defaults are the shipped program (POSITION.md 4)
        vt::algoPositionProgram(p, 0, 0, -1);
        REQUIRE(p == shipped);
    }
}
