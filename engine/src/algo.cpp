// ALGO VM: a literal port of tools/ref/algo_ref.py (switcher.main 4.2). See vt/algo.h. Each
// helper carries its Python counterpart's name and 68000 address; arithmetic keeps the
// reference's 16-bit / 32-bit wraps and signed / unsigned reads.
#include "vt/algo.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>

namespace vt {
namespace {

constexpr uint32_t M16 = 0xffff;

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("ALGO: ") + what); }

int s16(int64_t x) { return int16_t(uint16_t(x)); }
int64_t s32(uint32_t x) { return int32_t(x); }

// Big-endian reads of payload / parameter bytes (algo_ref L / W); out of range is malformed.
struct Bytes {
    const uint8_t* d;
    size_t n;
    uint32_t W(int64_t o) const {
        if (o < 0 || uint64_t(o) + 2 > n) fail("malformed program");
        return uint32_t(d[o] << 8 | d[o + 1]);
    }
    uint32_t L(int64_t o) const { return W(o) << 16 | W(o + 2); }
};

// A channel's word block (algo_ref Chan): words by byte offset.
struct Chan {
    std::array<uint8_t, 0x36>& b;
    unsigned w(int o) const { return unsigned(b[size_t(o)] << 8 | b[size_t(o) + 1]); }
    int operator[](int o) const { return s16(w(o)); }
    uint32_t l(int o) const { return uint32_t(w(o)) << 16 | w(o + 2); }
    void set(int o, int v) {
        b[size_t(o)] = uint8_t(v >> 8);
        b[size_t(o) + 1] = uint8_t(v);
    }
    void setl(int o, uint32_t v) {
        set(o, int(v >> 16));
        set(o + 2, int(v));
    }
};

// Python indexing of the H / V tables and channel bytes: a negative index counts from the end.
uint8_t& at(std::span<uint8_t> a, int64_t i) {
    if (i < 0) i += int64_t(a.size());
    if (i < 0 || i >= int64_t(a.size())) fail("program indexes past a table");
    return a[size_t(i)];
}

// ------------------------------------------------------------------ fixed-point helpers

// mul_t 0x7873e: 16.16 value x unsigned fraction t (t >= 0x10000 returns d0 unchanged).
uint32_t mulT(uint32_t d0, uint32_t t) {
    if (t >> 16) return d0;
    const int64_t lo = d0 & M16, hi = s16(d0 >> 16);
    return uint32_t(((lo * t + 0x8000) >> 16) + hi * int64_t(t));
}

// mul_ff 0x786f0: signed 16.16 x 16.16 with the low product rounded.
uint32_t mulFF(uint32_t d0, uint32_t d1) {
    const int64_t lo0 = d0 & M16, hi0 = s16(d0 >> 16), lo1 = d1 & M16, hi1 = s16(d1 >> 16);
    return uint32_t(((lo0 * lo1 + 0x8000) >> 16) + hi1 * lo0 + ((hi0 * hi1) << 16) + hi0 * lo1);
}

// mul_fi 0x78776: 16.16 x signed word (no rounding).
uint32_t mulFI(uint32_t d0, int d1w) {
    const int64_t d1 = s16(d1w);
    return uint32_t(d1 * (d0 & M16) + ((s16(d0 >> 16) * d1) << 16));
}

// div_ff 0x7879a: signed word / signed word -> 16.16 (truncated toward zero).
uint32_t divFF(int aw, int bw) {
    int64_t a = s16(aw), b = s16(bw);
    if (b == 0) return a == 0 ? 0 : 0x7fffffff;  // only reachable with a >= 0
    const bool neg = (a < 0) != (b < 0);
    a = std::abs(a);
    b = std::abs(b);
    const int64_t q = a / b, r = a % b, q2 = (r << 16) / b;
    if (!neg) return uint32_t((q << 16) | q2);
    const int64_t hi = (-q - (r ? 1 : 0)) & M16;
    return uint32_t((hi << 16) | (-q2 & M16));
}

// store 0x78c3a: 16.16 -> word, rounded, except a high word of 0x7fff.
int store(uint32_t v) {
    if ((v >> 16) == 0x7fff) return 0x7fff;
    return int(((uint64_t(v) + 0x8000) >> 16) & M16);
}

