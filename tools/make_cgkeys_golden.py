"""Golden data for the ToasterCG key pages (4.2/CG/Keys), written to tests/golden/cgkeys.jsonl.

Run from anywhere:

    python -P -E vtoast_reborn/tools/make_cgkeys_golden.py

The source of truth is <root>/tools/re/cgkey_ref.py (tools/re/CGKEY.md): its page decoder (load), its
dissolve law (ramp, verified against the original under emulation) and its mix guess (mix_guess).

"page" lines, one per page: "source" (relative to the Toaster root), "y" (BMHD y), "fill_crc": the zlib
crc32 of the 768x482 RGBA frame (alpha 255) holding the decoded composite at raster rows 1 + y .. (x 0)
and black elsewhere; "alpha_crc": the crc32 of the 768x482 byte plane holding the 2-bit alpha at the
same place and 0 elsewhere; "levels" / "mix_crc": the crc32 of Program at those dissolve levels, the
fill over Main = navy 0x1B2A80 solid: out = (Main * (255 - k) + fill * k + 127) // 255 per gun with
k = mix_guess(alpha, L).

"ramp" lines: the levels an Auto writes ("speed" = $1f0, "on" = the on flag, "tbar" = the T-bar
0..511). "trap" = 1 where the original divides by zero (CGKEY.md section 2); there "levels" is the
ruling of docs/2026-10-30-cg-keys-design.md section 2 D: one step to the end, the final write alone.
"""
import json, os, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # the Toaster root folder
OUT = os.path.join(os.path.dirname(HERE), 'tests', 'golden', 'cgkeys.jsonl')
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import cgkey_ref as CG

PAGES = ('4StarCinema', 'Feature', 'KeyPage1', 'KeyPage2', 'LowerThird1', 'LowerThird2')
W, H, Y0 = 768, 482, 1                                    # kRasterW, kRasterH, kFramestoreY0
NAVY = (0x1B, 0x2A, 0x80)
LEVELS = (17, 100, 191, 255)
RAMPS = [(60, on, 0) for on in (0, 1)] + [(20, on, 0) for on in (0, 1)] + [(4, on, 0) for on in (0, 1)] + \
        [(60, on, p) for on in (0, 1) for p in (100, 300, 510)] + \
        [(60, on, 34) for on in (0, 1)] + [(2, on, 0) for on in (0, 1)]


def crc(a):
    return zlib.crc32(np.ascontiguousarray(a).tobytes()) & 0xFFFFFFFF


def main():
    with open(OUT, 'w', encoding='utf-8', newline='\n') as fo:
        for nm in PAGES:
            path = os.path.join(CG.KEYS, nm)
            pg = CG.load(path)
            y, h, w = pg['y'], pg['h'], pg['w']
            fill = np.zeros((H, W, 4), np.uint8)
            fill[..., 3] = 255
            fill[Y0 + y:Y0 + y + h, :w, :3] = pg['rgb']
            alpha = np.zeros((H, W), np.uint8)
            alpha[Y0 + y:Y0 + y + h, :w] = pg['alpha']
            main_ = np.zeros_like(fill)
            main_[..., :3] = NAVY
            main_[..., 3] = 255
            mixes = []
            for L in LEVELS:
                k = np.array([CG.mix_guess(a, L) for a in range(4)], np.int64)[alpha][..., None]
                out = main_.astype(np.int64).copy()
                out[..., :3] = (main_[..., :3] * (255 - k) + fill[..., :3].astype(np.int64) * k + 127) // 255
                mixes.append(crc(out.astype(np.uint8)))
            rec = dict(kind='page', source=os.path.relpath(path, ROOT).replace(os.sep, '/'), y=int(y),
                       fill_crc=crc(fill), alpha_crc=crc(alpha), levels=list(LEVELS), mix_crc=mixes)
            fo.write(json.dumps(rec, separators=(',', ':')) + '\n')
        for speed, on, p in RAMPS:
            try:
                levels, trap = CG.ramp(speed, on, p), 0
            except ZeroDivisionError:
                levels, trap = [CG._level(0xffff, on)], 1
            fo.write(json.dumps(dict(kind='ramp', speed=speed, on=on, tbar=p, trap=trap, levels=levels),
                                separators=(',', ':')) + '\n')
    print(OUT, len(PAGES), len(RAMPS))


if __name__ == '__main__':
    main()
