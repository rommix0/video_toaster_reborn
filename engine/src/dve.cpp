// DVE control decoding and rendering (port of tools/toaster_dve.py decode/render and
// toaster_fx_convert.write_dve's role rule). See vt/dve.h.
#include "vt/dve.h"

#include <algorithm>
#include <stdexcept>

namespace vt {
namespace {

constexpr uint8_t kSig[4] = {0x15, 0x2D, 0x06, 0x83};
constexpr int kCtrlRows = 11;                     // plane 2 rows holding the line table
constexpr int kTop = 13, kBottom = 12, kLeft = 12, kRight = 20;  // toaster_dve.OVERSCAN

bool startsWithSig(const Planes& p, int plane) {
    const uint8_t* row = p.plane(plane);
    return std::equal(kSig, kSig + 4, row);
}

// Fraction of key cells >= 64 with the converter's overscan fill (toaster_dve.coverage): rows
// above kTop copy row kTop, rows from h - kBottom copy row h - kBottom - 1, then columns left of
// kLeft copy column kLeft and columns from 192 - kRight copy column 191 - kRight.
double filledCoverage(const DveControl& c) {
    auto srcLine = [](int k) { return std::clamp(k, kTop, kDveLines - kBottom - 1); };
    auto srcCell = [](int x) { return std::clamp(x, kLeft, kDveCells - kRight - 1); };
    long count = 0;
    for (int k = 0; k < kDveLines; ++k) {
        const uint8_t* row = &c.key[size_t(srcLine(k)) * kDveCells];
        for (int x = 0; x < kDveCells; ++x) count += row[srcCell(x)] >= 64;
    }
    return double(count) / double(kDveLines * kDveCells);
}

constexpr Rgba8 kBars[8] = {
    {0, 0, 191, 255},   {191, 0, 0, 255},   {191, 0, 191, 255}, {0, 191, 0, 255},
    {0, 191, 191, 255}, {191, 191, 0, 255}, {191, 191, 191, 255}, {0, 0, 0, 255},
};
constexpr Rgba8 kUnknownGrey{128, 128, 128, 255};

}  // namespace

bool isDve(const Planes& p) {
    if (p.count < 3 || p.rowbytes != 96 || p.h < 1) return false;
    if (p.bytes.size() < size_t(p.count) * size_t(p.h) * size_t(p.rowbytes)) return false;
    return startsWithSig(p, 2);
}

DveControl decodeDve(const Planes& p) {
    if (!isDve(p)) throw std::runtime_error("not DVE control data");
    if (p.h != kDveLines) throw std::runtime_error("DVE control frame is not 241 lines");
    DveControl c;
    // key: plane 1 nibble high, plane 0 nibble low; even cells use the byte's high nibble
    const uint8_t* p0 = p.plane(0);
    const uint8_t* p1 = p.plane(1);
    for (int i = 0; i < kDveLines * 96; ++i) {
        c.key[size_t(2 * i)] = uint8_t((p1[i] & 0xF0) | (p0[i] >> 4));
        c.key[size_t(2 * i + 1)] = uint8_t(((p1[i] & 0x0F) << 4) | (p0[i] & 0x0F));
    }
    // line table: the sprite plane's rows 0..10 as big-endian words, skip 2 (SPR1POS/CTL), then a
    // word pair per line; the sprite plane is the last one with the signature (plane 2 unless "!")
    int sprite = 2;
    for (int pl = 3; pl < p.count; ++pl)
        if (startsWithSig(p, pl)) sprite = pl;
    const uint8_t* t = p.plane(sprite);
    static_assert(2 + 2 * kDveLines <= kCtrlRows * 96 / 2, "line table fits in the control rows");
    auto word = [t](int w) { return unsigned(t[2 * w] << 8 | t[2 * w + 1]); };
    for (int k = 0; k < kDveLines; ++k) {
        const unsigned first = word(2 + 2 * k), second = word(3 + 2 * k);
        const int line = int(((second >> 12) & 15) << 4 | ((first >> 12) & 15));
        c.line[size_t(k)] = uint8_t(std::min(line, kDveLines - 1));
    }
    // display index: every plane but the sprite, bit i from the i-th (more than 3 planes only; a
    // bit past 7 cannot be held and is dropped)
    if (p.count > 3) {
        c.index.assign(size_t(kDveLines) * kDveWidth, 0);
        int ibit = 0;
        for (int pl = 0; pl < p.count && ibit < 8; ++pl) {
            if (pl == sprite) continue;
            const uint8_t* src = p.plane(pl);
            for (int k = 0; k < kDveLines; ++k)
                for (int x = 0; x < kDveWidth; ++x)
                    if (src[k * 96 + x / 8] & (0x80 >> (x % 8))) c.index[size_t(k) * kDveWidth + size_t(x)] |= uint8_t(1 << ibit);
            ++ibit;
        }
    }
    // art: planes 3.. without the signature; the i-th such plane gives bit i of the group
    int bit = 0;
    for (int pl = 3; pl < p.count; ++pl) {
        if (startsWithSig(p, pl)) continue;
        // checked before shifting: 1 << bit is undefined from bit 32 on (corrupt files)
        if (bit >= 8) throw std::runtime_error("DVE art layer has more than 8 planes");
        if (c.art.empty()) c.art.assign(size_t(kDveLines) * kDveWidth, 0);
        const uint8_t* src = p.plane(pl);
        for (int k = 0; k < kDveLines; ++k)
            for (int x = 0; x < kDveWidth; ++x)
                if (src[k * 96 + x / 8] & (0x80 >> (x % 8))) c.art[size_t(k) * kDveWidth + size_t(x)] |= uint8_t(1 << bit);
        ++bit;
    }
    return c;
}

DveInfo dveInfo(const Anim& a) {
    if (a.planes.empty()) throw std::runtime_error("DVE effect without frames");
    std::vector<double> cov;
    cov.reserve(a.planes.size());
    for (const auto& p : a.planes) cov.push_back(filledCoverage(decodeDve(p)));
    const double first = cov.front(), last = cov.back();
    if (first > 0.9 && last > 0.9) {
        const auto k = int(std::min_element(cov.begin(), cov.end()) - cov.begin());  // first minimum
        if (cov[size_t(k)] < 0.5) return {DveRole::Flip, k};
        return {DveRole::DistortA, -1};
    }
    return {first >= last ? DveRole::MovingA : DveRole::MovingB, -1};
}

std::vector<DveRoute> dveRoutes(const std::vector<uint8_t>& tag1b, const std::vector<Rgb>& cmap) {
    // 0xe80e4: word rows, word step, word first index, word count, 8 mask longs, then count longs
    // for row 0 (paletteRoutes' layout)
    if (tag1b.size() < 40) return {};
    auto be16 = [&](size_t o) { return unsigned(tag1b[o] << 8 | tag1b[o + 1]); };
    auto be32 = [&](size_t o) { return be16(o) << 16 | be16(o + 2); };
    const size_t first = be16(4), count = be16(6);
    if (tag1b.size() < 40 + count * 4) return {};
    std::vector<DveRoute> r(first + count);
    for (size_t k = 0; k < count; ++k) {
        const size_t i = first + k;
        const unsigned e = be32(40 + k * 4), mask = be32(8 + 4 * ((e >> 28) & 7));
        const unsigned s = (e >> 12) & 15;
        if ((mask & 0x604) == 0x604 && (s == 1 || s == 3 || s == 9)) r[i].source = uint8_t(s);
        r[i].level = uint8_t(((e & mask) | (~mask & (i < cmap.size() ? unsigned(cmap[i].b) : 0u))) & 3);
    }
    return r;
}

int renderDve(const DveControl& c, const Frame& moving, const Frame& background, const std::vector<Rgb>& cmap,
              Frame& out, const HardwareGuesses& g, const DveMix* mix) {
    const bool routed = mix && mix->routes && !c.index.empty();
    // shadow alpha per strength s = min(v, 7), as toaster_dve.render: uint8(s * (255 * opacity) / 7)
    const double opacity = std::clamp(double(g.dveShadowOpacity), 0.0, 1.0);
    int shadowAlpha[8];
    for (int s = 0; s < 8; ++s) shadowAlpha[s] = std::clamp(int(s * (255.0 * opacity) / 7.0), 0, 255);
    const int cellPixel = g.dveCellPixel < 0 ? -1 : std::min(g.dveCellPixel, 3);
    int unknown = 0;
    for (int y = 0; y < kRasterH; ++y) {
        const int k = y / 2, parity = y & 1;
        const uint8_t* key = &c.key[size_t(k) * kDveCells];
        const bool colourLine = c.line[size_t(k)] == kDveColourLine;
        const Rgba8 border = kBars[std::clamp<int>(c.borderFill, 16, 23) - 16];
        const int sy = std::min<int>(c.line[size_t(k)], kDveLines - 1) * 2 + parity;
        const Rgba8* bg = &background.at(0, y);
        const Rgba8* src = &moving.at(0, sy);
        Rgba8* dst = &out.at(0, y);
        for (int x = 0; x < kRasterW; ++x) {
            const int v = key[x / 4];
            if (v >= 64) {
                dst[x] = colourLine ? border
                                    : src[std::clamp(4 * (255 - v - g.dveSourceCellOffset) + (cellPixel < 0 ? x % 4 : cellPixel),
                                                     0, kRasterW - 1)];
            } else if (v == 0) {
                dst[x] = bg[x];
            } else if (v < 16) {
                const int al = shadowAlpha[std::min(v, 7)];
                const Rgba8 b = bg[x];
                auto dk = [al](int ch) { return uint8_t((ch * (255 - al) + 127) / 255); };
                dst[x] = {dk(b.r), dk(b.g), dk(b.b), b.a};
            } else if (v < 24) {
                dst[x] = kBars[v - 16];
            } else {
                dst[x] = kUnknownGrey;
                ++unknown;
            }
        }
        bool routedPx[kRasterW] = {};  // pixels the level mix drew: the art rule leaves them alone
        if (routed) {
            const uint8_t* index = &c.index[size_t(k) * kDveWidth];
            const auto& routes = *mix->routes;
            for (int x = 0; x < kRasterW; ++x) {
                if (index[x] >= routes.size() || routes[index[x]].source == 0) continue;
                const DveRoute rt = routes[index[x]];
                const Frame* bus = rt.source == 1 ? mix->s1 : rt.source == 3 ? mix->s3 : mix->generator;
                Rgba8 s = mix->colour;
                if (bus) s = bus->at(x, y);
                else if (rt.source != 9) continue;  // no frame for that bus: leave the pixel to the art rule
                const int w = g.dveLevelMix[rt.level & 3];
                const Rgba8 d = dst[x];
                auto m = [w](int sc, int dc) { return uint8_t((sc * (255 - w) + dc * w + 127) / 255); };
                dst[x] = {m(s.r, d.r), m(s.g, d.g), m(s.b, d.b), 255};
                routedPx[x] = true;
            }
        }
        if (!c.art.empty()) {
            const uint8_t* art = &c.art[size_t(k) * kDveWidth];
            for (int x = 0; x < kRasterW; ++x) {
                const size_t idx = size_t(art[x]) * 8;
                if (art[x] != 0 && idx < cmap.size() && !routedPx[x]) dst[x] = {cmap[idx].r, cmap[idx].g, cmap[idx].b, 255};
            }
        }
    }
    return unknown;
}

}  // namespace vt
