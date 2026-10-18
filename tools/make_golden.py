"""Generate the golden data the C++ engine tests compare against.

Run from the Toaster root folder (the parent of vtoast_reborn):

    python -P -E vtoast_reborn/tools/make_golden.py

The Python reference decoders in <root>/tools are the source of truth; this
script walks converted/manifest.csv (rows with status "converted") and writes
JSON-lines files to vtoast_reborn/tests/golden/:

  ilbm.jsonl      gradient wipes: size, index CRC, gray LUT
  anims.jsonl     ANIM and DVE effects: per-frame index CRC, per-frame bit-plane CRC
  mixtables.jsonl ANIM effects: how the converter maps indices to matte/overlay
  dve.jsonl       DVE effects: per-frame key/line/art CRCs (overscan unfilled)
  timing.jsonl    effect timing from the crouton tags
  chromafx.jsonl  ChromaFX LUTs from the croutons and the bank files

All CRCs are zlib crc32. Index arrays are hashed as uint8, shape (h, w); the one
9-plane bitmap (Warp To Globe !) is hashed as little-endian uint16 instead.
"""
import csv, json, os, sys, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # the Toaster root folder
OUT = os.path.join(os.path.dirname(HERE), 'tests', 'golden')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import toaster_iff as T
import toaster_dve
import toaster_fx_convert as C
import chromafx_to_cube as X

TAG_MODE, TAG_SPEED = 0x80000022, 0x80000055
TAG_SLOW, TAG_MED, TAG_FAST, TAG_VAR = 0x80000057, 0x80000058, 0x80000059, 0x80000056
# Tag 0x55 is decoded through switcher.main's table at 0xe660c (Ruling 8):
SPEED_BY_TAG55 = ('medium', 'fast', 'variable', 'slow')
HUNK_MAGIC = b'\0\0\x03\xf3'


def crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def u8(a):
    """Index array -> uint8 (the C++ side holds one byte per pixel)."""
    a = np.asarray(a)
    assert a.size == 0 or (a.min() >= 0 and a.max() < 256), 'index does not fit in a byte'
    return np.ascontiguousarray(a, dtype=np.uint8)


def idx_bytes(a, planes):
    """Index array -> bytes. One byte per pixel up to 8 planes; a 9-plane bitmap
    (Warp To Globe !) has indices up to 511 and is hashed as little-endian uint16."""
    return u8(a).tobytes() if planes <= 8 else np.ascontiguousarray(a, dtype='<u2').tobytes()


def open_source(rel):
    """Mirror toaster_fx_convert.convert_file: (bytes, forms, type, tags)."""
    d = open(os.path.join(ROOT, rel), 'rb').read()
    if d[:4] == HUNK_MAGIC:
        return d, T.unwrap_hunk(d) or [], None, {}
    typ, tags = T.crud_info(d)
    return d, list(T.forms(d)), typ, tags


def tag_int(tags, k, default):
    v = tags.get(k, default)
    assert isinstance(v, int), f'tag {k:#x} is not a simple tag'
    return v


