#!/usr/bin/env python3
"""NES 2C07 (PAL) palette exactly as MAME computes it (src/devices/video/ppu2c0x.cpp nespal_to_RGB + apply_color_emphasis_and_clamp, is_pal = true).
64 colours x 8 emphasis combinations (emphasis bits as written to $2001 bits 5-7; PAL swaps bits 1 and 2 of the emphasis value)."""
import math
def nespal_to_rgb(intensity, num, emph, pal=True):
    tint, hue = 0.22, 287.0
    Kr, Kb, Ku, Kv = 0.2989, 0.1145, 2.029, 1.140
    bright = [[0.50, 0.75, 1.0, 1.0], [0.29, 0.45, 0.73, 0.9], [0, 0.24, 0.47, 0.77]]
    if num == 0: sat, rad, y = 0, 0, bright[0][intensity]
    elif num == 13: sat, rad, y = 0, 0, bright[2][intensity]
    elif num in (14, 15): sat, rad, y = 0, 0, 0
    else: sat, rad, y = tint, math.radians(num * 30 + hue), bright[1][intensity]
    u, v = sat * math.cos(rad), sat * math.sin(rad)
    R = (y + Kv * v) * 255.0
    G = (y - (Kb * Ku * u + Kr * Kv * v) / (1 - Kb - Kr)) * 255.0
    B = (y + Ku * u) * 255.0
    if pal:   # bitswap<3>(e, 2, 0, 1): new bit2 = old bit2, new bit1 = old bit0, new bit0 = old bit1
        emph = (emph & 4) | ((emph & 1) << 1) | ((emph >> 1) & 1)
    mod = [(1.0, 1.0, 1.0), (1.24, 0.915, 0.743), (0.794, 1.09, 0.882), (0.905, 1.03, 1.28), (0.741, 0.987, 1.0), (1.02, 0.908, 0.979), (1.02, 0.98, 0.653), (0.75, 0.75, 0.75)]
    clamp = lambda x: min(max(x, 0.0), 255.0)
    return tuple(int(math.floor(clamp(c * m) + 0.5)) for c, m in zip((R, G, B), mod[emph]))
def table(emph=0): return [nespal_to_rgb((i >> 4) & 3, i & 15, emph) for i in range(64)]
if __name__ == '__main__':
    t = table()
    print(['%02X%02X%02X' % c for c in t[:16]])
