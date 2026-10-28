"""Pure-Python reference of the Switcher's ALGO renderer (switcher.main 4.2), see tools/re/ALGO.md.

Copied from <root>/tools/re/algo_ref.py (verified bit-exact against the 68000 code by algo_check.py).
Changes: the sine table comes from set_root(root) (default: the Toaster root above vtoast_reborn)
through the hunk.py next to this file instead of algo_util; Algo handles kind 2 (level fades).

    python -P -E vtoast_reborn/tools/ref/algo_ref.py <crouton> [tbar ...]     print H / V tables
    python -P -E vtoast_reborn/tools/ref/algo_ref.py <crouton> --png out.png   contact sheet (0x63 = 0 only)

Algo(payload, kind).step(t) -> dict: kind 2 (level fades): {'colour': [w0, w2, w4]} of channel 23;
kind 0 (tag 0x63 = 0): H = 0x170-byte cell row ($afa),
V = 0xf4-byte line table ($afe), lines = (first, last) control lines that show the H row
(the 0x78d5e run list; None = whole field background); kind 1 (wipes): spans.
State (channel words, buffers) persists between steps exactly as in the Switcher.
Every routine is a literal port; addresses in the comments.  algo_check.py compares it with
the 68000 code run under unicorn.
"""
import os, struct, sys

M16, M32 = 0xffff, 0xffffffff


def s16(x):
    x &= M16
    return x - 0x10000 if x & 0x8000 else x


def s32(x):
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


# ------------------------------------------------------------------ fixed-point helpers

def mul_t(d0, t):
    """0x7873e: 16.16 value x unsigned fraction t (t >= 0x10000 returns d0 unchanged)."""
    if t >> 16:
        return d0 & M32
    lo, hi = d0 & M16, s16(d0 >> 16)
    return (((lo * t + 0x8000) >> 16) + hi * t) & M32


def mul_ff(d0, d1):
    """0x786f0: signed 16.16 x 16.16 with the low product rounded."""
    lo0, hi0, lo1, hi1 = d0 & M16, s16(d0 >> 16), d1 & M16, s16(d1 >> 16)
    r = ((lo0 * lo1 + 0x8000) >> 16) + hi1 * lo0 + ((hi0 * hi1) << 16) + hi0 * lo1
    return r & M32


def mul_fi(d0, d1):
    """0x78776: 16.16 x signed word (no rounding)."""
    d1 = s16(d1)
    return (d1 * (d0 & M16) + ((s16(d0 >> 16) * d1) << 16)) & M32


def div_ff(a, b):
    """0x7879a: signed word / signed word -> 16.16 (truncated toward zero)."""
    a, b = s16(a), s16(b)
    if b == 0:                                  # only reachable with a >= 0
        return 0 if a == 0 else 0x7fffffff
    neg = (a < 0) != (b < 0)
    a, b = abs(a), abs(b)
    q, r = divmod(a, b)
    q2 = (r << 16) // b
    if not neg:
        return ((q << 16) | q2) & M32
    hi = (-q - (1 if r else 0)) & M16
    return ((hi << 16) | (-q2 & M16)) & M32


def store(v):
    """0x78c3a: 16.16 -> word, rounded, except a high word of 0x7fff."""
    v &= M32
    if (v >> 16) == 0x7fff:
        return 0x7fff
    return ((v + 0x8000) >> 16) & M16


SINE = None
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def set_root(root):
    """Toaster root (the folder holding 4.2/): the sine table lives in 4.2/Programs/switcher.main."""
    global ROOT, SINE
    ROOT, SINE = root, None


