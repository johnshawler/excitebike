#!/usr/bin/env python3
"""contact sheet PNG from a pix dump (records: 2-byte frame + 256x240 palette indexes (+emphasis in bits 6-7)).  usage: pix2png.py pix.bin out.png frame..."""
import sys, zlib, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import palette
want = [int(x) for x in sys.argv[3:]]
data = open(sys.argv[1], 'rb').read(); FS = 2 + 61440
frames = {}
for i in range(len(data) // FS):
    r = data[i * FS:(i + 1) * FS]; frames[r[0] | r[1] << 8] = r[2:]
pals = [palette.table(e) for e in range(8)]
cols = 4; rows = (len(want) + cols - 1) // cols; W, H = 256 * cols, 240 * rows
img = bytearray(W * H * 3)
for n, f in enumerate(want):
    px = frames.get(f)
    if px is None: continue
    ox, oy = (n % cols) * 256, (n // cols) * 240
    for y in range(240):
        for x in range(256):
            p = px[y * 256 + x]; c = pals[p >> 6][p & 63]; o = ((oy + y) * W + ox + x) * 3
            img[o:o + 3] = bytes(c)
raw = b''.join(b'\x00' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
def chunk(t, d): c = struct.pack('>I', len(d)) + t + d; return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
