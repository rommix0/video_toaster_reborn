#include <array>
#include <cmath>
#include <random>
#include <stdexcept>

#include "catch_amalgamated.hpp"
#include "golden.h"
#include "vt/crc32.h"
#include "vt/crouton.h"
#include "vt/ilbm.h"
#include "vt/sound.h"

namespace {

constexpr std::array<char, 4> kIlbm{'I', 'L', 'B', 'M'};
constexpr std::array<char, 4> kBmhd{'B', 'M', 'H', 'D'};

int64_t num(const golden::Record& r, const char* key) { return std::get<int64_t>(r.at(key).v); }
const std::string& str(const golden::Record& r, const char* key) { return std::get<std::string>(r.at(key).v); }

// Big-endian helpers for building IFF fixtures.
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void putTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
void putChunk(std::vector<uint8_t>& v, const char* id, const std::vector<uint8_t>& data) {
    putTag(v, id);
    put32(v, uint32_t(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    if (data.size() & 1) v.push_back(0);
}
std::vector<uint8_t> bmhd(int w, int h, int planes, int masking, int compression) {
    std::vector<uint8_t> b(20, 0);
    b[0] = uint8_t(w >> 8);
    b[1] = uint8_t(w);
    b[2] = uint8_t(h >> 8);
    b[3] = uint8_t(h);
    b[8] = uint8_t(planes);
    b[9] = uint8_t(masking);
    b[10] = uint8_t(compression);
    return b;
}
// FORM <type> wrapping the given (already encoded) chunk bytes.
std::vector<uint8_t> makeForm(const char* type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f;
    putTag(f, "FORM");
    put32(f, uint32_t(4 + body.size()));
    putTag(f, type);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

bool isIcon(const std::vector<uint8_t>& bytes, const vt::Form& f) {
    for (auto& c : vt::formChunks(bytes, f)) {
        if (c.id == kBmhd && c.size >= 4) {
            int w = (bytes[c.data] << 8) | bytes[c.data + 1];
            int h = (bytes[c.data + 2] << 8) | bytes[c.data + 3];
            return w == 80 && h == 50;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("every gradient ILBM decodes to the golden index CRC") {
    auto recs = golden::readLines("ilbm.jsonl");
    REQUIRE(recs.size() == 106);
    for (auto& r : recs) {
        INFO(str(r, "source"));
        auto c = vt::loadCrouton(golden::sourcePath(str(r, "source")));
        REQUIRE(c);
        const vt::Form* pick = nullptr;
        for (auto& f : c->forms)
            if (f.type == kIlbm && !isIcon(c->bytes, f)) {
                pick = &f;
                break;
            }
        REQUIRE(pick);
        auto [bm, img] = vt::readIlbm(c->bytes, *pick);
        REQUIRE(bm.w == num(r, "w"));
        REQUIRE(bm.h == num(r, "h"));
        REQUIRE(bm.planes == num(r, "planes"));
        REQUIRE(img.w == bm.w);
        REQUIRE(img.h == bm.h);
        REQUIRE(img.px.size() == size_t(bm.w) * size_t(bm.h));
        REQUIRE(vt::crc32(img.px) == uint32_t(num(r, "index_crc")));
    }
}

TEST_CASE("unpackByteRun1 handles literals, repeats and the 0x80 no-op") {
    // literal 3 bytes, no-op, repeat 0xAA x4
    std::vector<uint8_t> src = {2, 1, 2, 3, 0x80, uint8_t(257 - 4), 0xAA};
    auto out = vt::unpackByteRun1(src, 7);
    REQUIRE(out == std::vector<uint8_t>{1, 2, 3, 0xAA, 0xAA, 0xAA, 0xAA});
}

TEST_CASE("unpackByteRun1 rejects malformed streams") {
    // run longer than the output
    REQUIRE_THROWS_AS(vt::unpackByteRun1(std::vector<uint8_t>{uint8_t(257 - 9), 7}, 4), std::runtime_error);
    REQUIRE_THROWS_AS(vt::unpackByteRun1(std::vector<uint8_t>{5, 1, 2, 3, 4, 5, 6}, 4), std::runtime_error);
    // literal run cut off by the end of the source
    REQUIRE_THROWS_AS(vt::unpackByteRun1(std::vector<uint8_t>{5, 1, 2}, 6), std::runtime_error);
    // repeat with no data byte
    REQUIRE_THROWS_AS(vt::unpackByteRun1(std::vector<uint8_t>{uint8_t(257 - 3)}, 3), std::runtime_error);
    // source ends before the output is full
    REQUIRE_THROWS_AS(vt::unpackByteRun1(std::vector<uint8_t>{0, 9}, 4), std::runtime_error);
    // n == 0 needs nothing
    REQUIRE(vt::unpackByteRun1(std::vector<uint8_t>{}, 0).empty());
}

TEST_CASE("bodyToPlanes de-interleaves rows and skips the mask plane") {
    vt::Bitmap bm{};
    bm.w = 16;
    bm.h = 2;
    bm.planes = 2;
    bm.masking = 1;
    bm.compression = 0;
    REQUIRE(bm.rowbytes() == 2);
    // row0: p0=AA00 p1=BB11 mask=FFFF ; row1: p0=CC22 p1=DD33 mask=0000
    std::vector<uint8_t> body = {0xAA, 0x00, 0xBB, 0x11, 0xFF, 0xFF, 0xCC, 0x22, 0xDD, 0x33, 0x00, 0x00};
    auto p = vt::bodyToPlanes(bm, body);
    REQUIRE(p.count == 2);
    REQUIRE(p.h == 2);
    REQUIRE(p.rowbytes == 2);
    REQUIRE(p.bytes.size() == 8);
    REQUIRE(std::vector<uint8_t>(p.plane(0), p.plane(0) + 4) == std::vector<uint8_t>{0xAA, 0x00, 0xCC, 0x22});
    REQUIRE(std::vector<uint8_t>(p.plane(1), p.plane(1) + 4) == std::vector<uint8_t>{0xBB, 0x11, 0xDD, 0x33});
    // a BODY that is too short is an error
    REQUIRE_THROWS_AS(vt::bodyToPlanes(bm, std::span<const uint8_t>(body).first(11)), std::runtime_error);
}

TEST_CASE("planesToIndex builds indices, crops to width, rejects > 8 planes") {
    vt::Planes p{2, 1, 2, std::vector<uint8_t>(4)};
    p.plane(0)[0] = 0b10100000;
    p.plane(0)[1] = 0;
    p.plane(1)[0] = 0b11000000;
    p.plane(1)[1] = 0;
    auto img = vt::planesToIndex(p, 5);
    REQUIRE(img.w == 5);
    REQUIRE(img.h == 1);
    REQUIRE(img.px == std::vector<uint8_t>{3, 2, 1, 0, 0});
    REQUIRE_THROWS_AS(vt::planesToIndex(p, 17), std::runtime_error);  // wider than the planes
    vt::Planes nine{9, 1, 2, std::vector<uint8_t>(18)};
    REQUIRE_THROWS_AS(vt::planesToIndex(nine, 8), std::runtime_error);
}

TEST_CASE("readIlbm: synthetic uncompressed and ByteRun1 pictures, CMAP and CAMG") {
    // 16x2, 1 plane: rows 0xF0F0 / 0x0FFF
    std::vector<uint8_t> raw = {0xF0, 0xF0, 0x0F, 0xFF};
    std::vector<uint8_t> packed = {3, 0xF0, 0xF0, 0x0F, 0xFF};
    std::vector<uint8_t> cmap = {1, 2, 3, 4, 5, 6, 7};  // 7 bytes: the stray byte is dropped
    std::vector<uint8_t> camg = {0, 0, 0x08, 0x04};
    for (int comp = 0; comp < 2; ++comp) {
        std::vector<uint8_t> body;
        putChunk(body, "BMHD", bmhd(16, 2, 1, 0, comp));
        putChunk(body, "CMAP", cmap);
        putChunk(body, "CAMG", camg);
        putChunk(body, "BODY", comp ? packed : raw);
        auto file = makeForm("ILBM", body);
        auto forms = vt::iffForms(file);
        REQUIRE(forms.size() == 1);
        auto [bm, img] = vt::readIlbm(file, forms[0]);
        REQUIRE(bm.w == 16);
        REQUIRE(bm.h == 2);
        REQUIRE(bm.planes == 1);
        REQUIRE(bm.compression == comp);
        REQUIRE(bm.camg == 0x0804);
        REQUIRE(bm.cmap.size() == 2);
        REQUIRE(bm.cmap[1].r == 4);
        REQUIRE(bm.cmap[1].b == 6);
        std::vector<uint8_t> expect = {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                       0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        REQUIRE(img.px == expect);
    }
}

TEST_CASE("readIlbm rejects missing chunks and a short BODY") {
    std::vector<uint8_t> noBody;
    putChunk(noBody, "BMHD", bmhd(16, 2, 1, 0, 0));
    auto f1 = makeForm("ILBM", noBody);
    REQUIRE_THROWS_AS(vt::readIlbm(f1, vt::iffForms(f1)[0]), std::runtime_error);

    std::vector<uint8_t> noHeader;
    putChunk(noHeader, "BODY", std::vector<uint8_t>(4));
    auto f2 = makeForm("ILBM", noHeader);
    REQUIRE_THROWS_AS(vt::readIlbm(f2, vt::iffForms(f2)[0]), std::runtime_error);

    std::vector<uint8_t> shortBody;
    putChunk(shortBody, "BMHD", bmhd(16, 2, 1, 0, 0));
    putChunk(shortBody, "BODY", std::vector<uint8_t>(3));
    auto f3 = makeForm("ILBM", shortBody);
    REQUIRE_THROWS_AS(vt::readIlbm(f3, vt::iffForms(f3)[0]), std::runtime_error);
}

TEST_CASE("formChunks clamps a truncated chunk to the bytes present") {
    std::vector<uint8_t> body;
    putChunk(body, "BMHD", bmhd(16, 2, 1, 0, 0));
    putTag(body, "BODY");
    put32(body, 1000);                         // claims 1000 bytes ...
    body.insert(body.end(), {1, 2, 3});  // ... only 3 present
    auto file = makeForm("ILBM", body);
    auto forms = vt::iffForms(file);
    REQUIRE(forms.size() == 1);
    auto ch = vt::formChunks(file, forms[0]);
    REQUIRE(ch.size() == 2);
    REQUIRE(ch[1].id == std::array<char, 4>{'B', 'O', 'D', 'Y'});
    REQUIRE(ch[1].size == 3);
    REQUIRE(ch[1].data + ch[1].size == file.size());
    // a chunk that overruns its FORM (but not the buffer) is clamped to the FORM
    auto padded = file;
    padded.resize(padded.size() + 50, 0xEE);
    auto f2 = vt::iffForms(padded)[0];
    auto c2 = vt::formChunks(padded, f2);
    REQUIRE(c2[1].data + c2[1].size <= f2.offset + 8 + f2.size);
    // the decoder fails cleanly instead of reading out of range
    REQUIRE_THROWS_AS(vt::readIlbm(file, forms[0]), std::runtime_error);
}

TEST_CASE("readSound decodes a stereo 8SVX effect sound") {
    // First converted/manifest.csv row with a sound: Falling Sheep, 2ch 24429 Hz 5.08 s.
    auto bytes = readFile(dataRoot() / "4.2/Effects/2000FX/Animals/Falling Sheep");
    auto s = vt::readSound(bytes);
    REQUIRE(s);
    REQUIRE(s->rate == 24429);
    REQUIRE(s->channels == 2);
    const double expect = 5.08 * 24429 * 2;
    REQUIRE(std::abs(double(s->pcm.size()) - expect) <= expect * 0.01);
    REQUIRE(s->pcm.size() % 2 == 0);
    for (int16_t v : s->pcm) REQUIRE(v % 256 == 0);  // 8-bit samples scaled by 256
    // Exact samples: CRC-32 of the interleaved int16 little-endian PCM, computed from
    // tools/toaster_iff.read_8svx (left block, right block; L/R interleaved, x256).
    REQUIRE(s->pcm.size() == 248152);
    std::vector<uint8_t> le;
    le.reserve(s->pcm.size() * 2);
    for (int16_t v : s->pcm) {
        le.push_back(uint8_t(uint16_t(v)));
        le.push_back(uint8_t(uint16_t(v) >> 8));
    }
    REQUIRE(vt::crc32(le) == 1166533720u);
}

TEST_CASE("readSound: mono, stereo by CHAN, no 8SVX, bad data") {
    auto vhdr = [](uint32_t one, uint32_t rep, uint16_t rate, uint8_t comp) {
        std::vector<uint8_t> v;
        put32(v, one);
        put32(v, rep);
        put32(v, 0);
        v.push_back(uint8_t(rate >> 8));
        v.push_back(uint8_t(rate));
        v.push_back(1);
        v.push_back(comp);
        put32(v, 0x10000);
        return v;
    };
    {  // mono, body longer than the sample count: only `one` samples are used
        std::vector<uint8_t> b;
        putChunk(b, "VHDR", vhdr(3, 0, 8000, 0));
        putChunk(b, "BODY", {1, 0xFF, 2, 9, 9});
        auto f = makeForm("8SVX", b);
        auto s = vt::readSound(f);
        REQUIRE(s);
        REQUIRE(s->rate == 8000);
        REQUIRE(s->channels == 1);
        REQUIRE(s->pcm == std::vector<int16_t>{256, -256, 512});
    }
    {  // stereo from CHAN = 6: left block then right block, interleaved on output
        std::vector<uint8_t> b;
        putChunk(b, "VHDR", vhdr(2, 0, 8000, 0));
        putChunk(b, "CHAN", {0, 0, 0, 6});
        putChunk(b, "BODY", {1, 2, 3, 4});
        std::vector<uint8_t> file = {'J', 'U', 'N', 'K', 0, 0, 0, 2, 0, 0};  // the FORM need not be first
        auto f = makeForm("8SVX", b);
        file.insert(file.end(), f.begin(), f.end());
        auto s = vt::readSound(file);
        REQUIRE(s);
        REQUIRE(s->channels == 2);
        REQUIRE(s->pcm == std::vector<int16_t>{256, 768, 512, 1024});
    }
    {  // stereo from BODY >= 2 x samples
        std::vector<uint8_t> b;
        putChunk(b, "VHDR", vhdr(2, 0, 8000, 0));
        putChunk(b, "BODY", {1, 2, 3, 4});
        auto s = vt::readSound(makeForm("8SVX", b));
        REQUIRE(s);
        REQUIRE(s->channels == 2);
    }
    {  // no 8SVX at all
        std::vector<uint8_t> b;
        putChunk(b, "BMHD", bmhd(16, 2, 1, 0, 0));
        REQUIRE(!vt::readSound(makeForm("ILBM", b)));
        REQUIRE(!vt::readSound(std::vector<uint8_t>{}));
    }
    {  // compressed 8SVX is unsupported; missing BODY is malformed
        std::vector<uint8_t> b;
        putChunk(b, "VHDR", vhdr(2, 0, 8000, 1));
        putChunk(b, "BODY", {1, 2});
        REQUIRE_THROWS_AS(vt::readSound(makeForm("8SVX", b)), std::runtime_error);
        std::vector<uint8_t> n;
        putChunk(n, "VHDR", vhdr(2, 0, 8000, 0));
        REQUIRE_THROWS_AS(vt::readSound(makeForm("8SVX", n)), std::runtime_error);
    }
}

TEST_CASE("packByteRun1 round trips") {
    std::mt19937 rng(7);
    for (size_t n : {0, 1, 2, 127, 128, 129, 300}) {
        std::vector<uint8_t> noise(n), flat(n, 0x5a), mixed(n);
        for (auto& b : noise) b = uint8_t(rng());
        for (size_t i = 0; i < n; ++i) mixed[i] = uint8_t((i / 7) % 3 == 0 ? 9 : rng() % 4);  // runs, pairs and noise
        for (const auto& row : {noise, flat, mixed}) {
            INFO("n = " << n);
            const auto packed = vt::packByteRun1(row);
            CHECK(vt::unpackByteRun1(packed, row.size()) == row);
            // a run of 128 is one repeat pair; 3 equal bytes are worth a repeat
            if (n == 300 && row == flat) CHECK(packed.size() == 6);
        }
    }
}
