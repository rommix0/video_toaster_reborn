"""Amiga colour fonts (diskfont size files "NN.xC": hunk executable, DiskFontHeader 0x0F80 + ColorTextFont).

load_font(path) -> dict with ysize, style, flags, xsize, baseline, lo, hi, modulo, depth, planes (flat addresses of the
bitplanes), colours (12-bit RGB words of ColorFontColors), loc [(bit offset, width)], space (advance per char,
tf_XSize when the font has no CharSpace table), kern (0 when none) -- all per char lo..hi plus the last
"not found" glyph -- and m (the flat image).
strike(f) -> bytes, modulo*8 x ysize colour indices.  draw_text() is Text() in JAM2 or JAM1.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hunk


def load_font(path):
    h = hunk.load(path)
    m = h.mem
    b = h.hunks[0][1]
    u16 = lambda a: struct.unpack('>H', m[a:a + 2])[0]
    u32 = lambda a: struct.unpack('>I', m[a:a + 4])[0]
    s16 = lambda a: struct.unpack('>h', m[a:a + 2])[0]
    assert u16(b + 4 + 14) == 0x0F80, 'not a DiskFontHeader'
    tf = b + 4 + 54                                   # moveq/rts + DiskFontHeader
    f = dict(ysize=u16(tf + 20), style=m[tf + 22], flags=m[tf + 23], xsize=u16(tf + 24), baseline=u16(tf + 26),
             lo=m[tf + 32], hi=m[tf + 33], modulo=u16(tf + 38), charloc=u32(tf + 40),
             charspace=u32(tf + 44), charkern=u32(tf + 48), m=m)
    assert f['style'] & 0x40 and u16(tf + 52) & 1, 'not a colour font'
    f['depth'] = m[tf + 54]
    f['planes'] = [u32(tf + 64 + 4 * i) for i in range(f['depth'])]
    cc = u32(tf + 60)                                 # ColorFontColors: reserved, count, table
    f['colours'] = [u16(u32(cc + 4) + 2 * i) for i in range(u16(cc + 2))]
    n = f['hi'] - f['lo'] + 2
    f['loc'] = [(u16(f['charloc'] + 4 * i), u16(f['charloc'] + 4 * i + 2)) for i in range(n)]
    f['space'] = [s16(f['charspace'] + 2 * i) for i in range(n)] if f['charspace'] else [f['xsize']] * n
    f['kern'] = [s16(f['charkern'] + 2 * i) for i in range(n)] if f['charkern'] else [0] * n
    return f


def pixel(f, x, y):
    """Colour index of strike pixel (x, y): plane p contributes bit p."""
    c = 0
    for p, pb in enumerate(f['planes']):
        if f['m'][pb + y * f['modulo'] + (x >> 3)] >> (7 - (x & 7)) & 1:
            c |= 1 << p
    return c


def strike(f):
    return bytes(pixel(f, x, y) for y in range(f['ysize']) for x in range(f['modulo'] * 8))


def glyph(f, ch):
    """Index of a character: outside lo..hi it is the last ("not found") glyph."""
    i = ch - f['lo']
    return i if 0 <= i <= f['hi'] - f['lo'] else f['hi'] - f['lo'] + 1


def text_width(f, s):
    return sum(f['space'][glyph(f, ord(ch))] for ch in s)


def draw_text(px, f, x, baseline, s, bgpen=0, jam2=True):
    """Text() of a ColorTextFont: JAM2 turns colour-0 glyph pixels (and the rest of the advance cell)
    into bgpen; JAM1 leaves them alone."""
    top = baseline - f['baseline']
    for ch in s:
        i = glyph(f, ord(ch))
        off, w = f['loc'][i]
        kern, adv = f['kern'][i], f['space'][i]
        for yy in range(f['ysize']):
            for xx in range(max(w, adv - kern)):
                c = pixel(f, off + xx, yy) if xx < w else 0
                if not c:
                    if not jam2:
                        continue
                    c = bgpen
                X, Y = x + kern + xx, top + yy
                if 0 <= Y < len(px) and 0 <= X < len(px[0]):
                    px[Y][X] = c
        x += adv