// Quarter-wave sine table, 256 words at switcher.main 0x7a082 (generated once from the hunk).
constexpr uint16_t kSine[256] = {
    0x0000, 0x0192, 0x0324, 0x04b6, 0x0648, 0x07da, 0x096c, 0x0afe, 0x0c8f, 0x0e21, 0x0fb2, 0x1144,
    0x12d5, 0x1466, 0x15f6, 0x1787, 0x1917, 0x1aa7, 0x1c37, 0x1dc7, 0x1f56, 0x20e5, 0x2273, 0x2402,
    0x2590, 0x271d, 0x28aa, 0x2a37, 0x2bc4, 0x2d50, 0x2edb, 0x3066, 0x31f1, 0x337b, 0x3505, 0x368e,
    0x3817, 0x399f, 0x3b26, 0x3cad, 0x3e33, 0x3fb9, 0x413e, 0x42c3, 0x4447, 0x45ca, 0x474d, 0x48ce,
    0x4a50, 0x4bd0, 0x4d50, 0x4ecf, 0x504d, 0x51ca, 0x5347, 0x54c3, 0x563e, 0x57b8, 0x5931, 0x5aaa,
    0x5c22, 0x5d98, 0x5f0e, 0x6083, 0x61f7, 0x636a, 0x64dc, 0x664d, 0x67bd, 0x692d, 0x6a9b, 0x6c08,
    0x6d74, 0x6edf, 0x7049, 0x71b1, 0x7319, 0x7480, 0x75e5, 0x774a, 0x78ad, 0x7a0f, 0x7b70, 0x7cd0,
    0x7e2e, 0x7f8b, 0x80e7, 0x8242, 0x839c, 0x84f4, 0x864b, 0x87a1, 0x88f5, 0x8a48, 0x8b9a, 0x8cea,
    0x8e39, 0x8f87, 0x90d3, 0x921e, 0x9368, 0x94b0, 0x95f6, 0x973c, 0x987f, 0x99c2, 0x9b02, 0x9c42,
    0x9d7f, 0x9ebc, 0x9ff6, 0xa12f, 0xa267, 0xa39d, 0xa4d2, 0xa605, 0xa736, 0xa866, 0xa994, 0xaac0,
    0xabeb, 0xad14, 0xae3b, 0xaf61, 0xb085, 0xb1a8, 0xb2c8, 0xb3e7, 0xb504, 0xb620, 0xb73a, 0xb852,
    0xb968, 0xba7c, 0xbb8f, 0xbca0, 0xbdae, 0xbebc, 0xbfc7, 0xc0d0, 0xc1d8, 0xc2de, 0xc3e2, 0xc4e3,
    0xc5e4, 0xc6e2, 0xc7de, 0xc8d8, 0xc9d1, 0xcac7, 0xcbbb, 0xccae, 0xcd9f, 0xce8d, 0xcf7a, 0xd064,
    0xd14d, 0xd233, 0xd318, 0xd3fa, 0xd4db, 0xd5b9, 0xd695, 0xd770, 0xd848, 0xd91e, 0xd9f2, 0xdac4,
    0xdb94, 0xdc61, 0xdd2d, 0xddf6, 0xdebe, 0xdf83, 0xe046, 0xe106, 0xe1c5, 0xe282, 0xe33c, 0xe3f4,
    0xe4aa, 0xe55e, 0xe60f, 0xe6be, 0xe76b, 0xe816, 0xe8bf, 0xe965, 0xea09, 0xeaab, 0xeb4b, 0xebe8,
    0xec83, 0xed1c, 0xedb2, 0xee46, 0xeed8, 0xef68, 0xeff5, 0xf080, 0xf109, 0xf18f, 0xf213, 0xf295,
    0xf314, 0xf391, 0xf40b, 0xf484, 0xf4fa, 0xf56d, 0xf5de, 0xf64d, 0xf6ba, 0xf724, 0xf78b, 0xf7f1,
    0xf854, 0xf8b4, 0xf912, 0xf96e, 0xf9c7, 0xfa1e, 0xfa73, 0xfac5, 0xfb14, 0xfb62, 0xfbac, 0xfbf5,
    0xfc3b, 0xfc7e, 0xfcbf, 0xfcfe, 0xfd3a, 0xfd74, 0xfdab, 0xfde0, 0xfe13, 0xfe43, 0xfe70, 0xfe9b,
    0xfec4, 0xfeea, 0xff0e, 0xff2f, 0xff4e, 0xff6a, 0xff84, 0xff9c, 0xffb1, 0xffc3, 0xffd3, 0xffe1,
    0xffec, 0xfff4, 0xfffb, 0xfffe,
};

// sine 0x7a004: sine of the low word (0x10000 = one turn) -> 16.16.
uint32_t sine(uint32_t d0) {
    int d3 = 0;
    uint32_t x = d0 & M16;
    if (d0 & 0x80000000u) {
        x = (0u - x) & M16;
        d3 = 1;
    }
    if (x & 0x8000) {
        x = (0u - x) & M16;
        if (x == 0x8000) return 0;
        d3 ^= 1;
    }
    uint32_t d1 = x;
    if (x == 0x4000) return d3 ? 0xffff0000u : 0x10000u;
    if (x > 0x4000) d1 = (0x8000 - x) & M16;
    const uint32_t d2 = d1 & 0x3f, i = d1 >> 6;
    uint32_t v = kSine[i];
    const uint32_t diff = i == 255 ? (0u - v) & M16 : (kSine[i + 1] - v) & M16;
    v = (v + (((diff * d2) & M16) >> 6)) & M16;
    return d3 ? 0u - v : v;
}

// ------------------------------------------------------------------ opcodes (slot animators)

// The common tail of op_lin / op_window: |d0| clamped to [L@o+8, L@o+12], keeping d0's sign.
uint32_t clampAbs(uint32_t d0, const Bytes& p, int o) {
    uint32_t a = uint32_t(std::abs(s32(d0)));
    const int64_t lo = s32(p.L(o + 8)), hi = s32(p.L(o + 12));
    if (s32(a) < lo)
        a = uint32_t(lo);
    else if (s32(a) > hi)
        a = uint32_t(hi);
    else
        return d0;
    return s32(d0) < 0 ? 0u - a : a;
}

