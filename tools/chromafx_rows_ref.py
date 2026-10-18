"""Python reference for the ChromaFX row builder (independent of the C++ engine).

Decoded from 4.2/Programs/AuxLibs/ChromaFX (full symbols); addresses are in the
hunk image that tools/re/hunk.py loads. See tools/re/FINDINGS.md, "ChromaFX row
builder", for the write-up. Integer arithmetic only, so the C++ port can match it
bit for bit.

A 904-byte LUT record: 'LUT\\x01', $4 poster slider (editor only), $5 interpolation
style 0..6, $6 flags, $8 T-bar word, name at 24, then three tables of 96 RGB
entries at $28, $148, $268. Only tables 0 and 1 reach the rows (table 2 is the
editor's pre-posterize backup, LUT_SavePosterBuffer 0x830a).

LUT_SendMap (0x840a) calls SENDM9[$5] (0x848a; index >= 7 -> 0):
  0 LUT_SendDVEMap (spread)      1 LUT_SendSpectrumDVE      2 DORROT (rotate, d0=0)
  3 DOLROT (rotate, d0=-1)       4 DORNUK (nuke, d0=0)      5 DOLNUK (nuke, d0=-1)
  6 LUT_SendPosterDVE
Each builds 480 steps (one per DV line, d4 = 0..479). Step 479 is never computed:
LUT_CheckLinear (0xa05e) writes LUT_LinearRamp there instead.
Every step is a row of 96 composite "quads" (RGBYIQ 0x4938 then YIQQUAD 0x49c2):
bytes Y-I, Y-Q, Y+I, Y+Q, each clamped to 32..221.

rows(record) returns 480 rows of 96 RGBA tuples: each quad decoded back to RGB with
quad_to_rgb (the inverse RGBYIQ matrix, DV black 71 / white 200 -> 0 / 255).
"""
import struct

LUT_SIZE = 904
STEPS = 480
ENTRIES = 96


# ---------------------------------------------------------------------------
# 68000 helpers
def w16(x):
    return x & 0xFFFF


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def s32(x):
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x & 0x80000000 else x


def divs_w(dividend, divisor):
    """divs.w: 32-bit signed / 16-bit signed -> quotient word (truncated toward 0).
    On overflow the 68000 leaves the destination unchanged: return its low word."""
    n = s32(dividend)
    q = abs(n) // abs(divisor)
    if (n < 0) != (divisor < 0):
        q = -q
    if q < -0x8000 or q > 0x7FFF:
        return w16(dividend)
    return w16(q)


def divu_w(dividend, divisor):
    """divu.w: 32-bit unsigned / 16-bit unsigned -> quotient word (overflow: unchanged)."""
    n = dividend & 0xFFFFFFFF
    q = n // divisor
    if q > 0xFFFF:
        return w16(n)
    return q


# ---------------------------------------------------------------------------
# RGBYIQ 0x4938 and YIQQUAD 0x49c2
def rgbyiq(r, g, b):
    """-> (Y, I, Q) bytes as the routine packs them (I and Q two's complement)."""
    y = ((r * 0x26DF + g * 0x4C51 + b * 0x0ED2 + 0x470000) >> 16) & 0xFF
    i8 = (r * 0x4D7C - g * 0x23C1 - b * 0x29BB) >> 8          # asr.l #8
    i8 = i8 + 0x80 if i8 >= 0 else i8 - 0x80                   # bpl / addi.w / subi.w
    ib = (w16(i8) >> 8) & 0xFF                                 # clr.b, or.w
    q = r * 0x1B90 - g * 0x43FF + b * 0x286F
    q = q + 0x8000 if q >= 0 else q - 0x8000
    qb = ((q & 0xFFFFFFFF) >> 16) & 0xFF                       # swap, andi.w #$ff
    return y, ib, qb


def yiqquad(y, ib, qb):
    """-> (Y-I, Y-Q, Y+I, Y+Q): byte arithmetic, each clamped to 0x20..0xdd (unsigned)."""
    i = ib - 256 if ib & 0x80 else ib

    def clamp(v):
        v &= 0xFF
        return 0xDD if v >= 0xDD else (0x20 if v < 0x20 else v)
    return (clamp(y - i), clamp(y - qb), clamp(y + i), clamp(y + qb))


