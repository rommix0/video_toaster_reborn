"""Amiga hunk executable loader: lays hunks out in one flat image and applies relocations.

load(path) -> Image with .mem (bytearray), .hunks [(type, base, size)], .symbols {addr: name}
Hunk i is placed at an aligned base; RELOC32 entries are applied so absolute
pointers in the image point at the right flat addresses.
"""
import struct, sys

HUNK_NAMES = {0x3E7: 'UNIT', 0x3E8: 'NAME', 0x3E9: 'CODE', 0x3EA: 'DATA', 0x3EB: 'BSS', 0x3EC: 'RELOC32',
              0x3ED: 'RELOC16', 0x3EE: 'RELOC8', 0x3EF: 'EXT', 0x3F0: 'SYMBOL', 0x3F1: 'DEBUG', 0x3F2: 'END',
              0x3F3: 'HEADER', 0x3F5: 'OVERLAY', 0x3F6: 'BREAK', 0x3F7: 'DREL32', 0x3F8: 'DREL16',
              0x3F9: 'DREL8', 0x3FA: 'LIB', 0x3FB: 'INDEX', 0x3FC: 'RELOC32SHORT', 0x3FD: 'RELRELOC32'}


class Image:
    pass


def load(path, base=0x1000, align=0x1000):
    d = open(path, 'rb').read()
    u32 = lambda o: struct.unpack('>I', d[o:o + 4])[0]
    o = 0
    assert u32(0) == 0x3F3, 'not a hunk executable'
    o = 4
    while u32(o):                       # resident library names (none in practice)
        o += 4 + u32(o) * 4
    o += 4
    count, first, last = u32(o), u32(o + 4), u32(o + 8); o += 12
    sizes = []
    for i in range(last - first + 1):
        s = u32(o); o += 4
        if (s >> 30) == 3:
            o += 4
        sizes.append((s & 0x3FFFFFFF) * 4)
    bases = []
    a = base
    for s in sizes:
        bases.append(a)
        a = (a + s + align - 1) // align * align + align
    mem = bytearray(a)
    img = Image(); img.mem = mem; img.hunks = []; img.symbols = {}; img.relocs = []
    hi = -1
    types = {}
    while o < len(d):
        t = u32(o) & 0x3FFFFFFF; o += 4
        if t in (0x3E9, 0x3EA):
            n = u32(o) * 4; o += 4
            hi += 1
            mem[bases[hi]:bases[hi] + n] = d[o:o + n]; o += n
            types[hi] = HUNK_NAMES[t]
        elif t == 0x3EB:
            o += 4; hi += 1; types[hi] = 'BSS'
        elif t in (0x3EC, 0x3F7):
            while True:
                n = u32(o); o += 4
                if not n:
                    break
                target = u32(o); o += 4
                for _ in range(n):
                    off = u32(o); o += 4
                    p = bases[hi] + off
                    v = struct.unpack('>I', mem[p:p + 4])[0] + bases[target]
                    mem[p:p + 4] = struct.pack('>I', v & 0xFFFFFFFF)
                    img.relocs.append(p)
        elif t in (0x3FC, 0x3FD):
            while True:
                n = struct.unpack('>H', d[o:o + 2])[0]; o += 2
                if not n:
                    break
                target = struct.unpack('>H', d[o:o + 2])[0]; o += 2
                for _ in range(n):
                    off = struct.unpack('>H', d[o:o + 2])[0]; o += 2
                    p = bases[hi] + off
                    v = struct.unpack('>I', mem[p:p + 4])[0] + bases[target]
                    mem[p:p + 4] = struct.pack('>I', v & 0xFFFFFFFF)
                    img.relocs.append(p)
            if o & 2:
                o += 2
        elif t == 0x3F0:
            while True:
                n = u32(o); o += 4
                if not n:
                    break
                name = d[o:o + n * 4].rstrip(b'\0').decode('latin1'); o += n * 4
                img.symbols[bases[hi] + u32(o)] = name; o += 4
        elif t == 0x3F1:
            n = u32(o); o += 4 + n * 4
        elif t == 0x3F2:
            pass
        elif t == 0x3F6:
            break
        else:
            raise ValueError(f'unhandled hunk type {t:#x} at {o - 4:#x}')
    img.hunks = [(types.get(i, '?'), bases[i], sizes[i]) for i in range(len(sizes))]
    return img


if __name__ == '__main__':
    img = load(sys.argv[1])
    for t, b, s in img.hunks:
        print(f'{t:5} {b:#08x} size {s:#x}')
    print(len(img.relocs), 'relocs,', len(img.symbols), 'symbols')
