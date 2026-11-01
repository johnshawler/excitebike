#!/usr/bin/env python3
"""FM2 movie -> pad file (one byte per frame, bit0=A 1=B 2=Select 3=Start 4=Up 5=Down 6=Left 7=Right).  usage: fm2pad.py in.fm2 out.pad"""
import sys
out = bytearray()
for l in open(sys.argv[1], encoding='latin1'):
    if not l.startswith('|'): continue
    f = l.split('|')[2]                     # RLDUTSBA
    if len(f) != 8: continue
    b = 0
    for ch, bit in zip(f, (7, 6, 5, 4, 3, 2, 1, 0)):
        if ch != '.': b |= 1 << bit
    out.append(b)
open(sys.argv[2], 'wb').write(out)
print(len(out), 'frames')
