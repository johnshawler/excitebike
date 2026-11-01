/* nesref.c - reference emulator of the NES/PAL board (RP2A07 6502 + 2C07 PPU, NROM-128, 2 pads) for the Excitebike (Europe) port.
 * Purpose: step 1 of the port.  It is checked against MAME (nespal) with trace files in the same format as ref/mame_trace.lua.
 * It is bus-cycle accurate for the CPU (every cycle is one bus access incl. dummy accesses) and dot accurate for the PPU
 * (3.2 dots per CPU cycle, 341 dots x 312 lines, no odd-frame skip on PAL).  The APU is not synthesised here: register writes are
 * logged (sound is verified separately by the APU model in sound/).
 * Build: gcc -O2 -o nesref nesref.c      Run: nesref rom.nes frames start_frame [trace-out] [dump-every]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

/* ------------------------------------------------------------------ machine state */
static u8 ram[2048], prg[16384], chr[8192], nt[2][1024], pal[32], oam[256];
static int vmirror = 1;
static u64 cpu_cycle;               /* CPU cycles since reset */
static struct { u8 a, x, y, s, p; u16 pc; } c;
enum { FC = 1, FZ = 2, FI = 4, FD = 8, FB = 16, FU = 32, FV = 64, FN = 128 };

/* PPU */
static u8 ctrl, mask, status, oamaddr, rbuf, wlatch, finex;
static u16 v, t;
static int scan = 0, dot = 0;       /* scan 0..311 (0-239 visible, 240 post, 241-310 vblank, 311 pre-render) */
static int ppu_units;               /* 1/5 dot units */
static u64 frames;
static int nmi_line, nmi_prev, nmi_pending; static u64 nmi_edge_cycle;
static int vbl_suppress;
static u16 bg_pat_lo, bg_pat_hi; static u8 bg_at_lo, bg_at_hi; static u8 nt_latch, at_latch, pat_lo_latch, pat_hi_latch;
static u8 at_bits_lo, at_bits_hi;
/* sprites for the current line */
static struct { u8 lo, hi, attr, x; int is0; } spr[8]; static int nspr, spr0_in_line;
static u8 frame_pix[240][256];
static int rendering_at_start;

/* controllers */
static u8 pad_state[2], pad_shift[2], strobe;

/* trace */
static FILE *tr; static int dump_every = 1;
static int input_frame_start = 600;

static void tracef(const char *k, u16 a, u8 val, int writing);

/* ------------------------------------------------------------------ PPU memory */
static u8 *ntp(u16 a) { int i = vmirror ? ((a >> 10) & 1) : ((a >> 11) & 1); return &nt[i][a & 0x3ff]; }
static u8 ppu_rd(u16 a) {
    a &= 0x3fff;
    if (a < 0x2000) return chr[a];
    if (a < 0x3f00) return *ntp(a);
    a &= 0x1f; if ((a & 0x13) == 0x10) a &= 0x0f; return pal[a];
}
static void ppu_wr(u16 a, u8 d) {
    a &= 0x3fff;
    if (a < 0x2000) return;
    if (a < 0x3f00) { *ntp(a) = d; return; }
    a &= 0x1f; if ((a & 0x13) == 0x10) a &= 0x0f; pal[a] = d & 0x3f;
}
static inline int rend_on(void) { return mask & 0x18; }

