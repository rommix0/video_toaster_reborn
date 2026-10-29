#include "vt/framestore.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

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
// [firstLane, firstLane + lanes) must come out with all eight bits set.
std::vector<uint8_t> gatherLanes(const Planes& pl, int w, const std::vector<std::pair<int, int>>& table,
                                 int firstLane, int lanes) {
    std::vector<uint8_t> out(size_t(lanes) * pl.h * w);
    std::vector<int> seen(static_cast<size_t>(lanes), 0);
    for (int n = 0; n < pl.count; ++n) {
        const auto [lane, bit] = table[size_t(n)];
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
    if (std::ranges::any_of(seen, [](int m) { return m != 0xff; })) throw Refusal("PLTP does not fill the lanes");
    return out;
}

// FREEZE.md §4 (P1): lanes 6 and 7 are NTSC composite samples at 4 fsc, the DvQuad samples. Every
// 4th row pair trades the lanes; a pixel pair x, x^1 supplies the four samples of one quad.
void decodeComposite(const std::vector<uint8_t>& lanes, int w, int h, Frame& out, Rect r) {
    const uint8_t* l6 = lanes.data();
    const uint8_t* l7 = lanes.data() + size_t(h) * w;
    for (int y = 0; y < h; ++y) {
        const uint8_t* a = l6 + size_t(y) * w;
        const uint8_t* b = l7 + size_t(y) * w;
        if (((y + 1) >> 1) & 1) std::swap(a, b);
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

}  // namespace

Rect framestoreRect(int w, int h) { return {(kRasterW - w) / 2, kFramestoreY0, w, h}; }

std::optional<Frame> loadFramestore(const std::filesystem::path& path, std::string* whyNot) {
    auto fail = [&](std::string why) -> std::optional<Frame> {
        if (whyNot) *whyNot = std::move(why);
        return std::nullopt;
    };
    try {
        auto c = loadCrouton(path);
        if (!c) return fail("cannot read " + path.string());
        // The first FORM ILBM is the picture; later ones are the 80x50 icons.
        const auto form = std::ranges::find(c->forms, kIlbmForm, &Form::type);
        if (form == c->forms.end()) return fail("no picture (FORM ILBM)");
        const Bitmap bm = readBitmapHeader(c->bytes, *form);
        if (bm.w > kMaxW || bm.h > kMaxH || bm.w < 2 || bm.h < 1 || (bm.w | bm.h) & 1)
            return fail("picture size " + std::to_string(bm.w) + "x" + std::to_string(bm.h) +
                        " (want even, at most 768x480)");

        const auto chunks = formChunks(c->bytes, *form);
        const auto body = std::ranges::find(chunks, kBody, &Chunk::id);
        if (body == chunks.end()) return fail("no BODY");
        if (body->data > c->bytes.size()) return fail("BODY cut off");
        std::vector<std::pair<int, int>> table;
        if (const auto pltp = std::ranges::find(chunks, kPltp, &Chunk::id); pltp != chunks.end()) {
            for (size_t i = 0; i + 1 < std::min(pltp->size, c->bytes.size() - pltp->data); i += 2)
                table.emplace_back(c->bytes[pltp->data + i], c->bytes[pltp->data + i + 1]);
        } else if (bm.planes == 24) {
            for (int n = 0; n < 24; ++n) table.emplace_back(8 + n / 8, n % 8);  // plain 24-bit ILBM
        }
        if (int(table.size()) < bm.planes)
            return fail(std::to_string(bm.planes) + " planes without a PLTP entry for each");
        if (bm.planes != 16 && bm.planes != 24)
            return fail(std::to_string(bm.planes) + " planes (want 16 composite or 24 RGB)");

        const Planes pl = bodyToPlanes(bm, std::span<const uint8_t>(c->bytes).subspan(
                                               body->data, std::min(body->size, c->bytes.size() - body->data)));
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

}  // namespace vt
