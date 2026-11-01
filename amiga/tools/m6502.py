"""6502 (official opcodes only) instruction table: mnemonic, addressing mode, length, base cycles, page-cross penalty, kind.
Modes: imp acc imm zp zpx zpy abs abx aby izx izy ind rel
Source of truth for timing: MAME m6502 core / MOS data sheet; checked against ref/nesref.c in the per-instruction tests."""
MODES = {'imp':1,'acc':1,'imm':2,'zp':2,'zpx':2,'zpy':2,'abs':3,'abx':3,'aby':3,'izx':2,'izy':2,'ind':3,'rel':2}
# (aaa,bbb) decode for cc=1,2,0
G1 = ['ora','and','eor','adc','sta','lda','cmp','sbc']
M1 = ['izx','zp','imm','abs','izy','zpx','aby','abx']
G2 = ['asl','rol','lsr','ror','stx','ldx','dec','inc']
M2 = ['imm','zp','acc','abs',None,'zpx','imp','abx']
G0 = [None,'bit','jmp','jmp','sty','ldy','cpy','cpx']
M0 = ['imm','zp',None,'abs',None,'zpx',None,'abx']
OPS = {}
def _add(op, mn, mode):
    OPS[op] = (mn, mode)
for aaa in range(8):
    for bbb in range(8):
        op = aaa<<5 | bbb<<2 | 1
        if aaa == 4 and bbb == 2: continue
        _add(op, G1[aaa], M1[bbb])
for aaa in range(8):
    for bbb in range(8):
        op = aaa<<5 | bbb<<2 | 2
        mn = G2[aaa]; mode = M2[bbb]
        if mode is None or bbb == 6 and aaa not in (4,5): 
            # bbb=6 is implied (txs tsx dex nop) handled below
            continue
        if bbb == 0 and aaa not in (5,): continue          # only LDX #imm
        if bbb == 2 and aaa >= 4: continue                 # txa tax dex nop are implied ops
        if bbb == 6: continue
        if aaa in (4,5) and bbb == 5: mode = 'zpy'
        if aaa == 5 and bbb == 7: mode = 'aby'
        if aaa == 4 and bbb == 7: continue
        _add(op, mn, mode)
for op,(mn,mode) in {0x9a:('txs','imp'),0xba:('tsx','imp'),0x8a:('txa','imp'),0xaa:('tax','imp'),0xca:('dex','imp'),0xea:('nop','imp')}.items(): _add(op,mn,mode)
for aaa in range(8):
    for bbb in (0,1,3,5,7):
        op = aaa<<5 | bbb<<2
        mn = G0[aaa]; mode = M0[bbb]
        if mn is None or mn=='jmp': continue
        if mn == 'bit' and bbb not in (1,3): continue
        if mn == 'sty' and bbb in (0,7): continue
        if mn in ('cpy','cpx') and bbb in (5,7): continue
        if mn == 'ldy' and False: pass
        _add(op, mn, mode)
_add(0x4c,'jmp','abs'); _add(0x6c,'jmp','ind'); _add(0x20,'jsr','abs'); _add(0x60,'rts','imp'); _add(0x40,'rti','imp'); _add(0x00,'brk','imp')
for i,mn in enumerate(['bpl','bmi','bvc','bvs','bcc','bcs','bne','beq']): _add(i*32+0x10, mn, 'rel')
for op,mn in {0x08:'php',0x28:'plp',0x48:'pha',0x68:'pla',0x88:'dey',0xa8:'tay',0xc8:'iny',0xe8:'inx',0x18:'clc',0x38:'sec',0x58:'cli',0x78:'sei',0x98:'tya',0xb8:'clv',0xd8:'cld',0xf8:'sed'}.items(): _add(op,mn,'imp')

# remove forms that do not exist on the 6502
for op in list(OPS):
    mn,mode = OPS[op]
    if mn=='sta' and mode=='imm': del OPS[op]
    if mn in ('asl','rol','lsr','ror','dec','inc') and mode in ('imm',): del OPS[op]
    if mn in ('stx','ldx') and mode in ('abx','zpx'): del OPS[op]
    if mn in ('sty','ldy') and mode in ('aby','zpy'): del OPS[op]
    if mn=='ldx' and mode=='abx': del OPS[op]
    if mn=='ldy' and mode=='abx': pass
    if mn=='stx' and mode=='imm': del OPS[op]
