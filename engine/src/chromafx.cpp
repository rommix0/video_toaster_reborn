// ChromaFX row builder: a port of AuxLibs/ChromaFX's LUT_Send*DVE routines (addresses in the
// hunk image of tools/re/hunk.py). Mirrors tools/chromafx_rows_ref.py, which was checked against
// the 68000 code run in an emulator for all seven styles, every crouton and every bank slot.
#include "vt/chromafx.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "vt/dv_colour.h"
#include "vt/iff.h"

namespace vt {
namespace {

constexpr int kSteps = kChromaRows;
constexpr int kE = kChromaEntries;
using QuadRow = std::array<DvQuad, kE>;

// ---- 68000 arithmetic ------------------------------------------------------------------------

// divs.w: 32-bit signed / 16-bit signed, quotient truncated toward zero. On overflow the 68000
// leaves the destination unchanged, so the low word of the dividend remains.
uint16_t divsW(uint32_t dividend, int16_t divisor) {
    const int32_t q = int32_t(dividend) / divisor;
    if (q < -0x8000 || q > 0x7fff) return uint16_t(dividend);
    return uint16_t(q);
}

uint16_t divuW(uint32_t dividend, uint16_t divisor) {
    const uint32_t q = dividend / divisor;
    if (q > 0xffff) return uint16_t(dividend);
    return uint16_t(q);
}

// d1 = b - a (sub.l), then asl.w #8 (only the low word shifts), as the step dividend.
uint32_t stepDividend(int a, int b) {
    const auto d = uint32_t(b - a);
    return (d & 0xffff0000u) | uint16_t(d << 8);
}

// The saturation dividend: moveq #0 / move.w / asr.w #8 sign-extends S in the low word only.
uint32_t satDividend(int sa, int sb) {
    auto sx = [](int v) { return uint32_t(v & 0x80 ? v | 0xff00 : v); };
    const uint32_t d = sx(sb) - sx(sa);
    return (d & 0xffff0000u) | uint16_t(d << 8);
}

constexpr int16_t kDiv = 0x1e0;  // 480

// 8.8 interpolator: one word step, one word accumulator (LUT_SendDVEMap's 4-byte entries).
struct Lerp {
    uint16_t step, acc;
    uint8_t next() {
        acc = uint16_t(acc + step);
        return uint8_t(acc >> 8);
    }
};

Lerp lerp(int a, int b) { return {divsW(stepDividend(a, b), kDiv), uint16_t((a << 8) | 0x80)}; }

DvQuad quadOf(int r, int g, int b) { return rgbToDvQuad({uint8_t(r), uint8_t(g), uint8_t(b)}); }

// LUT_LinearRamp (0xa036) written by LUT_CheckLinear (0xa05e) at step 479: longs 4, 6, ... 252,
// then 254, from pixel 0; the colour rows start at pixel 64 = long 16.
QuadRow rampRow() {
    QuadRow row{};
    for (int k = 0; k < kE; ++k) {
        const int n = 16 + k;
        const auto v = uint8_t(n < 125 ? 4 + 2 * n : 254);
        row[size_t(k)] = {v, v, v, v};
    }
    return row;
}

// ---- Spread: LUT_SendDVEMap 0x5b5c -------------------------------------------------------------

std::vector<QuadRow> spread(const LutRecord& r) {
    std::array<Lerp, kE * 3> ch{};
    for (int e = 0; e < kE; ++e) {
        const Rgb a = r.tables[0][size_t(e)], b = r.tables[1][size_t(e)];
        ch[size_t(e * 3)] = lerp(a.r, b.r);
        ch[size_t(e * 3 + 1)] = lerp(a.g, b.g);
        ch[size_t(e * 3 + 2)] = lerp(a.b, b.b);
    }
    std::vector<QuadRow> rows(kSteps - 1);
    for (auto& row : rows)
        for (int e = 0; e < kE; ++e) {
            const int cr = ch[size_t(e * 3)].next(), cg = ch[size_t(e * 3 + 1)].next(), cb = ch[size_t(e * 3 + 2)].next();
            row[size_t(e)] = quadOf(cr, cg, cb);
        }
    return rows;
}

// ---- Spectrum: LUT_SendSpectrumDVE 0x5f00 --------------------------------------------------------

struct Hsi {
    uint16_t h;
    uint8_t s, v;
};

// RGBHSI 0x47c0 (H 16 bits, S and V bytes).
Hsi rgbHsi(Rgb c) {
    const int d2 = (c.r << 4) | 8, d3 = (c.g << 4) | 8, d4 = (c.b << 4) | 8;
    const int mn = std::min({d2, d3, d4}), mx = std::max({d2, d3, d4});
    const int dl = mx - mn;
    uint32_t s;
    if (mx == 0) s = 0;
    else if (mx == dl) s = 0xffff;
    else s = (uint32_t(dl) << 16) / uint32_t(mx);
    const auto v = uint8_t((mx >> 4) & 0xff);
    if (s == 0) return {0, 0, v};
    auto e = [&](int x) { return int((uint32_t(mx - x) << 12) / uint32_t(dl)); };  // RGBHSE 0x4882
    int h;
    if (mx == d2) h = e(d4) - e(d3);
    else if (mx == d3) h = e(d2) - e(d4) + 0x2000;
    else h = e(d3) - e(d2) + 0x4000;
    auto hw = uint16_t(h);
    if (hw & 0x8000) hw = uint16_t(hw + 0x6000);
    const auto hue = uint16_t(((uint32_t(hw) * 0xaaabu) << 2) >> 16);
    return {hue, uint8_t((s >> 8) & 0xff), v};
}

// LUT_LongHue 0x5cca: hue step from H0 to H1 over 480 steps. d7 carries the previous entry's
// difference (low word) and a turn counter (high word) from entry to entry.
uint16_t longHue(uint16_t h1, uint16_t h0, uint32_t& d7) {
    const auto diff = uint16_t(h1 - h0);
    uint16_t hi = uint16_t(d7 >> 16), lo = uint16_t(d7);
    enum { H2, H3, H4, H5, H8 } t = H8;
    if (diff & 0x8000) {
        if (!(lo & 0x8000)) t = diff < 0xc000 ? H4 : H2;
    } else if (lo & 0x8000) {
        t = diff >= 0x4000 ? H4 : H2;
    }
    if (t == H2) {
        if (hi & 2) t = H5;
        else t = hi ? H3 : H8;  // clr.w d7, tst.l d7
    } else if (t == H4) {
        t = (hi & 2) ? H3 : H5;
    }
    if (t == H3) hi = uint16_t(hi + 1);
    else if (t == H5) hi = hi == 0 ? uint16_t(1) : uint16_t(hi - 1);
    lo = diff;
    d7 = (uint32_t(hi) << 16) | lo;
    uint32_t d0 = (uint32_t(uint16_t(int16_t(hi) >> 1)) << 16) + diff;  // asr.l #1, clr.w, add.l
    const bool neg = (hi & 1) ? !(d0 & 0x8000) : bool(d0 & 0x8000);
    if (neg) {
        d0 = (d0 & 0xffff0000u) | uint16_t(-int(d0 & 0xffff));
        return uint16_t(-int(divuW(d0, uint16_t(kDiv))));
    }
    return divuW(d0, uint16_t(kDiv));
}

// HSIRGB 0x4890.
Rgb hsiRgb(uint16_t h, uint8_t s, uint8_t v) {
    const uint32_t d3 = (uint32_t(uint8_t(~s)) << 8) | 0x80;
    const uint32_t d4 = uint32_t(uint16_t(v << 8)) | 0x80;
    const uint32_t prod = uint32_t(h) * 6;
    const uint32_t f = prod & 0xffff, nf = ~prod & 0xffff;
    auto mul = [](uint32_t a, uint32_t b) { return ((a & 0xffff) * (b & 0xffff)) >> 16; };
    uint32_t r = 0, g = 0, b = 0;
    switch (prod >> 16) {
        case 0: r = d4; g = mul(f, d4); b = 0; break;
        case 1: r = mul(nf, d4); g = d4; b = 0; break;
        case 2: r = 0; g = d4; b = mul(f, d4); break;
        case 3: r = 0; g = mul(nf, d4); b = d4; break;
        case 4: r = mul(f, d4); g = 0; b = d4; break;
        default: r = d4; g = 0; b = mul(nf, d4); break;
    }
    r = uint16_t(r + mul(uint16_t(d4 - r), d3));
    g = uint16_t(g + mul(uint16_t(d4 - g), d3));
    b = uint16_t(b + mul(uint16_t(d4 - b), d3));
    return {uint8_t(r >> 8), uint8_t(g >> 8), uint8_t(b >> 8)};
}

std::vector<QuadRow> spectrum(const LutRecord& r) {
    struct Entry {
        uint16_t hStep, h;
        Lerp s, v;
    };
    std::array<Entry, kE> en{};
    uint32_t d7 = 0;
    for (int e = 0; e < kE; ++e) {
        const Hsi a = rgbHsi(r.tables[0][size_t(e)]), b = rgbHsi(r.tables[1][size_t(e)]);
        const uint16_t hs = longHue(b.h, a.h, d7);
        en[size_t(e)] = {hs, a.h, {divsW(satDividend(a.s, b.s), kDiv), uint16_t((a.s << 8) | 0x80)}, lerp(a.v, b.v)};
    }
    std::vector<QuadRow> rows(kSteps - 1);
    for (auto& row : rows)
        for (int e = 0; e < kE; ++e) {
            Entry& x = en[size_t(e)];
            x.h = uint16_t(x.h + x.hStep);
            const uint8_t s = x.s.next(), v = x.v.next();
            const Rgb c = hsiRgb(x.h, s, v);
            row[size_t(e)] = quadOf(c.r, c.g, c.b);
        }
    return rows;
}

// ---- Rotate, Nuke, Poster: table 0 as quads ---------------------------------------------------------

QuadRow tableQuads(const std::array<Rgb, kE>& t) {
    QuadRow q{};
    for (int e = 0; e < kE; ++e) q[size_t(e)] = rgbToDvQuad(t[size_t(e)]);
    return q;
}

// d6 per step (16.16 offset): step n uses d6 after n additions; a negative result gets `wrap`.
std::vector<int> offsets(int32_t start, int32_t inc, int32_t wrap) {
    std::vector<int> out;
    int32_t d6 = start;
    for (int n = 0; n < kSteps - 1; ++n) {
        out.push_back(int(uint32_t(d6) >> 16));
        d6 += inc;
        if (d6 < 0) d6 += wrap;
    }
    return out;
}

// LUT_SendMapRotate 0x6216 + RotateLong 0x4b66: out[i] = src[(i + r) mod 96], r += 0.2 per step
// (0x3334); d0 = 0 (DORROT, "right") starts at 0xffff and subtracts, d0 = -1 (DOLROT) adds.
std::vector<QuadRow> rotate(const LutRecord& r, bool left) {
    const QuadRow src = tableQuads(r.tables[0]);
    const auto offs = left ? offsets(0, 0x3334, 0x600000) : offsets(0xffff, -0x3334, 0x600000);
    std::vector<QuadRow> rows;
    for (int o : offs) {
        QuadRow row{};
        for (int i = 0; i < kE; ++i) row[size_t(i)] = src[size_t((i + o) % kE)];
        rows.push_back(row);
    }
    return rows;
}

// LUT_SendMapNuke 0x64ce + NukeLong 0x4c22: the table read forwards then backwards (period 192),
// r += 0.4 per step (0x6667).
std::vector<QuadRow> nuke(const LutRecord& r, bool left) {
    const QuadRow src = tableQuads(r.tables[0]);
    const auto offs = left ? offsets(0, 0x6667, 0xc00000) : offsets(0xffff, -0x6667, 0xc00000);
    auto pal = [&](int k) { return k < kE ? src[size_t(k)] : src[size_t(2 * kE - 1 - k)]; };
    std::vector<QuadRow> rows;
    for (int o : offs) {
        QuadRow row{};
        for (int i = 0; i < kE; ++i) row[size_t(i)] = pal((i + o) % (2 * kE));
        rows.push_back(row);
    }
    return rows;
}

// LUT_STEPQUANTIZE 0x817f.
constexpr int kStepQuantize[14] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 48};

// LUT_DoQuadQuantize 0x810c (d2 = 14): blocks of n entries take their middle entry; the
// remainder is split before (rem / 2) and after.
QuadRow quadQuantize(const QuadRow& src, int q) {
    const int n = kStepQuantize[uint16_t(q * 14) >> 8];
    const int blocks = kE / n, rem = kE - n * blocks, lead = rem >> 1, tail = rem - lead;
    QuadRow out{};
    int o = 0;
    for (int i = 0; i < lead; ++i) out[size_t(o++)] = src[size_t(lead >> 1)];
    int base = lead;
    for (int b = 0; b < blocks; ++b, base += n)
        for (int i = 0; i < n; ++i) out[size_t(o++)] = src[size_t(base + (n >> 1))];
    for (int i = 0; i < tail; ++i) out[size_t(o++)] = src[size_t(base + (tail >> 1))];
    return out;
}

// LUT_QuadAverageQuantize 0x80a4: each byte averaged with the same-phase bytes of its neighbours.
QuadRow quadAverage(const QuadRow& t) {
    QuadRow out{};
    for (int e = 0; e < kE; ++e)
        for (size_t k = 0; k < 4; ++k) {
            int v;
            if (e == 0) v = (t[0][k] + t[1][k]) >> 1;
            else if (e == kE - 1) v = (t[size_t(e)][k] + t[size_t(e - 1)][k]) >> 1;
            else v = (t[size_t(e)][k] + t[size_t(e + 1)][k] + t[size_t(e - 1)][k]) / 3;
            out[size_t(e)][k] = uint8_t(v);
        }
    return out;
}

// LUT_SendPosterDVE 0x6798: level q = d6 >> 16 with d6 += 0x8888 per step; from q 54 the
// quantized row is smoothed, from q 18 the output is the mean of the last four rows.
std::vector<QuadRow> poster(const LutRecord& r) {
    const QuadRow src = tableQuads(r.tables[0]);
    std::array<QuadRow, 3> hist{};  // h1, h2, h3
    std::vector<QuadRow> rows;
    uint32_t d6 = 0;
    for (int n = 0; n < kSteps - 1; ++n, d6 += 0x8888) {
        QuadRow h0 = quadQuantize(src, int(d6 >> 16));
        if (d6 >= 0x360000) h0 = quadAverage(h0);
        if (d6 >= 0x120000) {
            QuadRow out{};
            for (size_t e = 0; e < size_t(kE); ++e)
                for (size_t k = 0; k < 4; ++k)
                    out[e][k] = uint8_t((h0[e][k] + hist[0][e][k] + hist[1][e][k] + hist[2][e][k]) >> 2);
            rows.push_back(out);
        } else {
            rows.push_back(h0);
        }
        hist = {h0, hist[0], hist[1]};
    }
    return rows;
}

// ---- parsing ------------------------------------------------------------------------------------

std::string latin1Name(std::span<const uint8_t> b) {
    std::string s;
    for (uint8_t c : b) {
        if (c == 0) break;
        if (c < 0x80) s += char(c);
        else {
            s += char(0xc0 | (c >> 6));
            s += char(0x80 | (c & 0x3f));
        }
    }
    const char* ws = " \t\n\r\v\f";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

constexpr uint32_t kTagTBar = 0x80000086, kTagColour = 0x8000007e, kTagCycle = 0x8000007f,
                   kTagTransition = 0x80000080, kTagSpeed = 0x80000055;

int wrapV(int v) { return ((v % kChromaCycle) + kChromaCycle) % kChromaCycle; }

}  // namespace

LutRecord parseLut(std::span<const uint8_t> b) {
    if (b.size() < size_t(kLutRecordSize) || b[0] != 'L' || b[1] != 'U' || b[2] != 'T' || b[3] != 1)
        throw std::runtime_error("not a ChromaFX LUT record");
    LutRecord r;
    r.name = latin1Name(b.subspan(24, 16));
    r.poster = b[4];
    r.interp = b[5];
    r.flags = b[6];
    r.tbar = uint16_t((b[8] << 8) | b[9]);
    for (size_t t = 0; t < 3; ++t)
        for (size_t e = 0; e < size_t(kE); ++e) {
            const size_t o = 40 + t * 288 + e * 3;
            r.tables[t][e] = {b[o], b[o + 1], b[o + 2]};
        }
    return r;
}

std::vector<LutRecord> readLutBank(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    const std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    std::vector<LutRecord> out;
    for (size_t o = 32; o + kLutRecordSize <= d.size(); o += kLutRecordSize) {
        const std::span<const uint8_t> rec(d.data() + o, size_t(kLutRecordSize));
        if (rec[0] != 'L' || rec[1] != 'U' || rec[2] != 'T' || rec[3] != 1) continue;
        LutRecord r = parseLut(rec);
        if (!r.name.empty()) out.push_back(std::move(r));
    }
    return out;
}

std::optional<LutRecord> lutFromCrouton(const Crouton& c) {
    for (const auto& form : c.forms) {
        if (form.type != std::array<char, 4>{'C', 'r', 'U', 'D'}) continue;
        for (const auto& ch : formChunks(c.bytes, form))
            if (ch.id == std::array<char, 4>{' ', 'L', 'U', 'T'})
                return parseLut(std::span<const uint8_t>(c.bytes).subspan(ch.data, ch.size));
    }
    return std::nullopt;
}

ChromaInterp chromaInterp(const LutRecord& r) { return r.interp < 7 ? ChromaInterp(r.interp) : ChromaInterp::Spread; }

ChromaSettings chromaSettings(const Crouton& c, const LutRecord& r) {
    ChromaSettings s;
    s.interp = chromaInterp(r);
    s.mode = c.tagOr(kTagTransition) ? ChromaMode::Transition : ChromaMode::Filter;
    s.cycle = ChromaCycle(c.tagOr(kTagCycle) & 3);
    s.colour = c.tagOr(kTagColour) != 0;
    s.speed = decodeSpeedTag(c.tagOr(kTagSpeed));
    s.tbar = int(c.tagOr(kTagTBar) & 0xffff);
    return s;
}

ChromaSettings chromaSettings(const LutRecord& r) {
    ChromaSettings s;
    s.interp = chromaInterp(r);
    s.mode = (r.flags & 0x20) ? ChromaMode::Transition : ChromaMode::Filter;
    s.cycle = ChromaCycle((r.flags >> 2) & 3);
    s.colour = (r.flags & 0x80) != 0;
    // LUT_ReadNewTags: tag 3 (Slow) -> 0, tag 0 (Medium) -> 1, anything else -> 2; 3 is never written.
    static constexpr Speed kBits[4] = {Speed::Slow, Speed::Medium, Speed::Fast, Speed::Medium};
    s.speed = kBits[r.flags & 3];
    s.tbar = r.tbar;
    return s;
}

ChromaRows buildRows(const LutRecord& r, ChromaInterp in) {
    std::vector<QuadRow> q;
    switch (in) {
        case ChromaInterp::Spread: q = spread(r); break;
        case ChromaInterp::Spectrum: q = spectrum(r); break;
        case ChromaInterp::RotateRight: q = rotate(r, false); break;
        case ChromaInterp::RotateLeft: q = rotate(r, true); break;
        case ChromaInterp::NukeRight: q = nuke(r, false); break;
        case ChromaInterp::NukeLeft: q = nuke(r, true); break;
        case ChromaInterp::Poster: q = poster(r); break;
    }
    q.push_back(rampRow());
    ChromaRows out;
    out.rows.resize(q.size());
    for (size_t i = 0; i < q.size(); ++i)
        for (size_t e = 0; e < size_t(kE); ++e) out.rows[i][e] = dvQuadToRgb(q[i][e]);
    return out;
}

int chromaRowStart(int tbar) {
    const int t = std::clamp(tbar, 0, 511);
    return std::max(0, (t >> 2) - 9);
}

std::array<uint8_t, 241> chromaLineTable(ChromaMode mode, int v) {
    v = wrapV(v);
    std::array<uint8_t, 241> t{};
    if (mode == ChromaMode::Transition) {
        const auto d0 = uint8_t(2 * v + 2);
        for (size_t k = 0; k < t.size(); ++k) t[k] = (k % 2 == 0) ? d0 : uint8_t(d0 + 1);
    } else {
        int d0 = 2 * v + 2, d1 = d0 - 1;
        for (size_t k = 1; k + 1 < t.size(); k += 2) {
            t[k] = uint8_t(d1);
            t[k + 1] = uint8_t(d0);
            d1 += 2;
            d0 += 2;
            if (d0 > 0xee) {
                d1 = 1;
                d0 = 2;
            }
        }
    }
    t[0] = 0xf0;
    return t;
}

int rowForLine(ChromaMode mode, int tbar, int line, int cycleOffset) {
    if (line < 0 || line >= kRasterH) return -1;
    const auto t = chromaLineTable(mode, chromaRowStart(tbar) + wrapV(cycleOffset));
    const int row = 2 * t[size_t(line / 2)] + (line & 1);
    return row < kChromaRows ? row : -1;
}

uint32_t chromaCycleStep(Speed s) {
    static constexpr uint32_t kTable[4] = {0x4000, 0x8000, 0x10000, 0x8000};  // 0x9b14
    uint32_t tag;  // the speed in tag 0x55 encoding ($1f4(a5))
    switch (s) {
        case Speed::Medium: tag = 0; break;
        case Speed::Fast: tag = 1; break;
        case Speed::Variable: tag = 2; break;
        default: tag = 3; break;
    }
    return kTable[(tag + 1) & 3];
}

int chromaCycleRow(ChromaCycle c, uint32_t step, uint64_t fields) {
    constexpr uint64_t kPeriod = uint64_t(kChromaCycle) << 16;
    // Exact for steps up to 0x10000 (one row per field): the routines' wraps then always land
    // on 119 (or 238, or -1) whole rows, which is the modulo below.
    switch (c) {
        case ChromaCycle::Forward: return int(((fields % kPeriod) * step % kPeriod) >> 16);
        case ChromaCycle::Backward: {
            const uint64_t a = (fields % kPeriod) * step % kPeriod;
            return int(((kPeriod - a) % kPeriod) >> 16);
        }
        case ChromaCycle::Both: {
            const uint64_t h = ((fields % (2 * kPeriod)) * step % (2 * kPeriod)) >> 16;
            return h < uint64_t(kChromaCycle) ? int(h) : int(0xed - h);
        }
        default: return 0;
    }
}

void applyChroma(const ChromaRows& rows, ChromaMode mode, int tbar, int cycleOffset, const Frame& in, Frame& out,
                 float lumaOffset) {
    out.px.resize(in.px.size());
    const auto t = chromaLineTable(mode, chromaRowStart(tbar) + wrapV(cycleOffset));
    const double off = lumaOffset;
    for (int y = 0; y < kRasterH; ++y) {
        const int row = 2 * t[size_t(y / 2)] + (y & 1);
        Rgba8* o = &out.at(0, y);
        const Rgba8* i = &in.at(0, y);
        if (row >= int(rows.rows.size())) {
            std::fill(o, o + kRasterW, Rgba8{0, 0, 0, 255});
            continue;
        }
        const auto& map = rows.rows[size_t(row)];
        for (int x = 0; x < kRasterW; ++x) {
            const int ysum = 299 * i[x].r + 587 * i[x].g + 114 * i[x].b;  // 1000 x Rec.601 luma
            const double e = std::floor(off + ysum / 4000.0);
            const int entry = e < 0 ? 0 : (e > kE - 1 ? kE - 1 : int(e));
            o[x] = map[size_t(entry)];
        }
    }
}

}  // namespace vt
