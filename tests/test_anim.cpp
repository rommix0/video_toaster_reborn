#include <array>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/anim.h"
#include "vt/crc32.h"
#include "vt/crouton.h"

namespace {

constexpr std::array<char, 4> kAnim{'A', 'N', 'I', 'M'};

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }
const std::vector<int64_t>& arr(const golden::Record& r, const char* key) {
    return std::get<std::vector<int64_t>>(r.at(key).v);
}

// The first FORM ANIM, the same choice as tools/make_golden.py.
struct Loaded {
    vt::Crouton c;
    vt::Form form;
};
Loaded openAnim(const std::string& source) {
    auto c = vt::loadCrouton(golden::sourcePath(source));
    REQUIRE(c);
    for (auto& f : c->forms)
        if (f.type == kAnim) {
            vt::Form form = f;
            return {std::move(*c), form};
        }
    FAIL("no FORM ANIM in " << source);
    throw std::logic_error("unreachable");
}

const golden::Record& findRecord(const std::vector<golden::Record>& recs, const std::string& source) {
    for (auto& r : recs)
        if (str(r, "source") == source) return r;
    FAIL(source << " is not in anims.jsonl");
    throw std::logic_error("unreachable");
}

// Decode one golden record and compare everything it records.
void checkRecord(const golden::Record& r) {
    const std::string& src = str(r, "source");
    INFO(src);
    auto [c, form] = openAnim(src);
    vt::Anim a = vt::readAnim(c.bytes, form);

    const auto& planeCrcs = arr(r, "plane_crcs");
    REQUIRE(a.bm.w == num(r, "w"));
    REQUIRE(a.bm.h == num(r, "h"));
    REQUIRE(a.bm.planes == num(r, "planes"));
    REQUIRE(a.dropped == num(r, "dropped"));
    REQUIRE(a.planes.size() == planeCrcs.size());
    REQUIRE(a.reltimes.size() == planeCrcs.size());
    REQUIRE(a.cmaps.size() == planeCrcs.size());
    for (size_t i = 0; i < planeCrcs.size(); ++i) {
        INFO("frame " << i);
        REQUIRE(vt::crc32(a.planes[i].bytes) == uint32_t(planeCrcs[i]));
    }
    if (a.bm.planes > 8) {
        // Ruling 6: the golden frame CRCs of the 9-plane DVE are uint16 little-endian; our
        // IndexImage is one byte per pixel, so frames is left empty and only planes is compared.
        REQUIRE(a.frames.empty());
        return;
    }
    const auto& frameCrcs = arr(r, "frame_crcs");
    REQUIRE(a.frames.size() == frameCrcs.size());
    for (size_t i = 0; i < frameCrcs.size(); ++i) {
        INFO("frame " << i);
        REQUIRE(a.frames[i].w == a.bm.w);
        REQUIRE(a.frames[i].h == a.bm.h);
        REQUIRE(vt::crc32(a.frames[i].px) == uint32_t(frameCrcs[i]));
    }
}

