/* nes_rt.c - target independent runtime of the NES/PAL board for the translated Excitebike code.
 * No Amiga hardware names in here.  Port of the models in ref/nesref.c into a lazy form:
 *   - every CPU access to $2000-$401F arrives with its exact CPU cycle ("stamp"),
 *   - the PPU is advanced to that cycle on demand (ppu_run_to), line segment by line segment, which also renders the picture,
 *   - frame hooks (line 240) and NMI edges are scheduled through rt.ev_due, which the translated main-thread code polls.
 * Time: CPU cycle c has processed floor(16*c/5) PPU dots (3.2 dots per CPU cycle, as in the reference).
 * Platform hooks (weak): plat_frame(), plat_trace().
 */
#include <stdint.h>
#include <string.h>
#include "rt.h"

struct RT rt;
u8 ram[2048];
const u8 *prg;                 /* 16 KB at $C000 (mirrored at $8000) */
const u8 *chr;                 /* 8 KB */
u8 nt[2][1024], pal[32], oam[256];
u8 frame_pix[240][256];        /* palette index (bits 0-5) + emphasis (bits 6-7) per pixel, completed at line 240 */

#define FD 106392u             /* dots per frame: 312 * 341 */
#define EPOCH_CYC 66495u       /* two frames = 66495 CPU cycles = 212784 dots exactly: time is kept relative to an epoch that is re-based in c_event */
#define EPOCH_DOTS 212784u
static u64 epoch_cycles;       /* cycles before the current epoch (for traces only) */
static u8 ctrl, mask, status, oamaddr, rbuf, wlatch, finex, open_bus;
static u16 v, t;
static u32 dots_done;
static u32 frame_no;
static int vbl_suppress;
static int nmi_line_prev;
static u8 pad_state[2], pad_shift[2], strobe;
static u32 last_pad_frame = ~0u;

__attribute__((weak)) void plat_frame(u32 frame) { (void)frame; }
__attribute__((weak)) void plat_trace(char kind, u32 frame, u32 cyc, u16 addr, u8 val) { (void)kind; (void)frame; (void)cyc; (void)addr; (void)val; }
__attribute__((weak)) void plat_pad(u32 frame) { (void)frame; }

static inline u32 processed(u32 c) { return (c * 16u) / 5u; }
static inline u32 cycle_of_dot(u32 d) { return (5u * (d + 1u) + 15u) >> 4; }       /* cycle in which dot d is processed */

static inline u8 *ntp(u16 a) { return &nt[(a >> 10) & 1][a & 0x3ff]; }                /* vertical mirroring */
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

/* ------------------------------------------------------------------ PPU line state */
static u16 line_hh, line_vv;              /* horizontal / vertical bits of v for the line being drawn (tile 0 coordinates) */
static struct { u8 id, attr, lo, hi; } tile[40];
static struct { u8 lo, hi, attr, x; int is0; } spr[8]; static int nspr;

static inline int rend_on(void) { return mask & 0x18; }
static void inc_y(void) {
    if ((v & 0x7000) != 0x7000) v += 0x1000;
    else { v &= ~0x7000; int y = (v & 0x3e0) >> 5; if (y == 29) { y = 0; v ^= 0x800; } else if (y == 31) y = 0; else y++; v = (v & ~0x3e0) | (y << 5); }
}
static void inc_x(void) { if ((v & 0x1f) == 31) { v &= ~0x1f; v ^= 0x400; } else v++; }

