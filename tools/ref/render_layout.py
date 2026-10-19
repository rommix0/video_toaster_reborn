"""Video Toaster 4.2 Switcher panel: extract the layout from switcher.main and render it.

    python -P -E tools/re/render_layout.py --extract        # switcher.main -> tools/re/switcher_layout.json
    python -P -E tools/re/render_layout.py [--out X.png]    # json (+ switcher.main pixels) -> PNG

The panel art is raw planar image data inside switcher.main (DATA hunks 11/15-17/19-24), referenced
by "panel nodes" (see SCREEN.md).  The JSON stores node addresses as flat-image addresses produced
by hunk.load() (hunk i at its 4 KB-aligned base), so the renderer re-reads the exact pixels from the
executable and reproduces the screen bit-exactly (2 bitplanes, palette below).
Text drawn at run time (BKG pop-up, speed pop-up, key level) uses the real CGInterface/14 colour font.
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hunk

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXE = os.path.join(ROOT, '4.2', 'Programs', 'switcher.main')
FONT = os.path.join(ROOT, '4.2', 'Programs', 'System_Support', 'Fonts', 'CGInterface', '14.4C')
JSON = os.path.join(HERE, 'switcher_layout.json')
MANUAL = os.path.join(os.environ.get('TEMP', ''), 'claude', 'C--Users-abart-Desktop-Video-Toaster-v4-3',
                      '941404fe-802a-4743-8da9-3e71d1eb0274', 'scratchpad', 'pages', 'p164.png')
A5 = 0x5d5c

# words per 16-pixel column for the six draw methods (node +0x2c / 4), see SCREEN.md
WPC = {0: 4, 1: 2, 2: 2, 3: 1, 4: 4, 5: 3}

# list heads: a5 offset -> what the list holds
LISTS = [(0x158, 'mode'), (0x15c, 'superimpose'), (0x160, 'main_bus'), (0x164, 'preview_bus'),
         (0x16c, 'take_auto_framestore'), (0x170, 'tbar'), (0x174, 'speed'), (0x178, 'graphics'),
         (0x17c, 'key')]
EXTRA = [(0x4a15c, 'speed'), (0x4e328, 'marker')]   # referenced from code, not on a list

# node -> (id, kind, text); identified by eye from the decoded images (render with --extract first)
NAMES = {
    0x28000: ('setup', 'button', 'SETUP'),
    0x28074: ('mode_dyn_160_244', 'dynamic', None),
    0x280ec: ('gfx_paint', 'button', 'PAINT'),
    0x28164: ('gfx_cg', 'button', 'CG'),
    0x281dc: ('gfx_3d', 'button', '3D'),
    0x28254: ('mode_dyn_704_291', 'dynamic', None),
    0x35000: ('sup_1', 'button', '1'), 0x35074: ('sup_2', 'button', '2'), 0x350e8: ('sup_3', 'button', '3'),
    0x3515c: ('sup_4', 'button', '4'), 0x351d0: ('sup_dv1', 'button', 'DV1'), 0x35248: ('sup_dv2', 'button', 'DV2'),
    0x352c0: ('sup_bkg', 'button', 'BKG'), 0x35338: ('hdr_superimpose', 'label', 'SUPERIMPOSE'),
    0x37000: ('main_1', 'button', '1'), 0x37074: ('main_2', 'button', '2'), 0x370e8: ('main_3', 'button', '3'),
    0x3715c: ('main_4', 'button', '4'), 0x371d0: ('main_dv1', 'button', 'DV1'), 0x37244: ('main_dv2', 'button', 'DV2'),
    0x372b8: ('main_bkg', 'button', 'BKG'), 0x3732c: ('lbl_main', 'label', 'MAIN'),
    0x3a000: ('prev_1', 'button', '1'), 0x3a074: ('prev_2', 'button', '2'), 0x3a0e8: ('prev_3', 'button', '3'),
    0x3a15c: ('prev_4', 'button', '4'), 0x3a1d0: ('prev_dv1', 'button', 'DV1'), 0x3a244: ('prev_dv2', 'button', 'DV2'),
    0x3a2b8: ('prev_bkg', 'button', 'BKG'), 0x3a32c: ('lbl_preview', 'label', 'PREVIEW'),
    0x3a3a0: ('logo', 'logo', 'VIDEO TOASTER 4.2 / (c)TM 1997 NewTek Inc.'),
    0x3a414: ('hdr_video_controls', 'label', 'VIDEO CONTROLS'),
    0x48000: ('auto', 'button', 'AUTO'), 0x48076: ('take', 'button', 'TAKE'),
    0x480ec: ('noimg_656_244', 'hotspot', None),
    0x4815e: ('bkg_popup', 'popup', None),
    0x43000: ('hdr_framestore', 'label', 'FRAMESTORE'), 0x43074: ('framestore_frame', 'frame', None),
    0x430e8: ('fs_freeze', 'button', 'FREEZE'), 0x43162: ('fs_live', 'button', 'LIVE'),
    0x431dc: ('fs_1', 'button', '1'), 0x43256: ('fs_2', 'button', '2'), 0x432d0: ('fs_3', 'button', '3'),
    0x4334a: ('fs_4', 'button', '4'), 0x433c4: ('fs_stabilize', 'button', 'STABILIZE'), 0x43438: ('fs_save', 'button', 'SAVE'),
    0x4e000: ('tbar', 'tbar', None), 0x4e1c8: ('tbar_dir', 'indicator', None),
    0x4e240: ('tbar_cap_top', 'frame', None), 0x4e2b4: ('tbar_cap_bottom', 'frame', None),
    0x4a000: ('speed_s', 'button', 'S'), 0x4a074: ('speed_m', 'button', 'M'), 0x4a0e8: ('speed_f', 'button', 'F'),
    0x4a15c: ('speed_popup', 'popup', None),
    0x3f000: ('gfx_3f000', 'unknown', None), 0x3f074: ('gfx_dyn_544', 'dynamic', None),
    0x3f0f4: ('gfx_dyn_560', 'dynamic', None), 0x3f170: ('gfx_dyn_624', 'dynamic', None),
    0x3f1ec: ('gfx_dyn_640', 'dynamic', None), 0x3f268: ('gfx_file_popup', 'popup', None),
    0x3f31a: ('hdr_graphics', 'label', 'GRAPHICS'), 0x3f38e: ('gfx_3f38e', 'unknown', None),
    0x3f402: ('gfx_load', 'button', 'LOAD'),
    0x4c000: ('key_level_popup', 'levelbox', None), 0x4c074: ('key_level_noimg', 'hotspot', None),
    0x4c0e4: ('key_dyn_416_270', 'dynamic', None), 0x4c158: ('key_off', 'button', 'OFF'),
    0x4c1d0: ('key_black', 'button', None), 0x4c248: ('key_white', 'button', None),
    0x4e328: ('popup_marker', 'marker', None),
}


class Img:
    def __init__(self, path=EXE):
        self.h = hunk.load(path)
        self.m = self.h.mem

    def u8(self, a): return self.m[a]
    def u16(self, a): return struct.unpack('>H', self.m[a:a + 2])[0]
    def s16(self, a): return struct.unpack('>h', self.m[a:a + 2])[0]
    def u32(self, a): return struct.unpack('>I', self.m[a:a + 4])[0]

    def cstr(self, a):
        return bytes(self.m[a:self.m.index(0, a)]).decode('latin1')


def decode(im, method, words, rows, image, selected=False, pattern=0xffff):
    """-> rows x (16*words) palette indices, exactly as the 68000 draw methods at 0x17574/0x17700 do."""
    out = []
    p = image
    alt = (pattern >> 2 | pattern << 14) & 0xffff
    for r in range(rows):
        line = []
        d7 = pattern if r % 2 == 0 else alt        # dither pattern alternates per line (approximation of
        for w in range(words):                     # the bitmap-row parity rule; exact for pattern 0xffff)
            ws = [im.u16(p + 2 * i) for i in range(WPC[method])]
            p += 2 * WPC[method]
            mask = 0
            if method == 0: p0, p1 = ws[0], ws[1]
            elif method == 1: p0 = p1 = ws[0]
            elif method == 2: p0, p1 = ws[0], ws[1]
            elif method == 3: p0 = p1 = ws[0]
            else: mask, p0, p1 = ws[0], ws[1], ws[2]
            if selected:
                if method in (4, 5):               # colour rotate inside the mask: 1->2, 2->3, 3->1
                    p0, p1 = (p0 & ~mask) | (p1 & mask), (p1 & ~mask) | ((p0 ^ p1) & mask)
                else:                              # XOR with the node's pattern word
                    p0 ^= d7; p1 ^= d7
            for b in range(15, -1, -1):
                line.append(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1))
        out.append(line)
    return out


# ---------------------------------------------------------------- extraction
def node_record(im, n, lst):
    nid, kind, text = NAMES.get(n, (f'node_{n:06x}', 'unknown', None))
    method = im.u16(n + 0x2c) // 4
    words = im.u16(n + 0x2e)
    rows = im.u16(n + 0x30) + 1
    off = im.u32(n + 0x34)
    dyn = words == 0xffff
    words = 0 if dyn else words + 1
    nst = im.u16(n + 0x3c)
    tab = im.u32(n + 0x40)
    states = [im.u32(tab + 4 * i) for i in range(nst)] if tab and nst < 200 else []
    cur = im.u32(n + 0x38)
    r = {
        'id': nid, 'kind': kind, 'list': lst, 'node': f'0x{n:06x}',
        'rect': [off % 96 * 8, off // 96, words * 16, rows],
        'hit_rect': [im.s16(n + 4), im.s16(n + 6), im.s16(n + 8), im.s16(n + 10)],
        'method': method, 'image': f'0x{cur:06x}' if cur else None,
        'states': [f'0x{s:06x}' for s in states],
        'value': im.u16(n + 0x4c),
        'select_pattern': im.u8(n + 0x4b),
        'hidden_default': im.u8(n + 0x49) != 0,
        'handler': f'0x{im.u32(n + 0x44):06x}' if im.u32(n + 0x44) else None,
        'source': f'switcher.main flat 0x{n:06x} (hunk {hunk_index(im, n)})',
    }
    if text:
        r['text'] = text
        r['font'] = 'baked into the image (bitmap art)'
    if dyn:
        r['inferred'] = True
        r['note'] = 'words-1 = 0xffff: width is set at run time; not drawn in the default state'
    if kind in ('button', 'popup', 'levelbox') and method in (4, 5):
        r['colours'] = {'normal': 'image pixels (palette indices 0-3)',
                        'selected': 'inside image mask: 0->0, 1->2, 2->3, 3->1 (pressed/lit look)'}
    if nid == 'tbar':
        r['tbar'] = {'position_var': '$1ec(a5) 0..511', 'state': 'position // 6 (0..85)',
                     'image_for_state_i': f'0x{states[0]:06x} - 0x30*i (window slides 1 row per state over one tall strip)',
                     'handle_top_at_state0': True}
    return r


def hunk_index(im, a):
    for i, (t, b, s) in enumerate(im.h.hunks):
        if b <= a < b + s:
            return i


def extract():
    im = Img()
    els, seen = [], set()
    for off, lst in LISTS:
        n = im.u32(A5 + off)
        while n and n not in seen:
            seen.add(n)
            els.append(node_record(im, n, f'${off:x}(a5) {lst}'))
            n = im.u32(n)
    for n, lst in EXTRA:
        if n not in seen:
            r = node_record(im, n, f'(not on a list) {lst}')
            if n == 0x4a15c:
                r['hidden_default'] = True
                r['note'] = 'drawn instead of S/M/F when a variable speed is set (code 0x1180c, 0x6228e)'
            if n == 0x4e328:
                r['hidden_default'] = True
                r['note'] = 'overlay drawn by 0x15000 at the owning node offset + 3 rows when node +0x5e bit0 is set'
            els.append(r)
    pal_rgb = ['000', '555', '999', 'DDD']
    lay = {
        'source': '4.2/Programs/switcher.main ("Video Toaster 4.06"), addresses = hunk.py flat image',
        'screen': {
            'width': 768, 'height': 481, 'depth': 2, 'viewmodes': '0x8004 (HIRES|LACE)',
            'newscreen': '0x0069d2 ($c76(a5))', 'type': '0x01cf CUSTOMSCREEN|CUSTOMBITMAP|SCREENBEHIND|SCREENQUIET',
            'font': 'CGInterface.font 14 (TextAttr 0x006a22, style 0x40 colour, flags 0x42)',
            'bitmap': 'custom, 96 bytes/row, planes 0xb460 bytes apart from $84(a5); 6 plane slots, depth 2',
            'window': {'rect': [0, 0, 768, 481], 'newwindow': '0x0069f2', 'detail_pen': 2, 'block_pen': 1},
            'gadgets': [{'id': 'SW', 'rect': [64, 244, 640, 240], 'source': '0x013220'},
                        {'id': 'project', 'rect': [64, 44, 640, 200], 'source': '0x01324c'}],
            'clear': {'colour': 1, 'x_range': [8, 760], 'rows': 'all 481 (or 270..480 when $352 bit5)',
                      'source': 'blitter fill 0x240b6, colour $7c(a5)=1, pattern rows 0x26000/0x26060'},
        },
        'palette': {
            'rgb_mode': pal_rgb,
            'rgb_mode_source': '0x02bf3a (used when $1d4(a5)==0 = Control-Panel RGB)',
            'preview_mode': ['000', '842', '7BC', 'FFF'],
            'preview_mode_source': '0x02bf30 (used when $1d4(a5)!=0 = PREVIEW; initial $1d4=0xffff); '
                                   'Toaster-encoded words, visible colour depends on Toaster hardware (open)',
            'fifth_word': '555 (both tables; use unknown)',
            'sprite_colours_17_31': ['876', '786', '886', '778', '878', '788', '888', '777', '877', '787',
                                     '887', '779', '879', '789', '889'],
            'with_picture': {'rgb_8': ['006', '666', '996', 'DD6', '009', '669', '999', 'DD9'],
                             'source': '0x12447c (8) / 0x12448e (16) / preview 0x124448, 0x12445a; used when a '
                                       'picture bitmap adds planes ($3e6(a5)); low nibble looks like a Toaster '
                                       'control code (6/7/8/9), not a colour (inferred)'},
            'note': '8-colour crouton/InterfaceLG palette 000 666 999 DDD 333 555 CB7 FE7 is not loaded by '
                    'the switcher UI code path decoded here (see SCREEN.md)',
        },
        'draw_methods': {
            '0': '4 words/col: p0 p1 + 2 alt words (plane0, plane1)',
            '1': '2 words/col: w0 -> both planes, w1 alt',
            '2': '2 words/col: p0 p1', '3': '1 word/col -> both planes',
            '4': '4 words/col: mask p0 p1 spare (all switcher art)', '5': '3 words/col: mask p0 p1',
            'selected': 'methods 4/5: colour rotate in mask; 0-3: XOR pattern (+0x4e word, rotated by 2 on odd rows)',
        },
        'texts': [
            {'id': 'bkg_popup_text', 'owner': 'bkg_popup', 'kind': 'label', 'pen_xy_baseline': [326, 429],
             'rule': 'Move(node.x+6, node.y+15); Text()', 'font': 'window RPort font = CGInterface 14',
             'draw_mode': 'JAM2 BgPen 0 (inferred: colour-0 strokes must be opaque)',
             'strings': ['Black  ', 'White  ', 'Red    ', 'Green  ', 'Blue   ', 'Yellow ', 'Magenta',
                         'Cyan   ', 'Snow   '], 'default': 'Black  ', 'source': '0x114432, table 0x11448a'},
            {'id': 'speed_popup_text', 'owner': 'speed_popup', 'kind': 'label', 'pen_xy_baseline': [232, 429],
             'rule': 'Move(node.x+24, node.y+15); text = speed letter S/M/F + 3-digit (frames+1)/2',
             'font': 'CGInterface 14', 'source': '0x0622da', 'hidden_default': True},
            {'id': 'key_level_text', 'owner': 'key_level_popup', 'kind': 'levelbox', 'itext_top_left': [656, 413],
             'rule': 'PrintIText(rp, IntuiText@0x0641b0, 0, 0): FrontPen 0 BackPen 0 JAM2',
             'font': 'CGInterface 14 (TextAttr 0x060c8c)', 'default': '128', 'range': '0..257 (0x101)',
             'source': '0x064188'},
        ],
        'project_grid': {'origin': [64, 44], 'cell': [80, 50], 'columns': 8, 'rows_visible': 4,
                         'slot_rule': 'x = 64 + 80*(slot%32%8), y = 44 + 50*(slot%32//8)  (0x0161d8)',
                         'icon': 'crouton ILBM 80x50x3 fills the whole cell (no gap)'},
        'elements': els,
    }
    with open(JSON, 'w') as f:
        json.dump(lay, f, indent=1)
    print(f'wrote {JSON}: {len(els)} elements')


# ---------------------------------------------------------------- colour font
def load_font(path=FONT):
    h = hunk.load(path)
    m = h.mem
    b = h.hunks[0][1]
    u16 = lambda a: struct.unpack('>H', m[a:a + 2])[0]
    u32 = lambda a: struct.unpack('>I', m[a:a + 4])[0]
    s16 = lambda a: struct.unpack('>h', m[a:a + 2])[0]
    tf = b + 4 + 54                                   # moveq/rts + DiskFontHeader
    f = dict(ysize=u16(tf + 20), style=m[tf + 22], xsize=u16(tf + 24), baseline=u16(tf + 26),
             lo=m[tf + 32], hi=m[tf + 33], modulo=u16(tf + 38), charloc=u32(tf + 40),
             charspace=u32(tf + 44), charkern=u32(tf + 48), m=m)
    f['depth'] = m[tf + 54] if f['style'] & 0x40 else 1
    f['planes'] = [u32(tf + 64 + 4 * i) for i in range(f['depth'])] if f['style'] & 0x40 else [u32(tf + 34)]
    n = f['hi'] - f['lo'] + 2
    f['loc'] = [(u16(f['charloc'] + 4 * i), u16(f['charloc'] + 4 * i + 2)) for i in range(n)]
    f['space'] = [s16(f['charspace'] + 2 * i) for i in range(n)] if f['charspace'] else None
    f['kern'] = [s16(f['charkern'] + 2 * i) for i in range(n)] if f['charkern'] else None
    return f


def draw_text(px, f, x, baseline, s, bgpen=0):
    """Text() of a ColorTextFont in JAM2: colour-0 glyph pixels become bgpen."""
    top = baseline - f['baseline']
    for ch in s:
        i = ord(ch) - f['lo']
        if not 0 <= i <= f['hi'] - f['lo']:
            i = f['hi'] - f['lo'] + 1
        off, w = f['loc'][i]
        kern = f['kern'][i] if f['kern'] else 0
        adv = f['space'][i] if f['space'] else f['xsize']
        for yy in range(f['ysize']):
            for xx in range(max(w, adv - kern)):
                c = bgpen
                if xx < w:
                    c = 0
                    for p, pb in enumerate(f['planes']):
                        bx = off + xx
                        if f['m'][pb + yy * f['modulo'] + (bx >> 3)] >> (7 - (bx & 7)) & 1:
                            c |= 1 << p
                    c = c or bgpen
                X, Y = x + kern + xx, top + yy
                if 0 <= Y < len(px) and 0 <= X < len(px[0]):
                    px[Y][X] = c
        x += adv


# ---------------------------------------------------------------- render
def render(out, manual=MANUAL):
    from PIL import Image
    lay = json.load(open(JSON))
    im = Img()
    W, H = lay['screen']['width'], lay['screen']['height']
    px = [[0] * W for _ in range(H)]
    c = lay['screen']['clear']
    for y in range(H):
        for x in range(c['x_range'][0], c['x_range'][1]):
            px[y][x] = c['colour']
    for e in lay['elements']:
        if e['hidden_default'] or not e['image'] or e['rect'][2] == 0:
            continue
        x, y, w, h = e['rect']
        img = decode(im, e['method'], w // 16, h, int(e['image'], 16))
        for r, line in enumerate(img):
            for cc, v in enumerate(line):
                if 0 <= x + cc < W and 0 <= y + r < H:
                    px[y + r][x + cc] = v
    f = load_font()
    for t in lay['texts']:
        if t.get('hidden_default'):
            continue
        if 'pen_xy_baseline' in t:
            draw_text(px, f, t['pen_xy_baseline'][0], t['pen_xy_baseline'][1], t['default'])
        else:
            draw_text(px, f, t['itext_top_left'][0], t['itext_top_left'][1] + f['baseline'], t['default'])
    pal = [tuple(int(ch, 16) * 17 for ch in col) for col in lay['palette']['rgb_mode']]
    scr = Image.new('RGB', (W, H))
    scr.putdata([pal[v] for row in px for v in row])
    full = scr.resize((W * 2, H * 2), Image.NEAREST)
    full.save(os.path.splitext(out)[0] + '_full.png')
    crop = scr.crop((56, 262, 728, 474)).resize((672 * 2, 212 * 2), Image.NEAREST)
    if manual and os.path.exists(manual):
        man = Image.open(manual).convert('RGB').crop((66, 452, 640, 668))
        man = man.resize((round(man.width * crop.height / man.height), crop.height), Image.LANCZOS)
        sheet = Image.new('RGB', (crop.width + man.width + 24, crop.height), (255, 255, 255))
        sheet.paste(crop, (0, 0))
        sheet.paste(man, (crop.width + 24, 0))
    else:
        sheet = crop
    sheet.save(out)
    print('wrote', out, 'and', os.path.splitext(out)[0] + '_full.png')


if __name__ == '__main__':
    a = sys.argv[1:]
    if '--extract' in a:
        extract()
    else:
        out = a[a.index('--out') + 1] if '--out' in a else os.path.join(
            os.path.dirname(MANUAL), '..', 'switcher_render.png')
        render(os.path.abspath(out), a[a.index('--manual') + 1] if '--manual' in a else MANUAL)