def quad_of(r, g, b):
    return yiqquad(*rgbyiq(r, g, b))


# ---------------------------------------------------------------------------
# Quad -> RGB (engine output). The quad is one subcarrier cycle of composite samples:
# Y = mean of the four, I = ((Y+I) - (Y-I)) / 2, Q = ((Y+Q) - (Y-Q)) / 2. RGB is the
# inverse of the RGBYIQ matrix scaled so that its Y row maps DV 71..200 onto 0..255
# (s = 129 / (255 * 33282 / 65536), the 0.99614 of tools/re/FINDINGS.md "Background
# colour generator"), rounded half up and clamped. Exact integer form:
#   rgb = round(adj(M) . (4Y - 284, 4I, 4Q) * 85 / DEN), DEN = 4 * 129 * det(M) / 99846
_ADJ = ((-280701684, -268269645, -173987289),
        (-280701684, 76232337, 181564317),
        (-280701684, 311070129, -478617435))
_NUM = -85                       # sign folded in: det(M) < 0
_DEN = 48280689648


def _round_div(n, d):
    return (2 * n + d) // (2 * d)   # floor((n + d/2) / d), d > 0


def yiq4_to_rgb(y4, i4, q4):
    """(4*(Y-71), 4*I, 4*Q) -> (r, g, b), each 0..255."""
    out = []
    for row in _ADJ:
        n = (row[0] * y4 + row[1] * i4 + row[2] * q4) * _NUM
        out.append(min(255, max(0, _round_div(n, _DEN))))
    return tuple(out)


def yiq_to_rgb(y, i, q):
    """Integer DV YIQ (Y 71..200) -> RGB."""
    return yiq4_to_rgb(4 * (y - 71), 4 * i, 4 * q)


def quad_to_rgb(qd):
    a, b, c, d = qd   # Y-I, Y-Q, Y+I, Y+Q
    return yiq4_to_rgb(a + b + c + d - 284, 2 * (c - a), 2 * (d - b))


# ---------------------------------------------------------------------------
# LUT_LinearRamp 0xa036 / LUT_CheckLinear 0xa05e: step 479 holds a ramp of 184 longs
# (bytes 4, 6, ... 252, then 254) written from pixel 0 of the line; the colour rows
# start at plane byte 8 = pixel 64 = long 16, so entry k of row 479 is long 16 + k.
def ramp_row():
    row = []
    for k in range(ENTRIES):
        n = 16 + k
        v = 4 + 2 * n if n < 125 else 254
        row.append((v, v, v, v))
    return row


# ---------------------------------------------------------------------------
# Spread: LUT_SendDVEMap 0x5b5c
def spread_steps(t0, t1):
    acc = []   # per channel [step, acc] 8.8
    for k in range(ENTRIES * 3):
        a, b = t0[k], t1[k]
        st = divs_w(_spread_dividend(a, b), 0x1E0)
        acc.append([st, (a << 8) | 0x80])
    rows = []
    for _ in range(STEPS - 1):
        row = []
        for e in range(ENTRIES):
            rgb = []
            for c in range(3):
                ch = acc[e * 3 + c]
                ch[1] = w16(ch[1] + ch[0])
                rgb.append(ch[1] >> 8)
            row.append(quad_of(*rgb))
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Spectrum: LUT_SendSpectrumDVE 0x5f00 with RGBHSI 0x47c0, LUT_LongHue 0x5cca, HSIRGB 0x4890
def rgbhsi(r, g, b):
    """-> (H word, S byte, V byte)."""
    d2, d3, d4 = (r << 4) | 8, (g << 4) | 8, (b << 4) | 8
    mn = min(d2, d3, d4)
    mx = max(d2, d3, d4)
    dl = mx - mn
    if mx == 0:
        s = 0
    elif mx == dl:
        s = 0xFFFF
    else:
        s = (dl << 16) // mx
    if s == 0:
        return 0, 0, mx >> 4 & 0xFF

    def e(x):
        return ((mx - x) << 12) // dl
    if mx == d2:
        h = e(d4) - e(d3)
    elif mx == d3:
        h = e(d2) - e(d4) + 0x2000
    else:
        h = e(d3) - e(d2) + 0x4000
    h = w16(h)
    if h & 0x8000:
        h = w16(h + 0x6000)
    h = (((h * 0xAAAB) << 2) & 0xFFFFFFFF) >> 16
    return h, (s >> 8) & 0xFF, (mx >> 4) & 0xFF