/* ------------------------------------------------------------------ PPU dot engine */
static void inc_x(void) { if ((v & 0x1f) == 31) { v &= ~0x1f; v ^= 0x400; } else v++; }
static void inc_y(void) {
    if ((v & 0x7000) != 0x7000) v += 0x1000;
    else { v &= ~0x7000; int y = (v & 0x3e0) >> 5; if (y == 29) { y = 0; v ^= 0x800; } else if (y == 31) y = 0; else y++; v = (v & ~0x3e0) | (y << 5); }
}
static void sprite_fetch_line(int line) {
    /* evaluate and fetch the sprites for 'line' (called at dot 257 of the previous line) */
    int h = (ctrl & 0x20) ? 16 : 8; nspr = 0; spr0_in_line = 0;
    for (int i = 0; i < 64 && nspr < 8; i++) {
        int y = oam[i * 4]; int row = line - y - 1; /* sprite Y is top-1 */
        if (row < 0 || row >= h) continue;
        u8 tile = oam[i * 4 + 1], attr = oam[i * 4 + 2], x = oam[i * 4 + 3];
        if (attr & 0x80) row = h - 1 - row;
        u16 addr;
        if (h == 8) addr = ((ctrl & 8) ? 0x1000 : 0) + tile * 16 + row;
        else addr = ((tile & 1) ? 0x1000 : 0) + (tile & 0xfe) * 16 + (row & 8 ? 16 : 0) + (row & 7);
        u8 lo = chr[addr & 0x1fff], hi = chr[(addr + 8) & 0x1fff];
        if (attr & 0x40) { /* flip horizontally */
            u8 l = 0, hh = 0; for (int b = 0; b < 8; b++) { l |= ((lo >> b) & 1) << (7 - b); hh |= ((hi >> b) & 1) << (7 - b); } lo = l; hi = hh;
        }
        spr[nspr].lo = lo; spr[nspr].hi = hi; spr[nspr].attr = attr; spr[nspr].x = x; spr[nspr].is0 = (i == 0); nspr++;
    }
    /* sprite overflow (hardware bug ignored: set when >8 found; Excitebike never reads bit 5) */
    for (int i = 0, n = 0; i < 64; i++) { int row = line - oam[i * 4] - 1; if (row >= 0 && row < h) n++; if (n > 8) { status |= 0x20; break; } }
}
static u16 at_sh_lo, at_sh_hi;
static void load_bg_shifters(void) {
    bg_pat_lo = (bg_pat_lo & 0xff00) | pat_lo_latch; bg_pat_hi = (bg_pat_hi & 0xff00) | pat_hi_latch;
    at_sh_lo = (at_sh_lo & 0xff00) | ((at_latch & 1) ? 0xff : 0); at_sh_hi = (at_sh_hi & 0xff00) | ((at_latch & 2) ? 0xff : 0);
}
static void ppu_dot(void) {
    int vis = scan < 240, pre = scan == 311;
    int on = rend_on();
    if (scan == 241 && dot == 1) { if (!vbl_suppress) status |= 0x80; vbl_suppress = 0; }
    if (pre && dot == 1) { status &= ~(0x80 | 0x40 | 0x20); }
    nmi_line = (status & 0x80) && (ctrl & 0x80);
    if (nmi_line && !nmi_prev) { nmi_pending = 1; nmi_edge_cycle = cpu_cycle; }
    nmi_prev = nmi_line;
    if (on && (vis || pre)) {
        if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) {
            if (mask & 8) { bg_pat_lo <<= 1; bg_pat_hi <<= 1; at_sh_lo <<= 1; at_sh_hi <<= 1; }
        }
        if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) {
            switch ((dot - 1) & 7) {
            case 0: load_bg_shifters(); nt_latch = ppu_rd(0x2000 | (v & 0xfff)); break;
            case 2: { u8 a = ppu_rd(0x23c0 | (v & 0xc00) | ((v >> 4) & 0x38) | ((v >> 2) & 7)); if (v & 0x40) a >>= 4; if (v & 2) a >>= 2; at_latch = a & 3; } break;
            case 4: pat_lo_latch = ppu_rd(((ctrl & 0x10) ? 0x1000 : 0) + nt_latch * 16 + ((v >> 12) & 7)); break;
            case 6: pat_hi_latch = ppu_rd(((ctrl & 0x10) ? 0x1000 : 0) + nt_latch * 16 + ((v >> 12) & 7) + 8); break;
            case 7: inc_x(); break;
            }
        }
        if (dot == 256) inc_y();
        if (dot == 257) { load_bg_shifters(); v = (v & ~0x41f) | (t & 0x41f); { int n = (scan + 1) % 312; if (n < 240) sprite_fetch_line(n); else nspr = 0; } }
        if (pre && dot >= 280 && dot <= 304) v = (v & ~0x7be0) | (t & 0x7be0);
    }
    if (vis && dot >= 1 && dot <= 256) {
        int x = dot - 1, col;
        if (!on) { col = ppu_rd(0x3f00); if ((v & 0x3f00) == 0x3f00) col = ppu_rd(v); }
        else {
            int px = 0, pal_i = 0;
            if ((mask & 8) && (x >= 8 || (mask & 2))) {
                int sh = 15 - finex; px = ((bg_pat_hi >> sh) & 1) << 1 | ((bg_pat_lo >> sh) & 1);
                pal_i = ((at_sh_hi >> sh) & 1) << 1 | ((at_sh_lo >> sh) & 1);
            }
            col = px ? ppu_rd(0x3f00 + pal_i * 4 + px) : ppu_rd(0x3f00);
            int bgpx = px;
            if ((mask & 0x10) && (x >= 8 || (mask & 4))) {
                for (int i = 0; i < nspr; i++) {
                    int dx = x - spr[i].x; if (dx < 0 || dx > 7) continue;
                    int sp = ((spr[i].hi >> (7 - dx)) & 1) << 1 | ((spr[i].lo >> (7 - dx)) & 1);
                    if (!sp) continue;
                    if (spr[i].is0 && bgpx && x < 255 && (mask & 8) && !(status & 0x40)) status |= 0x40;
                    if (!bgpx || !(spr[i].attr & 0x20)) col = ppu_rd(0x3f10 + (spr[i].attr & 3) * 4 + sp);
                    break;
                }
            }
        }
        if (mask & 1) col &= 0x30;
        frame_pix[scan][x] = (u8)(col & 0x3f) | ((mask >> 5) << 6);
    }
    if (++dot == 341) { dot = 0; if (++scan == 312) { scan = 0; frames++; } }
}
static u64 dump_count;
static void frame_hook(void);   /* at start of line 240 */

