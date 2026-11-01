#!/usr/bin/env python3
"""Compare a trace/pixel dump pair (reference emulator or translated build vs MAME).
usage: cmp_mame.py mame.txt mine.txt maxframe [mame.pix mine.pix]
 - NT (nametables), PAL (palette RAM; $3F10/14/18/1C mirrors ignored), OAMRAM ($0200 page) must be identical
 - RAM must be identical except $00 and $18-$1F (random number generator, runs a CPU/PPU phase dependent number of times) and the stack page
 - the sequences of PPU/APU/pad writes (address, value; $2004 excluded: MAME logs the DMA as $2004 writes) must be identical
 - pixels: the mapping my-palette-index -> MAME colour must be a function (several indexes share one colour, e.g. $20/$30)"""
import sys, collections
mx = int(sys.argv[3])
def load(fn):
    d = {}; w = []
    for l in open(fn):
        k = l.split(' ', 2)
        if k[0] in ('NT', 'PAL', 'OAMRAM', 'RAM'):
            f = int(k[1])
            if f <= mx: d[(k[0], f)] = bytearray.fromhex(k[2].strip())
        elif k[0] == 'W':
            p = l.split()
            if p[3] != '2004' and int(p[1]) <= mx: w.append((p[3], p[4]))
    return d, w
a, wa = load(sys.argv[1]); b, wb = load(sys.argv[2])
bad = collections.Counter(); first = {}
for key in a:
    if key not in b: continue
    x, y = a[key], b[key]
    if key[0] == 'RAM':
        for i in [0] + list(range(0x18, 0x20)) + list(range(0x100, 0x200)): x[i] = y[i] = 0
    if key[0] == 'PAL':
        for i in (0x10, 0x14, 0x18, 0x1c): x[i] = y[i] = 0
    if x != y: bad[key[0]] += 1; first.setdefault(key[0], key[1])
print('dump mismatches:', dict(bad) or 'none', 'first frames', first)
n = min(len(wa), len(wb)); i = next((i for i in range(n) if wa[i] != wb[i]), None)
print('write sequence: %d / %d writes, first difference: %s' % (len(wa), len(wb), i))
if i is not None: print(' ', wa[i - 2:i + 2], wb[i - 2:i + 2])
if len(sys.argv) > 5:
    A = open(sys.argv[4], 'rb').read(); B = open(sys.argv[5], 'rb').read(); FS = 2 + 61440
    fa = {A[i * FS] | A[i * FS + 1] << 8: A[i * FS + 2:(i + 1) * FS] for i in range(len(A) // FS)}
    fb = {B[i * FS] | B[i * FS + 1] << 8: B[i * FS + 2:(i + 1) * FS] for i in range(len(B) // FS)}
    mp = collections.defaultdict(collections.Counter); cnt = 0
    for f in sorted(fa):
        if f in fb and f <= mx:
            cnt += 1
            for x, y in zip(fa[f], fb[f]): mp[x][y] += 1
    best = {k: v.most_common(1)[0][0] for k, v in mp.items()}
    badf = []
    for f in sorted(fa):
        if f in fb and f <= mx and any(best[x] != y for x, y in zip(fa[f], fb[f])): badf.append(f)
    print('pixels: %d frames compared, colour map entries %d, frames with differences: %d %s' % (cnt, len(mp), len(badf), badf[:12]))
