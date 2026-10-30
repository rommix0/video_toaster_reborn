"""Golden data for the ToasterCG scroll and crawl pages (4.2/CG/Scrolls, 4.2/CG/Crawls), written to
tests/golden/cgscrolls.jsonl.

Run from anywhere:

    python -P -E vtoast_reborn/tools/make_cgscrolls_golden.py

The source of truth is <root>/tools/re/cgscroll_ref.py (tools/re/CGSCROLL.md): its page parser, its line
and crawl-window renderers and palette (verified bit-exact against switcher.main under emulation), and
its per-field motion (scroll_run / scroll_field, crawl_strip / crawl_field, crawl_end_field).

One line per (page, speed). "source" (relative to the Toaster root), "kind" (scroll / crawl), "speed" (the
speed the page is played at: its tag 0x8000006a, or another one where the test overrides it), "end_field"
(the first field with the page off), for a scroll "stop_field" and "hold" (pause mode, else -1 and 0),
for a crawl "windows" and "band" (frame lines [top, bottom)). "fields" / "crc": the zlib crc32 of the
768x482 RGBA Program frame (alpha 255) at those fields, j = 0 the first field after play, with Main = navy
0x1B2A80 solid and the page keyed over it:

* frame line Y is raster line Y; every field shows both line parities from its own state (vtoast's
  progressive frames, docs/2026-10-17-switcher-design.md 5.10): a scroll field j shows, on frame lines
  0..475, fed row T[j-1] + Y - 480 (scroll_field's law for every Y) at raster columns 64..703
  (HardwareGuesses::cgScrollX0); a crawl field j shows crawl_field's band at raster columns 0..767;
* Amiga index 0 is transparent (Main shows); 1..7 are palette() colours, nibble * 17 per gun;
* a pause scroll holds its last top from stop_field on; fields stop + hold + k (k = 0..21) fade at level
  L = 255 - 12k: out = (Main * (255 - L) + colour * L + 127) // 255 per gun;
* from end_field on, Main alone.
"""
import json, os, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # the Toaster root folder
OUT = os.path.join(os.path.dirname(HERE), 'tests', 'golden', 'cgscrolls.jsonl')
sys.path.insert(0, os.path.join(ROOT, 'tools', 're'))
import cgscroll_ref as R

W, H = 768, 482                                           # kRasterW, kRasterH
NAVY = (0x1B, 0x2A, 0x80)
SCROLL_X0 = 64                                            # HardwareGuesses::cgScrollX0
FADE = [255 - 12 * k for k in range(22)]


def crc(a):
    return zlib.crc32(np.ascontiguousarray(a).tobytes()) & 0xFFFFFFFF


def first_shown(frame, main):
    """The first field whose Program is not Main alone (the first rows / columns of the page on air)."""
    j = 0
    while crc(frame(j)) == main:
        j += 1
    return j


def lut(page):
    return np.array([[((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17] for c in R.palette(page)], np.int64)


def program(idx, top, left, colours, level=255):
    """Main = navy with the index rows idx keyed over it at raster (top, left)."""
    out = np.zeros((H, W, 4), np.int64)
    out[..., :3] = NAVY
    out[..., 3] = 255
    h, w = idx.shape
    reg = out[top:top + h, left:left + w, :3]
    c = colours[idx]
    mixed = (reg * (255 - level) + c * level + 127) // 255
    reg[idx != 0] = mixed[idx != 0]
    return out.astype(np.uint8)


def scroll_record(name, speed=None, extra=()):
    p = R.load(name)
    run = R.scroll_run(p, speed=speed)
    cov, sh = R.scroll_stream(p, run)
    rows = cov | (sh << 2)
    T = run['T']
    colours = lut(p)
    pause = run['mode'] == 2
    stop = run.get('stop_field', -1)
    hold = run.get('hold', 0)
    end = run['end_field']

    def frame(j):
        if j >= end:
            return program(np.zeros((1, 1), np.uint8), 0, 0, colours)
        t = T[min(j, len(T)) - 1] if j > 0 else 0
        idx = np.zeros((476, 640), np.uint8)
        m = t + np.arange(476) - 480
        ok = (m >= 0) & (m < len(rows))
        idx[ok] = rows[m[ok]]
        level = FADE[j - stop - hold] if pause and j >= stop + hold else 255
        return program(idx, 0, SCROLL_X0, colours, level)

    starts = [next(f for f, t in enumerate(T) if t >= s) for s in run['starts'][:3]]   # the first lines start
    first = first_shown(frame, crc(frame(end)))
    fields = [0, 1, 2, 3, 4, 5, first, first + 1] + [s + d for s in starts for d in (0, 1)] + [end // 3, end // 2, end - 1, end, end + 7]
    if pause:
        fields += [stop - 1, stop, stop + 1, stop + hold - 1, stop + hold, stop + hold + 1, stop + hold + 10,
                   stop + hold + 21]
    fields = sorted(set(f for f in list(fields) + list(extra) if f >= 0))
    return dict(kind='scroll', source='4.2/CG/' + name, speed=run['speed'], end_field=end, stop_field=stop,
                hold=hold, fields=fields, crc=[crc(frame(j)) for j in fields])


def crawl_record(name, speed=None):
    p = R.load(name)
    speed = p.tags.get(0x8000006a, p.speed) if speed is None else speed
    strip = R.crawl_strip(p)
    nwin = strip[2]
    end = R.crawl_end_field(p, nwin, speed)
    top, bot = R.crawl_band(p)
    colours = lut(p)
    v = R.CRAWL_V(speed)

    def frame(j):
        if j >= end:
            return program(np.zeros((1, 1), np.uint8), 0, 0, colours)
        return program(R.crawl_field(p, strip, j, speed), top, 0, colours)

    enter = (768 + 8 - 767 + v - 1) // v                  # the first glyph (strip column 8) reaches column 767
    first = first_shown(frame, crc(frame(end)))
    fields = sorted(set([0, 1, enter - 1, enter, enter + 1, first, first + 1, 100, end // 3, end // 2, end - 1, end, end + 5]))
    return dict(kind='crawl', source='4.2/CG/' + name, speed=speed, end_field=end, windows=nwin, band=[top, bot],
                fields=fields, crc=[crc(frame(j)) for j in fields])


def main():
    recs = [scroll_record('Scrolls/NightOfTerror'),
            scroll_record('Scrolls/MojoWorld.Scroll'),
            scroll_record('Scrolls/MojoWorld.Scroll', speed=0),   # one row per frame: long fields only
            crawl_record('Crawls/EBS.Crawl'),
            crawl_record('Crawls/Novel.Crawl'),
            crawl_record('Crawls/Novel.Crawl', speed=1)]
    with open(OUT, 'w', encoding='utf-8', newline='\n') as fo:
        for r in recs:
            fo.write(json.dumps(r, separators=(',', ':')) + '\n')
    for r in recs:
        print(r['source'], r['speed'], 'end', r['end_field'], len(r['fields']), 'fields')
    print(OUT)


if __name__ == '__main__':
    main()
