"""Golden frames for the 14 Bar ILBM croutons (4000FX/Border), written to tests/golden/bars.jsonl.

Run from anywhere:

    python -P -E vtoast_reborn/tools/make_bars_golden.py

The source of truth is the default model of <root>/tools/re/bars_ref.py (tools/re/BARS.md section 5).
One line per crouton: "source" (relative to the Toaster root), "tbar" (the positions), "x" / "y"
(the picture offset from channel 29 at each position, before the plane-half switch) and "crc": the
zlib crc32 of the whole 768x482 frame as RGBA bytes (alpha 255) with A = navy 0x1B2A80 and
B = crimson 0xB5122E solid.
"""
import json, os, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # the Toaster root folder
OUT = os.path.join(os.path.dirname(HERE), 'tests', 'golden', 'bars.jsonl')
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import bars_ref as BR

TBARS = (0, 128, 256, 384, 511)


def main():
    with open(OUT, 'w', encoding='utf-8', newline='\n') as fo:
        for nm in BR.NAMES:
            path = os.path.join(BR.BORDER, nm)
            b = BR.Bar(path)
            xy = [b.xy(p) for p in TBARS]
            crcs = []
            for p in TBARS:
                rgb = b.rgb(p)
                rgba = np.concatenate([rgb, np.full(rgb.shape[:2] + (1,), 255, np.uint8)], axis=2)
                crcs.append(zlib.crc32(np.ascontiguousarray(rgba).tobytes()) & 0xFFFFFFFF)
            rec = dict(source=os.path.relpath(path, ROOT).replace(os.sep, '/'), tbar=list(TBARS),
                       x=[x for x, _ in xy], y=[y for _, y in xy], crc=crcs)
            fo.write(json.dumps(rec, separators=(',', ':'), ensure_ascii=False) + '\n')
    print(OUT, len(BR.NAMES))


if __name__ == '__main__':
    main()
