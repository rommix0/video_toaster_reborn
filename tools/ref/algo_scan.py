"""Parse every ALGO crouton under 4.2/ (or argv[1]) and tally tags / programs.

usage: python -P -E vtoast_reborn/tools/ref/algo_scan.py [root] [-v]
"""
import collections, hashlib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))   # the Toaster root (this copy is in vtoast_reborn/tools/ref)


def chunks(d, o, end):
    while o + 8 <= end:
        cid, n = d[o:o + 4], struct.unpack('>I', d[o + 4:o + 8])[0]
        yield cid, o + 8, n
        o += 8 + n + (n & 1)


def crud(path):
    """-> (type, tags) where tags = [(id, int value | bytes)] in file order, or None."""
    d = open(path, 'rb').read()
    if d[:4] != b'FORM':
        return None
    for cid, o, n in chunks(d, 0, len(d)):
        if cid == b'FORM' and d[o:o + 4] == b'CrUD':
            typ, tags = None, []
            for c, p, m in chunks(d, o + 4, o + n):
                if c == b'TYPE':
                    typ = d[p:p + 4].decode('latin1')
                elif c == b'TAGS':
                    e = p + m
                    while p + 4 <= e:
                        t = struct.unpack('>I', d[p:p + 4])[0]; p += 4
                        if t == 0:
                            break
                        if t & 0x80000000:
                            tags.append((t, struct.unpack('>i', d[p:p + 4])[0])); p += 4
                        else:
                            k = struct.unpack('>I', d[p:p + 4])[0]; p += 4
                            tags.append((t, d[p:p + k])); p += k + (k & 1)
            return typ, tags
    return None


FIXED = {1: 0, 2: 2, 3: 16, 4: 20, 5: 40, 6: 20, 9: 4, 10: 4, 11: 4, 12: 4, 13: 4, 14: 4, 15: 4, 16: 20, 17: 4}
OPNAME = {1: 'skip', 2: 'const', 3: 'lin', 4: 'lin2', 5: 'sinmix', 6: 'linwin', 7: 'steptab', 8: 'lintab',
          9: 't', 10: 't2', 11: 't(2-t)', 12: 'sinramp', 13: 't3', 14: 't2(3-2t)/2', 15: 'rand', 16: 'sinlerp',
          17: 'constL', 18: 'piecewise', 19: 'sum', 20: 'keys'}


def op_len(d, o, n):
    """bytes of parameters after opcode n at offset o (mirrors the skipper at switcher 0x75f78)."""
    if n in FIXED:
        return FIXED[n]
    if n in (7, 8):
        return struct.unpack('>H', d[o:o + 2])[0]
    if n == 18:                       # (subop, params, word bp) ... until bp == 0
        p = o
        while True:
            k = struct.unpack('>H', d[p:p + 2])[0]; p += 2
            p += op_len(d, p, k)
            bp = struct.unpack('>H', d[p:p + 2])[0]; p += 2
            if not bp:
                return p - o
    if n == 19:                       # (subop, params) ... until subop == 0
        p = o
        while True:
            k = struct.unpack('>H', d[p:p + 2])[0]; p += 2
            if not k:
                return p - o
            p += op_len(d, p, k)
    p = o                             # 20+: records (long, long, word bp) until bp == 0
    while True:
        p += 8
        bp = struct.unpack('>H', d[p:p + 2])[0]; p += 2
        if not bp:
            return p - o


def subops(d, o, n, out):
    """collect nested sub-opcodes of 18/19"""
    if n == 18:
        p = o
        while True:
            k = struct.unpack('>H', d[p:p + 2])[0]; p += 2
            out.append(k); subops(d, p, k, out); p += op_len(d, p, k)
            bp = struct.unpack('>H', d[p:p + 2])[0]; p += 2
            if not bp:
                return
    if n == 19:
        p = o
        while True:
            k = struct.unpack('>H', d[p:p + 2])[0]; p += 2
            if not k:
                return
            out.append(k); subops(d, p, k, out); p += op_len(d, p, k)


def program(pl):
    """0x1C payload -> (header dict, [(channel, [(slot, op, params bytes)])], end offset).
    Header: long program offset (self-relative), long flags, word channel count (switcher 0x7516a)."""
    off, flags = struct.unpack('>II', pl[:8])
    hdr = {'off': off, 'flags': flags, 'nch': struct.unpack('>H', pl[8:10])[0]}
    o, chans = off, []
    while True:
        ch = struct.unpack('>H', pl[o:o + 2])[0]; o += 2
        if not ch:
            break
        slots, s = [], 0
        while True:
            n = struct.unpack('>H', pl[o:o + 2])[0]; o += 2
            if not n:
                break
            k = op_len(pl, o, n)
            slots.append((s, n, pl[o:o + k])); o += k; s += 1
        chans.append((ch, slots))
    return hdr, chans, o


def algos(root):
    for dp, dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            try:
                r = crud(p)
            except Exception:
                continue
            if r and r[0] == 'ALGO':
                yield p, dict(r[1])


if __name__ == '__main__':
    root = next((a for a in sys.argv[1:] if not a.startswith('-')), os.path.join(ROOT, '4.2'))
    files = list(algos(root))
    print(len(files), 'ALGO croutons under', root)
    tagcount = collections.Counter()
    for p, t in files:
        for k in t:
            tagcount[k] += 1
    print('tags:', {hex(k): v for k, v in sorted(tagcount.items())})
    for k in (0x80000063, 0x80000007):
        print(hex(k), sorted(collections.Counter(t.get(k) for p, t in files).items(), key=str))
    progs = collections.defaultdict(list)
    for p, t in files:
        progs[(t.get(0x80000063), t.get(0x80000007), t.get(0x1000001c))].append(os.path.relpath(p, root))
    print(len(progs), 'distinct (0x63, 0x07, 0x1C payload) programs')
    pls = {t.get(0x1000001c) for p, t in files if t.get(0x1000001c)}
    print(len(pls), 'distinct 0x1C payloads')
    ops, chs, flg, hd, sub = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    bad = 0
    for pl in pls:
        try:
            h, cl, end = program(pl)
        except Exception:
            bad += 1; continue
        flg[hex(h['flags'])] += 1; hd[(h['off'], h['nch'])] += 1
        for ch, sl in cl:
            chs[ch] += 1
            for s_, n, prm in sl:
                ops[n] += 1
                if n in (18, 19):
                    out = []; subops(prm, 0, n, out); sub.update(out)
    print('unparsed payloads', bad)
    print('header (prog offset, channel count):', dict(hd))
    print('flags:', dict(flg))
    print('channels used:', dict(sorted(chs.items())))
    print('opcodes:', {f'{k} {OPNAME.get(k, "keys")}': v for k, v in sorted(ops.items())})
    print('sub-opcodes in 18/19:', dict(sorted(sub.items())))
    if '-v' in sys.argv:
        for (a, b, pl), names in sorted(progs.items(), key=lambda x: x[1][0]):
            print(a, b, len(pl or b''), hashlib.md5(pl or b'').hexdigest()[:8], len(names), names[0])
