#!/usr/bin/env python3
"""Per-instruction tests of the translator (flat memory model) against SingleStepTests 65x02 vectors.
usage: gen_tests.py sst_dir out_dir [tests_per_opcode]
writes out_dir/tests.s (one translated function per selected test), out_dir/tests.txt (vectors)."""
import sys, os, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from m6502 import OPS, MODES
import xl6502
from xl6502 import Insn, Gen, Translator, FN, FZ, FC
sst, out, per = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 60
os.makedirs(out, exist_ok=True)
SKIP = {'bpl','bmi','bvc','bvs','bcc','bcs','bne','beq','jmp','jsr','rts','rti','brk','php','plp','sed','clv'}
tr = Translator.__new__(Translator); tr.model = 'flat'
fn = []; vec = []
for op in sorted(OPS):
    mn, mode = OPS[op]
    if mn in SKIP: continue
    tests = json.load(open(os.path.join(sst, '%02x.json' % op)))
    n = 0
    for t in tests:
        i0, f0 = t['initial'], t['final']
        if i0['s'] in (0, 1, 255) and mn in ('pha', 'pla', 'txs'): continue
        # operand bytes from the vector
        mem = {a: v for a, v in i0['ram']}
        pc = i0['pc']; nb = MODES[mode]
        arg = 0
        if nb == 2: arg = mem[(pc + 1) & 0xffff]
        elif nb == 3: arg = mem[(pc + 1) & 0xffff] | mem[(pc + 2) & 0xffff] << 8
        if mode in ('abx', 'aby') and arg + 255 > 0xffff: pass
        ins = Insn(pc, op, arg)
        g = Gen('t%d_' % len(fn))
        g.c_set('P', {'X', 'C'})
        try:
            g.e('lea %d(%%a5),%%a5' % ins.base)
            tr.insn(g, ins, FN | FZ | FC, FN | FZ | FC, lambda k: k)
        except ValueError:
            continue
        if mode in ('abx', 'aby') and (arg + (i0['x'] if mode == 'abx' else i0['y'])) > 0xffff: continue
        g.c_canon()
        fn.append((len(fn), g.out))
        # vector line (same format as sst2txt)
        l = ['T %02x_%s_%d|' % (op, mn, len(fn) - 1), '%d %d %d %d %d %d %d' % (i0['pc'], i0['s'], i0['a'], i0['x'], i0['y'], i0['p'], len(i0['ram']))]
        l += ['%d %d' % (a, v) for a, v in i0['ram']]
        l.append('%d %d %d %d %d %d %d' % (f0['pc'], f0['s'], f0['a'], f0['x'], f0['y'], f0['p'], len(f0['ram'])))
        l += ['%d %d' % (a, v) for a, v in f0['ram']]
        l.append('%d' % len(t['cycles']))
        l += ['%d %d %d' % (a_, v_, 0 if rw_ == 'read' else 1) for a_, v_, rw_ in t['cycles']]
        vec.append(' '.join(l))
        n += 1
        if n >= per: break
with open(os.path.join(out, 'tests.s'), 'w') as f:
    f.write('\t.text\n\t.even\n')
    for n, lines in fn:
        f.write('t%d:\n' % n); f.write('\n'.join(lines) + '\n\trts\n')
    f.write('\t.data\n\t.even\n\t.globl ttab\nttab:\n')
    for n, _ in fn: f.write('\t.long t%d\n' % n)
open(os.path.join(out, 'tests.txt'), 'w').write('\n'.join(vec) + '\n')
print(len(fn), 'tests')