// op_lin 0x787fc (op 3): L0 + L1*t, |result| clamped to [L2, L3].
uint32_t opLin(const Bytes& p, uint32_t t, int o = 0) {
    return clampAbs(mulT(p.L(o + 4), t) + p.L(o), p, o);
}

// op_window 0x7889c (op 6): op_lin over the T-bar window [W@16, W@18) (0 = 1.0).
uint32_t opWindow(const Bytes& p, uint32_t t) {
    uint32_t d0;
    const uint32_t d1 = p.W(16);
    if (t < d1) {
        d0 = 0;
    } else {
        uint32_t d2 = p.W(18);
        if (d2 && t >= d2) {
            d0 = p.L(4);
        } else {
            if (!d2) d2 = 0x10000;
            d2 -= d1;
            if (d2 == 0) {
                d0 = 0;
            } else {
                const uint32_t f = t - d1;
                d0 = p.L(4);
                if (d2 >= 0x10000)
                    d0 = mulT(d0, f);
                else if ((d2 & M16) != (f & M16))
                    d0 = mulT(d0, uint32_t((uint64_t(f) << 16) / d2) & M16);
            }
        }
    }
    return clampAbs(d0 + p.L(0), p, 0);
}

struct Value {
    uint32_t v;
    int word;  // >= 0: the word itself is stored (op 7), else store(v)
};

// evaluate: value (16.16) of opcode n with params p at t.
Value evaluate(int n, const Bytes& p, uint32_t t) {
    switch (n) {
        case 3: return {opLin(p, t), -1};
        case 4: return {mulT(opLin(p, t), t) + p.L(16), -1};  // 0x78838
        case 5: {                                              // 0x7885e
            const uint32_t ph = mulT(opLin(p, t), t) + p.L(16);
            return {mulFF(opLin(p, t, 20), sine(ph)) + p.L(36), -1};
        }
        case 6: return {opWindow(p, t), -1};
        case 7: {  // 0x78922: step table, stores the word itself
            int v;
            if (t >= 0xffff) {
                v = int(p.W(int(p.W(0)) - 4));
            } else {
                int o = 6;
                while (t > p.W(o)) o += 4;
                v = int(t == p.W(o) ? p.W(o + 2) : p.W(o - 2));
            }
            return {uint32_t(v) << 16, v};
        }
        case 8: {  // 0x7895c: piecewise linear (key, from, to) records, run as an op-6 block
            int o;
            if (t >= 0xffff) {
                o = int(p.W(0)) - 2;
            } else {
                o = 0xc;
                while (t > p.W(o)) o += 0xa;
                if (t == p.W(o)) o += 0xa;
            }
            uint32_t end = p.W(o);
            if (end == 0xffff) end = 0;
            // struct.pack('>IIIIHH', from, to - from, 0, 0x7fffffff, previous key, end)
            const uint32_t from = p.L(o - 8);
            const uint32_t longs[4] = {from, p.L(o - 4) - from, 0, 0x7fffffff};
            const uint32_t words[2] = {p.W(o - 0xa), end};
            uint8_t blk[20];
            for (int i = 0; i < 16; ++i) blk[i] = uint8_t(longs[i / 4] >> (24 - 8 * (i % 4)));
            for (int i = 0; i < 4; ++i) blk[16 + i] = uint8_t(words[i / 2] >> (8 - 8 * (i % 2)));
            return {opWindow(Bytes{blk, sizeof blk}, t), -1};
        }
        // 9..17: not used by ALGO croutons (9 and 17 by the four deep-colour bar ILBM/ANIM croutons)
        case 9: return {mulT(p.L(0), t), -1};                     // 0x789b6: L * t
        case 10: return {mulT(mulT(p.L(0), t), t), -1};           // 0x789c8: L * t^2
        case 13: return {mulT(mulT(mulT(p.L(0), t), t), t), -1};  // 0x789de: L * t^3
        case 11: return {mulFF(mulT(p.L(0), t), 0x20000 - t), -1};  // 0x789f8: L * t * (2 - t)
        case 14: {                                                // 0x78a16: (L/2 * t) * (3 - t^2)
            const uint32_t d2 = 0x30000 - mulT(t, t);
            return {mulFF(mulT(uint32_t(s32(p.L(0)) >> 1), t), d2), -1};
        }
        case 12: {  // 0x78a42: L/2 * (1 + sin(t/2 + 3/4 turn))
            const uint32_t s = sine(mulT(0x8000, t) + 0xc000) + 0x10000;
            return {mulFF(s, uint32_t(s32(p.L(0)) >> 1)), -1};
        }
        case 17: return {p.L(0), -1};  // 0x78b02: constant long
        default: fail("opcode not supported");  // refused at install
    }
}

// op_len (algo_scan, the skipper 0x75f78): parameter bytes of the opcodes the VM plays.
constexpr int kFixed[18] = {0, 0, 2, 16, 20, 40, 20, -1, -1, 4, 4, 4, 4, 4, 4, -1, -1, 4};

int chanSize(int n) { return n <= 10 ? 0x36 : n == 11 || n == 12 ? 8 : n <= 22 ? 0x18 : 0x0c; }

// ------------------------------------------------------------------ geometry and drawing

// _empty
void empty(Chan& c) {
    for (int o : {0x20, 0x24, 0x22, 0x26}) c.set(o, -1);
    c.set(0x32, 0);
    c.set(0x34, 0);
}

