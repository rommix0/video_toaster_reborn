#include "vt/framestore.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "log_detail.h"
#include "vt/crouton.h"
#include "vt/dv_colour.h"
#include "vt/ilbm.h"

namespace vt {
namespace {

constexpr std::array<char, 4> kIlbmForm{'I', 'L', 'B', 'M'};
constexpr std::array<char, 4> kPltp{'P', 'L', 'T', 'P'};
constexpr std::array<char, 4> kBody{'B', 'O', 'D', 'Y'};
constexpr int kMaxW = 768, kMaxH = 480;

struct Refusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The picture of a plane set as `lanes` 8-bit samples per pixel: PLTP entry n is the byte pair
// (lane, bit) of plane n, lane value = sum of plane bit << bit. Every lane in
// [firstLane, firstLane + lanes) must come out with the bits of `need` set. A plane for another lane
// is refused, or with skipOthers left out (a CG key page's alpha lane beside its composite).
std::vector<uint8_t> gatherLanes(const Planes& pl, int w, const std::vector<std::pair<int, int>>& table,
                                 int firstLane, int lanes, int need = 0xff, bool skipOthers = false) {
    std::vector<uint8_t> out(size_t(lanes) * pl.h * w);
    std::vector<int> seen(static_cast<size_t>(lanes), 0);
    for (int n = 0; n < pl.count; ++n) {
        const auto [lane, bit] = table[size_t(n)];
        if (skipOthers && (lane < firstLane || lane >= firstLane + lanes)) continue;
        if (lane < firstLane || lane >= firstLane + lanes || bit > 7)
            throw Refusal("plane " + std::to_string(n) + " goes to lane " + std::to_string(lane) + " bit " +
                          std::to_string(bit) + ", outside lanes " + std::to_string(firstLane) + "-" +
                          std::to_string(firstLane + lanes - 1));
        seen[size_t(lane - firstLane)] |= 1 << bit;
        uint8_t* dst = out.data() + size_t(lane - firstLane) * pl.h * w;
        for (int y = 0; y < pl.h; ++y) {
            const uint8_t* row = pl.plane(n) + size_t(y) * pl.rowbytes;
            for (int x = 0; x < w; ++x) dst[size_t(y) * w + x] |= uint8_t((row[x >> 3] >> (7 - (x & 7)) & 1) << bit);
        }
    }
    if (std::ranges::any_of(seen, [&](int m) { return (m & need) != need; })) throw Refusal("PLTP does not fill the lanes");
    return out;
}

// FREEZE.md §4: lanes 6 and 7 are NTSC composite samples at 4 fsc, the DvQuad samples. Every
// 4th row pair trades the lanes; a pixel pair x, x^1 supplies the four samples of one quad.
// The phase follows the DV line, file row + line0 (CGKEY.md §4): 0 for a framestore, BMHD y for a
// CG key page.
void decodeComposite(const std::vector<uint8_t>& lanes, int w, int h, Frame& out, Rect r, int line0 = 0) {
    const uint8_t* l6 = lanes.data();
    const uint8_t* l7 = lanes.data() + size_t(h) * w;
    for (int y = 0; y < h; ++y) {
        const uint8_t* a = l6 + size_t(y) * w;
        const uint8_t* b = l7 + size_t(y) * w;
        if (((y + line0 + 1) >> 1) & 1) std::swap(a, b);
        for (int x = 0; x < w; ++x) {
            DvQuad q{};
            for (int px : {x, x ^ 1}) {
                q[size_t(px & 3)] = a[px];
                q[size_t((px + 2) & 3)] = b[px];
            }
            out.at(r.x + x, r.y + y) = dvQuadToRgb(q);
        }
    }
}

// The picture of a crouton: its first FORM ILBM (later ones are the 80x50 icons), its header (passed
// to sizeOk, which throws a Refusal for a size it does not take), PLTP table and BODY.
struct Picture {
    Bitmap bm;
    std::vector<std::pair<int, int>> table;
    std::span<const uint8_t> body;
};
template <class SizeOk>
Picture readPicture(const Crouton& c, SizeOk sizeOk) {
    const auto form = std::ranges::find(c.forms, kIlbmForm, &Form::type);
    if (form == c.forms.end()) throw Refusal("no picture (FORM ILBM)");
    Picture p{readBitmapHeader(c.bytes, *form), {}, {}};
    const Bitmap& bm = p.bm;
    sizeOk(bm);
    const auto chunks = formChunks(c.bytes, *form);
    const auto body = std::ranges::find(chunks, kBody, &Chunk::id);
    if (body == chunks.end()) throw Refusal("no BODY");
    if (body->data > c.bytes.size()) throw Refusal("BODY cut off");
    if (const auto pltp = std::ranges::find(chunks, kPltp, &Chunk::id); pltp != chunks.end()) {
        for (size_t i = 0; i + 1 < std::min(pltp->size, c.bytes.size() - pltp->data); i += 2)
            p.table.emplace_back(c.bytes[pltp->data + i], c.bytes[pltp->data + i + 1]);
    } else if (bm.planes == 24) {
        for (int n = 0; n < 24; ++n) p.table.emplace_back(8 + n / 8, n % 8);  // plain 24-bit ILBM
    }
    if (int(p.table.size()) < bm.planes) throw Refusal(std::to_string(bm.planes) + " planes without a PLTP entry for each");
    p.body = std::span<const uint8_t>(c.bytes).subspan(body->data, std::min(body->size, c.bytes.size() - body->data));
    return p;
}

// ---- saving: the layout of 4.2/Framestore/ColorBars (FREEZE.md §4) ----

using Bytes = std::vector<uint8_t>;

void put16(Bytes& v, unsigned x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
void put32(Bytes& v, uint32_t x) {
    put16(v, x >> 16);
    put16(v, x & 0xffff);
}
void putTag(Bytes& v, const char* t) { v.insert(v.end(), t, t + 4); }
void putChunk(Bytes& v, const char* id, const Bytes& data) {
    putTag(v, id);
    put32(v, uint32_t(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    if (data.size() & 1) v.push_back(0);
}
Bytes makeForm(const char* type, const Bytes& chunks) {
    Bytes f;
    putTag(f, "FORM");
    put32(f, uint32_t(4 + chunks.size()));
    putTag(f, type);
    f.insert(f.end(), chunks.begin(), chunks.end());
    return f;
}
// The " PUS" form after a FORM: one chunk holding the previous FORM's size field.
Bytes pusForm(const char* id, const Bytes& previous) {
    Bytes c;
    putChunk(c, id, Bytes(previous.begin() + 4, previous.begin() + 8));
    return makeForm(" PUS", c);
}
Bytes bmhd(int w, int h, int planes, int compression, int xAspect, int yAspect) {
    Bytes b;
    put16(b, unsigned(w));
    put16(b, unsigned(h));
    put32(b, 0);                                                       // x, y
    b.insert(b.end(), {uint8_t(planes), 0, uint8_t(compression), 0});  // masking 0, pad
    put16(b, 0);                                                       // transparent colour
    b.insert(b.end(), {uint8_t(xAspect), uint8_t(yAspect)});
    put16(b, unsigned(w));
    put16(b, unsigned(h));
    return b;
}
// One ILBM row of one plane: bit x of the row is bitAt(x). Rows are padded to 16 pixels.
template <class F>
Bytes planeRow(int w, F bitAt) {
    Bytes r(size_t((w + 15) / 16) * 2, 0);
    for (int x = 0; x < w; ++x) r[size_t(x >> 3)] |= uint8_t(bitAt(x) << (7 - (x & 7)));
    return r;
}

constexpr int kIconW = 80, kIconH = 50;  // vt::assets::kCroutonW/H; the engine does not link vt_assets

// An icon `planes` deep from one palette index per pixel, uncompressed, with its CMAP.
Bytes iconForm(int planes, const Bytes& cmap, const Bytes& index) {
    Bytes body;
    for (int y = 0; y < kIconH; ++y)
        for (int p = 0; p < planes; ++p) {
            const Bytes row = planeRow(kIconW, [&](int x) { return index[size_t(y) * kIconW + size_t(x)] >> p & 1; });
            body.insert(body.end(), row.begin(), row.end());
        }
    Bytes c;
    putChunk(c, "BMHD", bmhd(kIconW, kIconH, planes, 0, 1, 1));
    putChunk(c, "CMAP", cmap);
    putChunk(c, "BODY", body);
    return makeForm("ILBM", c);
}

// Forms 2-7 of a framestore: both icons and the CrUD FRAM tag form. This is the whole ".I" file.
// Icon pixels are the box average of their block of the still's rect (9.4 x 9.6 pixels at 752x480).
Bytes iconFile(const Frame& still, Rect r) {
    constexpr uint8_t kLevel[4] = {0, 85, 170, 255};
    constexpr uint8_t kGrey[4] = {0x00, 0x60, 0x90, 0xd0};
    Bytes cubeCmap, greyCmap;
    for (int i = 0; i < 64; ++i) cubeCmap.insert(cubeCmap.end(), {kLevel[i >> 4], kLevel[i >> 2 & 3], kLevel[i & 3]});
    for (uint8_t g : kGrey) greyCmap.insert(greyCmap.end(), {g, g, g});

    Bytes cubeIdx(size_t(kIconW) * kIconH), greyIdx(cubeIdx.size());
    for (int j = 0; j < kIconH; ++j)
        for (int i = 0; i < kIconW; ++i) {
            const int x0 = i * r.w / kIconW, x1 = (i + 1) * r.w / kIconW;
            const int y0 = j * r.h / kIconH, y1 = (j + 1) * r.h / kIconH;
            const int n = (x1 - x0) * (y1 - y0);
            int sum[3] = {0, 0, 0};
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const Rgba8 p = still.at(r.x + x, r.y + y);
                    sum[0] += p.r, sum[1] += p.g, sum[2] += p.b;
                }
            int avg[3];
            for (int c = 0; c < 3; ++c) avg[c] = (sum[c] + n / 2) / n;
            const size_t at = size_t(j) * kIconW + size_t(i);
            // nearest of 0/85/170/255 per gun
            cubeIdx[at] = uint8_t(((avg[0] + 42) / 85) << 4 | ((avg[1] + 42) / 85) << 2 | (avg[2] + 42) / 85);
            const int luma = (299 * avg[0] + 587 * avg[1] + 114 * avg[2] + 500) / 1000;  // Rec.601
            int best = 0;
            for (int g = 1; g < 4; ++g)
                if (std::abs(luma - kGrey[g]) < std::abs(luma - kGrey[best])) best = g;
            greyIdx[at] = uint8_t(best);
        }

    Bytes out;
    auto add = [&](const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); };
    for (const auto& [planes, cmap, idx] : {std::tuple{6, &cubeCmap, &cubeIdx}, std::tuple{2, &greyCmap, &greyIdx}}) {
        const Bytes icon = iconForm(planes, *cmap, *idx);
        add(icon);
        add(pusForm("ILBM", icon));
    }
    // CrUD FRAM (TYPE FRAM, LIBS "effects.library", empty TAGS), verbatim from ColorBars.
    static constexpr uint8_t kCrud[72] = {
        0x46, 0x4F, 0x52, 0x4D, 0x00, 0x00, 0x00, 0x40, 0x43, 0x72, 0x55, 0x44, 0x54, 0x59, 0x50, 0x45, 0x00, 0x00,
        0x00, 0x08, 0x46, 0x52, 0x41, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x49, 0x42, 0x53, 0x00, 0x00, 0x00, 0x18,
        0xFF, 0xFF, 0xFC, 0xD0, 0x00, 0x10, 0x65, 0x66, 0x66, 0x65, 0x63, 0x74, 0x73, 0x2E, 0x6C, 0x69, 0x62, 0x72,
        0x61, 0x72, 0x79, 0x00, 0x00, 0x00, 0x54, 0x41, 0x47, 0x53, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
    const Bytes crud(kCrud, kCrud + sizeof kCrud);
    add(crud);
    add(pusForm("CrUD", crud));
    return out;
}

// The picture: 24-plane RGB, PLTP lanes 8/9/10 (R, G, B) LSB first (FREEZE.md §4), ByteRun1 rows.
Bytes pictureForm(const Frame& still, Rect r) {
    Bytes pltp;
    for (int n = 0; n < 24; ++n) pltp.insert(pltp.end(), {uint8_t(8 + n / 8), uint8_t(n % 8)});
    Bytes body;
    for (int y = 0; y < r.h; ++y)
        for (int n = 0; n < 24; ++n) {
            const Bytes row = planeRow(r.w, [&](int x) {
                const Rgba8 p = still.at(r.x + x, r.y + y);
                return (n < 8 ? p.r : n < 16 ? p.g : p.b) >> (n % 8) & 1;
            });
            const Bytes packed = packByteRun1(row);
            body.insert(body.end(), packed.begin(), packed.end());
        }
    Bytes c;
    putChunk(c, "BMHD", bmhd(r.w, r.h, 24, 1, 10, 11));  // the Switcher's save template, switcher.main 0x11ca4c
    putChunk(c, "PLTP", pltp);
    putChunk(c, "BODY", body);
    return makeForm("ILBM", c);
}

void writeFile(const std::filesystem::path& p, const Bytes& a, const Bytes& b = {}) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(a.data()), std::streamsize(a.size()));
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    f.close();
    if (!f) throw std::runtime_error("cannot write " + detail::utf8(p));
}

}  // namespace

Rect framestoreRect(int w, int h) { return {(kRasterW - w) / 2, kFramestoreY0, w, h}; }

bool isFramestore(const std::filesystem::path& path, std::optional<Frame>* picture) {
    try {
        const auto type = croutonType(path);
        if (!type) return false;
        if (*type != CroutonType::None) return *type == CroutonType::Fram;
        auto still = loadFramestore(path);  // Theater (LightWaveDemo): no CrUD anywhere
        const bool ok = still.has_value();
        if (picture) *picture = std::move(still);
        return ok;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<Frame> loadFramestore(const std::filesystem::path& path, std::string* whyNot) {
    auto fail = [&](std::string why) -> std::optional<Frame> {
        if (whyNot) *whyNot = std::move(why);
        return std::nullopt;
    };
    try {
        auto c = loadCrouton(path);
        if (!c) return fail("cannot read " + detail::utf8(path));
        const Picture pic = readPicture(*c, [](const Bitmap& bm) {
            if (bm.w > kMaxW || bm.h > kMaxH || bm.w < 2 || bm.h < 1 || (bm.w | bm.h) & 1)
                throw Refusal("picture size " + std::to_string(bm.w) + "x" + std::to_string(bm.h) +
                              " (want even, at most 768x480)");
        });
        const Bitmap& bm = pic.bm;
        const auto& table = pic.table;
        if (bm.planes != 16 && bm.planes != 24)
            return fail(std::to_string(bm.planes) + " planes (want 16 composite or 24 RGB)");

        const Planes pl = bodyToPlanes(bm, pic.body);
        Frame f;
        std::fill(f.px.begin(), f.px.end(), Rgba8{0, 0, 0, 255});
        const Rect r = framestoreRect(bm.w, bm.h);
        if (bm.planes == 16) {
            decodeComposite(gatherLanes(pl, bm.w, table, 6, 2), bm.w, bm.h, f, r);
        } else {
            const auto rgb = gatherLanes(pl, bm.w, table, 8, 3);  // lane 8 R, 9 G, 10 B (FREEZE.md §4)
            const size_t plane = size_t(bm.h) * bm.w;
            for (int y = 0; y < bm.h; ++y)
                for (int x = 0; x < bm.w; ++x) {
                    const size_t i = size_t(y) * bm.w + x;
                    f.at(r.x + x, r.y + y) = {rgb[i], rgb[plane + i], rgb[2 * plane + i], 255};
                }
        }
        return f;
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
}

bool saveFramestore(const std::filesystem::path& path, const Frame& still, std::string* whyNot) {
    namespace fs = std::filesystem;
    fs::path icons = path, tmp = path, tmpIcons;
    icons += ".I";
    tmp += ".tmp";
    tmpIcons = icons;
    tmpIcons += ".tmp";
    try {
        const Rect r = framestoreRect();
        const Bytes picture = pictureForm(still, r), tail = iconFile(still, r);
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        writeFile(tmp, picture, tail);
        writeFile(tmpIcons, tail);
        fs::rename(tmp, path);
        fs::rename(tmpIcons, icons);
        return true;
    } catch (const std::exception& ex) {
        std::error_code ec;
        fs::remove(tmp, ec);
        fs::remove(tmpIcons, ec);
        if (whyNot) *whyNot = ex.what();
        return false;
    }
}

bool isCgKeyPage(const std::filesystem::path& path) {
    try {
        return croutonType(path) == CroutonType::Keyf;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<CgKeyPage> loadCgKeyPage(const std::filesystem::path& path, std::string* whyNot) {
    try {
        auto c = loadCrouton(path);
        if (!c) throw Refusal("cannot read " + detail::utf8(path));
        const Picture pic = readPicture(*c, [](const Bitmap& bm) {
            if (bm.w > kMaxW || bm.w < 2 || bm.w & 1 || bm.h < 1 || bm.y < 0 || bm.y + bm.h > kMaxH)
                throw Refusal("page " + std::to_string(bm.w) + "x" + std::to_string(bm.h) + " at y " +
                              std::to_string(bm.y) + " (want an even width up to 768, lines 0..480)");
        });
        const Bitmap& bm = pic.bm;
        const Planes pl = bodyToPlanes(bm, pic.body);
        CgKeyPage page{path, {}, std::vector<uint8_t>(size_t(kRasterW) * kRasterH), algoTiming(*c)};
        std::fill(page.fill.px.begin(), page.fill.px.end(), Rgba8{0, 0, 0, 255});
        // CGKEY.md §4: DV lines y + 1 .., x 0; the phase from the DV line.
        const Rect r{0, kFramestoreY0 + bm.y, bm.w, bm.h};
        decodeComposite(gatherLanes(pl, bm.w, pic.table, 6, 2, 0xff, true), bm.w, bm.h, page.fill, r, bm.y);
        const auto alpha = gatherLanes(pl, bm.w, pic.table, 0x0b, 1, 0x03, true);  // lane 0x0b bits 1-0
        for (int y = 0; y < bm.h; ++y)
            for (int x = 0; x < bm.w; ++x)
                page.alpha[size_t(r.y + y) * kRasterW + size_t(x)] = alpha[size_t(y) * bm.w + x] & 3;
        return page;
    } catch (const std::exception& ex) {
        if (whyNot) *whyNot = ex.what();
        return std::nullopt;
    }
}

}  // namespace vt
