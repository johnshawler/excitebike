#!/usr/bin/env python3
"""mkpkg.py out.pkg stage2.bin exb.bin exb.elf
Package = stage 2 loader (padded to 512) + game image + relocation table (offsets of the absolute 32-bit words, relative to the image start).
The loader (stage2.s) finds fast RAM at run time and relocates the image there."""
import struct, subprocess, sys, re
out, s2f, imgf, elf = sys.argv[1:5]
s2 = bytearray(open(s2f, 'rb').read()); img = open(imgf, 'rb').read()
img += b'\0' * (-len(img) % 4)
nm = subprocess.check_output(['m68k-linux-gnu-nm', elf], text=True)
sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
base = sym['_payload_base']; stack_top = sym['_stack_top']
rel = []
for l in subprocess.check_output(['m68k-linux-gnu-readelf', '-rW', elf], text=True).splitlines():
    p = l.split()
    if len(p) >= 3 and re.fullmatch(r'[0-9a-f]{8}', p[0]) and p[2].startswith('R_68K_'):
        if p[2] == 'R_68K_PC32': continue
        assert p[2] == 'R_68K_32', 'unsupported relocation ' + p[2]
        off = int(p[0], 16) - base
        assert 0 <= off < len(img) - 3, hex(off)
        rel.append(off)
rel = sorted(set(rel))
s2 += b'\0' * (-len(s2) % 512)
game_off = len(s2); rel_off = game_off + len(img)
tab = b''.join(struct.pack('>I', o) for o in rel)
min_size = stack_top - base + 0x1000
struct.pack_into('>IIIIII', s2, 8, game_off, len(img), rel_off, len(rel), base, min_size)
open(out, 'wb').write(bytes(s2) + img + tab)
print('%s: stage2 %d, image %d, %d relocations, needs %d bytes of fast RAM' % (out, game_off, len(img), len(rel), min_size))