static void ppu_run_cycle(void) {
    ppu_units += 16;
    while (ppu_units >= 5) { ppu_units -= 5;
        if (scan == 240 && dot == 0) frame_hook();
        ppu_dot(); }
}

/* ------------------------------------------------------------------ bus */
static int dma_pending;
static void apu_write(u16 a, u8 d);
static u8 open_bus;
static int sst; static u8 sstmem[65536]; static struct { u16 a; u8 d; char rw; } sstlog[64]; static int sstn;
static void cyc(void) { cpu_cycle++; if (!sst) ppu_run_cycle(); }

static u8 bus_rd(u16 a) {
    cyc();
    u8 d;
    if (sst) { d = sstmem[a]; if (sstn < 64) { sstlog[sstn].a = a; sstlog[sstn].d = d; sstlog[sstn].rw = 0; sstn++; } return d; }
    if (a < 0x2000) d = ram[a & 0x7ff];
    else if (a < 0x4000) {
        switch (a & 7) {
        case 2: d = (status & 0xe0) | (open_bus & 0x1f); status &= ~0x80; wlatch = 0;
                if (scan == 241 && dot == 0) vbl_suppress = 1; /* read just before the flag is set */
                break;
        case 4: d = oam[oamaddr]; break;
        case 7: { u16 ad = v & 0x3fff; if (ad >= 0x3f00) { d = ppu_rd(ad); rbuf = ppu_rd(ad - 0x1000); } else { d = rbuf; rbuf = ppu_rd(ad); }
                  v += (ctrl & 4) ? 32 : 1; } break;
        default: d = open_bus; break;
        }
        open_bus = d;
    }
    else if (a == 0x4016 || a == 0x4017) { int i = a & 1; d = 0x40 | (pad_shift[i] & 1); if (!strobe) pad_shift[i] = (pad_shift[i] >> 1) | 0x80; else pad_shift[i] = pad_state[i]; }
    else if (a >= 0x4000 && a < 0x4020) d = 0;
    else if (a >= 0x8000) d = prg[a & 0x3fff];
    else d = open_bus;
    tracef("R", a, d, 0);
    return d;
}
static void bus_wr(u16 a, u8 d) {
    cyc();
    if (sst) { sstmem[a] = d; if (sstn < 64) { sstlog[sstn].a = a; sstlog[sstn].d = d; sstlog[sstn].rw = 1; sstn++; } return; }
    tracef("W", a, d, 1);
    if (a < 0x2000) ram[a & 0x7ff] = d;
    else if (a < 0x4000) {
        open_bus = d;
        switch (a & 7) {
        case 0: ctrl = d; t = (t & ~0xc00) | ((d & 3) << 10); break;
        case 1: mask = d; break;
        case 3: oamaddr = d; break;
        case 4: oam[oamaddr++] = d; break;
        case 5: if (!wlatch) { t = (t & ~0x1f) | (d >> 3); finex = d & 7; } else t = (t & ~0x73e0) | ((d & 7) << 12) | ((d >> 3) << 5); wlatch ^= 1; break;
        case 6: if (!wlatch) { t = (t & 0xff) | ((d & 0x3f) << 8); } else { t = (t & 0xff00) | d; v = t; } wlatch ^= 1; break;
        case 7: ppu_wr(v, d); v += (ctrl & 4) ? 32 : 1; break;
        }
    }
    else if (a == 0x4014) { dma_pending = 1; /* handled by cpu loop */
        u16 base = d << 8;
        cyc(); if (cpu_cycle & 1) cyc();                    /* 1 (+1 on odd) alignment cycles */
        for (int i = 0; i < 256; i++) { u8 b; { cyc(); b = base < 0x2000 ? ram[(base + i) & 0x7ff] : 0; } cyc(); oam[(oamaddr + i) & 255] = b; }
        dma_pending = 0; }
    else if (a == 0x4016) { strobe = d & 1; if (strobe) { pad_shift[0] = pad_state[0]; pad_shift[1] = pad_state[1]; } }
    else if (a >= 0x4000 && a < 0x4018) apu_write(a, d);
}
static void apu_write(u16 a, u8 d) { (void)a; (void)d; }

