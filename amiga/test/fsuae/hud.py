#!/usr/bin/env python3
"""reads the HUD counters (game frame, real ticks, frames not displayed) from FS-UAE full screenshots: hud.py img..."""
import sys, re
from PIL import Image
src = open(__file__.rsplit('/',1)[0] + '/../../platform/nes_main.c').read()
rows = re.search(r'hexfont\[16\]\[8\] = \{(.*?)\};', src, re.S).group(1)
font = [[int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', g)] + [0] for g in re.findall(r'\{([^{}]*)\}', rows)]
for f in sys.argv[1:]:
    im = Image.open(f).convert('L'); out = []
    for w in range(10):
        s = ''; row = 0 if w < 3 else 8 if w < 8 else 16; ww = w if w < 3 else w - 3 if w < 8 else w - 8
        for d in range(5):
            bx = 138 + (ww * 6 + d) * 16; best = None
            for n in range(16):
                score = 0
                for r in range(8):
                    for c in range(8):
                        px = im.getpixel((bx + c * 2 + 1, 52 + (row + r) * 2 + 1)) > 235
                        score += px == bool((font[n][r] >> (7 - c)) & 1)
                if best is None or score > best[0]: best = (score, n)
            s += '%X' % best[1]
        out.append(int(s, 16))
    print(f.split('/')[-1], 'game', out[0], 'tick', out[1], 'notshown', out[2], 'shown%', round(100.0 * (out[0] - out[2]) / max(out[0], 1), 1), '| per render (cc; 70824/frame): build', out[3], 'clear+cells', out[4], 'priv', out[5], 'sprites', out[6], 'blitter-wait', out[7], '| render_log/frame', out[8], 'plat_frame', out[9])