// geometry 0x78000: place source [$6..$8] (base $a at anchor $10+$4) at size $12, clip to [$c..$e].
void geometry(Chan& c, bool vertical) {
    const int d4 = s16(c[0x10] + c[4]);
    if (c[0x12] == 0) {
        c.set(0x1c, d4);
        c.set(0x1e, d4);
        return empty(c);
    }
    int d5 = c[0xa];
    if (vertical && ((d5 ^ d4) & 1)) d5 = s16(d5 + 1);
    int d0 = c[0x12], d1 = c[8], d2 = d1;
    d1 = s16(d1 - c[6]);
    if (d1 < 0) {
        d2 = c[6];
        d0 = s16(-d0);
        d1 = s16(-d1);
    }
    d1 = s16(d1 + 1);
    const int d7 = d0, d6 = d1;
    const uint32_t sc = divFF(d0, d1);
    const uint32_t p = mulFF(sc, ((uint32_t(d2 - d5) & M16) << 16) | 0x8000);
    int e = s16(int(p >> 16) + d4 + (d7 < 0 ? 1 : 0));
    int s = s16(e - d7 + 1);
    if (d7 < 0) {
        s = s16(s - 2);
        std::swap(e, s);
    }
    c.set(0x1e, e);
    c.set(0x1c, s);
    const int lo = c[0xc], hi = c[0xe];
    int d3;
    if (d4 < lo) {
        d3 = lo;
        c.set(0x20, d3);
        c.set(0x24, d3);
        c.set(0x28, d3);
        d2 = c[0x1e];
        if (d2 > hi) {
            c.set(0x22, hi);
            c.set(0x26, hi);
            d2 = s16(hi - d3);
        } else {
            c.set(0x22, d2);
            c.set(0x26, d2);
            d2 = s16(d2 - d3);
            if (d2 < 0) return empty(c);
        }
        c.set(0x32, d2 + 1);
        c.set(0x34, 0);
        d3 = s16(d3 - d4);
    } else if (d4 > hi) {
        d3 = hi;
        c.set(0x28, d3);
        d2 = c[0x1c];
        if (d2 >= lo) {
            c.set(0x22, d3);
            c.set(0x26, d3);
            c.set(0x20, d2);
            c.set(0x24, d2);
            d2 = s16(d2 - d3);
            if (d2 > 0) return empty(c);
            d2 = s16(-d2);
        } else {
            c.set(0x22, d3);
            c.set(0x26, d3);
            c.set(0x20, lo);
            c.set(0x24, lo);
            d2 = s16(d3 - lo);
        }
        c.set(0x34, d2 + 1);
        c.set(0x32, 0);
        d3 = s16(d3 - d4);
    } else {
        c.set(0x28, d4);
        d2 = c[0x1e];
        if (d2 > hi) d2 = hi;
        c.set(0x22, d2);
        c.set(0x26, d2);
        c.set(0x32, d2 - d4 + 1);
        d3 = c[0x1c];
        if (d3 < lo) d3 = lo;
        c.set(0x20, d3);
        c.set(0x24, d3);
        c.set(0x34, d4 - d3 + 1);
        d3 = 0;
    }
    const uint32_t step = divFF(d6, d7);
    c.setl(0x2e, step);
    uint32_t base = (uint32_t(d5) & M16) << 16;
    if (d3) base += mulFI(step, d3);
    c.setl(0x2a, base);
}

// _fill
void fill(std::span<uint8_t> buf, int start, int n, int v) {
    for (int i = 0; i < n; ++i) at(buf, int64_t(start) + i) = uint8_t(v);
}

// _edgefix 0x782ec: replicate the first video cell (<= 0xbd) into cells 0..1 (on buf or struct!).
void edgefix(std::span<uint8_t> a) {
    int d0 = 2;
    if (a[2] > 0xbd) return;
    uint8_t d2 = a[2];
    for (;;) {
        if (--d0 < 0) return;
        if (a[size_t(d0)] > 0xbd) break;
        d2 = a[size_t(d0)];
    }
    for (; d0 >= 0; --d0) a[size_t(d0)] = d2;
}

// _margins 0x781f8 / 0x783ee: $0 / $2 margins with $16 (negative = drawn inside the picture).
template <class Put>
void margins(Chan& c, Put put) {
    const int lo = c[0xc], hi = c[0xe], d5 = c[0x1c], d6 = c[0x1e];
    int d1 = d5, d2 = c[0];
    if (d2 < 0) {
        if (c[0x12])
            for (int i = 0; i < -d2; ++i) {
                if (d1 > hi || d1 > d6) break;
                if (d1 >= lo) put(d1);
                ++d1;
            }
    } else {
        --d1;
        for (int i = 0; i < d2; ++i) {
            if (d1 < lo) break;
            if (d1 <= hi) {
                c.set(0x24, d1);
                put(d1);
            }
            --d1;
        }
        if (c[0x24] >= 0 && c[0x26] < 0) c.set(0x26, hi);
    }
    d1 = d6;
    d2 = c[2];
    if (d2 < 0) {
        if (c[0x12])
            for (int i = 0; i < -d2; ++i) {
                if (d1 < lo || d1 < d5) break;
                if (d1 <= hi) put(d1);
                --d1;
            }
    } else {
        if (c[0x12]) ++d1;
        for (int i = 0; i < d2; ++i) {
            if (d1 > hi) break;
            if (d1 >= lo) {
                c.set(0x26, d1);
                put(d1);
            }
            ++d1;
        }
        if (c[0x26] >= 0 && c[0x24] < 0) c.set(0x24, lo);
    }
}

