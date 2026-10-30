"""Golden data for the four Positionable croutons (4000FX/Special), written to tests/golden/position.jsonl.

Run from anywhere:

    python -P -E vtoast_reborn/tools/make_position_golden.py

The source of truth is <root>/tools/re/position_ref.py (tools/re/POSITION.md): reset + live(h, v)
(the mouse loop 0x5e2e8 / 0xb71b2), then for "bake" 1 the bake 0xb7054 at T-bar "tbar"
(Space / Return), then algo_ref.Algo on that payload. One line per case: "source" (relative to the
Toaster root), "kind", "moves" (the mouse-count deltas dx, dy, ... applied from the centre with
mouse_step and the clamps), the anchors "h" / "v" they give, "tbar" (the drop point), "bake",
"stop" ($30, T-bar t space) and "ec" (the new T-bar) of the bake (0 / "tbar" when not baked),
"payload" (zlib crc32 of the rewritten payload) and "frames": per rendered T-bar position (a fresh
Algo, one step at tbar_t), kind 3: the H / V table CRCs, cell and line ranges as in algo.jsonl;
kind 4: the span CRC, ranges, inks and "frame", the crc32 of the 768x482 RGBA frame (alpha 255)
vt::renderAlgoWipe draws with A = navy 0x1B2A80 and B = crimson 0xB5122E solid.
"""
import json, os, struct, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # the Toaster root folder
OUT = os.path.join(os.path.dirname(HERE), 'tests', 'golden', 'position.jsonl')
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import algo_ref as R
import position_ref as P

NAMES = ('Positionable Expand', 'Positionable Shrink', 'Positionable WIPE IN', 'Positionable WIPE OUT')
A, B = (0x1B, 0x2A, 0x80, 255), (0xB5, 0x12, 0x2E, 255)
W, H = 768, 482
# (moves, drop T-bar, bake): centred; small moves; a drift left; the clamp (every axis pinned).
CASES = [
    ([0, 0], 256, 0),
    ([10, -6, 3, 4], 200, 0),
    ([10, -6, 3, 4], 200, 1),
    ([-1, -1, -1, -1, 1, 0], 100, 1),
    ([-25, 12], 100, 1),
    ([127, -127] * 4, 300, 0),
    ([127, -127] * 4, 300, 1),
    ([-40, 30], 480, 1),
]


def anchors(moves, acc, c, e):
    ph, pv = c, e                                         # P = (H + $c, V + $e), from the centre
    for dx, dy in zip(moves[::2], moves[1::2]):
        pv = min(max(pv + P.mouse_step(dy, acc), 0), 2 * e - 1)
        ph = min(max(ph + P.mouse_step(dx, acc), 0), 2 * c - 1)
    return ph - c, pv - e


def crc(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def wipe_frame(r, inverse):
    """vt::renderAlgoWipe for one step, as RGBA bytes."""
    ink = r['ink'] & 1
    lo, hi = r['pixels']
    xs = np.arange(W)
    in_span = np.zeros(W, bool)
    for s0, s1 in r['spans']:
        in_span |= (s0 >= 0) & (s0 <= xs) & (xs <= s1)
    row = np.where(in_span, ink, np.where((xs >= lo) & (xs <= hi), ink ^ 1, 0)).astype(bool)
    b0, b1 = r['band']
    const_bit = 0 if (r['vink'] ^ inverse) & 1 else 1
    img = np.empty((H, W, 4), np.uint8)
    for y in range(H):
        in_band = b0 >= 0 and b0 <= y <= b1
        bits = np.full(W, bool(const_bit)) if (in_band if inverse else not in_band) else row
        img[y] = np.where(bits[:, None], B, A)
    return img.tobytes()


def frame(pl, kind, flags, tbar):
    r = R.Algo(bytes(pl), 1 if kind == 4 else 0).step(R.tbar_t(tbar, flags))
    f = dict(tbar=tbar)
    if kind == 3:
        l0, l1 = r['lines'] or (-1, -1)
        f.update(h=crc(r['H']), v=crc(r['V']), c0=r['cells'][0], c1=r['cells'][1], l0=l0, l1=l1)
    else:
        f.update(s=crc(b''.join(struct.pack('>hh', *sp) for sp in r['spans'])), p0=r['pixels'][0],
                 p1=r['pixels'][1], ink=r['ink'], b0=r['band'][0], b1=r['band'][1], vink=r['vink'],
                 frame=crc(wipe_frame(r, 1 if flags & 0x80 else 0)))
    return f


def main():
    n = 0
    with open(OUT, 'w', encoding='utf-8', newline='\n') as fo:
        for nm in NAMES:
            path = os.path.join(ROOT, '4.2', 'Effects', '4000FX', 'Special', nm)
            pl, kind, _ = R.load(path)
            acc, c, e = struct.unpack('>hhh', pl[0xa:0x10])
            flags = struct.unpack('>I', pl[4:8])[0]
            for moves, tbar, bake in CASES:
                h, v = anchors(moves, acc, c, e)
                p = P.live(pl, h, v)
                stop, ec = 0, tbar
                if bake:
                    p, stop, ec = P.bake(p, tbar)
                frames = [frame(p, kind, flags, pos) for pos in sorted({0, 128, tbar, ec, 384, 511})]
                rec = dict(source=os.path.relpath(path, ROOT).replace(os.sep, '/'), kind=kind, moves=moves,
                           h=h, v=v, tbar=tbar, bake=bake, stop=stop, ec=ec, payload=crc(p), frames=frames)
                fo.write(json.dumps(rec, separators=(',', ':'), ensure_ascii=False) + '\n')
                n += 1
    print(OUT, n)


if __name__ == '__main__':
    main()
