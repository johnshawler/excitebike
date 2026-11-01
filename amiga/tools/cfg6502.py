#!/usr/bin/env python3
"""Control-flow discovery for the Excitebike PRG (NROM-128, CPU $C000-$FFFF).
Recursive descent from the vectors and from seed addresses (jump-table targets taken from the JU disassembly labels as *hints*);
reports instruction starts, basic-block leaders, JSR targets (functions), indirect jumps, data ranges and anomalies.
Usage: cfg6502.py rom.nes [seeds.txt] > cfg.json"""
import sys, json, re
sys.path.insert(0, __import__('os').path.dirname(__file__))
from m6502 import OPS, MODES
def load_prg(fn):
    d = open(fn, 'rb').read(); return d[16:16+16384]
def rd(prg, a):
    return prg[a & 0x3fff] if a >= 0x8000 else None
def discover(prg, seeds, always=frozenset()):
    starts = {}; leaders = set(seeds); funcs = set(); indirect = []; work = list(seeds); issues = []
    rts_trick = []
    while work:
        a = work.pop()
        while True:
            if a < 0x8000 or a > 0xffff: issues.append('target outside ROM %04X' % a); break
            if a in starts: break
            op = prg[a & 0x3fff]
            if op not in OPS: issues.append('illegal/unknown opcode %02X at %04X' % (op, a)); break
            mn, mode = OPS[op]; n = MODES[mode]
            ops = [prg[(a + i) & 0x3fff] for i in range(1, n)]
            starts[a] = (op, mn, mode, n)
            nxt = a + n
            if mode == 'rel':
                t = (nxt + (ops[0] - 256 if ops[0] >= 128 else ops[0])) & 0xffff
                leaders.add(t); work.append(t)
                if a in always: break            # branch known (from the disassembly comment '; jmp') to be always taken; verified dynamically
                leaders.add(nxt); a = nxt; continue
            if mn == 'jmp':
                if mode == 'abs': t = ops[0] | ops[1] << 8; leaders.add(t); work.append(t)
                else: indirect.append(a)
                break
            if mn == 'jsr':
                t = ops[0] | ops[1] << 8; funcs.add(t); leaders.add(t); leaders.add(nxt); work.append(t); a = nxt; continue
            if mn in ('rts', 'rti', 'brk'): break
            a = nxt
    return starts, leaders, funcs, indirect, issues
if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(); ap.add_argument('rom'); ap.add_argument('--seeds'); ap.add_argument('--always'); ap.add_argument('--skip', default='')
    args = ap.parse_args()
    prg = load_prg(args.rom)
    vec = {n: prg[0x3ffa + 2*i] | prg[0x3ffb + 2*i] << 8 for i, n in enumerate(('nmi', 'reset', 'irq'))}
    seeds = {vec['nmi'], vec['reset']}            # the IRQ vector ($FFF0) points at data; no IRQ source is enabled (checked in the APU model)
    if args.seeds:
        for l in open(args.seeds):
            l = l.strip()
            if l: seeds.add(int(l, 16))
    skip = {int(x, 16) for x in args.skip.split(',') if x}
    seeds -= skip
    always = set()
    if args.always:
        always = {int(l, 16) for l in open(args.always) if l.strip()}
    starts, leaders, funcs, indirect, issues = discover(prg, seeds, always)
    covered = set()
    for a, (op, mn, mode, n) in starts.items(): covered.update(range(a, a + n))
    overlap = sorted(a for a in starts if any((a - k) in starts and starts[a - k][3] > k for k in (1, 2)))
    json.dump({'vectors': vec, 'starts': sorted(starts), 'leaders': sorted(leaders & set(starts)), 'funcs': sorted(funcs),
               'indirect_jumps': indirect, 'always_taken': sorted(always & set(starts)), 'issues': issues, 'overlap': overlap,
               'bytes_code': len(covered), 'bytes_total': 0x4000}, sys.stdout)
    print(file=sys.stderr)
    print('instructions %d, code bytes %d of %d, leaders %d, JSR targets %d, indirect JMP at %s' % (len(starts), len(covered), 0x4000, len(leaders & set(starts)), len(funcs), [hex(i) for i in indirect]), file=sys.stderr)
    print('issues', issues[:10], 'overlaps', [hex(i) for i in overlap][:10], file=sys.stderr)