// draw_h 0x781a8: horizontal cell row.
void drawH(Chan& c, std::span<uint8_t> buf) {
    const int d2 = c[0x14];
    if (d2 == 0) {
        int o = c[0x28];
        uint32_t d0 = c.l(0x2a) + 0x8000, d1 = d0;
        const uint32_t st = c.l(0x2e);
        for (int i = 0, n = c[0x32] & int(M16); i < n; ++i) {
            at(buf, o++) = uint8_t(d0 >> 16);
            d0 += st;
        }
        o = c[0x28] + 1;
        for (int i = 0, n = c[0x34] & int(M16); i < n; ++i) {
            at(buf, --o) = uint8_t(d1 >> 16);
            d1 -= st;
        }
    } else if (d2 > 0 && c[0x24] >= 0) {
        fill(buf, c[0x24], c[0x26] - c[0x24] + 1, d2);
    }
    if (c[0x16] >= 0) {
        const int v = c[0x16];
        margins(c, [&](int i) { at(buf, i) = uint8_t(v); });
    }
    const int bg = c[0x18];
    if (bg < 0) {
        edgefix(c.b);  // quirk: a0 is still the channel block here
        return;
    }
    int d1 = c[0x24];
    if (d1 < 0) d1 = c[0xe] + 1;
    fill(buf, c[0xc], d1 - c[0xc], bg);
    if (c[0x26] >= 0) fill(buf, c[0x26] + 1, c[0xe] - c[0x26], bg);
    edgefix(buf);
}

// _par: bit 0 of the line value follows the parity of the line index (0x7839a).
int par(int v, int i) { return i & 1 ? v | 1 : (v + 1) & ~1; }

// _pair: (even, odd) members of a line-value pair: v and v+1 ordered by parity.
std::pair<int, int> pairOf(int v) { return v & 1 ? std::pair{v + 1, v} : std::pair{v, v + 1}; }

// draw_v 0x7836e: vertical line table (values follow the line parity; 0x785aa pulls them inside).
void drawV(Chan& c, std::span<uint8_t> buf) {
    const int lo = c[0xc] & int(M16), hi = c[0xe] & int(M16);
    auto clamp = [&](int v) {
        v &= int(M16);
        return v < lo ? v + 2 : v > hi ? v - 2 : v;
    };
    const int d2 = c[0x14];
    if (d2 == 0) {
        int d4 = c[0x28], d5 = d4;
        uint32_t d0 = c.l(0x2a), d1 = d0;
        const uint32_t st = c.l(0x2e);
        int o = d4;
        for (int i = 0, n = c[0x32] & int(M16); i < n; ++i) {
            at(buf, o++) = uint8_t(clamp(par(int(d0 >> 16), d4++)));
            d0 += st;
        }
        o = c[0x28] + 1;
        for (int i = 0, n = c[0x34] & int(M16); i < n; ++i) {
            at(buf, --o) = uint8_t(clamp(par(int(d1 >> 16), d5--)));
            d1 -= st;
        }
    } else if (d2 > 0 && c[0x24] >= 0) {
        const auto [ev, od] = pairOf(d2);
        for (int i = c[0x24], end = c[0x26]; i <= end; ++i) at(buf, i) = uint8_t(i & 1 ? od : ev);
    }
    if (c[0x16] >= 0) {
        const auto pr = pairOf(c[0x16]);
        margins(c, [&](int i) { at(buf, i) = uint8_t(i & 1 ? pr.second : pr.first); });
    }
    const int bg = c[0x18];
    if (bg < 0) return;
    const auto [ev, od] = pairOf(bg);
    int d1 = c[0x24];
    if (d1 < 0) d1 = c[0xe] + 1;
    for (int i = c[0xc]; i < d1; ++i) at(buf, i) = uint8_t(i & 1 ? od : ev);
    if (c[0x26] >= 0)
        for (int i = c[0x26] + 1, end = c[0xe]; i <= end; ++i) at(buf, i) = uint8_t(i & 1 ? od : ev);
    if (buf[0xef] == uint8_t(od)) buf[0xef] = buf[0xed];
    if (buf[0xf0] == uint8_t(ev)) buf[0xf0] = buf[0xee];
    if (buf[1] == uint8_t(od)) buf[1] = buf[3];
}

