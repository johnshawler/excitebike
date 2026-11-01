#!/usr/bin/env python3
"""xl6502 - static translator 6502 -> 68020 (GNU as, MIT syntax) for the Excitebike (Europe) port.

Register map
  d0=A  d1=X  d2=Y        bits 8..31 of d1/d2 are always 0 (only byte operations are applied to them)
  d4,d5,d6                scratch   (d4 = address temp, d5 = ccr save / stub argument, d6 = data temp / I/O result)
  a0 = RAM base           ('flat' test model: a 64 KB array)
  a1 = PRG base           (prg[0] = $C000)
  a2 = 6502 stack pointer as a host pointer (= RAM + $100 + S)
  a3 = runtime state block, a4 = dispatch table, a5 = cycle counter (6502 CPU cycles; an address register so that adding to it never disturbs flags)
Flags
  68k N,Z = 6502 N,Z.  The 6502 carry lives in the 68k X and/or C bits with a static polarity ('P': bit = carry, 'N': bit = not carry,
  after sub/cmp).  Liveness analysis (flags.py ideas, reimplemented) decides where wrappers that preserve flags are needed.
  The 6502 V flag is not modelled (the ROM has no BVC/BVS/PHP/PLP/BRK; translate() refuses them); I and D are constants.
Timing
  Normal blocks add their static length to a5 on entry; blocks reachable from the main thread (where an NMI can land between two
  instructions) poll nmi_due before every instruction and add each instruction's cycles after it.  Page-cross and taken-branch
  penalties are added dynamically.  I/O accesses pass their exact cycle (a5 + d5) to the runtime.
"""
import sys, os, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from m6502 import OPS, MODES
CYC = {int(k): tuple(v) for k, v in json.load(open(os.path.join(HERE, 'cycles6502.json'))).items()}
FN, FZ, FC = 1, 2, 4
BRANCH_FLAG = {'bpl': FN, 'bmi': FN, 'bne': FZ, 'beq': FZ, 'bcc': FC, 'bcs': FC}
DEFS = {}
for _m in 'lda ldx ldy tax tay txa tya tsx pla and ora eor inc dec inx iny dex dey bit'.split(): DEFS[_m] = FN | FZ
for _m in 'cmp cpx cpy adc sbc asl lsr rol ror'.split(): DEFS[_m] = FN | FZ | FC
for _m in 'clc sec'.split(): DEFS[_m] = FC
USES = {'adc': FC, 'sbc': FC, 'rol': FC, 'ror': FC}
LOADALU = {'lda', 'ldx', 'ldy', 'and', 'ora', 'eor', 'adc', 'sbc', 'cmp', 'cpx', 'cpy', 'bit'}    # reads: page-cross penalty on abx/aby/izy
STORE = {'sta', 'stx', 'sty'}
RMW = {'asl', 'lsr', 'rol', 'ror', 'inc', 'dec'}
UNSUPPORTED = {'php', 'plp', 'brk', 'bvc', 'bvs'}

class Insn:
    __slots__ = ('addr', 'op', 'mn', 'mode', 'n', 'arg', 'base')
    def __init__(s, addr, op, arg):
        s.addr, s.op = addr, op; s.mn, s.mode = OPS[op]; s.n = MODES[s.mode]; s.arg = arg; s.base = CYC[op][0]
    @property
    def nxt(s): return (s.addr + s.n) & 0xffff
    def target(s):
        if s.mode == 'rel': return (s.nxt + (s.arg - 256 if s.arg >= 128 else s.arg)) & 0xffff
        return s.arg
def hx(v): return '0x%x' % v