def long_hue(h1, h0, d7):
    """LUT_LongHue: d0 = H1, d1 = H0, d3 = 480, d7 = state carried across entries.
    -> (hue step word, new d7)."""
    diff = w16(h1 - h0)
    hi, lo = (d7 >> 16) & 0xFFFF, d7 & 0xFFFF

    def b17():
        return hi & 2

    target = 'H8'
    if diff & 0x8000:                          # LONGH1
        if not (lo & 0x8000):
            target = 'H4' if diff < 0xC000 else 'H2'
    else:
        if lo & 0x8000:
            target = 'H4' if diff >= 0x4000 else 'H2'
    if target == 'H2':
        if b17():
            target = 'H5'
        else:
            lo = 0
            target = 'H3' if hi else 'H8'
    elif target == 'H4':
        target = 'H3' if b17() else 'H5'
    if target == 'H3':
        hi = w16(hi + 1)
    elif target == 'H5':
        hi = 1 if hi == 0 else w16(hi - 1)
    lo = diff                                   # LONGH8
    d7 = (hi << 16) | lo
    d0 = ((s16(hi) >> 1) << 16 & 0xFFFFFFFF) + diff   # asr.l #1, clr.w, add.l
    d0 &= 0xFFFFFFFF
    neg = (not (d0 & 0x8000)) if (hi & 1) else bool(d0 & 0x8000)
    if neg:                                     # LONGHB
        d0 = (d0 & 0xFFFF0000) | w16(-d0)
        q = divu_w(d0, 0x1E0)
        step = w16(-q)
    else:                                       # LONGHA
        step = divu_w(d0, 0x1E0)
    return step, d7


def hsirgb(h, s, v):
    d3 = ((~s & 0xFF) << 8) | 0x80
    d4 = w16(v << 8) | 0x80
    prod = h * 6
    f = prod & 0xFFFF
    sector = prod >> 16

    def mul(a, b):
        return ((a & 0xFFFF) * (b & 0xFFFF)) >> 16
    nf = ~f & 0xFFFF
    if sector == 0:
        r, g, b = d4, mul(f, d4), 0
    elif sector == 1:
        r, g, b = mul(nf, d4), d4, 0
    elif sector == 2:
        r, g, b = 0, d4, mul(f, d4)
    elif sector == 3:
        r, g, b = 0, mul(nf, d4), d4
    elif sector == 4:
        r, g, b = mul(f, d4), 0, d4
    else:
        r, g, b = d4, 0, mul(nf, d4)
    r = w16(r + mul(w16(d4 - r), d3))
    g = w16(g + mul(w16(d4 - g), d3))
    b = w16(b + mul(w16(d4 - b), d3))
    return r >> 8, g >> 8, b >> 8


def spectrum_steps(t0, t1):
    acc = []
    d7 = 0
    for e in range(ENTRIES):
        h0, sa, va = rgbhsi(*t0[e * 3:e * 3 + 3])
        h1, sb, vb = rgbhsi(*t1[e * 3:e * 3 + 3])
        hstep, d7 = long_hue(h1, h0, d7)
        sst = divs_w(_sat_dividend(sa, sb), 0x1E0)
        vst = divs_w(_spread_dividend(va, vb), 0x1E0)
        acc.append([[hstep, h0], [sst, (sa << 8) | 0x80], [vst, (va << 8) | 0x80]])
    rows = []
    for _ in range(STEPS - 1):
        row = []
        for ch in acc:
            for c in ch:
                c[1] = w16(c[1] + c[0])
            row.append(quad_of(*hsirgb(ch[0][1], ch[1][1] >> 8, ch[2][1] >> 8)))
        rows.append(row)
    return rows