// mosaic 0x1120ae (H) / 0x112192 (V, values follow the line parity): in-place block replicate.
// m = channel 11 / 12 words: lo, hi, centre, block size (negative = abs).
void mosaic(std::span<uint8_t> buf, const std::array<int, 4>& m, bool vertical) {
    const int size = std::abs(s16(m[3]));
    if (!size) return;
    int lo = s16(m[0]), hi = s16(m[1]);
    if (lo > hi) std::swap(lo, hi);
    const int d1 = size >> 1, d2 = size - d1;
    auto val = [&](int v, int i) {
        if (!vertical) return uint8_t(v);
        const auto [e, o] = pairOf(v);
        return uint8_t(i & 1 ? o : e);
    };
    const int cen = s16(m[2]);
    int d4 = cen, d7 = cen;  // forward pass (0x1120dc)
    for (bool first = true;; first = !first) {
        const int v = first ? at(buf, d7 < lo ? lo : d7) : at(buf, d7 > hi ? hi : d7);
        bool stop = false;
        for (int i = 0, n = first ? d2 : d1; i < n; ++i) {
            if (d4 >= lo) {
                if (d4 > hi) {
                    stop = true;
                    break;
                }
                at(buf, d4) = val(v, d4);
            }
            ++d4;
        }
        if (stop) break;
        if (first) d7 += size;
    }
    d4 = d7 = cen;  // backward pass (0x11212c)
    for (bool first = true;; first = !first) {
        const int v = first ? at(buf, d7 > hi ? hi : d7) : at(buf, d7 < lo ? lo : d7);
        bool stop = false;
        for (int i = 0, n = first ? d1 : d2; i < n; ++i) {
            --d4;
            if (d4 <= hi) {
                if (d4 < lo) {
                    stop = true;
                    break;
                }
                at(buf, d4) = val(v, d4);
            }
        }
        if (stop) break;
        if (first) d7 -= size;
    }
}

// _dbra: decrement a word counter; false once it wraps to 0xffff.
bool dbra(int& x) {
    x = (x - 1) & int(M16);
    return x != int(M16);
}

// _tile: common tail of 0x76204 / 0x7627a: copy the run [a4, a1) cyclically right then left.
void tile(std::span<uint8_t> buf, int a4, int a1, int d0, int d1, int d2) {
    int a2 = a4, a3 = a1, d3 = d2;
    for (bool go = dbra(d0); go; go = dbra(d0)) {
        at(buf, a3++) = at(buf, a2++);
        if (!dbra(d3)) a2 = a4, d3 = d2;
    }
    a2 = a1, d3 = d2;
    for (bool go = dbra(d1); go; go = dbra(d1)) {
        --a2, --a4;
        at(buf, a4) = at(buf, a2);
        if (!dbra(d3)) a2 = a1, d3 = d2;
    }
}

// tile_h 0x76204 (flag 0x200): repeat the first non-background run over [$c..$e] both ways.
void tileH(const Chan& c, std::span<uint8_t> buf) {
    int a1 = c[0xc];
    int d0 = (c[0xe] - c[0xc]) & int(M16);
    const int d5 = d0, bg = c[0x18] & 0xff;
    for (;;) {
        if (at(buf, a1++) != bg) break;
        if (!dbra(d0)) return;
    }
    const int a4 = a1 - 1, d1 = (d5 - d0) & int(M16);
    for (bool go = dbra(d0); go; go = dbra(d0))
        if (at(buf, a1++) == bg) break;
    --a1;
    d0 = (d0 + 1) & int(M16);
    const int d2 = (d5 + 1 - d0 - d1 - 1) & int(M16);
    tile(buf, a4, a1, d0, d1, d2);
}

// tile_v 0x7627a (flag 0x400): as tile_h for the line table, run length rounded to line pairs.
void tileV(const Chan& c, std::span<uint8_t> buf) {
    int a1 = c[0xc];
    int d0 = (c[0xe] - c[0xc]) & int(M16);
    const int d5 = d0;
    int d4 = c[0x18] & 0xff, d6 = (d4 + 1) & 0xff;
    if (!(d6 & 1)) std::swap(d4, d6);
    for (;;) {
        if (at(buf, a1) != d4 && at(buf, a1) != d6) break;
        ++a1;
        if (!dbra(d0)) return;
    }
    const int a4 = a1, d1 = (d5 - d0) & int(M16);
    ++a1;
    for (bool go = dbra(d0); go; go = dbra(d0)) {
        if (at(buf, a1) == d4 || at(buf, a1) == d6) break;
        ++a1;
    }
    d0 = (d0 + 1) & int(M16);
    int d2 = (d5 + 1 - d0 - d1) & int(M16);
    if (d2 & 1) {
        d2 &= ~1;
        d0 = (d0 + 1) & int(M16);
        --a1;
    }
    d2 = s16(d2 - 1);
    if (d2 < 0) return;
    tile(buf, a4, a1, d0, d1, d2);
}

// rect 0x78cd0 (wipe channels 13..22, 0x18-byte blocks): one span [$14..$16] (-1 = none).
void rect(Chan& c) {
    const int64_t a = c[0xa], frac = c.w(2);
    int d0 = a >= 0 ? s16(-((a * frac) >> 16)) : int(((-a) * frac) >> 16);
    d0 = s16(d0 + c[8] + c[0]);
    if (a == 0) {
        c.set(0x10, d0);
        c.set(0x12, d0);
        c.set(0x14, -1);
        c.set(0x16, -1);
        return;
    }
    int d1 = s16(a + d0 - 1 + (a < 0 ? 2 : 0));
    if (d1 <= d0) std::swap(d0, d1);
    c.set(0x10, d0);
    c.set(0x12, d1);
    if (d0 > c[6] || d1 < c[4]) {
        c.set(0x14, -1);
        c.set(0x16, -1);
        return;
    }
    c.set(0x14, d0 < c[4] ? c[4] : d0);
    c.set(0x16, d1 > c[6] ? c[6] : d1);
}