def sine(d0):
    """0x7a004: sine of the low word (0x10000 = one turn) -> 16.16. Table at 0x7a082."""
    global SINE
    if SINE is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import hunk
        mem = hunk.load(os.path.join(ROOT, '4.2', 'Programs', 'switcher.main')).mem
        SINE = [struct.unpack('>H', mem[0x7a082 + 2 * i:0x7a084 + 2 * i])[0] for i in range(256)]
    d3 = 0
    x = d0 & M16
    if d0 & 0x80000000:
        x = -x & M16
        d3 = 1
    if x & 0x8000:
        x = -x & M16
        if x == 0x8000:
            return 0
        d3 ^= 1
    d1 = x
    if x == 0x4000:
        return 0xffff0000 if d3 else 0x10000
    if x > 0x4000:
        d1 = (0x8000 - x) & M16
    d2 = d1 & 0x3f
    i = (d1 >> 6) & M16
    v = SINE[i]
    diff = (-v & M16) if i == 255 else ((SINE[i + 1] - v) & M16)
    v = (v + (((diff * d2) & M16) >> 6)) & M16
    return (-v) & M32 if d3 else v


# ------------------------------------------------------------------ opcodes (slot animators)

L = lambda b, o: struct.unpack('>I', b[o:o + 4])[0]
W = lambda b, o: struct.unpack('>H', b[o:o + 2])[0]


def op_lin(p, t, o=0):
    """0x787fc (op 3): L0 + L1*t, |result| clamped to [L2, L3]."""
    d0 = (mul_t(L(p, o + 4), t) + L(p, o)) & M32
    a = abs(s32(d0)) & M32
    lo, hi = s32(L(p, o + 8)), s32(L(p, o + 12))
    if s32(a) < lo:
        a = lo & M32
    elif s32(a) > hi:
        a = hi & M32
    else:
        return d0
    return (-a & M32) if s32(d0) < 0 else a