class Gen:
    """one stream of generated code with the carry-tracking state"""
    def __init__(s, lp='L'):
        s.out = []; s.lp = lp; s.nl = 0; s.cpol = 'P'; s.cloc = set()
    def e(s, line): s.out.append('\t' + line)
    def lab(s, name): s.out.append(name + ':')
    def newlabel(s): s.nl += 1; return '.%s%d' % (s.lp, s.nl)
    def snap(s): return (s.cpol, set(s.cloc))
    def restore(s, sn): s.cpol, s.cloc = sn[0], set(sn[1])
    # --- carry
    def c_set(s, pol, loc): s.cpol, s.cloc = pol, set(loc)
    def c_flip(s):
        m = (0x10 if 'X' in s.cloc else 0) | (1 if 'C' in s.cloc else 0)
        assert m, 'carry flip while undefined'
        s.e('eori.b #%s,%%ccr' % hx(m)); s.cpol = 'N' if s.cpol == 'P' else 'P'
    def c_need_x(s):
        if 'X' in s.cloc: return
        assert 'C' in s.cloc, 'carry used while undefined'
        s.e('move.w %ccr,%d5'); s.e('move.w %d5,%d4'); s.e('andi.w #1,%d4'); s.e('lsl.w #4,%d4'); s.e('andi.w #0x0f,%d5'); s.e('or.w %d4,%d5'); s.e('move.w %d5,%ccr')
        s.cloc.add('X')
    def c_need_c(s):
        if 'C' in s.cloc: return
        assert 'X' in s.cloc, 'carry used while undefined'
        s.e('move.w %ccr,%d5'); s.e('move.w %d5,%d4'); s.e('lsr.w #4,%d4'); s.e('andi.w #1,%d4'); s.e('andi.w #0x1e,%d5'); s.e('or.w %d4,%d5'); s.e('move.w %d5,%ccr')
        s.cloc.add('C')
    def c_canon(s):
        """canonical state at block boundaries when the carry is live: polarity P, X and C both valid"""
        if not s.cloc: return
        if s.cpol == 'N': s.c_flip()
        s.c_need_x(); s.c_need_c()
    # --- wrappers
    def wrap(s, lines):                          # preserve the whole ccr
        s.e('move.w %ccr,%d5')
        for l in lines: s.e(l)
        s.e('move.w %d5,%ccr')
    def wrap_nz(s, lines):                       # keep the new N,Z, restore the old X,C
        s.e('move.w %ccr,%d5')
        for l in lines: s.e(l)
        s.e('move.w %ccr,%d6'); s.e('andi.w #0x0c,%d6'); s.e('andi.w #0x11,%d5'); s.e('or.w %d5,%d6'); s.e('move.w %d6,%ccr')