// Algo.YIQ0: byte 0 of the 0x6d88 colour table (0xe8 + code) per Effect Color index.
constexpr uint8_t kYiq0[8] = {0xe8, 0xe9, 0xee, 0xec, 0xef, 0xea, 0xed, 0xeb};

// auto_ts's step d3 (word): floor((flags & 8 ? 0x20000 : 0x10000) / n).
uint32_t autoDelta(int n, uint32_t flags) { return ((flags & 8 ? 0x20000u : 0x10000u) / uint32_t(n)) & M16; }

}  // namespace

// Algo.__init__ (bg = 0xff, the init at 0x75370 with flag 0x80 clear and $1d2(a5) >= 0).
AlgoVm::AlgoVm(std::vector<uint8_t> payload, int kind, int colour) : payload_(std::move(payload)) {
    if (kind < 0 || kind > 4) fail("kind not playable");
    if (colour < 0 || colour > 7) fail("colour index out of range");
    kind_ = AlgoKind(kind);
    render_ = kind == 3 ? 0 : kind == 4 ? 1 : kind;
    const Bytes p{payload_.data(), payload_.size()};
    flags_ = p.L(4);
    nch_ = render_ == 2 ? 0 : int(p.W(8));  // kind 2 (ALGO.md 4): the program starts at long 0 = 8
    if (nch_ > 10) fail("too many channels");
    if (render_ == 0 && (flags_ & 0x80)) fail("flag 0x80 on a digital effect (0x785c0 / 0x78632)");
    install();
    step_.h.fill(0xff);
    for (size_t i = 0; i < step_.v.size(); ++i) step_.v[i] = uint8_t(i & 1 ? 0xef : 0xee);
    step_.inverse = render_ == 1 && (flags_ & 0x80);
    if (render_ == 2) return;
    Chan c1{ch_[1]};  // 0x753ae: channel 1 overrides
    if (c1[0x18] != -1) c1.set(0x18, 0xff);
    if (c1[0x16] != -1) c1.set(0x16, kYiq0[colour]);
}

// _install 0x76044: channel word, then (opcode, params) per slot until 0; slot k = word 2k.
void AlgoVm::install() {
    const Bytes p{payload_.data(), payload_.size()};
    int64_t o = p.L(0);
    for (;;) {
        const int n = int(p.W(o));
        o += 2;
        if (!n) break;
        if (n > 25) fail("channel out of range");
        for (int k = 0;; ++k) {
            const int op = int(p.W(o));
            o += 2;
            if (!op) break;
            if (op >= int(std::size(kFixed)) || op == 15 || op == 16) fail("opcode not supported");
            const int ln = op == 7 || op == 8 ? int(p.W(o)) : kFixed[op];
            if (uint64_t(o) + uint64_t(ln) > payload_.size()) fail("malformed program");
            if (op != 1 && 2 * k + 2 > chanSize(n)) fail("slot past the channel block");
            if (op == 2)
                Chan{ch_[size_t(n)]}.set(2 * k, int(p.W(o)));
            else if (op != 1)
                anim_[size_t(n)].push_back({2 * k, op, size_t(o), size_t(ln)});
            o += ln;
        }
    }
}

// _eval
void AlgoVm::eval(int n, uint32_t t) {
    Chan c{ch_[size_t(n)]};
    for (const Slot& s : anim_[size_t(n)]) {
        const Value r = evaluate(s.op, Bytes{payload_.data() + s.prm, s.len}, t);
        c.set(s.off, r.word >= 0 ? r.word : store(r.v));
    }
}

// Algo.step
const AlgoStep& AlgoVm::step(uint32_t t) {
    t = std::min(t, 0x10000u);
    if (flags_ & 4) t = 0x10000 - t;
    if (render_ == 2)
        eval(23, t);  // 0x75d5a: levels only (ALGO.md 8)
    else if (render_ == 1)
        wipe(t);
    else
        digital(t);
    step_.level = flags_ & 0x10 ? Chan{ch_[23]}[0] : -1;
    return step_;
}

// Algo.step, kind 0 (0x7516a).
void AlgoVm::digital(uint32_t t) {
    const uint32_t f = flags_;
    for (int n = 1; n <= nch_; ++n) eval(n, t);
    eval(10, t);
    if (f & 0x800) eval(11, t);
    if (f & 0x1000) eval(12, t);
    if (f & 0x10) eval(23, t);
    int lo = 0xb7, hi = 0;
    for (int n = 1; n <= nch_; ++n) {
        Chan c{ch_[size_t(n)]};
        geometry(c, false);
        drawH(c, step_.h);
        lo = std::min(lo, c[0xc] & int(M16));
        hi = std::max(hi, c[0xe] & int(M16));
    }
    auto words = [&](int n) {
        const Chan c{ch_[size_t(n)]};
        return std::array<int, 4>{c[0], c[2], c[4], c[6]};
    };
    if (f & 0x800) mosaic(step_.h, words(11), false);
    if (f & 0x200) tileH(Chan{ch_[1]}, step_.h);
    Chan cv{ch_[10]};
    geometry(cv, true);
    if (cv[0x26] == 0xee) cv.set(0x26, 0xf0);
    if (cv[0x24] == 2) cv.set(0x24, 1);
    drawV(cv, step_.v);
    if (f & 0x1000) mosaic(step_.v, words(12), true);
    if (f & 0x400) {
        tileV(cv, step_.v);
        cv.set(0x24, 0);
        cv.set(0x26, 0xf0);
    }
    step_.cellLo = lo;
    step_.cellHi = hi;
    step_.lineLo = cv[0x24] < 0 ? -1 : cv[0x24];
    step_.lineHi = cv[0x24] < 0 ? -1 : cv[0x26];
}