/* ------------------------------------------------------------------ CPU */
#define RD(a) bus_rd(a)
#define WR(a, d) bus_wr(a, d)
static inline void setnz(u8 v8) { c.p = (c.p & ~(FN | FZ)) | (v8 & 0x80) | (v8 ? 0 : FZ); }
static inline void push(u8 d) { WR(0x100 | c.s, d); c.s--; }
static inline u8 pull(void) { c.s++; return RD(0x100 | c.s); }
static u8 fetch(void) { return RD(c.pc++); }
static void adc(u8 m) { int r = c.a + m + (c.p & FC); c.p &= ~(FC | FV); if (r > 255) c.p |= FC; if (~(c.a ^ m) & (c.a ^ r) & 0x80) c.p |= FV; c.a = r; setnz(c.a); }
static void cmp(u8 r, u8 m) { int d = r - m; c.p = (c.p & ~FC) | (r >= m ? FC : 0); setnz(d & 255); }
static u8 asl(u8 m) { c.p = (c.p & ~FC) | (m >> 7); m <<= 1; setnz(m); return m; }
static u8 lsr(u8 m) { c.p = (c.p & ~FC) | (m & 1); m >>= 1; setnz(m); return m; }
static u8 rol(u8 m) { u8 o = c.p & FC; c.p = (c.p & ~FC) | (m >> 7); m = (m << 1) | o; setnz(m); return m; }
static u8 ror(u8 m) { u8 o = c.p & FC; c.p = (c.p & ~FC) | (m & 1); m = (m >> 1) | (o << 7); setnz(m); return m; }