def _spread_dividend(a, b):
    """d1 = b - a (long), then asl.w #8: only the low word shifts."""
    d = (b - a) & 0xFFFFFFFF
    return (d & 0xFFFF0000) | w16(d << 8)


def _sat_dividend(sa, sb):
    """Saturation: moveq #0 / move.w / asr.w #8 sign-extends S into the low word only."""
    def sx(v):
        return v | 0xFF00 if v & 0x80 else v
    d = (sx(sb) - sx(sa)) & 0xFFFFFFFF
    return (d & 0xFFFF0000) | w16(d << 8)


# ---------------------------------------------------------------------------
# Rotate / Nuke / Poster work on table 0 converted to quads.
def table_quads(t):
    return [quad_of(*t[e * 3:e * 3 + 3]) for e in range(ENTRIES)]


def _offsets(start, inc, wrap):
    """d6 per step: step n uses d6 after n additions (wrap added when negative)."""
    d6 = start
    out = []
    for _ in range(STEPS - 1):
        out.append(d6 >> 16)
        d6 += inc
        if d6 < 0:
            d6 += wrap
    return out


def rotate_steps(t0, left):
    """LUT_SendMapRotate 0x6216 / RotateLong 0x4b66: out[i] = src[(i + r) % 96]."""
    src = table_quads(t0)
    offs = _offsets(0, 0x3334, 0x600000) if left else _offsets(0xFFFF, -0x3334, 0x600000)
    return [[src[(i + r) % ENTRIES] for i in range(ENTRIES)] for r in offs]


def nuke_steps(t0, left):
    """LUT_SendMapNuke 0x64ce / NukeLong 0x4c22: the table read back and forth, period 192."""
    src = table_quads(t0)
    pal = src + src[::-1]
    offs = _offsets(0, 0x6667, 0xC00000) if left else _offsets(0xFFFF, -0x6667, 0xC00000)
    return [[pal[(i + r) % (2 * ENTRIES)] for i in range(ENTRIES)] for r in offs]


STEPQUANTIZE = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 48)   # LUT_STEPQUANTIZE 0x817f


def quad_quantize(src, q):
    """LUT_DoQuadQuantize 0x810c with d2 = 14, a2 = LUT_STEPQUANTIZE."""
    n = STEPQUANTIZE[(w16(q * 14)) >> 8]
    blocks = ENTRIES // n
    rem = ENTRIES - n * blocks
    lead = rem >> 1
    tail = rem - lead
    out = [src[lead >> 1]] * lead if lead else []
    base = lead
    for _ in range(blocks):
        out += [src[base + (n >> 1)]] * n
        base += n
    if tail:
        out += [src[base + (tail >> 1)]] * tail
    return out