static void sprite_fetch_line(int line) {
    int h = (ctrl & 0x20) ? 16 : 8; nspr = 0;
    for (int i = 0; i < 64 && nspr < 8; i++) {
        int y = oam[i * 4]; int row = line - y - 1;
        if (row < 0 || row >= h) continue;
        u8 tid = oam[i * 4 + 1], attr = oam[i * 4 + 2], x = oam[i * 4 + 3];
        if (attr & 0x80) row = h - 1 - row;
        u16 addr;
        if (h == 8) addr = ((ctrl & 8) ? 0x1000 : 0) + tid * 16 + row;
        else addr = ((tid & 1) ? 0x1000 : 0) + (tid & 0xfe) * 16 + (row & 8 ? 16 : 0) + (row & 7);
        u8 lo = chr[addr & 0x1fff], hi = chr[(addr + 8) & 0x1fff];
        if (attr & 0x40) { u8 l = 0, hh = 0; for (int b = 0; b < 8; b++) { l |= ((lo >> b) & 1) << (7 - b); hh |= ((hi >> b) & 1) << (7 - b); } lo = l; hi = hh; }
        spr[nspr].lo = lo; spr[nspr].hi = hi; spr[nspr].attr = attr; spr[nspr].x = x; spr[nspr].is0 = (i == 0); nspr++;
    }
    for (int i = 0, n = 0; i < 64; i++) { int row = line - oam[i * 4] - 1; if (row >= 0 && row < h) n++; if (n > 8) { status |= 0x20; break; } }
}
/* fetch stream tile m for the line (id+attr from the nametable, pattern bytes with the ctrl of the fetch time) */
static void tile_nt(int m) {
    int coarse = (line_hh & 31) + m; int ntx = ((line_hh >> 10) & 1) ^ ((coarse >> 5) & 1); coarse &= 31;
    int cy = (line_vv >> 5) & 31, nty = (line_vv >> 11) & 1;
    u16 a = 0x2000 | (nty << 11) | (ntx << 10) | (cy << 5) | coarse;
    tile[m].id = ppu_rd(a);
    u8 at = ppu_rd(0x23c0 | (nty << 11) | (ntx << 10) | ((cy >> 2) << 3) | (coarse >> 2));
    if (cy & 2) at >>= 4; if (coarse & 2) at >>= 2;
    tile[m].attr = at & 3;
}
static inline void tile_lo(int m) { tile[m].lo = ppu_rd(((ctrl & 0x10) ? 0x1000 : 0) + tile[m].id * 16 + ((line_vv >> 12) & 7)); }
static inline void tile_hi(int m) { tile[m].hi = ppu_rd(((ctrl & 0x10) ? 0x1000 : 0) + tile[m].id * 16 + ((line_vv >> 12) & 7) + 8); }