// Algo._wipe
void AlgoVm::wipe(uint32_t t) {
    const uint32_t f = flags_;
    for (int n = 13; n < 13 + nch_; ++n) eval(n, t);
    eval(22, t);
    if (f & 0x10) eval(23, t);
    int lo = 0x2df, hi = 0;
    step_.spans.clear();
    for (int n = 13; n < 13 + nch_; ++n) {
        Chan c{ch_[size_t(n)]};
        rect(c);
        lo = std::min(lo, c[4] & int(M16));
        hi = std::max(hi, c[6] & int(M16));
        step_.spans.emplace_back(c[0x14], c[0x16]);
    }
    Chan cv{ch_[22]};
    rect(cv);
    step_.pixelLo = lo;
    step_.pixelHi = hi;
    step_.ink = Chan{ch_[13]}[0xc];
    step_.bandLo = cv[0x14];
    step_.bandHi = cv[0x16];
    step_.bandInk = cv[0xc];
}

// tbar_t 0x7510e / 0x75882: T-bar 0..511 -> t (16.16); flags bit 3 = there and back.
uint32_t algoTBarT(int pos, uint32_t flags) {
    pos = std::clamp(pos, 0, 511);
    if (flags & 8) {
        pos *= 2;
        if (pos > 0x1ff) pos = 0x3fe - pos;
    }
    const uint32_t q = (uint32_t(pos) << 16) / 0x1ff;
    return q > 0xffff ? 0x10000 : q;
}

// auto_ts 0x75006 / 0x75794 started at T-bar 0: t of step k (1..n).
uint32_t algoAutoT(int k, int n, uint32_t flags) {
    n = std::max(n, 1);
    const uint32_t d4 = uint32_t(std::clamp(k, 1, n)) * autoDelta(n, flags);
    return (flags & 8) && d4 > 0x10000 ? 0x20000 - d4 : d4;
}

bool algoSecondStep(int tbar, int autoStep, int n, uint32_t flags) {
    if (autoStep < 1) return tbar >= 256;
    n = std::max(n, 1);
    return uint32_t(std::clamp(autoStep, 1, n)) * autoDelta(n, flags) >= (flags & 8 ? 0x10000u : 0x8000u);
}

DveControl algoControl(const AlgoStep& s) {
    DveControl c;
    std::copy_n(s.v.begin(), kDveLines, c.line.begin());
    if (s.lineLo >= 0)
        for (int k = s.lineLo; k <= std::min(s.lineHi, kDveLines - 1); ++k)
            for (int cell = std::max(s.cellLo, 0); cell <= std::min(s.cellHi, kDveCells - 1); ++cell)
                c.key[size_t(k) * kDveCells + size_t(cell)] = uint8_t(255 - s.h[size_t(cell)]);
    return c;
}

int algoBorderFill(int colourIndex) { return colourIndex >= 0 && colourIndex <= 7 ? 255 - kYiq0[colourIndex] : 23; }

int algoColourIndex(const EffectColour& c) {
    if (c.custom || c.preset == BkgColour::Snow) return 0;
    return int(c.preset) - int(BkgColour::Black);
}

void renderAlgoWipe(const AlgoStep& s, const Frame& a, const Frame& b, Frame& out) {
    std::array<uint8_t, kRasterW> spanRow{};
    for (int x = 0; x < kRasterW; ++x) {
        bool inSpan = false;
        for (const auto& [s0, s1] : s.spans) inSpan |= s0 >= 0 && s0 <= x && x <= s1;
        spanRow[size_t(x)] = inSpan ? uint8_t(s.ink & 1) : x >= s.pixelLo && x <= s.pixelHi ? uint8_t((s.ink & 1) ^ 1) : 0;
    }
    for (int y = 0; y < kRasterH; ++y) {
        const bool inBand = s.bandLo >= 0 && s.bandLo <= y && y <= s.bandHi;
        const bool constant = s.inverse ? inBand : !inBand;
        // constant row r: row 0 all 1 (B), row 1 all 0 (A)
        const int constBit = (s.bandInk ^ (s.inverse ? 1 : 0)) & 1 ? 0 : 1;
        for (int x = 0; x < kRasterW; ++x)
            out.at(x, y) = (constant ? constBit : spanRow[size_t(x)]) ? b.at(x, y) : a.at(x, y);
    }
}

void mixLevel(const Frame& in0, const Frame& in1, int level, Frame& out) {
    const int l = std::clamp(level, 0, 255);
    auto mix = [l](int p, int q) { return uint8_t((p * (255 - l) + q * l + 127) / 255); };
    for (size_t i = 0; i < out.px.size(); ++i) {
        const Rgba8 p = in0.px[i], q = in1.px[i];
        out.px[i] = {mix(p.r, q.r), mix(p.g, q.g), mix(p.b, q.b), 255};
    }
}

}  // namespace vt