class Translator:
    def __init__(s, prg, cfg, model='nes', ind_targets=(), trace=False, fuse=True):
        s.trace = trace
        s.prg = prg; s.model = model; s.cfg = cfg
        s.ins = {}
        for a in cfg['starts']:
            op = prg[a & 0x3fff]; n = MODES[OPS[op][1]]; arg = 0
            if n == 2: arg = prg[(a + 1) & 0x3fff]
            elif n == 3: arg = prg[(a + 1) & 0x3fff] | prg[(a + 2) & 0x3fff] << 8
            s.ins[a] = Insn(a, op, arg)
        s.ind_targets = set(ind_targets)
        s.always = set(cfg.get('always_taken', []))
        s.leaders = set(cfg['leaders']) | s.ind_targets | set(cfg['vectors'].values())
        for i in s.ins.values():
            if i.mn in UNSUPPORTED: raise ValueError('unsupported %s at %04X' % (i.mn, i.addr))
            if i.mode == 'rel': s.leaders.add(i.target())
            if i.mn in ('jsr', 'jmp') and i.mode == 'abs': s.leaders.add(i.target())
        s.fuse = fuse
        IDLE = bytes.fromhex('2026d3a54830f9f0f7'); RNG = bytes.fromhex('a5182902850 0a5192902450018f00138661866196 61a661b661c661d661e661f60'.replace(' ', ''))
        s.idle_ok = bytes(prg[0x1d8:0x1e1]) == IDLE and bytes(prg[0x1326:0x1326 + len(RNG)]) == RNG
        s.reach(); s.liveness()
    # ------------------------------------------------------------ analyses
    def succs(s, i):
        if i.mode == 'rel': return [i.target()] if i.addr in s.always else [i.target(), i.nxt]
        if i.mn == 'jmp': return sorted(s.ind_targets) if i.mode == 'ind' else [i.target()]
        if i.mn in ('rts', 'rti'): return []
        return [i.nxt]
    def liveness(s):
        live = {a: 0 for a in s.ins}; calls = [i for i in s.ins.values() if i.mn == 'jsr']
        rts_out = 0; changed = True
        while changed:
            changed = False
            rts_out = 0
            for c in calls: rts_out |= live.get(c.nxt, 0)
            for a in sorted(s.ins, reverse=True):
                i = s.ins[a]
                if i.mn == 'jsr': li = live.get(i.target(), 0)
                elif i.mn == 'rti': li = 0
                else:
                    if i.mn == 'rts': lo = rts_out
                    else:
                        lo = 0
                        for t in s.succs(i): lo |= live.get(t, 0)
                    li = BRANCH_FLAG.get(i.mn, 0) | USES.get(i.mn, 0) | (lo & ~DEFS.get(i.mn, 0))
                    if a in s.main: li |= FN | FZ | FC          # an NMI can arrive here: the pushed P must be exact
                if li != live[a]: live[a] = li; changed = True
        s.live_in = live; s.rts_out = rts_out
    def live_out(s, i):
        if i.mn == 'rts': return s.rts_out
        if i.mn == 'rti': return 0
        if i.mn == 'jsr': return s.live_in.get(i.target(), 0)       # flags the callee may read
        lo = 0
        for t in s.succs(i): lo |= s.live_in.get(t, 0)
        return lo
    def reach(s):
        def walk(roots):
            seen = set(); work = list(roots)
            while work:
                a = work.pop()
                if a in seen or a not in s.ins: continue
                seen.add(a); i = s.ins[a]
                work += ([i.target(), i.nxt] if i.mn == 'jsr' else s.succs(i))
            return seen
        v = s.cfg['vectors']; s.main = walk([v['reset']]); s.nmi = walk([v['nmi']])
    # ------------------------------------------------------------ memory classification
    def classify(s, addr):
        if s.model == 'flat': return 'flat'
        if addr < 0x2000: return 'ram'
        if addr < 0x4000: return 'ppu'
        if addr < 0x4020: return 'apu'
        if addr >= 0x8000: return 'rom'
        return 'open'
    def ea_static(s, addr):
        r = s.classify(addr)
        if r == 'flat': return '%s(%%a0)' % hx(addr)
        if r == 'ram': return '%s(%%a0)' % hx(addr & 0x7ff)
        if r == 'rom': return '%s(%%a1)' % hx(addr & 0x3fff)
        return None
    def idx_region(s, base):
        """region of base..base+255 if one directly addressable region, else None"""
        if s.model == 'flat': return 'flat' if base + 255 <= 0xffff else None
        if base + 255 <= 0x7ff: return 'ram'
        if base >= 0x8000 and base + 255 <= 0xffff: return 'rom'
        return None
    # ------------------------------------------------------------ operands
    def pagecross_abs(s, g, idx, lo8, live_in):
        sv = bool(live_in)
        if sv: g.e('move.w %ccr,%d5')
        g.e('move.l %s,%%d3' % idx); g.e('add.b #%s,%%d3' % hx(lo8))
        l = g.newlabel(); g.e('bcc.s %s' % l); g.e('addq.l #1,%a5'); g.lab(l)
        if sv: g.e('move.w %d5,%ccr')
    def pre(s, g, live_in, lines):
        """address set-up code uses flag-setting arithmetic: keep live flags (N,Z,carry) intact"""
        if live_in: g.e('move.w %ccr,%d5')
        for l in lines: g.e(l)
        if live_in: g.e('move.w %d5,%ccr')
    def operand(s, g, i, kind, live_in):
        """-> (ea, io).  Emits address set-up into g.  kind: 'r' read, 'w' write, 'm' read-modify-write.  io: None or (region, addr|None)"""
        mode, a = i.mode, i.arg
        if mode == 'imm': return '#%s' % hx(a), None
        if mode == 'zp': return '%s(%%a0)' % hx(a), None
        if mode in ('zpx', 'zpy'):
            s.pre(g, live_in, ['move.l %s,%%d4' % ('%d1' if mode == 'zpx' else '%d2'), 'add.b #%s,%%d4' % hx(a)]); return '(%a0,%d4.l)', None
        if mode == 'abs':
            r = s.classify(a)
            if r in ('flat', 'ram', 'rom'):
                if r == 'rom' and kind != 'r': raise ValueError('store to ROM at %04X' % i.addr)
                return s.ea_static(a), None
            return None, (r, a)
        if mode in ('abx', 'aby'):
            idx = '%d1' if mode == 'abx' else '%d2'; reg = s.idx_region(a)
            if reg:
                if i.mn in LOADALU: s.pagecross_abs(g, idx, a & 0xff, live_in)
                if reg == 'flat': return '%s(%%a0,%s.l)' % (hx(a), idx), None
                if reg == 'ram': return '%s(%%a0,%s.l)' % (hx(a), idx), None
                return '%s(%%a1,%s.l)' % (hx(a & 0x3fff), idx), None
            s.pre(g, live_in, ['moveq #0,%d4', 'move.w #%s,%%d4' % hx(a), 'add.w %s,%%d4' % idx])
            if i.mn in LOADALU: s.pagecross_abs(g, idx, a & 0xff, live_in)
            if s.model == 'flat': return '(%a0,%d4.l)', None
            return None, ('gen', None)
        if mode == 'izy':
            z, z1 = a, (a + 1) & 0xff
            lines = ['moveq #0,%d4', 'move.b %s(%%a0),%%d4' % hx(z1), 'lsl.w #8,%d4', 'move.b %s(%%a0),%%d4' % hx(z)]
            if i.mn in LOADALU:
                l = g.newlabel(); lines += ['move.b %d4,%d3', 'add.b %d2,%d3', 'bcc.s %s' % l, 'addq.l #1,%a5', l + ':']
            lines.append('add.w %d2,%d4')
            s.pre(g, live_in, lines)
            if s.model == 'flat': return '(%a0,%d4.l)', None
            return None, ('gen', None)
        if mode == 'izx':
            s.pre(g, live_in, ['move.l %d1,%d4', 'add.b #%s,%%d4' % hx(a), 'moveq #0,%d3', 'move.b (%a0,%d4.l),%d3', 'addq.b #1,%d4', 'moveq #0,%d6',
                               'move.b (%a0,%d4.l),%d6', 'lsl.w #8,%d6', 'or.w %d3,%d6', 'move.l %d6,%d4'])
            if s.model == 'flat': return '(%a0,%d4.l)', None
            return None, ('gen', None)
        raise NotImplementedError(mode)
    def io_call(s, g, io, stamp, read, li=0):
        """call a runtime stub; the stub clobbers the ccr, so flags that are live into the instruction are kept on the stack"""
        kind, a = io
        if li: g.e('move.w %ccr,-(%sp)')
        g.e(('moveq #%d,%%d5' % stamp) if -128 <= stamp <= 127 else ('move.l #%d,%%d5' % stamp))
        if kind == 'gen': g.e('jsr rt_%s_gen' % ('rd' if read else 'wr'))
        else: g.e('jsr rt_%s_%04x' % ('rd' if read else 'wr', (0x2000 + (a & 7)) if kind == 'ppu' else a))
        if li: g.e('move.w (%sp)+,%ccr')
    # ------------------------------------------------------------ instruction translation
    def insn(s, g, i, lo, live_in, stamp):
        """Translate one non-control instruction.  lo = flags live after it, live_in = flags live before it,
        stamp(k) = offset (relative to a5) of the k-th cycle of the instruction."""
        mn, m = i.mn, i.mode
        kr = {'imm': 2, 'zp': 3, 'zpx': 4, 'zpy': 4, 'abs': 4, 'abx': 4, 'aby': 4, 'izy': 5, 'izx': 6}.get(m, 2)          # read cycle (loads)
        kw = {'zp': 3, 'zpx': 4, 'zpy': 4, 'abs': 4, 'abx': 5, 'aby': 5, 'izy': 6, 'izx': 6}.get(m, 2)                  # write cycle (stores)
        clob_nz = bool(lo & (FN | FZ)); c_in_cbit_only = ('C' in g.cloc and 'X' not in g.cloc and bool(lo & FC))
        if mn in ('lda', 'ldx', 'ldy'):
            dst = {'lda': '%d0', 'ldx': '%d1', 'ldy': '%d2'}[mn]
            ea, io = s.operand(g, i, 'r', live_in)
            if io: s.io_call(g, io, stamp(kr), True, live_in); ea = '%d6'
            g.e('move.b %s,%s' % (ea, dst)); g.cloc.discard('C'); return
        if mn in STORE:
            src = {'sta': '%d0', 'stx': '%d1', 'sty': '%d2'}[mn]
            ea, io = s.operand(g, i, 'w', live_in)
            if io:
                g.e('move.b %s,%%d6' % src); s.io_call(g, io, stamp(kw), False, live_in)
                return
            if clob_nz or c_in_cbit_only: g.wrap(['move.b %s,%s' % (src, ea)])
            else: g.e('move.b %s,%s' % (src, ea)); g.cloc.discard('C')
            return
        if mn in ('tax', 'tay', 'txa', 'tya'):
            d, sr = {'tax': ('%d1', '%d0'), 'tay': ('%d2', '%d0'), 'txa': ('%d0', '%d1'), 'tya': ('%d0', '%d2')}[mn]
            g.e('move.b %s,%s' % (sr, d)); g.cloc.discard('C'); return
        if mn == 'txs': g.e('lea 0x100(%a0,%d1.l),%a2'); return                        # lea: no flags
        if mn == 'tsx':
            seq = ['move.l %a2,%d4', 'sub.l %a0,%d4', 'sub.w #0x100,%d4', 'move.b %d4,%d1']
            if g.cloc and (lo & FC): g.wrap_nz(seq)
            else:
                for l in seq: g.e(l)
                g.cloc.clear()
            return
        if mn in ('inx', 'iny', 'dex', 'dey'):
            r = '%d1' if mn in ('inx', 'dex') else '%d2'; op = 'addq.b' if mn in ('inx', 'iny') else 'subq.b'
            if g.cloc and (lo & FC): g.wrap_nz(['%s #1,%s' % (op, r)])
            else: g.e('%s #1,%s' % (op, r)); g.cloc.clear()
            return
        if mn == 'clc':
            if lo & (FN | FZ): g.e('andi.b #0xee,%ccr')
            else: g.e('sub.b %d6,%d6')
            g.c_set('P', {'X', 'C'}); return
        if mn == 'sec':
            if lo & (FN | FZ): g.e('ori.b #0x11,%ccr')
            else: g.e('moveq #1,%d6'); g.e('neg.b %d6')
            g.c_set('P', {'X', 'C'}); return
        if mn in ('sei', 'cld', 'cli', 'clv', 'sed', 'nop'):
            if mn in ('sed',): raise ValueError('decimal mode')
            return
        if mn in ('and', 'ora', 'eor'):
            ea, io = s.operand(g, i, 'r', live_in)
            if io: s.io_call(g, io, stamp(kr), True, live_in); ea = '%d6'
            if mn == 'eor':
                if not ea.startswith('%d'): g.e('move.b %s,%%d6' % ea); ea = '%d6'
                g.e('eor.b %s,%%d0' % ea)
            else: g.e('%s.b %s,%%d0' % ('and' if mn == 'and' else 'or', ea))
            g.cloc.discard('C'); return
        if mn in ('cmp', 'cpx', 'cpy'):
            r = {'cmp': '%d0', 'cpx': '%d1', 'cpy': '%d2'}[mn]
            ea, io = s.operand(g, i, 'r', live_in)
            if io: s.io_call(g, io, stamp(kr), True, live_in); ea = '%d6'
            g.e('cmp.b %s,%s' % (ea, r)); g.c_set('N', {'C'}); return
        if mn in ('adc', 'sbc'):
            if mn == 'adc' and g.cpol == 'N' or mn == 'sbc' and g.cpol == 'P': g.c_flip()
            g.c_need_x()
            ea, io = s.operand(g, i, 'r', live_in)
            if io: raise NotImplementedError('adc/sbc on I/O')
            if ea.startswith('%d'): sr = ea
            else: g.e('move.b %s,%%d6' % ea); sr = '%d6'          # move keeps X
            if lo & FZ: g.e('ori.b #0x04,%ccr')                    # addx/subx only clear Z: pre-set it
            g.e('%s.b %s,%%d0' % ('addx' if mn == 'adc' else 'subx', sr)); g.c_set('P' if mn == 'adc' else 'N', {'X', 'C'}); return
        if mn == 'bit':
            ea, io = s.operand(g, i, 'r', live_in)
            if io: s.io_call(g, io, stamp(kr), True, live_in); ea = '%d6'
            elif not ea.startswith('%d'): g.e('move.b %s,%%d6' % ea); ea = '%d6'
            g.e('move.b %s,%%d5' % ea); g.e('and.b %d0,%d6'); g.e('move.w %ccr,%d4'); g.e('andi.w #0x14,%d4')
            g.e('andi.b #0x80,%d5'); g.e('lsr.b #4,%d5'); g.e('or.w %d5,%d4'); g.e('move.w %d4,%ccr'); g.cloc.discard('C'); return
        if mn in ('asl', 'lsr', 'rol', 'ror'):
            if mn in ('rol', 'ror'):
                if g.cpol == 'N': g.c_flip()
                g.c_need_x()
            if m == 'acc':
                if mn == 'asl': g.e('add.b %d0,%d0')
                elif mn == 'lsr': g.e('lsr.b #1,%d0')
                else: g.e('%s.b #1,%%d0' % ('roxl' if mn == 'rol' else 'roxr'))
                g.c_set('P', {'X', 'C'}); return
            ea, io = s.operand(g, i, 'm', live_in)
            if io: raise NotImplementedError('shift on I/O')
            g.e('move.b %s,%%d6' % ea)
            if mn == 'asl': g.e('add.b %d6,%d6')
            elif mn == 'lsr': g.e('lsr.b #1,%d6')
            else: g.e('%s.b #1,%%d6' % ('roxl' if mn == 'rol' else 'roxr'))
            g.wrap(['move.b %%d6,%s' % ea]); g.c_set('P', {'X', 'C'}); return
        if mn in ('inc', 'dec'):
            ea, io = s.operand(g, i, 'm', live_in)
            if io: raise NotImplementedError('inc/dec on I/O')
            op = 'addq.b' if mn == 'inc' else 'subq.b'
            if g.cloc and (lo & FC): g.wrap_nz(['%s #1,%s' % (op, ea)])
            else: g.e('%s #1,%s' % (op, ea)); g.cloc.clear()
            return
        if mn == 'pha':
            if lo or g.cloc: g.wrap(['move.b %d0,(%a2)'])
            else: g.e('move.b %d0,(%a2)')
            g.e('lea -1(%a2),%a2'); return
        if mn == 'pla':
            g.e('lea 1(%a2),%a2'); g.e('move.b (%a2),%d0'); g.cloc.discard('C'); return
        raise NotImplementedError(mn)

    # ------------------------------------------------------------ fused busy-wait loops (exact closed forms in rt/nes_rt.c, verified by trace equality)
    def fusion_for(s, blk):
        a = blk[0]
        if len(blk) == 3 and a.mn == 'lda' and a.mode == 'abs' and a.arg == 0x2002 and blk[1].mn == 'and' and blk[1].mode == 'imm' \
                and blk[2].mn in ('bne', 'beq') and blk[2].target() == a.addr:
            m, nz = blk[1].arg, blk[2].mn == 'bne'
            if (m == 0x40) or (m == 0x80 and not nz): return ('poll', m, nz)
        if len(blk) == 1 and a.mn == 'ldy' and a.mode == 'imm' and a.addr not in s.main:
            i1, i2, i3, i4 = (s.ins.get(a.addr + k) for k in (2, 3, 5, 6))
            if i1 and i2 and i3 and i4 and i1.mn == 'dey' and i2.mn == 'bne' and i2.target() == i1.addr and i3.mn == 'dex' and i4.mn == 'bne' and i4.target() == a.addr:
                return ('delay', a.arg)
        return None
    def emit_fused(s, g, blk, fz, li, flush, out):
        a = blk[0]
        if fz[0] == 'poll':
            _, m, nz = fz
            if li: g.e('move.w %ccr,-(%sp)')
            g.e('move.l #%s,%%d4' % hx(m)); g.e('moveq #%d,%%d5' % (1 if nz else 0))
            g.e('move.l ev_due(%a3),%d6' if flush else 'move.l #0x7fffffff,%d6')
            g.e('jsr rt_fuse_poll'); g.e('tst.l %d5')
            ok = '.F%04X' % a.addr
            g.e('bne.s %s' % ok)
            if li: g.e('move.w (%sp)+,%ccr')
            g.e('jmp %s_n' % s.lbl(a.addr)); g.lab(ok)
            if li: g.e('move.w (%sp)+,%ccr')
            g.e('move.b %d6,%d0'); g.cloc.discard('C')
            nxt = a.addr + 7
            if s.live_in.get(nxt, 0) & FC: g.c_canon()
            g.e('jmp %s' % s.lbl(nxt))
            return
        if fz[0] == 'delay':
            n = fz[1] or 256
            if li: g.e('move.w %ccr,%d5')
            g.e('move.l %d1,%d4'); g.e('bne.s .D%04X' % a.addr); g.e('move.w #256,%d4'); g.lab('.D%04X' % a.addr)
            g.e('mulu.w #%d,%%d4' % (5 * n + 6)); g.e('lea -1(%a5,%d4.l),%a5')
            if li: g.e('move.w %d5,%ccr')
            g.e('moveq #0,%d1'); g.e('moveq #0,%d2'); g.cloc.discard('C')
            g.e('jmp %s' % s.lbl(a.addr + 8))
            return
    # ------------------------------------------------------------ whole program
    def block_ranges(s):
        addrs = sorted(s.ins); blocks = []; cur = None
        for a in addrs:
            i = s.ins[a]
            if cur is None or a in s.leaders or cur[-1].nxt != a:
                cur = []; blocks.append(cur)
            cur.append(i)
            if i.mn in ('jmp', 'rts', 'rti') or (i.mn == 'jsr') or i.mode == 'rel':
                cur = None
        return blocks
    def lbl(s, a): return 'L_%04X' % a
    def emit_program(s):
        out = ['\t.equ ev_due,0', '\t.text', '\t.even', '\t.globl L_C184', '\t.globl L_C23B']
        stubs = []
        blocks = s.block_ranges()
        polled = 0
        for blk in blocks:
            first = blk[0]
            flush = first.addr in s.main
            L = sum(i.base for i in blk)
            fz = s.fusion_for(blk) if s.fuse else None
            entry_label = s.lbl(first.addr)
            if fz:
                li0 = s.live_in.get(first.addr, 0)
                gf = Gen('f%04X_' % first.addr); out.append(entry_label + ':')
                if li0 & FC: gf.c_set('P', {'X', 'C'})
                if flush:                                      # main thread: the usual poll in front of the fused code
                    lab = entry_label + '_fp'; sv = bool(li0)
                    if sv: gf.e('move.w %ccr,%d5')
                    gf.e('cmpa.l ev_due(%a3),%a5'); gf.e('bcs.s %s_k' % lab)
                    if not sv: gf.e('moveq #0,%d5')
                    gf.e('move.w #%s,%%d4' % hx(first.addr)); gf.e('jsr rt_nmi_deliver')
                    if sv: gf.e('bra.s %s' % lab)
                    gf.lab(lab + '_k')
                    if sv: gf.e('move.w %d5,%ccr')
                    gf.lab(lab)
                s.emit_fused(gf, blk, fz, li0, flush, out)
                out += gf.out
                if fz[0] == 'delay': continue
                entry_label = entry_label + '_n'                 # the translated loop (taken when the fused code declines: an event is due inside the next iteration)
            g = Gen('b%04X_' % first.addr); out.append(entry_label + ':')
            if s.trace: g.e('pea %s' % hx(first.addr)); g.e('jsr rt_dbg_block'); g.e('addq.l #4,%sp')
            if not flush and L: g.e('lea %d(%%a5),%%a5' % L)
            if s.live_in.get(first.addr, 0) & FC: g.c_set('P', {'X', 'C'})
            soff = 0
            for idx, i in enumerate(blk):
                lo = s.live_out(i); li = s.live_in.get(i.addr, 0)
                if flush:
                    polled += 1
                    # poll: flags live at this point are kept in canonical form across the (possible) NMI
                    if li & FC: g.c_canon()
                    lab = s.lbl(i.addr) + '_p'
                    sv = bool(li)
                    if sv: g.e('move.w %ccr,%d5')
                    g.e('cmpa.l ev_due(%a3),%a5'); g.e('bcs.s %s_k' % lab)
                    if not sv: g.e('moveq #0,%d5')
                    g.e('move.w #%s,%%d4' % hx(i.addr)); g.e('jsr rt_nmi_deliver')
                    if sv: g.e('bra.s %s' % lab)
                    g.lab(lab + '_k')
                    if sv: g.e('move.w %d5,%ccr')
                    g.lab(lab)
                    if s.fuse and i.addr == 0xC1D8 and s.idle_ok: g.e('jsr rt_idle_fuse')
                    st = (lambda k: k)
                else:
                    st = (lambda k, soff=soff, L=L: soff + k - L)
                s.control_or_insn(g, i, lo, li, st, flush, stubs, blk)
                if flush and i.mn not in ('jmp', 'rts', 'rti', 'jsr') and i.mode != 'rel':
                    g.e('lea %d(%%a5),%%a5' % i.base)
                soff += i.base
            last = blk[-1]
            if last.mn not in ('jmp', 'rts', 'rti') and last.mode != 'rel' and last.mn != 'jsr':
                if s.live_in.get(last.nxt, 0) & FC: g.c_canon()
            out += g.out
        out += stubs
        out += ['\t.data', '\t.even', '\t.globl dispatch_tab', 'dispatch_tab:']
        for a in range(0xc000, 0x10000):
            out.append('\t.long %s' % (s.lbl(a) if a in s.ins and a in s.leaders else 'rt_bad_jump'))
        return '\n'.join(out) + '\n', polled
    def control_or_insn(s, g, i, lo, li, st, flush, stubs, blk):
        mn = i.mn
        def adv(n):                                # cycles of this instruction added before leaving (flush mode)
            if flush and n: g.e('lea %d(%%a5),%%a5' % n)
        if i.mode == 'rel':
            cond = {'beq': 'eq', 'bne': 'ne', 'bpl': 'pl', 'bmi': 'mi'}.get(mn)
            if mn in ('bcs', 'bcc'):
                g.c_need_c(); cond = ('cs' if mn == 'bcs' else 'cc') if g.cpol == 'P' else ('cc' if mn == 'bcs' else 'cs')
            t = i.target(); pen = 1 + (1 if (t & 0xff00) != (i.nxt & 0xff00) else 0)
            sn = g.snap(); adv(i.base)
            inv = {"eq": "ne", 'ne': 'eq', 'pl': 'mi', 'mi': 'pl', 'cs': 'cc', 'cc': 'cs'}[cond]
            lab = '.N%04X' % i.addr
            g.e('b%s.s %s' % (inv, lab))
            tg = Gen('t%04X_' % i.addr); tg.restore(sn)
            if s.live_in.get(t, 0) & FC: tg.c_canon()
            tg.e('lea %d(%%a5),%%a5' % pen); tg.e('jmp %s' % s.lbl(t))
            g.out += tg.out
            g.lab(lab)
            if i.addr in s.always: g.e('jsr rt_bad_flow')           # hint '; jmp' of the disassembly: must never fall through
            elif s.live_in.get(i.nxt, 0) & FC: g.c_canon()
            return
        if mn == 'jmp':
            adv(i.base)
            if i.mode == 'abs':
                if s.live_in.get(i.arg, 0) & FC: g.c_canon()
                g.e('jmp %s' % s.lbl(i.arg)); return
            a = i.arg                                             # jmp (zp-pointer)
            if s.classify(a) not in ('ram', 'flat'): raise ValueError('indirect jump pointer outside RAM')
            ea = (a & 0x7ff) if s.model == 'nes' else a
            ea1 = ((a + 1) & 0xffff) if (a & 0xff) != 0xff else (a & 0xff00)
            ea1 = (ea1 & 0x7ff) if s.model == 'nes' else ea1
            g.e('moveq #0,%d4'); g.e('move.b %s(%%a0),%%d4' % hx(ea1)); g.e('lsl.w #8,%d4'); g.e('move.b %s(%%a0),%%d4' % hx(ea))
            g.e('and.w #0x3fff,%d4'); g.e('movea.l (%a4,%d4.l*4),%a6'); g.e('jmp (%a6)'); return
        if mn == 'jsr':
            adv(i.base)
            r = (i.nxt - 1) & 0xffff; word = ((r & 0xff) << 8) | (r >> 8)
            if lo: g.wrap(['move.w #%s,-1(%%a2)' % hx(word)])
            else: g.e('move.w #%s,-1(%%a2)' % hx(word))
            g.e('lea -2(%a2),%a2'); g.e('jsr %s' % s.lbl(i.target())); return
        if mn == 'rts':
            adv(i.base); g.e('lea 2(%a2),%a2'); g.e('rts'); return
        if mn == 'rti':
            adv(i.base); g.e('jmp rt_rti'); return
        s.insn(g, i, lo, li, st)

def build(romfile, cfgfile, seedsfile, model='nes', trace=False, fuse=True):
    prg = open(romfile, 'rb').read()[16:16 + 16384]
    cfg = json.load(open(cfgfile))
    ind = [int(l, 16) for l in open(seedsfile) if l.strip()]
    return Translator(prg, cfg, model, [t for t in ind if t in set(cfg['starts'])], trace, fuse)

if __name__ == '__main__':
    t = build(sys.argv[1], sys.argv[2], sys.argv[3], trace=('--trace' in sys.argv), fuse=('--nofuse' not in sys.argv))
    text, polled = t.emit_program()
    open(sys.argv[4], 'w').write(text)
    print('main-thread instructions %d, nmi %d, shared %d; polled instructions %d' % (len(t.main), len(t.nmi), len(t.main & t.nmi), polled), file=sys.stderr)