/* effective address; wr = 1 for stores/RMW (always does the dummy read) */
static u16 ea(int mode, int wr) {
    u16 a; u8 z, lo, hi;
    switch (mode) {
    case 1: return c.pc++;                                           /* imm */
    case 2: return fetch();                                          /* zp */
    case 3: z = fetch(); RD(z); return (u8)(z + c.x);                /* zpx */
    case 4: z = fetch(); RD(z); return (u8)(z + c.y);                /* zpy */
    case 5: lo = fetch(); hi = fetch(); return lo | hi << 8;         /* abs */
    case 6: lo = fetch(); hi = fetch(); a = (lo | hi << 8) + c.x;    /* abx */
        if (wr || (a & 0xff00) != (hi << 8)) RD((hi << 8) | (u8)(lo + c.x));
        return a;
    case 7: lo = fetch(); hi = fetch(); a = (lo | hi << 8) + c.y;    /* aby */
        if (wr || (a & 0xff00) != (hi << 8)) RD((hi << 8) | (u8)(lo + c.y));
        return a;
    case 8: z = fetch(); RD(z); lo = RD((u8)(z + c.x)); hi = RD((u8)(z + c.x + 1)); return lo | hi << 8; /* izx */
    case 9: z = fetch(); lo = RD(z); hi = RD((u8)(z + 1)); a = (lo | hi << 8) + c.y;                   /* izy */
        if (wr || (a & 0xff00) != (hi << 8)) RD((hi << 8) | (u8)(lo + c.y));
        return a;
    }
    return 0;
}
static void branch(int cond) {
    int8_t o = (int8_t)fetch();
    if (cond) { RD(c.pc); u16 n = c.pc + o; if ((n & 0xff00) != (c.pc & 0xff00)) RD((c.pc & 0xff00) | (n & 0xff)); c.pc = n; }
}
static int inst_end_nmi_check;
static u64 inst_start_cycle;
static void do_interrupt(u16 vec, int brk) {
    RD(c.pc); RD(c.pc);
    push(c.pc >> 8); push(c.pc & 255); push((c.p & ~FB) | FU | (brk ? FB : 0));
    c.p |= FI; u8 lo = RD(vec), hi = RD(vec + 1); c.pc = lo | hi << 8;
}
static u8 rdv(u16 a) { return a >= 0xc000 ? prg[a & 0x3fff] : 0; }