def timing(tags, frames, anim):
    """Per-speed timing, arrays ordered [slow, medium, fast, variable] (tags 0x57/58/59/56)."""
    order = (TAG_SLOW, TAG_MED, TAG_FAST, TAG_VAR)
    sp = SPEED_BY_TAG55[tag_int(tags, TAG_SPEED, 1 if anim else 0) & 3]
    if anim:
        fields = [tag_int(tags, k, d) for k, d in zip(order, (3, 2, 1, 0))]
        duration = [(f * frames + 1) // 2 for f in fields]
    else:
        fields = []
        duration = [tag_int(tags, k, d) for k, d in zip(order, (0x5a, 0x3c, 0x1e, 0x3c))]
    return dict(frames=frames, speed_default=sp, fields=fields, duration=duration)


def mix_record(a, mode):
    """lut / over_lut / flash_peak exactly as toaster_fx_convert.render_anim builds them."""
    kind, detail = C.plan_anim(a, mode)
    assert not kind.startswith('defer:'), kind
    bm = a.bm
    top = (1 << bm.planes) - 1
    video = ()
    if kind == 'soft':
        lut = (np.arange(top + 1) * 255 // top).astype(np.uint8)
        if detail == 'inverted':
            lut = lut[::-1].copy()
    elif kind == 'roles':
        A, B = detail
        video = (A, B)
        lut = np.zeros(top + 1, np.uint8); lut[B] = 255
    else:
        video = (detail,)
        lut = None
    used = np.unique(np.concatenate([f[::2, ::2].ravel() for f in a.frames]))
    over_lut = C.overlay_colours(bm, video, bm.cmap, used) if video else None
    has_overlay = over_lut is not None and bool(over_lut[used, 3].any())
    flash_peak = -1
    palette_changes = False
    if lut is None and has_overlay:
        # render_anim's flash rule: the overlay covers >= 97% of the screen at its peak
        first_lut = over_lut
        cover = []
        for f, rt, cm in zip(a.frames, a.reltimes, a.cmaps):
            if cm is not None and cm is not bm.cmap:
                over_lut = C.overlay_colours(bm, video, cm, used)
                palette_changes = True
            cover += [float(over_lut[f][::4, ::4, 3].mean())] * max(rt, 1)
        peak = int(np.argmax(cover))
        if cover[peak] >= 0.97 * 255:
            flash_peak = peak
        over_lut = first_lut
    if isinstance(detail, tuple):
        detail = list(detail)
    rec = dict(kind=kind, detail=detail, lut=[int(x) for x in lut] if lut is not None else [],
               over_rgba=[int(x) for x in over_lut.reshape(-1)] if has_overlay else [],
               flash_peak=flash_peak)
    return rec, dict(multi_rt=max(a.reltimes) > 1, palette_changes=palette_changes)


def main():
    os.makedirs(OUT, exist_ok=True)
    files = {n: [] for n in ('ilbm', 'anims', 'mixtables', 'dve', 'timing', 'chromafx')}
    rows = [r for r in csv.DictReader(open(os.path.join(ROOT, 'converted', 'manifest.csv'), encoding='utf-8'))
            if r['status'] == 'converted']
    man = {}
    for r in rows:
        man[r['kind']] = man.get(r['kind'], 0) + 1
    notes = {'multi_rt_flash': [], 'palette_change_flash': [], 'kind_mismatch': []}
    for r in rows:
        rel = r['source']
        d, fl, typ, tags = open_source(rel)
        anims = [f for f in fl if f[0] == b'ANIM']
        stills = [f for f in fl if f[0] == b'ILBM' and not C.is_icon(d, f[1])]
        mode = tags.get(TAG_MODE)
        if anims:
            ft, fo, fs = anims[0]
            a = T.read_anim(d, fo, fs)
            bm = a.bm
            is_dve = toaster_dve.is_dve(a.planes[0])
            if (r['kind'] == 'dve') != is_dve:
                notes['kind_mismatch'].append(rel)
            files['anims'].append(dict(
                source=rel, w=bm.w, h=bm.h, planes=bm.planes, dropped=a.dropped,
                frame_crcs=[crc(idx_bytes(f, bm.planes)) for f in a.frames],
                plane_crcs=[crc(np.ascontiguousarray(p).tobytes()) for p in a.planes]))
            if is_dve:
                frames = []
                for p in a.planes:
                    key, line, art = toaster_dve.decode(p, fill_overscan=False)
                    frames.append(dict(key_crc=crc(u8(key).tobytes()), line_crc=crc(u8(line).tobytes()),
                                       art_crc=crc(u8(art).tobytes()) if art is not None else 0))
                files['dve'].append(dict(source=rel, frames=frames))
            else:
                rec, flags = mix_record(a, mode)
                files['mixtables'].append(dict(source=rel, **rec))
                if rec['flash_peak'] >= 0 and flags['multi_rt']:
                    notes['multi_rt_flash'].append(rel)
                if flags['palette_changes']:
                    notes['palette_change_flash'].append(rel)
            files['timing'].append(dict(source=rel, **timing(tags, len(a.frames), True)))
        else:
            ft, fo, fs = stills[0]
            bm, idx = T.read_ilbm(d, fo, fs)
            files['ilbm'].append(dict(source=rel, w=bm.w, h=bm.h, planes=bm.planes,
                                      index_crc=crc(u8(idx).tobytes()),
                                      gray_lut=[int(x) for x in C.gray_lut(bm)]))
            files['timing'].append(dict(source=rel, **timing(tags, 1, False)))

    # ChromaFX: croutons, then the bank files (same discovery as chromafx_to_cube.export_all)
    def chroma(source, name, t, tags):
        files['chromafx'].append(dict(source=source, name=name,
                                      tables_crc=[crc(np.ascontiguousarray(t[k]).tobytes()) for k in range(3)],
                                      tags=tags))
    nc = nb = 0
    for c in X.CROUTONS:
        base = os.path.join(ROOT, c)
        for f in sorted(os.listdir(base)):
            if f.lower().endswith('.i'):
                continue
            p = os.path.join(base, f)
            res = X.from_crouton(p)
            if res is None:
                continue
            _, tg = T.crud_info(open(p, 'rb').read())
            tg = {f'0x{k:08x}': v for k, v in sorted(tg.items()) if isinstance(v, int)}
            chroma(f'{c}/{f}', res[0], res[1], tg)
            nc += 1
    for bank in X.BANKS:
        if os.path.exists(os.path.join(ROOT, bank)):
            for name, t in X.from_bank(os.path.join(ROOT, bank)):
                chroma(f'{bank} [{name}]', name, t, {})
                nb += 1

    for name, recs in files.items():
        with open(os.path.join(OUT, name + '.jsonl'), 'w', encoding='utf-8', newline='\n') as fo:
            for rec in recs:
                fo.write(json.dumps(rec, separators=(',', ':'), ensure_ascii=False) + '\n')
    n = {k: len(v) for k, v in files.items()}
    print('manifest converted rows by kind:', man)
    print('records:', n, f'(chromafx = {nc} crouton + {nb} bank)')
    for k, v in notes.items():
        print(f'{k}: {len(v)}', v[:5])


if __name__ == '__main__':
    main()
