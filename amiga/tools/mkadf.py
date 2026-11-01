#!/usr/bin/env python3
"""Build a 880K Amiga DD ADF: bootblock (1024 bytes, OFS 'DOS\\0') + raw payload from sector 2 onward.
usage: mkadf.py out.adf bootblock.bin [payload.bin]"""
import sys, struct
def boot_checksum(bb):
    bb = bytearray(bb); bb[4:8] = b'\0\0\0\0'
    s = 0
    for i in range(0, 1024, 4):
        s += struct.unpack('>I', bb[i:i+4])[0]
        if s > 0xFFFFFFFF: s = (s & 0xFFFFFFFF) + 1
    bb[4:8] = struct.pack('>I', (~s) & 0xFFFFFFFF)
    return bytes(bb)
def main():
    out, boot = sys.argv[1], open(sys.argv[2], 'rb').read()
    assert len(boot) <= 1024, "bootblock too large: %d" % len(boot)
    bb = boot + bytes(1024 - len(boot))
    bb = b'DOS\0' + bb[4:]
    bb = boot_checksum(bb)
    adf = bytearray(880 * 1024)
    adf[0:1024] = bb
    if len(sys.argv) > 3:
        p = open(sys.argv[3], 'rb').read()
        assert 1024 + len(p) <= len(adf)
        adf[1024:1024+len(p)] = p
    open(out, 'wb').write(adf)
main()