static void cpu_step(void) {
    /* interrupt sampling: an NMI edge that occurred before the last cycle of the previous instruction is taken now */
    if (nmi_pending && nmi_edge_cycle <= cpu_cycle - 1) { nmi_pending = 0; do_interrupt(0xfffa, 0); return; }
    u8 op = fetch();
    int cc = op & 3, bbb = (op >> 2) & 7, aaa = op >> 5;
    static const int m01[8] = { 8, 2, 1, 5, 9, 3, 7, 6 };
    switch (op) {
    case 0x00: fetch(); push(c.pc >> 8); push(c.pc & 255); push(c.p | FB | FU); c.p |= FI; { u8 lo = RD(0xfffe), hi = RD(0xffff); c.pc = lo | hi << 8; } return;
    case 0x20: { u8 lo = fetch(); RD(0x100 | c.s); push(c.pc >> 8); push(c.pc & 255); u8 hi = fetch(); c.pc = lo | hi << 8; } return;
    case 0x40: RD(c.pc); RD(0x100 | c.s); c.p = (pull() & ~FB) | FU; { u8 lo = pull(); u8 hi = pull(); c.pc = lo | hi << 8; } return;
    case 0x60: RD(c.pc); RD(0x100 | c.s); { u8 lo = pull(); u8 hi = pull(); c.pc = lo | hi << 8; } RD(c.pc); c.pc++; return;
    case 0x4c: { u8 lo = fetch(); u8 hi = fetch(); c.pc = lo | hi << 8; } return;
    case 0x6c: { u8 lo = fetch(); u8 hi = fetch(); u16 p = lo | hi << 8; u8 l = RD(p), h = RD((p & 0xff00) | (u8)(p + 1)); c.pc = l | h << 8; } return;
    case 0x08: RD(c.pc); push(c.p | FB | FU); return;
    case 0x28: RD(c.pc); RD(0x100 | c.s); c.p = (pull() & ~FB) | FU; return;
    case 0x48: RD(c.pc); push(c.a); return;
    case 0x68: RD(c.pc); RD(0x100 | c.s); c.a = pull(); setnz(c.a); return;
    case 0x88: RD(c.pc); c.y--; setnz(c.y); return;
    case 0xa8: RD(c.pc); c.y = c.a; setnz(c.y); return;
    case 0xc8: RD(c.pc); c.y++; setnz(c.y); return;
    case 0xe8: RD(c.pc); c.x++; setnz(c.x); return;
    case 0x18: RD(c.pc); c.p &= ~FC; return;  case 0x38: RD(c.pc); c.p |= FC; return;
    case 0x58: RD(c.pc); c.p &= ~FI; return;  case 0x78: RD(c.pc); c.p |= FI; return;
    case 0xb8: RD(c.pc); c.p &= ~FV; return;  case 0xd8: RD(c.pc); c.p &= ~FD; return;  case 0xf8: RD(c.pc); c.p |= FD; return;
    case 0x8a: RD(c.pc); c.a = c.x; setnz(c.a); return;  case 0x98: RD(c.pc); c.a = c.y; setnz(c.a); return;
    case 0x9a: RD(c.pc); c.s = c.x; return;  case 0xaa: RD(c.pc); c.x = c.a; setnz(c.x); return;
    case 0xba: RD(c.pc); c.x = c.s; setnz(c.x); return;  case 0xca: RD(c.pc); c.x--; setnz(c.x); return;
    case 0xea: RD(c.pc); return;
    case 0x10: branch(!(c.p & FN)); return; case 0x30: branch(c.p & FN); return;
    case 0x50: branch(!(c.p & FV)); return; case 0x70: branch(c.p & FV); return;
    case 0x90: branch(!(c.p & FC)); return; case 0xb0: branch(c.p & FC); return;
    case 0xd0: branch(!(c.p & FZ)); return; case 0xf0: branch(c.p & FZ); return;
    }
    if (cc == 1) {
        if (aaa == 4) { if (bbb == 2) goto bad; u16 a = ea(m01[bbb], 1); WR(a, c.a); return; }
        if (bbb == 2 && aaa == 4) goto bad;
        u16 a = ea(m01[bbb], 0); u8 m = RD(a);
        switch (aaa) {
        case 0: c.a |= m; setnz(c.a); break; case 1: c.a &= m; setnz(c.a); break; case 2: c.a ^= m; setnz(c.a); break;
        case 3: adc(m); break; case 5: c.a = m; setnz(c.a); break; case 6: cmp(c.a, m); break; case 7: adc(m ^ 255); break;
        }
        return;
    }
    if (cc == 2) {
        if (bbb == 2) { RD(c.pc); switch (aaa) { case 0: c.a = asl(c.a); break; case 1: c.a = rol(c.a); break; case 2: c.a = lsr(c.a); break; case 3: c.a = ror(c.a); break; default: goto bad; } return; }
        int mode = bbb == 0 ? 1 : bbb == 1 ? 2 : bbb == 3 ? 5 : bbb == 5 ? ((aaa == 4 || aaa == 5) ? 4 : 3) : bbb == 7 ? (aaa == 5 ? 7 : 6) : 0;
        if (!mode || aaa == 4 && (bbb == 0 || bbb == 7) ) goto bad;
        if (aaa == 4) { u16 a = ea(mode, 1); WR(a, c.x); return; }
        if (aaa == 5) { u16 a = ea(mode, 0); c.x = RD(a); setnz(c.x); return; }
        if (bbb == 0) goto bad;
        u16 a = ea(mode, 1); u8 m = RD(a); WR(a, m);
        switch (aaa) { case 0: m = asl(m); break; case 1: m = rol(m); break; case 2: m = lsr(m); break; case 3: m = ror(m); break; case 6: m--; setnz(m); break; case 7: m++; setnz(m); break; }
        WR(a, m); return;
    }
    if (cc == 0) {
        int mode = bbb == 0 ? 1 : bbb == 1 ? 2 : bbb == 3 ? 5 : bbb == 5 ? 3 : bbb == 7 ? 6 : 0;
        if (!mode) goto bad;
        if (aaa == 1) { u16 a = ea(mode, 0); u8 m = RD(a); c.p = (c.p & ~(FN | FV | FZ)) | (m & 0xc0) | ((m & c.a) ? 0 : FZ); return; }
        if (aaa == 4) { if (mode == 1 || mode == 6) goto bad; u16 a = ea(mode, 1); WR(a, c.y); return; }
        if (aaa == 5) { u16 a = ea(mode, 0); c.y = RD(a); setnz(c.y); return; }
        if (aaa == 6 || aaa == 7) { if (mode == 3 || mode == 6) goto bad; u16 a = ea(mode, 0); u8 m = RD(a); cmp(aaa == 6 ? c.y : c.x, m); return; }
    }
bad:
    fprintf(stderr, "illegal opcode %02X at %04X\n", op, (u16)(c.pc - 1)); exit(2);
}

