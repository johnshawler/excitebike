#!/usr/bin/env python3
"""decodes the SCROLLTEST screenshots /tmp/o/sc_c<N>_full.png: label (hex scroll value) and where the two test bars appear; prints OK/BAD against 80-x0 and 336-x0"""
import re
from PIL import Image
src = open(__file__.rsplit('/', 1)[0] + '/../../platform/nes_main.c').read()
rows = re.search(r'hexfont\[16\]\[8\] = \{(.*?)\};', src, re.S).group(1)
font = [[int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', g)] + [0] for g in re.findall(r'\{([^{}]*)\}', rows)]
res = {}
for i in range(1, 41):
    try: im = Image.open('/tmp/o/sc_c%d_full.png' % i).convert('L'); imc = Image.open('/tmp/o/sc_c%d_full.png' % i).convert('RGB')
    except Exception: continue
    s = ''
    for d in range(3):
        bx = 138 + d * 16; best = None
        for n in range(16):
            sc = sum((im.getpixel((bx + c * 2 + 1, 52 + r * 2 + 1)) > 128) == bool((font[n][r] >> (7 - c)) & 1) for r in range(8) for c in range(8))
            if best is None or sc > best[0]: best = (sc, n)
        s += '%X' % best[1]
    x0 = int(s, 16); segs = []
    for x in range(138, 650):
        p = imc.getpixel((x, 300))
        if p != (0, 0, 0) and p != (17, 17, 17):
            if segs and x <= segs[-1][1] + 2: segs[-1][1] = x
            else: segs.append([x, x])
    res[x0] = [((a - 138) // 2, (b - 138) // 2 + 1) for a, b in segs]
bad = 0
for x0 in sorted(res):
    e0, e1 = 80 - x0, 336 - x0
    want = [(e, e + 8) for e in (e0, e1) if 0 <= e <= 248]
    ok = res[x0] == want; bad += not ok
    print(x0, 'expected', want, 'seen', res[x0], 'OK' if ok else 'BAD')
print('bad', bad)