// ---- tiny IFF builders ----
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
void putTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
void putChunk(std::vector<uint8_t>& v, const char* id, const std::vector<uint8_t>& data) {
    putTag(v, id);
    put32(v, uint32_t(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    if (data.size() & 1) v.push_back(0);
}
std::vector<uint8_t> makeForm(const char* type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f;
    putTag(f, "FORM");
    put32(f, uint32_t(4 + body.size()));
    putTag(f, type);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}
std::vector<uint8_t> bmhd(int w, int h, int planes) {
    std::vector<uint8_t> b(20, 0);
    b[0] = uint8_t(w >> 8);
    b[1] = uint8_t(w);
    b[2] = uint8_t(h >> 8);
    b[3] = uint8_t(h);
    b[8] = uint8_t(planes);
    return b;
}
std::vector<uint8_t> anhd(int op, uint32_t reltime, int interleave, uint32_t bits) {
    std::vector<uint8_t> a(40, 0);
    a[0] = uint8_t(op);
    for (int k = 0; k < 4; ++k) {
        a[14 + k] = uint8_t(reltime >> (24 - 8 * k));
        a[20 + k] = uint8_t(bits >> (24 - 8 * k));
    }
    a[18] = uint8_t(interleave);
    return a;
}
// First frame: 16x2, 1 plane, uncompressed (2 rows of 2 bytes).
std::vector<uint8_t> firstFrame(const std::vector<uint8_t>& body4) {
    std::vector<uint8_t> b;
    putChunk(b, "BMHD", bmhd(16, 2, 1));
    putChunk(b, "BODY", body4);
    return makeForm("ILBM", b);
}
std::vector<uint8_t> deltaFrame(int op, const std::vector<uint8_t>& dlta, uint32_t bits = 0, int interleave = 1) {
    std::vector<uint8_t> b;
    putChunk(b, "ANHD", anhd(op, 3, interleave, bits));
    putChunk(b, "DLTA", dlta);
    return makeForm("ILBM", b);
}
std::vector<uint8_t> makeAnim(const std::vector<std::vector<uint8_t>>& subs) {
    std::vector<uint8_t> body;
    for (auto& s : subs) body.insert(body.end(), s.begin(), s.end());
    return makeForm("ANIM", body);
}
vt::Anim decode(const std::vector<uint8_t>& file) {
    auto forms = vt::iffForms(file);
    REQUIRE(forms.size() == 1);
    return vt::readAnim(file, forms[0]);
}
// plane 0 of frame k as bytes
std::vector<uint8_t> plane0(const vt::Anim& a, size_t k) {
    const auto& p = a.planes[k];
    return {p.plane(0), p.plane(0) + size_t(p.h) * size_t(p.rowbytes)};
}
// op-5 DLTA: 16 u32 pointers (plane 0 only) followed by the column streams.
std::vector<uint8_t> dlta5(const std::vector<uint8_t>& stream) {
    std::vector<uint8_t> d;
    put32(d, 64);
    for (int k = 1; k < 16; ++k) put32(d, 0);
    d.insert(d.end(), stream.begin(), stream.end());
    return d;
}

}  // namespace

TEST_CASE("every golden ANIM and DVE effect decodes to its frame and plane CRCs") {
    auto recs = golden::readLines("anims.jsonl");
    REQUIRE(recs.size() == 345);
    size_t checked = 0;
    for (auto& r : recs) {
        checkRecord(r);
        ++checked;
    }
    REQUIRE(checked == 345);
    WARN("ANIM golden records checked: " << checked);
}

TEST_CASE("Warp To Globe ! (9 planes) keeps complete planes and no byte frames") {
    auto recs = golden::readLines("anims.jsonl");
    const auto& r = findRecord(recs, "4.2/Effects/4000FX/Digital/Warp To Globe !");
    REQUIRE(num(r, "planes") == 9);
    checkRecord(r);
}

TEST_CASE("op-5 byte vertical delta: fill, literal and skip runs per column") {
    // 16x2, one plane: rows {AA, BB} / {CC, DD}. Column 0: fill both rows with 0x11.
    // Column 1: skip one row, then one literal byte 0x77.
    std::vector<uint8_t> stream = {1, 0, 2, 0x11, 2, 1, 0x81, 0x77};
    auto file = makeAnim({firstFrame({0xAA, 0xBB, 0xCC, 0xDD}), deltaFrame(5, dlta5(stream))});
    auto a = decode(file);
    REQUIRE(a.planes.size() == 2);
    REQUIRE(a.frames.size() == 2);
    REQUIRE(a.dropped == 0);
    REQUIRE(a.reltimes == std::vector<int>{0, 3});
    REQUIRE(plane0(a, 0) == std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD});
    REQUIRE(plane0(a, 1) == std::vector<uint8_t>{0x11, 0xBB, 0x11, 0x77});
}

TEST_CASE("op-7 NewTek word delta writes literal and repeated words, drops writes past the plane") {
    // bits 0 = word data, horizontal runs; the pointer counts words from the top-left.
    // The plane is 2 rows x 1 word. Group 1: one literal word at word 0. Group 2: offset 0 (the pointer
    // is already at word 1), repeat (-2) of 0x1234 -> words 1 and 2; word 2 is past the plane and is dropped.
    std::vector<uint8_t> dl;
    put16(dl, 1); put16(dl, 0); put16(dl, 0xBEEF);
    put16(dl, uint16_t(-2)); put16(dl, 0); put16(dl, 0x1234);
    put16(dl, 0);
    auto a = decode(makeAnim({firstFrame({0xAA, 0xBB, 0xCC, 0xDD}), deltaFrame(7, dl, 0)}));
    REQUIRE(a.dropped == 0);
    REQUIRE(plane0(a, 1) == std::vector<uint8_t>{0xBE, 0xEF, 0x12, 0x34});
}

TEST_CASE("op-7 byte/vertical variant and the -1 jump marker") {
    // bits 0x40|0x10: byte data, vertical (the pointer steps a whole row of 2 byte columns).
    // Jump: count -1, offset 1 -> pointer 1 (no data). Then 2 literal bytes from pointer 1 -> bytes 1 and 3.
    std::vector<uint8_t> dl;
    dl.push_back(uint8_t(-1)); put16(dl, 1);
    dl.push_back(2); put16(dl, 0); dl.push_back(0x55); dl.push_back(0x66);
    dl.push_back(0);
    auto a = decode(makeAnim({firstFrame({1, 2, 3, 4}), deltaFrame(7, dl, 0x50)}));
    REQUIRE(plane0(a, 1) == std::vector<uint8_t>{1, 0x55, 3, 0x66});
}