/* ------------------------------------------------------------------ driver / trace */
static void tracef(const char *k, u16 a, u8 val, int writing) {
    (void)writing;
    if (!tr) return;
    if (k[0] == 'W') { if (!((a >= 0x2000 && a <= 0x2007) || (a >= 0x4000 && a <= 0x4017))) return; }
    else if (!(a == 0x2002 || a == 0x4016 || a == 0x4017)) return;
    fprintf(tr, "%s %llu %llu %04X %02X\n", k, (unsigned long long)dump_count, (unsigned long long)cpu_cycle, a, val);
}
static void hexline(const char *k, u8 *p, int n) { fprintf(tr, "%s %llu ", k, (unsigned long long)dump_count); for (int i = 0; i < n; i++) fprintf(tr, "%02X", p[i]); fputc('\n', tr); }
static int  maxframes;
static void set_pad(void) {
    long f = (long)dump_count;       /* same schedule as ref/mame_trace.lua (called at frame_done after increment) */
    int s = input_frame_start; u8 b = 0;
    if (f >= s && f < s + 4) b |= 0x08;                                       /* Start */
    if (f >= s + 10 && (f % 7) != 0) b |= 0x01;                               /* A */
    if (f >= s + 300 && (f % 40) < 20) b |= 0x80;                             /* Right */
    if (f >= s + 200 && (f % 97) < 5) b |= 0x10;                              /* Up */
    if (f >= s + 260 && (f % 89) < 5) b |= 0x20;                              /* Down */
    if (f >= s + 120 && (f % 3) == 0) b |= 0x02;                              /* B */
    pad_state[0] = b;
}
static FILE *pixf;
static void frame_hook(void) {
    dump_count++;
    set_pad();
    if (pixf && ((dump_count >= 495 && dump_count <= 530) || (dump_count % 10 == 0 && dump_count >= 100))) {
        fputc(dump_count & 255, pixf); fputc(dump_count >> 8, pixf); fwrite(frame_pix, 1, sizeof frame_pix, pixf); }
    if (!tr) return;
    u8 ntdump[2048]; for (int i = 0; i < 2048; i++) ntdump[i] = nt[(i >> 10)][i & 0x3ff];
    /* MAME's videoram view of 0x2000-0x2fff with vertical mirroring: 2000=NT0, 2400=NT1, 2800=NT0, 2c00=NT1 */
    u8 full[4096]; for (int i = 0; i < 4096; i++) full[i] = *ntp(0x2000 + i);
    hexline("NT", full, 4096); hexline("PAL", pal, 32); hexline("OAMRAM", ram + 0x200, 256); hexline("RAM", ram, 2048);
    if (dump_count >= (u64)maxframes) { fclose(tr); tr = NULL; exit(0); }
}

/* ---- SingleStepTests runner: "nesref --sst tests.txt" ; file lines written by tools/sst2txt.py:
 *   T name | pc s a x y p | nin addr val ... | nout addr val ... | ncyc addr val rw ... */