def quad_average(row):
    """LUT_QuadAverageQuantize 0x80a4: each byte with its neighbours' same-phase bytes."""
    out = []
    for e in range(ENTRIES):
        if e == 0:
            out.append(tuple((row[0][k] + row[1][k]) >> 1 for k in range(4)))
        elif e == ENTRIES - 1:
            out.append(tuple((row[e][k] + row[e - 1][k]) >> 1 for k in range(4)))
        else:
            out.append(tuple((row[e][k] + row[e + 1][k] + row[e - 1][k]) // 3 for k in range(4)))
    return out


def poster_steps(t0):
    """LUT_SendPosterDVE 0x6798: d6 += 0x8888 per step, level q = d6 >> 16."""
    src = table_quads(t0)
    hist = [None, None, None]   # h1, h2, h3 (previous steps' h0)
    rows = []
    d6 = 0
    for _ in range(STEPS - 1):
        h0 = quad_quantize(src, d6 >> 16)
        if d6 >= 0x360000:
            h0 = quad_average(h0)
        if d6 >= 0x120000:
            out = [tuple((h0[e][k] + hist[0][e][k] + hist[1][e][k] + hist[2][e][k]) >> 2 for k in range(4))
                   for e in range(ENTRIES)]
        else:
            out = h0
        rows.append(out)
        hist = [h0, hist[0], hist[1]]
        d6 += 0x8888
    return rows


# ---------------------------------------------------------------------------
INTERP_NAMES = ('spread', 'spectrum', 'rotate_right', 'rotate_left', 'nuke_right', 'nuke_left', 'poster')


def interp_index(rec):
    """SENDM2/SENDM3: $5 >= 7 -> 0."""
    v = rec[5]
    return v if v < 7 else 0


def quad_rows(rec, interp=None):
    """480 rows of 96 quads (DV lines 0..479)."""
    if len(rec) < LUT_SIZE or rec[:4] != b'LUT\x01':
        raise ValueError('not a ChromaFX LUT record')
    t0 = rec[0x28:0x148]
    t1 = rec[0x148:0x268]
    k = interp_index(rec) if interp is None else interp
    if k == 0:
        rows = spread_steps(t0, t1)
    elif k == 1:
        rows = spectrum_steps(t0, t1)
    elif k in (2, 3):
        rows = rotate_steps(t0, left=(k == 3))
    elif k in (4, 5):
        rows = nuke_steps(t0, left=(k == 5))
    else:
        rows = poster_steps(t0)
    return rows + [ramp_row()]


def rows(rec, interp=None):
    """480 rows of 96 (r, g, b, 255)."""
    return [[quad_to_rgb(q) + (255,) for q in row] for row in quad_rows(rec, interp)]


def row_bytes(row):
    return b''.join(struct.pack('4B', *px) for px in row)


# ---------------------------------------------------------------------------
# Line selection: LUT_TransitionMode 0x89e8 / LUT_FilterMode 0x89d4 / sprites 0x9bbc, 0x9c00
def tbar_row(tbar):
    v = (tbar >> 2) - 9
    return v if v >= 0 else 0


def line_table(transition, v):
    """The 241-byte table LUT_BuildTranSprite / LUT_BuildFilterSprite send (sw_m318)."""
    t = [0] * 241
    if transition:
        d0 = (2 * v + 2) & 0xFF
        for k in range(241):
            t[k] = d0 if k % 2 == 0 else (d0 + 1) & 0xFF
        t[0] = 0xF0
    else:
        d0 = 2 * v + 2
        d1 = d0 - 1
        t[0] = 0xF0
        k = 1
        for _ in range(120):
            t[k] = d1 & 0xFF
            t[k + 1] = d0 & 0xFF
            k += 2
            d1 += 2
            d0 += 2
            if d0 > 0xEE:
                d1, d0 = 1, 2
    return t


def raster_row(table, y):
    """DV row (index into rows()) read by raster line y: control line y // 2 gives the table
    value w, the line parity picks DV line 2w or 2w + 1 (the DVE convention, spec 5.10).
    None when that row was never written (w = 0xf0 on control line 0 -> rows 480, 481)."""
    r = 2 * table[y // 2] + (y & 1)
    return r if r < STEPS else None


# Cycling: LUT_CycleModeOn 0x9b24 clears the 16.16 accumulator $1f9f0 and stores the step in
# $1f9f4; the Switcher calls the LUT_*CycleRoutine once per video field (0x312c, polled on
# every VPOSR LOF change at 0x5e1fa / 0x5e3d4). LUT_GridCycle* (0x9e56..) takes the step from
# the table at 0x9b14 indexed by ($1f4(a5) + 1) & 3, $1f4 = tag 0x55 & 3 (0x171ea).
CYCLE_STEP = (0x4000, 0x8000, 0x10000, 0x8000)   # 0x9b14


def cycle_step(tag55):
    return CYCLE_STEP[((tag55 & 3) + 1) & 3]


def cycle_row(mode, step, fields):
    """Row position v after `fields` calls; mode 1 forward, 2 backward, 3 both (tag 0x7f)."""
    if mode == 1:
        return ((fields * step) % (0x77 << 16)) >> 16
    if mode == 2:
        return ((-fields * step) % (0x77 << 16)) >> 16
    if mode == 3:
        h = ((fields * step) % (0xEE << 16)) >> 16
        return h if h < 0x77 else 0xED - h
    return 0