TEST_CASE("op-8 NewTek delta fills a run of words backwards") {
    // 16x2 plane = 2 words. start 0, count 2 -> q = 0, r = 2: one longword.
    std::vector<uint8_t> dl;
    put32(dl, 0); put32(dl, 2); put16(dl, 0); put16(dl, 2);
    put16(dl, 0x1111); put16(dl, 0x2222);
    auto a = decode(makeAnim({firstFrame({0xAA, 0xBB, 0xCC, 0xDD}), deltaFrame(8, dl)}));
    REQUIRE(plane0(a, 1) == std::vector<uint8_t>{0x11, 0x11, 0x22, 0x22});

    // count 1 (odd): the single word replaces word 1 only (start 1).
    std::vector<uint8_t> dl1;
    put32(dl1, 1); put32(dl1, 1); put16(dl1, 0); put16(dl1, 1);
    put16(dl1, 0x9999);
    auto a1 = decode(makeAnim({firstFrame({0xAA, 0xBB, 0xCC, 0xDD}), deltaFrame(8, dl1)}));
    REQUIRE(plane0(a1, 1) == std::vector<uint8_t>{0xAA, 0xBB, 0x99, 0x99});

    // a header whose loop counters do not match the count is rejected (last frame -> dropped)
    std::vector<uint8_t> bad;
    put32(bad, 0); put32(bad, 2); put16(bad, 5); put16(bad, 5);
    put16(bad, 0); put16(bad, 0);
    auto a2 = decode(makeAnim({firstFrame({0xAA, 0xBB, 0xCC, 0xDD}), deltaFrame(8, bad)}));
    REQUIRE(a2.dropped == 1);
    REQUIRE(a2.planes.size() == 1);
}

TEST_CASE("a delta in the last frames that cannot be decoded is dropped and counted") {
    std::vector<uint8_t> good = dlta5({1, 0, 2, 0x11, 0});  // column 0 filled, column 1 has 0 ops
    std::vector<uint8_t> bad(10, 0);                          // shorter than the 64-byte pointer table
    auto first = firstFrame({0xAA, 0xBB, 0xCC, 0xDD});

    auto a = decode(makeAnim({first, deltaFrame(5, good), deltaFrame(5, bad)}));
    REQUIRE(a.dropped == 1);
    REQUIRE(a.frames.size() == 2);
    REQUIRE(a.planes.size() == 2);
    REQUIRE(a.reltimes.size() == 2);
    REQUIRE(a.cmaps.size() == 2);

    // two damaged frames at the end -> dropped == 2
    auto b = decode(makeAnim({first, deltaFrame(5, good), deltaFrame(5, bad), deltaFrame(5, bad)}));
    REQUIRE(b.dropped == 2);
    REQUIRE(b.frames.size() == 2);

    // a damaged frame that is not one of the last two is fatal
    auto fatal = makeAnim({first, deltaFrame(5, bad), deltaFrame(5, good), deltaFrame(5, good)});
    REQUIRE_THROWS_AS(vt::readAnim(fatal, vt::iffForms(fatal)[0]), std::runtime_error);
}

TEST_CASE("unsupported ops and a broken op-0 BODY are fatal") {
    auto first = firstFrame({0xAA, 0xBB, 0xCC, 0xDD});
    auto unsupported = makeAnim({first, deltaFrame(4, {1, 2, 3, 4})});
    REQUIRE_THROWS_AS(vt::readAnim(unsupported, vt::iffForms(unsupported)[0]), std::runtime_error);

    // op 0 with a BODY that is too short is outside the delta try block, so it stays fatal
    // even in the last frame.
    std::vector<uint8_t> b;
    putChunk(b, "ANHD", anhd(0, 1, 1, 0));
    putChunk(b, "BODY", {1, 2});
    auto shortBody = makeAnim({first, makeForm("ILBM", b)});
    REQUIRE_THROWS_AS(vt::readAnim(shortBody, vt::iffForms(shortBody)[0]), std::runtime_error);
}

TEST_CASE("op-0 replaces the frame, interleave picks the reference, per-frame CMAP is kept") {
    auto first = firstFrame({0xAA, 0xBB, 0xCC, 0xDD});
    std::vector<uint8_t> f1, f2;
    putChunk(f1, "ANHD", anhd(0, 1, 0, 0));  // interleave 0 -> 2
    putChunk(f1, "BODY", {1, 2, 3, 4});
    putChunk(f1, "CMAP", {10, 20, 30, 40, 50, 60});
    putChunk(f2, "ANHD", anhd(5, 2, 2, 0));
    putChunk(f2, "DLTA", dlta5({1, 0, 2, 0x11, 0}));
    auto a = decode(makeAnim({first, makeForm("ILBM", f1), makeForm("ILBM", f2)}));
    REQUIRE(a.planes.size() == 3);
    REQUIRE(plane0(a, 1) == std::vector<uint8_t>{1, 2, 3, 4});
    // frame 2 is a delta from frame 0 (interleave 2), not from frame 1
    REQUIRE(plane0(a, 2) == std::vector<uint8_t>{0x11, 0xBB, 0x11, 0xDD});
    REQUIRE(!a.cmaps[0]);
    REQUIRE(a.cmaps[1]);
    REQUIRE(a.cmaps[1]->size() == 2);
    REQUIRE((*a.cmaps[1])[1].g == 50);
    REQUIRE(!a.cmaps[2]);
    REQUIRE(a.reltimes == std::vector<int>{0, 1, 2});
}