static int sst_run(const char *fn) {
    FILE *f = fopen(fn, "r"); if (!f) { perror(fn); return 1; }
    char name[64]; long total = 0, fail = 0;
    sst = 1;
    unsigned pc, s, a, x, y, p, n;
    while (fscanf(f, " T %63[^|]|", name) == 1) {
        if (fscanf(f, "%u %u %u %u %u %u %u", &pc, &s, &a, &x, &y, &p, &n) != 7) return 1;
        static unsigned ia[64], iv[64]; if (n > 64) return 1;
        for (unsigned i = 0; i < n; i++) { if (fscanf(f, "%u %u", &ia[i], &iv[i]) != 2) return 1; }
        memset(sstmem, 0, sizeof sstmem); for (unsigned i = 0; i < n; i++) sstmem[ia[i]] = iv[i];
        c.pc = pc; c.s = s; c.a = a; c.x = x; c.y = y; c.p = p; sstn = 0; nmi_pending = 0;
        cpu_step();
        unsigned fpc, fs, fa, fx, fy, fp, nf; if (fscanf(f, "%u %u %u %u %u %u %u", &fpc, &fs, &fa, &fx, &fy, &fp, &nf) != 7) return 1;
        int bad = 0; char why[128] = "";
        if (c.pc != fpc) { bad = 1; sprintf(why, "pc %04x!=%04x", c.pc, fpc); }
        else if (c.s != fs || c.a != fa || c.x != fx || c.y != fy) { bad = 1; sprintf(why, "regs s%02x a%02x x%02x y%02x vs s%02x a%02x x%02x y%02x", c.s, c.a, c.x, c.y, fs, fa, fx, fy); }
        else if ((c.p | 0x30) != (fp | 0x30)) { bad = 1; sprintf(why, "p %02x!=%02x", c.p, fp); }
        for (unsigned i = 0; i < nf; i++) { unsigned aa, vv; if (fscanf(f, "%u %u", &aa, &vv) != 2) return 1; if (!bad && sstmem[aa] != vv) { bad = 1; sprintf(why, "mem[%04x]=%02x!=%02x", aa, sstmem[aa], vv); } }
        unsigned nc; if (fscanf(f, "%u", &nc) != 1) return 1;
        if (!bad && (int)nc != sstn) { bad = 1; sprintf(why, "cycles %d!=%u", sstn, nc); }
        for (unsigned i = 0; i < nc; i++) { unsigned aa, vv, rw; if (fscanf(f, "%u %u %u", &aa, &vv, &rw) != 3) return 1;
            if (!bad && i < (unsigned)sstn && (sstlog[i].a != aa || sstlog[i].d != vv || sstlog[i].rw != (int)rw)) { bad = 1; sprintf(why, "cycle %u: %04x %02x %d vs %04x %02x %u", i, sstlog[i].a, sstlog[i].d, sstlog[i].rw, aa, vv, rw); } }
        total++; if (bad) { if (fail < 5) printf("FAIL %s: %s\n", name, why); fail++; }
    }
    printf("%s: %ld tests, %ld failures\n", fn, total, fail); fclose(f); return fail != 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--sst")) return sst_run(argv[2]);
    if (argc < 4) { fprintf(stderr, "usage: nesref rom.nes frames start_frame [trace]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("rom"); return 1; }
    u8 hdr[16]; if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "NES\x1a", 4)) { fprintf(stderr, "bad iNES\n"); return 1; }
    if (hdr[4] != 1 || hdr[5] != 1 || (hdr[6] >> 4)) { fprintf(stderr, "only NROM-128\n"); return 1; }
    vmirror = hdr[6] & 1;
    if (fread(prg, 1, 16384, f) != 16384 || fread(chr, 1, 8192, f) != 8192) return 1; fclose(f);
    maxframes = atoi(argv[2]); input_frame_start = atoi(argv[3]);
    if (argc > 4) tr = fopen(argv[4], "w");
    if (getenv("PIX_OUT")) pixf = fopen(getenv("PIX_OUT"), "wb");
    for (int i = 0; i < 2048; i++) ram[i] = (i & 1) ? 0xff : 0x00;                                /* MAME NES RAM power-on pattern is checked in verification */
    c.s = 0xfd; c.p = FI | FU;
    /* reset sequence: 7 cycles; MAME logs the first write at cycle 16 */
    { const char *e = getenv("PPU_PHASE"); if (e) { int p = atoi(e); ppu_units = p % 5; dot = p / 5; } }
    for (int i = 0; i < 5; i++) cyc();
    { u8 lo = rdv(0xfffc), hi = rdv(0xfffd); c.pc = lo | hi << 8; cyc(); cyc(); }
    for (;;) cpu_step();
    return 0;
}