def op_window(p, t):
    """0x7889c (op 6): op_lin over the T-bar window [W@16, W@18) (0 = 1.0)."""
    d1 = W(p, 16)
    if t < d1:
        d0 = 0
    else:
        d2 = W(p, 18)
        if d2 and t >= d2:
            d0 = L(p, 4)
        else:
            if not d2:
                d2 = 0x10000
            d2 -= d1
            if d2 == 0:
                d0 = 0
            else:
                f = t - d1
                d0 = L(p, 4)
                if d2 >= 0x10000:
                    d0 = mul_t(d0, f)
                elif (d2 & M16) != (f & M16):
                    d0 = mul_t(d0, ((f << 16) // d2) & M16)
    d0 = (d0 + L(p, 0)) & M32
    a = abs(s32(d0)) & M32
    lo, hi = s32(L(p, 8)), s32(L(p, 12))
    if s32(a) < lo:
        a = lo & M32
    elif s32(a) > hi:
        a = hi & M32
    else:
        return d0
    return (-a & M32) if s32(d0) < 0 else a


def evaluate(n, p, t):
    """value (16.16) of opcode n with params p at t; returns (value, word or None for 'store')."""
    if n == 3:
        return op_lin(p, t), None
    if n == 4:                                  # 0x78838
        return (mul_t(op_lin(p, t), t) + L(p, 16)) & M32, None
    if n == 5:                                  # 0x7885e
        ph = (mul_t(op_lin(p, t), t) + L(p, 16)) & M32
        return (mul_ff(op_lin(p, t, 20), sine(ph)) + L(p, 36)) & M32, None
    if n == 6:
        return op_window(p, t), None
    if n == 7:                                  # 0x78922: step table, stores the word itself
        if t >= 0xffff:
            v = W(p, W(p, 0) - 4)
        else:
            o = 6
            while t > W(p, o):
                o += 4
            v = W(p, o + 2) if t == W(p, o) else W(p, o - 2)
        return v << 16, v
    if n == 8:                                  # 0x7895c: piecewise linear (key, from, to) records
        if t >= 0xffff:
            o = W(p, 0) - 2
        else:
            o = 0xc
            while t > W(p, o):
                o += 0xa
            if t == W(p, o):
                o += 0xa
        end = W(p, o)
        end = 0 if end == 0xffff else end
        blk = struct.pack('>IIIIHH', L(p, o - 8), (L(p, o - 4) - L(p, o - 8)) & M32, 0, 0x7fffffff,
                          W(p, o - 0xa), end)
        return op_window(blk, t), None
    # 9..17: not used by ALGO croutons (9 and 17 by the four deep-colour bar ILBM/ANIM croutons)
    if n == 9:                                  # 0x789b6: L * t
        return mul_t(L(p, 0), t), None
    if n == 10:                                 # 0x789c8: L * t^2
        return mul_t(mul_t(L(p, 0), t), t), None
    if n == 13:                                 # 0x789de: L * t^3
        return mul_t(mul_t(mul_t(L(p, 0), t), t), t), None
    if n == 11:                                 # 0x789f8: L * t * (2 - t)
        return mul_ff(mul_t(L(p, 0), t), (0x20000 - t) & M32), None
    if n == 14:                                 # 0x78a16: (L/2 * t) * (3 - t^2)
        d2 = (0x30000 - mul_t(t, t)) & M32
        return mul_ff(mul_t(s32(L(p, 0)) >> 1 & M32, t), d2), None
    if n == 12:                                 # 0x78a42: L/2 * (1 + sin(t/2 + 3/4 turn))
        s = (sine((mul_t(0x8000, t) + 0xc000) & M32) + 0x10000) & M32
        return mul_ff(s, s32(L(p, 0)) >> 1 & M32), None
    if n == 17:                                 # 0x78b02: constant long
        return L(p, 0), None
    raise NotImplementedError(f'opcode {n} (random / sine-lerp / composite: no shipped crouton uses it)')


# ------------------------------------------------------------------ geometry and drawing

class Chan:
    """0x36-byte channel word block (0x7153a + 0x36*(n-1)); words by byte offset."""

    def __init__(self, size=0x36):
        self.b = bytearray(size)

    def __getitem__(self, o):
        return s16(W(self.b, o))

    def __setitem__(self, o, v):
        self.b[o:o + 2] = struct.pack('>H', v & M16)

    def l(self, o):
        return L(self.b, o)

    def setl(self, o, v):
        self.b[o:o + 4] = struct.pack('>I', v & M32)


def geometry(c, vertical):
    """0x78000: place source [$6..$8] (base $a at anchor $10+$4) at size $12, clip to [$c..$e]."""
    d4 = s16(c[0x10] + c[4])
    if c[0x12] == 0:
        c[0x1c] = c[0x1e] = d4
        return _empty(c)
    d5 = c[0xa]
    if vertical and (d5 ^ d4) & 1:
        d5 = s16(d5 + 1)
    d0 = c[0x12]
    d1 = c[8]
    d2 = d1
    d1 = s16(d1 - c[6])
    if d1 < 0:
        d2 = c[6]
        d0 = s16(-d0)
        d1 = s16(-d1)
    d1 = s16(d1 + 1)
    d7, d6 = d0, d1
    sc = div_ff(d0, d1)
    p = mul_ff(sc, (((d2 - d5) & M16) << 16) | 0x8000)
    e = s16((p >> 16) + d4 + (1 if d7 < 0 else 0))
    s = s16(e - d7 + 1)
    if d7 < 0:
        s = s16(s - 2)
        e, s = s, e
    c[0x1e], c[0x1c] = e, s
    lo, hi = c[0xc], c[0xe]
    if d4 < lo:
        d3 = lo
        c[0x20] = c[0x24] = c[0x28] = d3
        d2 = c[0x1e]
        if d2 > hi:
            c[0x22] = c[0x26] = hi
            d2 = s16(hi - d3)
        else:
            c[0x22] = c[0x26] = d2
            d2 = s16(d2 - d3)
            if d2 < 0:
                return _empty(c)
        c[0x32] = d2 + 1
        c[0x34] = 0
        d3 = s16(d3 - d4)
    elif d4 > hi:
        d3 = hi
        c[0x28] = d3
        d2 = c[0x1c]
        if d2 >= lo:
            c[0x22] = c[0x26] = d3
            c[0x20] = c[0x24] = d2
            d2 = s16(d2 - d3)
            if d2 > 0:
                return _empty(c)
            d2 = s16(-d2)
        else:
            c[0x22] = c[0x26] = d3
            c[0x20] = c[0x24] = lo
            d2 = s16(d3 - lo)
        c[0x34] = d2 + 1
        c[0x32] = 0
        d3 = s16(d3 - d4)
    else:
        c[0x28] = d4
        d2 = c[0x1e]
        if d2 > hi:
            d2 = hi
        c[0x22] = c[0x26] = d2
        c[0x32] = d2 - d4 + 1
        d3 = c[0x1c]
        if d3 < lo:
            d3 = lo
        c[0x20] = c[0x24] = d3
        c[0x34] = d4 - d3 + 1
        d3 = 0
    step = div_ff(d6, d7)
    c.setl(0x2e, step)
    d5 = (d5 & M16) << 16
    if d3:
        d5 = (d5 + mul_fi(step, d3)) & M32
    c.setl(0x2a, d5)


def _empty(c):
    for o in (0x20, 0x24, 0x22, 0x26):
        c[o] = -1
    c[0x32] = c[0x34] = 0


def _fill(buf, start, n, v):
    for i in range(max(n, 0)):
        buf[start + i] = v & 0xff


def _edgefix(a):
    """0x782ec: replicate the first video cell (<= 0xbd) into cells 0..1 (on buf or struct!)."""
    d0 = 2
    if a[d0] > 0xbd:
        return
    d2 = a[d0]
    while True:
        d0 -= 1
        if d0 < 0:
            return
        if a[d0] > 0xbd:
            break
        d2 = a[d0]
    while d0 >= 0:
        a[d0] = d2
        d0 -= 1


def _margins(c, buf, put):
    """0x781f8 / 0x783ee: $0 / $2 margins with $16 (negative = drawn inside the picture)."""
    lo, hi, d5, d6 = c[0xc], c[0xe], c[0x1c], c[0x1e]
    d1, d2 = d5, c[0]
    if d2 < 0:
        if c[0x12]:
            for _ in range(-d2):
                if d1 > hi or d1 > d6:
                    break
                if d1 >= lo:
                    put(d1)
                d1 += 1
    else:
        d1 -= 1
        for _ in range(d2):
            if d1 < lo:
                break
            if d1 <= hi:
                c[0x24] = d1
                put(d1)
            d1 -= 1
        if c[0x24] >= 0 and c[0x26] < 0:
            c[0x26] = hi
    d1, d2 = d6, c[2]
    if d2 < 0:
        if c[0x12]:
            for _ in range(-d2):
                if d1 < lo or d1 < d5:
                    break
                if d1 <= hi:
                    put(d1)
                d1 -= 1
    else:
        if c[0x12]:
            d1 += 1
        for _ in range(d2):
            if d1 > hi:
                break
            if d1 >= lo:
                c[0x26] = d1
                put(d1)
            d1 += 1
        if c[0x26] >= 0 and c[0x24] < 0:
            c[0x24] = lo


def draw_h(c, buf):
    """0x781a8: horizontal cell row."""
    d2 = c[0x14]
    if d2 == 0:
        o = c[0x28]
        d0 = (c.l(0x2a) + 0x8000) & M32
        d1, st = d0, c.l(0x2e)
        for _ in range(c[0x32] & M16):
            buf[o] = (d0 >> 16) & 0xff
            o += 1
            d0 = (d0 + st) & M32
        o = c[0x28] + 1
        for _ in range(c[0x34] & M16):
            o -= 1
            buf[o] = (d1 >> 16) & 0xff
            d1 = (d1 - st) & M32
    elif d2 > 0 and c[0x24] >= 0:
        _fill(buf, c[0x24], c[0x26] - c[0x24] + 1, d2)
    if c[0x16] >= 0:
        v = c[0x16]

        def put(i):
            buf[i] = v & 0xff
        _margins(c, buf, put)
    bg = c[0x18]
    if bg < 0:
        _edgefix(c.b)                           # quirk: a0 is still the channel block here
        return
    d1 = c[0x24]
    if d1 < 0:
        d1 = c[0xe] + 1
    _fill(buf, c[0xc], d1 - c[0xc], bg)
    if c[0x26] >= 0:
        _fill(buf, c[0x26] + 1, c[0xe] - c[0x26], bg)
    _edgefix(buf)


def _par(v, i):
    """bit 0 of the line value follows the parity of the line index (0x7839a)."""
    return (v | 1) if i & 1 else ((v + 1) & ~1)


def draw_v(c, buf):
    """0x7836e: vertical line table (values follow the line parity; 0x785aa pulls them inside)."""
    lo, hi = c[0xc] & M16, c[0xe] & M16

    def clamp(v):
        v &= M16
        if v < lo:
            return v + 2
        if v > hi:
            return v - 2
        return v
    d2 = c[0x14]
    if d2 == 0:
        d4 = d5 = c[0x28]
        d0 = d1 = c.l(0x2a)
        st = c.l(0x2e)
        o = d4
        for _ in range(c[0x32] & M16):
            buf[o] = clamp(_par(d0 >> 16, d4)) & 0xff
            o += 1
            d4 += 1
            d0 = (d0 + st) & M32
        o = c[0x28] + 1
        for _ in range(c[0x34] & M16):
            o -= 1
            buf[o] = clamp(_par(d1 >> 16, d5)) & 0xff
            d5 -= 1
            d1 = (d1 - st) & M32
    elif d2 > 0 and c[0x24] >= 0:
        ev, od = (d2, d2 + 1) if not d2 & 1 else (d2 + 1, d2)
        for i in range(c[0x24], c[0x26] + 1):
            buf[i] = (od if i & 1 else ev) & 0xff
    if c[0x16] >= 0:
        a = c[0x16]
        ev, od = (a, a + 1) if not a & 1 else (a + 1, a)

        def put(i):
            buf[i] = (od if i & 1 else ev) & 0xff
        _margins(c, buf, put)
    bg = c[0x18]
    if bg < 0:
        return
    ev, od = (bg, bg + 1) if not bg & 1 else (bg + 1, bg)
    d1 = c[0x24]
    if d1 < 0:
        d1 = c[0xe] + 1
    for i in range(c[0xc], d1):
        buf[i] = (od if i & 1 else ev) & 0xff
    if c[0x26] >= 0:
        for i in range(c[0x26] + 1, c[0xe] + 1):
            buf[i] = (od if i & 1 else ev) & 0xff
    if buf[0xef] == od & 0xff:
        buf[0xef] = buf[0xed]
    if buf[0xf0] == ev & 0xff:
        buf[0xf0] = buf[0xee]
    if buf[1] == od & 0xff:
        buf[1] = buf[3]




def _pair(v):
    """(even, odd) members of a line-value pair: v and v+1 ordered by parity."""
    return (v, v + 1) if not v & 1 else (v + 1, v)


def mosaic(buf, m, vertical):
    """0x1120ae (H) / 0x112192 (V, values follow the line parity): in-place block replicate.
    m = channel 11 / 12 words: lo, hi, centre, block size (negative = abs)."""
    size = abs(s16(m[3]))
    if not size:
        return
    lo, hi = s16(m[0]), s16(m[1])
    if lo > hi:
        lo, hi = hi, lo
    d1 = size >> 1
    d2 = size - d1

    def val(v, i):
        if not vertical:
            return v & 0xff
        e, o = _pair(v)
        return (o if i & 1 else e) & 0xff
    cen = s16(m[2])
    d4 = d7 = cen                                   # forward pass (0x1120dc)
    first = True
    while True:
        if first:
            v = buf[lo] if d7 < lo else buf[d7]
        else:
            v = buf[hi] if d7 > hi else buf[d7]
        stop = False
        for _ in range(d2 if first else d1):
            if d4 >= lo:
                if d4 > hi:
                    stop = True
                    break
                buf[d4] = val(v, d4)
            d4 += 1
        if stop:
            break
        if first:
            d7 += size
        first = not first
    d4 = d7 = cen                                   # backward pass (0x11212c)
    first = True
    while True:
        if first:
            v = buf[hi] if d7 > hi else buf[d7]
        else:
            v = buf[lo] if d7 < lo else buf[d7]
        stop = False
        for _ in range(d1 if first else d2):
            d4 -= 1
            if d4 <= hi:
                if d4 < lo:
                    stop = True
                    break
                buf[d4] = val(v, d4)
        if stop:
            break
        if first:
            d7 -= size
        first = not first


def _dbra(x):
    x = (x - 1) & M16
    return x, x != M16


def _tile(buf, a4, a1, d0, d1, d2):
    """common tail of 0x76204 / 0x7627a: copy the run [a4, a1) cyclically right then left."""
    a2, a3, d3 = a4, a1, d2
    d0, go = _dbra(d0)
    while go:
        buf[a3] = buf[a2]; a3 += 1; a2 += 1
        d3, g3 = _dbra(d3)
        if not g3:
            a2, d3 = a4, d2
        d0, go = _dbra(d0)
    a2, d3 = a1, d2
    d1, go = _dbra(d1)
    while go:
        a2 -= 1; a4 -= 1
        buf[a4] = buf[a2]
        d3, g3 = _dbra(d3)
        if not g3:
            a2, d3 = a1, d2
        d1, go = _dbra(d1)


def tile_h(c, buf):
    """0x76204 (flag 0x200): repeat the first non-background run over [$c..$e] both ways."""
    a1 = c[0xc]
    d0 = (c[0xe] - c[0xc]) & M16
    d5 = d0
    bg = c[0x18] & 0xff
    while True:
        v = buf[a1]; a1 += 1
        if v != bg:
            break
        d0, go = _dbra(d0)
        if not go:
            return
    a4 = a1 - 1
    d1 = (d5 - d0) & M16
    d0, go = _dbra(d0)
    while go:
        v = buf[a1]; a1 += 1
        if v == bg:
            break
        d0, go = _dbra(d0)
    a1 -= 1
    d0 = (d0 + 1) & M16
    d2 = (d5 + 1 - d0 - d1 - 1) & M16
    _tile(buf, a4, a1, d0, d1, d2)


def tile_v(c, buf):
    """0x7627a (flag 0x400): as tile_h for the line table, run length rounded to line pairs."""
    a1 = c[0xc]
    d0 = (c[0xe] - c[0xc]) & M16
    d5 = d0
    d4 = c[0x18] & 0xff
    d6 = (d4 + 1) & 0xff
    if not d6 & 1:
        d4, d6 = d6, d4
    while True:
        if buf[a1] != d4 and buf[a1] != d6:
            break
        a1 += 1
        d0, go = _dbra(d0)
        if not go:
            return
    a4 = a1
    d1 = (d5 - d0) & M16
    a1 += 1
    d0, go = _dbra(d0)
    while go:
        if buf[a1] == d4 or buf[a1] == d6:
            break
        a1 += 1
        d0, go = _dbra(d0)
    d0 = (d0 + 1) & M16
    d2 = (d5 + 1 - d0 - d1) & M16
    if d2 & 1:
        d2 &= ~1
        d0 = (d0 + 1) & M16
        a1 -= 1
    d2 = s16(d2 - 1)
    if d2 < 0:
        return
    _tile(buf, a4, a1, d0, d1, d2)


def rect(c):
    """0x78cd0 (wipe channels 13..22, 0x18-byte blocks): one span [$14..$16] (-1 = none)."""
    a, frac = c[0xa], W(c.b, 2)
    d0 = s16(-((a * frac) >> 16)) if a >= 0 else (((-a) * frac) >> 16)
    d0 = s16(d0 + c[8] + c[0])
    if a == 0:
        c[0x10] = c[0x12] = d0
        c[0x14] = c[0x16] = -1
        return
    d1 = s16(a + d0 - 1 + (2 if a < 0 else 0))
    if d1 <= d0:
        d0, d1 = d1, d0
    c[0x10], c[0x12] = d0, d1
    if d0 > c[6] or d1 < c[4]:
        c[0x14] = c[0x16] = -1
        return
    c[0x14] = c[4] if d0 < c[4] else d0
    c[0x16] = c[6] if d1 > c[6] else d1


# ------------------------------------------------------------------ the effect

def _chan_size(n):
    return 0x36 if n <= 10 else 8 if n in (11, 12) else 0x18 if n <= 22 else 0x0c


class Algo:
    """One ALGO crouton. kind = renderer: 0 digital, 1 wipe, 2 level fade (tag 0x80000063; 3 / 4 = 0 / 1). colour = Effect Color
    digital index 0..7 (0x6d88 table, byte 0 = 0xe8 + code). bg = the H background key value
    (0xff; the init at 0x75370 uses 0xfe with flag 0x80 and 0xe8/0xe9 when $1d2(a5) < 0)."""

    YIQ0 = [0xe8, 0xe9, 0xee, 0xec, 0xef, 0xea, 0xed, 0xeb]

    def __init__(self, payload, kind=0, colour=0, bg=0xff):
        self.p, self.kind = payload, kind
        self.flags = L(payload, 4)
        self.nch = 0 if kind == 2 else W(payload, 8)    # kind 2 (ALGO.md 4): the program starts at long 0 = 8
        self.ch = {n: Chan(_chan_size(n)) for n in range(1, 26)}
        self.anim = {n: [] for n in self.ch}        # (word offset, opcode, params)
        self._install()
        self.H = bytearray([bg]) * 0x170
        self.V = bytearray(0xef if i & 1 else 0xee for i in range(0xf4))
        if kind == 2:
            return
        c1 = self.ch[1]                             # 0x753ae: channel 1 overrides
        if c1[0x18] != -1:
            c1[0x18] = bg
        if c1[0x16] != -1:
            c1[0x16] = self.YIQ0[colour]

    def _install(self):
        """0x76044: channel word, then (opcode, params) per slot until 0; slot k = word 2k."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from algo_scan import op_len
        o = L(self.p, 0)
        while True:
            n = W(self.p, o); o += 2
            if not n:
                break
            k = 0
            while True:
                op = W(self.p, o); o += 2
                if not op:
                    break
                ln = op_len(self.p, o, op)
                prm = self.p[o:o + ln]; o += ln
                if op == 2:
                    self.ch[n][2 * k] = W(prm, 0)
                elif op != 1:
                    self.anim[n].append((2 * k, op, prm))
                k += 1

    def _eval(self, n, t):
        for off, op, prm in self.anim[n]:
            v, word = evaluate(op, prm, t)
            self.ch[n][off] = word if word is not None else store(v)

    def step(self, t):
        f = self.flags
        if f & 4:
            t = 0x10000 - t
        if self.kind == 2:                          # 0x75d5a: levels only (ALGO.md 8)
            self._eval(23, t)
            return {'colour': [self.ch[23][i] for i in (0, 2, 4)]}
        if self.kind == 1:
            return self._wipe(t)
        for n in range(1, self.nch + 1):
            self._eval(n, t)
        self._eval(10, t)
        if f & 0x800:
            self._eval(11, t)
        if f & 0x1000:
            self._eval(12, t)
        if f & 0x10:
            self._eval(23, t)
        if f & 0x80:
            raise NotImplementedError('flag 0x80 on a digital effect (0x785c0 / 0x78632)')
        lo, hi = 0xb7, 0
        for n in range(1, self.nch + 1):
            c = self.ch[n]
            geometry(c, False)
            draw_h(c, self.H)
            lo, hi = min(lo, c[0xc] & M16), max(hi, c[0xe] & M16)
        if f & 0x800:
            mosaic(self.H, [self.ch[11][i] for i in (0, 2, 4, 6)], False)
        if f & 0x200:
            tile_h(self.ch[1], self.H)
        cv = self.ch[10]
        geometry(cv, True)
        if cv[0x26] == 0xee:
            cv[0x26] = 0xf0
        if cv[0x24] == 2:
            cv[0x24] = 1
        draw_v(cv, self.V)
        if f & 0x1000:
            mosaic(self.V, [self.ch[12][i] for i in (0, 2, 4, 6)], True)
        if f & 0x400:
            tile_v(cv, self.V)
            cv[0x24], cv[0x26] = 0, 0xf0
        return {'H': bytes(self.H), 'V': bytes(self.V), 'cells': (lo, hi),
                'lines': None if cv[0x24] < 0 else (cv[0x24], cv[0x26]),
                'vrange': (cv[0xc], cv[0xe]),
                'colour': [self.ch[23][i] for i in (0, 2, 4)] if f & 0x10 else None}

    def _wipe(self, t):
        f = self.flags
        for n in range(13, 13 + self.nch):
            self._eval(n, t)
        self._eval(22, t)
        if f & 0x10:
            self._eval(23, t)
        lo, hi, spans = 0x2df, 0, []
        for n in range(13, 13 + self.nch):
            c = self.ch[n]
            rect(c)
            lo, hi = min(lo, c[4] & M16), max(hi, c[6] & M16)
            spans.append((c[0x14], c[0x16]))
        cv = self.ch[22]
        rect(cv)
        return {'spans': spans, 'pixels': (lo, hi), 'ink': self.ch[13][0xc], 'band': (cv[0x14], cv[0x16]),
                'vink': cv[0xc], 'flag80': bool(f & 0x80),
                'colour': [self.ch[23][i] for i in (0, 2, 4)] if f & 0x10 else None}


def tbar_t(pos, flags=0):
    """0x7510e / 0x75882: T-bar 0..511 -> t (16.16); flags bit 3 = there and back."""
    if flags & 8:
        pos *= 2
        if pos > 0x1ff:
            pos = 0x3fe - pos
    q = (pos << 16) // 0x1ff
    return 0x10000 if q > 0xffff else q


def auto_ts(fields, flags=0):
    """0x75006 / 0x75794 started at T-bar 0: t of each of the `fields` steps."""
    d3 = ((0x20000 if flags & 8 else 0x10000) // fields) & M16
    out, d4 = [], d3
    for _ in range(fields):
        out.append(0x20000 - d4 if flags & 8 and d4 > 0x10000 else d4)
        d4 += d3
    return out


def load(path):
    """crouton file -> (0x1C payload, tag 0x63, tags); the data tree is wherever the path is."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import algo_scan as S
    tags = dict(S.crud(path)[1])
    return tags[0x1000001c], tags.get(0x80000063, 0), tags


def picture(r):
    """241 x 192 cell picture of one digital step: 0..0xbf source cell (video), 0xe8..0xef colour
    fill, 0xff/0xfe background key; lines outside r['lines'] and cells outside r['cells'] are the
    background row (0x311ec copper switch). Returns (key bytes, source line per control line)."""
    out = [[0xff] * 192 for _ in range(241)]
    lo, hi = r['cells']
    if r['lines']:
        for k in range(r['lines'][0], r['lines'][1] + 1):
            out[k][lo:hi + 1] = r['H'][lo:hi + 1]
    return out


if __name__ == '__main__':
    pl, kind, tags = load(sys.argv[1])
    a = Algo(pl, kind)
    if '--png' in sys.argv:
        import numpy as np
        from PIL import Image
        dst = sys.argv[sys.argv.index('--png') + 1]
        tiles = []
        for pos in range(0, 512, 34):
            r = a.step(tbar_t(pos, a.flags))
            if kind:
                img = np.zeros((241, 736), np.uint8) + 40
                b0, b1 = r['band']
                for s0, s1 in r['spans']:
                    if s0 >= 0 and b0 >= 0:
                        img[b0:b1 + 1, s0:s1 + 1] = 220
                img = img[:, ::4]
            else:
                cells = picture(r)
                img = np.zeros((241, 192), np.uint8)
                for k in range(241):
                    for x in range(192):
                        h = cells[k][x]
                        if h <= 0xbf:      # video: checker of 8-cell x 16-line source blocks
                            img[k, x] = 140 + 90 * ((((h - 3) >> 3) + (r['V'][k] >> 4)) & 1)
                        elif 0xe8 <= h <= 0xef:
                            img[k, x] = 70
            tiles.append(img)
        sheet = np.vstack([np.hstack(tiles[i:i + 4]) for i in range(0, 16, 4)])
        Image.fromarray(sheet).resize((sheet.shape[1] * 2, sheet.shape[0] * 2), Image.NEAREST).save(dst)
        sys.exit()
    for pos in [int(x) for x in sys.argv[2:]] or [0, 256, 511]:
        r = a.step(tbar_t(pos, a.flags))
        print(pos, {k: (v.hex() if isinstance(v, bytes) else v) for k, v in r.items()})