/* dots [d0,d1) of a rendering line (visible or pre-render) with constant register state */
static void line_segment(int scan, int d0, int d1) {
    int vis = scan < 240, pre = scan == 311;
    int on = rend_on();
    /* the dots of this call are handled in dot order:  1..256 (tile fetch + pixels) | 256 y step | 257 x copy, sprites | 304 y copy | 321..336 prefetch */
    if (on && (vis || pre)) {
        for (int dot = d0 < 1 ? 1 : d0; dot < d1 && dot <= 256; dot++) {
            int g = (dot - 1) >> 3, ph = (dot - 1) & 7, m = g + 2;
            if (ph == 0) tile_nt(m); else if (ph == 4) tile_lo(m); else if (ph == 6) tile_hi(m);
        }
    }
    if (vis) {
        int xa = d0 < 1 ? 0 : d0 - 1, xb = d1 > 257 ? 256 : d1 - 1;
        for (int x = xa; x < xb; x++) {
            int col;
            if (!on) { col = ppu_rd(0x3f00); if ((v & 0x3f00) == 0x3f00) col = ppu_rd(v); }
            else {
                int px = 0, pal_i = 0;
                if ((mask & 8) && (x >= 8 || (mask & 2))) {
                    int sx = x + finex, m = sx >> 3, fx = sx & 7;
                    px = (((tile[m].hi >> (7 - fx)) & 1) << 1) | ((tile[m].lo >> (7 - fx)) & 1); pal_i = tile[m].attr;
                }
                col = px ? ppu_rd(0x3f00 + pal_i * 4 + px) : ppu_rd(0x3f00);
                int bgpx = px;
                if ((mask & 0x10) && (x >= 8 || (mask & 4))) {
                    for (int i = 0; i < nspr; i++) {
                        int dx = x - spr[i].x; if (dx < 0 || dx > 7) continue;
                        int sp = (((spr[i].hi >> (7 - dx)) & 1) << 1) | ((spr[i].lo >> (7 - dx)) & 1);
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
    }
    if (on && (vis || pre)) {
        if (d0 <= 256 && 256 < d1) inc_y();
        if (d0 <= 257 && 257 < d1) {
            v = (v & ~0x41f) | (t & 0x41f);
            int n = (scan + 1) % 312;
            if (n < 240) sprite_fetch_line(n); else nspr = 0;
        }
        if (pre && d0 <= 304 && 304 < d1) v = (v & ~0x7be0) | (t & 0x7be0);   /* the ref copies at every dot 280..304; the last one wins */
        for (int dot = d0 < 321 ? 321 : d0; dot < d1 && dot <= 336; dot++) {       /* tiles 0,1 of the next line, coordinates of v after the copies above */
            int g = (dot - 321) >> 3, ph = (dot - 321) & 7;
            if (dot == 321) { line_hh = v & 0x41f; line_vv = v & 0x7be0; }
            if (ph == 0) tile_nt(g); else if (ph == 4) tile_lo(g); else if (ph == 6) tile_hi(g);
        }
    }
}
static void nmi_eval(u32 cyc);
static int dump_pending;
static void frame_hook(void) {
    frame_no++;
    plat_pad(frame_no);                 /* input is sampled at the frame boundary (as in the reference) */
    dump_pending = 1;                   /* the frame dump is made at the next instruction boundary */
}
/* advance the PPU to P dots processed */
static void ppu_run_to(u32 P) {
    while (dots_done < P) {
        u32 fpos = dots_done % FD; int scan = (int)(fpos / 341), dot = (int)(fpos % 341);
        u32 line_start = dots_done - dot;
        u32 stop = line_start + 341; if (stop > P) stop = P;
        int d1 = (int)(stop - line_start);
        if (scan == 240 && dot == 0) { frame_hook(); }
        /* events with fixed dot positions that affect status flags */
        if (scan == 241 && dot <= 1 && 1 < d1) { if (!vbl_suppress) status |= 0x80; vbl_suppress = 0; nmi_eval(cycle_of_dot(line_start + 1)); }
        if (scan == 311 && dot <= 1 && 1 < d1) { status &= ~(0x80 | 0x40 | 0x20); nmi_eval(cycle_of_dot(line_start + 1)); }
        if (scan < 240 || scan == 311) line_segment(scan, dot, d1);
        dots_done = stop;
    }
}

/* ------------------------------------------------------------------ NMI / event scheduling */
static int nmi_pending; static u32 nmi_edge;
static void nmi_eval(u32 cyc) {
    int line = (status & 0x80) && (ctrl & 0x80);
    if (line && !nmi_line_prev) { nmi_pending = 1; nmi_edge = cyc; }
    nmi_line_prev = line;
}
static void reschedule(void) {
    u32 due = 0x7fffffff;
    u32 fi = dots_done / FD;
    u32 hk = fi * FD + 240 * 341; if (hk < dots_done) hk += FD;
    u32 vb = fi * FD + 241 * 341 + 1; if (vb < dots_done) vb += FD;
    u32 c;
    c = cycle_of_dot(hk); if (c < due) due = c;
    c = cycle_of_dot(vb); if (c < due) due = c;
    if (nmi_pending && nmi_edge + 1 < due) due = nmi_edge + 1;
    if (dump_pending) due = 0;
    rt.ev_due = due;
}
/* called from the poll stub: bring the model to 'now'; return 1 when an NMI is to be taken at this instruction boundary */
u32 c_event(u32 now) {
    rt.rebase = 0;
    ppu_run_to(processed(now));
    if (dump_pending) { dump_pending = 0; plat_frame(frame_no); }
    while (now >= 2 * EPOCH_CYC) {                      /* re-base the time scale: the translated code subtracts rt.rebase from its cycle counter */
        now -= EPOCH_CYC; rt.rebase += EPOCH_CYC; epoch_cycles += EPOCH_CYC;
        dots_done -= EPOCH_DOTS; if (nmi_pending) nmi_edge -= EPOCH_CYC;
    }
    reschedule();
    return nmi_pending && nmi_edge + 1 <= now;
}
u64 c_epoch(void) { return epoch_cycles; }
void c_nmi_taken(void) { nmi_pending = 0; reschedule(); }

/* ------------------------------------------------------------------ bus */
static void io_sync(u32 stamp) { ppu_run_to(processed(stamp)); }

u32 c_rd(u32 stamp, u32 addr) {
    u8 d = 0;
    io_sync(stamp);
    addr &= 0xffff;
    if (addr < 0x2000) d = ram[addr & 0x7ff];
    else if (addr < 0x4000) {
        switch (addr & 7) {
        case 2: d = (status & 0xe0) | (open_bus & 0x1f); status &= ~0x80; wlatch = 0;
                if ((dots_done % FD) == 241 * 341) vbl_suppress = 1;
                nmi_eval(stamp + 1);
                break;
        case 4: d = oam[oamaddr]; break;
        case 7: { u16 ad = v & 0x3fff; if (ad >= 0x3f00) { d = ppu_rd(ad); rbuf = ppu_rd(ad - 0x1000); } else { d = rbuf; rbuf = ppu_rd(ad); } v += (ctrl & 4) ? 32 : 1; } break;
        default: d = open_bus; break;
        }
        open_bus = d;
    }
    else if (addr == 0x4016 || addr == 0x4017) {
        int i = addr & 1; d = 0x40 | (pad_shift[i] & 1);
        if (!strobe) pad_shift[i] = (pad_shift[i] >> 1) | 0x80; else pad_shift[i] = pad_state[i];
    }
    else if (addr >= 0x4000 && addr < 0x4020) d = 0;
    else if (addr >= 0x8000) d = prg[addr & 0x3fff];
    else d = open_bus;
    if (addr == 0x2002 || addr == 0x4016 || addr == 0x4017) plat_trace('R', frame_no, stamp, addr, d);
    reschedule();
    return d;
}
/* returns extra CPU cycles consumed (OAM DMA) */
u32 c_wr(u32 stamp, u32 addr, u32 val) {
    u8 d = (u8)val; u32 extra = 0;
    io_sync(stamp);
    addr &= 0xffff;
    if ((addr >= 0x2000 && addr <= 0x2007) || (addr >= 0x4000 && addr <= 0x4017)) plat_trace('W', frame_no, stamp, addr, d);
    if (addr < 0x2000) ram[addr & 0x7ff] = d;
    else if (addr < 0x4000) {
        open_bus = d;
        switch (addr & 7) {
        case 0: ctrl = d; t = (t & ~0xc00) | ((d & 3) << 10); nmi_eval(stamp + 1); break;
        case 1: mask = d; break;
        case 3: oamaddr = d; break;
        case 4: oam[oamaddr++] = d; break;
        case 5: if (!wlatch) { t = (t & ~0x1f) | (d >> 3); finex = d & 7; } else t = (t & ~0x73e0) | ((d & 7) << 12) | ((d >> 3) << 5); wlatch ^= 1; break;
        case 6: if (!wlatch) { t = (t & 0xff) | ((d & 0x3f) << 8); } else { t = (t & 0xff00) | d; v = t; } wlatch ^= 1; break;
        case 7: ppu_wr(v, d); v += (ctrl & 4) ? 32 : 1; break;
        }
    }
    else if (addr == 0x4014) {
        u16 base = (u16)(d << 8);
        u32 c = stamp + 1; extra = 1; if ((c + (u32)epoch_cycles) & 1) { c++; extra++; }
        for (int i = 0; i < 256; i++) oam[(oamaddr + i) & 255] = base < 0x2000 ? ram[(base + i) & 0x7ff] : 0;
        extra += 512;
    }
    else if (addr == 0x4016) { strobe = d & 1; if (strobe) { pad_shift[0] = pad_state[0]; pad_shift[1] = pad_state[1]; } }
    reschedule();
    return extra;
}
void c_set_pad(u8 p) { pad_state[0] = p; }
void c_reset(void) {
    for (int i = 0; i < 2048; i++) ram[i] = (i & 1) ? 0xff : 0x00;
    memset(nt, 0, sizeof nt); memset(pal, 0, sizeof pal); memset(oam, 0, sizeof oam);
    ctrl = mask = status = oamaddr = rbuf = wlatch = finex = open_bus = 0; v = t = 0; dots_done = 0; frame_no = 0; nmi_pending = 0; nmi_line_prev = 0;
    reschedule();
}
u32 c_frame_no(void) { return frame_no; }
