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
static u32 frame_no; static int due_dirty = 1;
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
static void fr_nt_write(u16 a);
static void flush_pending(void);      /* render the logged line segments (the state they were logged with is still in place) */
static u8 ppu_rd(u16 a) {
    a &= 0x3fff;
    if (a < 0x2000) return chr[a];
    if (a < 0x3f00) return *ntp(a);
    a &= 0x1f; if ((a & 0x13) == 0x10) a &= 0x0f; return pal[a];
}
static void ppu_wr(u16 a, u8 d) {
    a &= 0x3fff;
    if (a < 0x2000) return;
    flush_pending();
    if (a < 0x3f00) { *ntp(a) = d; fr_nt_write(a); return; }
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
static void draw_segment(int scan, int d0, int d1) {
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

/* ------------------------------------------------------------------ deferred drawing
 * The live model (timing, status flags, v evolution, sprite-0 hit) runs at every sync; the pixels are produced later by replaying the
 * logged segments through draw_segment() (flush at the frame hook, and before any write that changes what drawing reads: NT, palette, OAM). */
struct Seg { u16 d0, d1, scan, v, t; u8 finex, ctrl, mask; u16 nl; };   /* nl > 0: a run of nl complete lines starting at 'scan' (no register access inside) */
#define SEGMAX 4096
static struct Seg seglog[SEGMAX]; static int nseg;
static u32 live_hit_dot = ~0u, replay_hit_dot = ~0u;
static u16 lv_hh, lv_vv;
u32 stat_flushes, stat_hit_mismatch, stat_fr_bad, stat_segs, stat_syncs; u8 fr_enabled = 1;
__attribute__((weak)) void plat_fr_report(u32 frame, int bad, int first) { (void)frame; (void)bad; (void)first; }

static void render_log(int at_hook);
static u8 fr_midflush;
static void midflush_check(void) { if (nseg) fr_midflush = 1; }
u8 rt_skip_next;                      /* set by the platform: do not render the frame that is about to finish (the game state is unaffected) */
u8 rt_frame_fast;                      /* 2: frame skipped, 1: the finished frame is described by fr_bands (hardware scroll display), 0: frame_pix is valid */
#include "fastr.c"
#include "apu.c"
__attribute__((weak)) void plat_apu(const struct ApuOut *o, u32 frame) { (void)o; (void)frame; }
static void render_slow(void);
static void render_log(int at_hook) {
    if (at_hook && rt_skip_next && !fr_midflush) { nseg = 0; rt_frame_fast = 2; return; }
    if (!nseg) return;
    stat_segs += nseg;
    if (at_hook) {
        int ok = 0;
        if (rt_skip_next && !fr_midflush) { nseg = 0; rt_frame_fast = 2; stat_flushes++; return; }
        if (!fr_midflush && fr_enabled) ok = fr_render(seglog, nseg);
        fr_midflush = 0;
        rt_frame_fast = (u8)ok;
#ifndef FR_VERIFY
        if (ok) { nseg = 0; stat_flushes++; return; }
#endif
        render_slow();
#ifdef FR_VERIFY
        if (ok) { static u8 rec[240][256]; fr_reconstruct(rec); int bad = 0, fl = -1; for (int l = 0; l < 240; l++) for (int x = 0; x < 256; x++) if (rec[l][x] != (frame_pix[l][x] & 0x3f)) { if (fl < 0) fl = l * 256 + x; bad++; } if (bad) { stat_fr_bad++; if (stat_fr_bad < 4) plat_fr_report(frame_no, bad, fl); } }
#endif
        return;
    }
    render_slow();
}
static void render_slow(void) {
    u16 sv = v, st = t; u8 sf = finex, sc = ctrl, sm = mask, ss = status;
    for (int i = 0; i < nseg; i++) {
        const struct Seg *r = &seglog[i];
        v = r->v; t = r->t; finex = r->finex; ctrl = r->ctrl; mask = r->mask; status = 0;
        if (r->nl) { for (int k = 0; k < r->nl; k++) draw_segment(r->scan + k, 0, 341); }
        else draw_segment(r->scan, r->d0, r->d1);
        if ((status & 0x40) && replay_hit_dot == ~0u) replay_hit_dot = 0x40000000u + r->scan * 341u + r->d0;   /* only used as "a hit was found" marker */
    }
    v = sv; t = st; finex = sf; ctrl = sc; mask = sm; status = ss;
    nseg = 0; stat_flushes++;
}
/* does sprite 0 cover this line?  returns its row bits */
static int spr0_row(int line, u8 *lo, u8 *hi) {
    int h = (ctrl & 0x20) ? 16 : 8; int row = line - oam[0] - 1;
    if (row < 0 || row >= h) return 0;
    u8 tid = oam[1], attr = oam[2];
    if (attr & 0x80) row = h - 1 - row;
    u16 addr;
    if (h == 8) addr = ((ctrl & 8) ? 0x1000 : 0) + tid * 16 + row;
    else addr = ((tid & 1) ? 0x1000 : 0) + (tid & 0xfe) * 16 + (row & 8 ? 16 : 0) + (row & 7);
    u8 l = chr[addr & 0x1fff], hh = chr[(addr + 8) & 0x1fff];
    if (attr & 0x40) { u8 a = 0, b = 0; for (int i = 0; i < 8; i++) { a |= ((l >> i) & 1) << (7 - i); b |= ((hh >> i) & 1) << (7 - i); } l = a; hh = b; }
    *lo = l; *hi = hh; return 1;
}
static inline int bgpx_live(int x) {
    int sx = x + finex, m = sx >> 3, fx = sx & 7;
    int coarse = (lv_hh & 31) + m; int ntx = ((lv_hh >> 10) & 1) ^ ((coarse >> 5) & 1); coarse &= 31;
    int cy = (lv_vv >> 5) & 31, nty = (lv_vv >> 11) & 1;
    u8 id = ppu_rd(0x2000 | (nty << 11) | (ntx << 10) | (cy << 5) | coarse);
    u16 a = ((ctrl & 0x10) ? 0x1000 : 0) + id * 16 + ((lv_vv >> 12) & 7);
    return (((chr[(a + 8) & 0x1fff] >> (7 - fx)) & 1) << 1) | ((chr[a & 0x1fff] >> (7 - fx)) & 1);
}
static int hit_x_found;
static void hit_scan(int scan, int d0, int d1, int commit) {
    hit_x_found = -1;
    if ((unsigned)(scan - oam[0] - 1) >= (unsigned)((ctrl & 0x20) ? 16 : 8)) return;      /* sprite 0 is not on this line */
    if ((mask & 0x18) != 0x18 || (status & 0x40)) return;
    u8 lo, hi; if (!spr0_row(scan, &lo, &hi)) return;
    int xa = d0 < 1 ? 0 : d0 - 1, xb = d1 > 257 ? 256 : d1 - 1; int sx = oam[3];
    if (xa < sx) xa = sx; if (xb > sx + 8) xb = sx + 8;
    for (int x = xa; x < xb; x++) {
        if (x >= 255) break;
        if (x < 8 && !((mask & 4) && (mask & 2))) continue;
        int dx = x - sx; if (!(((hi >> (7 - dx)) & 1) | ((lo >> (7 - dx)) & 1))) continue;
        if (bgpx_live(x)) { hit_x_found = x; if (commit) { status |= 0x40; live_hit_dot = (u32)(scan * 341 + x + 1); } return; }
    }
}
static u8 oam_cnt[240]; static int oam_cnt_valid, oam_cnt_h, oam_ovf_line = 9999;       /* number of sprites on each line (sprite overflow flag), recomputed after OAM or the sprite size changed */
static void oam_cnt_build(int h) {
    int vis = 0; for (int i = 0; i < 64; i++) if (oam[i * 4] < 239) vis++;
    if (vis <= 8) { memset(oam_cnt, 0, sizeof oam_cnt); oam_ovf_line = 9999; oam_cnt_valid = 1; oam_cnt_h = h; return; }       /* at most 8 sprites: no line can overflow */
    static s16 diff[250 + 20]; memset(diff, 0, sizeof diff);
    for (int i = 0; i < 64; i++) { int y = oam[i * 4]; if (y < 239) { diff[y + 1]++; diff[y + 1 + h]--; } }
    int acc = 0; oam_ovf_line = 9999; for (int l = 0; l < 240; l++) { acc += diff[l]; oam_cnt[l] = (u8)acc; if (acc > 8 && oam_ovf_line == 9999) oam_ovf_line = l; }
    oam_cnt_valid = 1; oam_cnt_h = h;
}
static void live_segment(int scan, int d0, int d1) {
    int vis = scan < 240, pre = scan == 311, on = rend_on();
    if (rt_skip_next) goto nolog;
    if (nseg == SEGMAX) { fr_midflush = 1; render_log(0); }
    { struct Seg *r = &seglog[nseg++]; r->d0 = d0; r->d1 = d1; r->scan = scan; r->v = v; r->t = t; r->finex = finex; r->ctrl = ctrl; r->mask = mask; r->nl = 0; }
    nolog:
    if (!(on && (vis || pre))) return;
    if (vis && d1 > 1 && (unsigned)(scan - oam[0] - 1) < ((ctrl & 0x20) ? 16u : 8u)) hit_scan(scan, d0, d1, 1);
    if (d0 <= 256 && 256 < d1) inc_y();
    if (d0 <= 257 && 257 < d1) {
        v = (v & ~0x41f) | (t & 0x41f);
        int n = (scan + 1) % 312;
        if (n < 240) { int h = (ctrl & 0x20) ? 16 : 8; if (!oam_cnt_valid || oam_cnt_h != h) oam_cnt_build(h); if (oam_cnt[n] > 8) status |= 0x20; }
    }
    if (pre && d0 <= 304 && 304 < d1) v = (v & ~0x7be0) | (t & 0x7be0);
    if (d0 <= 321 && 321 < d1) { lv_hh = v & 0x41f; lv_vv = v & 0x7be0; }
}

static void flush_pending(void) { midflush_check(); render_log(0); }
/* n complete visible lines starting at scan0 without any register access: the same events as n calls of live_segment(line, 0, 341) */
static inline u16 vinc_y(u16 vv) {
    if ((vv & 0x7000) != 0x7000) return vv + 0x1000;
    vv &= ~0x7000; int y = (vv & 0x3e0) >> 5;
    if (y == 29) { y = 0; vv ^= 0x800; } else if (y == 31) y = 0; else y++;
    return (u16)((vv & ~0x3e0) | (y << 5));
}
/* vertical part of v after k line increments (coarse y wraps 29 -> 0 with the nametable bit flipped) */
static u16 vadvance(u16 vl, int k) {
    int cy = (vl >> 5) & 31, fy = (vl >> 12) & 7;
    if (cy >= 30) { while (k--) vl = vinc_y(vl); return vl; }
    int tot = cy * 8 + fy + k, wraps = tot / 240; tot -= wraps * 240; if (tot >= 240) tot -= 240;
    cy = tot >> 3; fy = tot & 7;
    vl = (u16)((vl & ~0x73e0) | (fy << 12) | (cy << 5));
    if (wraps & 1) vl ^= 0x800;
    return vl;
}
static void live_lines(int scan0, int n) {
    if (!rt_skip_next) {
    if (nseg == SEGMAX) { fr_midflush = 1; render_log(0); }
    { struct Seg *r = &seglog[nseg++]; r->d0 = 0; r->d1 = 341; r->scan = scan0; r->v = v; r->t = t; r->finex = finex; r->ctrl = ctrl; r->mask = mask; r->nl = n; }
    }
    if (!rend_on()) return;
    int h = (ctrl & 0x20) ? 16 : 8; if (!oam_cnt_valid || oam_cnt_h != h) oam_cnt_build(h);
    u16 v0 = v;
    int hit_possible = ((mask & 0x18) == 0x18) && !(status & 0x40);
    int s0 = (int)oam[0] + 1;                                         /* first line covered by sprite 0 */
    if (hit_possible && s0 < scan0 + n && s0 + h > scan0) {          /* only the few lines of sprite 0 need the line by line treatment */
        int k0 = s0 > scan0 ? s0 - scan0 : 0, k1 = (s0 + h < scan0 + n ? s0 + h : scan0 + n) - scan0;
        for (int k = k0; k < k1 && hit_possible; k++) {
            u16 vl = vadvance(v0, k); if (k) vl = (u16)((vl & ~0x41f) | (t & 0x41f));
            lv_hh = vl & 0x41f; lv_vv = vl & 0x7be0; v = vl; hit_scan(scan0 + k, 0, 341, 1); if (status & 0x40) hit_possible = 0;
        }
    }
    if (oam_ovf_line <= scan0 + n && oam_ovf_line > scan0) status |= 0x20;   /* sprite overflow is flagged at dot 257 of the previous line */
    u16 vl = vadvance(v0, n); vl = (u16)((vl & ~0x41f) | (t & 0x41f));
    v = vl; lv_hh = vl & 0x41f; lv_vv = vl & 0x7be0;
}
static void nmi_eval(u32 cyc);
static int dump_pending;
static void frame_hook(void) {
    frame_no++;
    plat_pad(frame_no);                 /* input is sampled at the frame boundary (as in the reference) */
    { struct ApuOut ao; apu_run_to(cycle_of_dot(dots_done), &ao); plat_apu(&ao, frame_no); }
    dump_pending = 1; due_dirty = 1;     /* the frame dump is made at the next instruction boundary */
}
/* advance the PPU to P dots processed */
static u32 fbase;                 /* dots_done at the start of the current frame */
static int p_scan, p_dot;         /* position of dots_done inside the frame */
static void set_pos_from(u32 d) { u32 o = d - fbase; p_scan = (int)(((u64)o * 98401u) >> 25); p_dot = (int)(o - (u32)p_scan * 341); }
static void ppu_run_to(u32 P) {
    while (dots_done < P) {
        int scan = p_scan, dot = p_dot;
        if (scan == 240 && dot == 0) { render_log(1); live_hit_dot = ~0u; replay_hit_dot = ~0u; frame_hook(); }
        if ((scan >= 242 && scan < 311) || (scan == 241 && dot >= 2)) {                  /* quiet part of the vertical blank: nothing happens until the pre-render line */
            u32 target = fbase + 311 * 341; if (target > P) target = P;
            dots_done = target; set_pos_from(target); continue;
        }
        if (dot == 0 && scan < 239 && P - dots_done >= 2 * 341) {                      /* whole lines in one go */
            u32 avail = (P - dots_done) / 341; int n = (int)(avail < (u32)(240 - scan) ? avail : (u32)(240 - scan));
            live_lines(scan, n); dots_done += 341u * (u32)n; p_scan += n; continue;
        }
        int d1 = dot + (int)(P - dots_done); if (d1 > 341) d1 = 341;
        u32 stop = dots_done + (u32)(d1 - dot), line_start = dots_done - dot;
        if (scan == 241 && dot <= 1 && 1 < d1) { if (!vbl_suppress) status |= 0x80; vbl_suppress = 0; nmi_eval(cycle_of_dot(line_start + 1)); }
        if (scan == 311 && dot <= 1 && 1 < d1) { status &= ~(0x80 | 0x40 | 0x20); nmi_eval(cycle_of_dot(line_start + 1)); }
        if (scan < 240 || scan == 311) live_segment(scan, dot, d1);
        dots_done = stop;
        if (d1 == 341) { p_dot = 0; if (++p_scan == 312) { p_scan = 0; fbase += FD; } } else p_dot = d1;
    }
}

/* ------------------------------------------------------------------ NMI / event scheduling */
static int nmi_pending; static u32 nmi_edge;
static void nmi_eval(u32 cyc) {
    int line = (status & 0x80) && (ctrl & 0x80);
    if (line && !nmi_line_prev) { nmi_pending = 1; nmi_edge = cyc; due_dirty = 1; }
    nmi_line_prev = line;
}
static void reschedule(void) {
    due_dirty = 0;
    u32 due = 0x7fffffff;
    u32 hk = fbase + 240 * 341; if (hk < dots_done) hk += FD;
    u32 vb = fbase + 241 * 341 + 1; if (vb < dots_done) vb += FD;
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
        apu_rebase(EPOCH_CYC); dots_done -= EPOCH_DOTS; fbase -= EPOCH_DOTS; if (nmi_pending) nmi_edge -= EPOCH_CYC;
    }
    reschedule();
    return nmi_pending && nmi_edge + 1 <= now;
}
u64 c_epoch(void) { return epoch_cycles; }
void c_nmi_taken(void) { nmi_pending = 0; reschedule(); }

/* ------------------------------------------------------------------ bus */
static void io_sync(u32 stamp) { stat_syncs++; ppu_run_to(processed(stamp)); }

u32 stat_rd[8], stat_wr[8];
u32 c_rd(u32 stamp, u32 addr) {
    u8 d = 0; stat_rd[addr < 0x2000 ? 0 : addr < 0x4000 ? 1 + (addr & 7) % 7 : 7]++;
#ifdef RT_TRACE
    io_sync(stamp);
#else
    if (!((addr & 0xfffe) == 0x4016) && addr >= 0x2000) io_sync(stamp);       /* controller reads do not depend on the PPU: no need to advance it */
#endif
    addr &= 0xffff;
    if (addr < 0x2000) d = ram[addr & 0x7ff];
    else if (addr < 0x4000) {
        switch (addr & 7) {
        case 2: d = (status & 0xe0) | (open_bus & 0x1f); status &= ~0x80; wlatch = 0;
                if (dots_done - fbase == 241 * 341) vbl_suppress = 1;
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
#ifdef RT_TRACE
    if (addr == 0x2002 || addr == 0x4016 || addr == 0x4017) plat_trace('R', frame_no, stamp, addr, d);
#endif
    if (due_dirty) reschedule();
    return d;
}
/* returns extra CPU cycles consumed (OAM DMA) */
u32 c_wr(u32 stamp, u32 addr, u32 val) {
    u8 d = (u8)val; u32 extra = 0; stat_wr[addr < 0x2000 ? 0 : addr < 0x4000 ? 1 + (addr & 7) % 7 : 7]++;
    addr &= 0xffff;
#ifdef RT_TRACE
    io_sync(stamp);
    if ((addr >= 0x2000 && addr <= 0x2007) || (addr >= 0x4000 && addr <= 0x4017)) plat_trace('W', frame_no, stamp, addr, d);
#else
    if (addr < 0x4000 || addr == 0x4014) io_sync(stamp);                        /* only the PPU (and the OAM DMA, which reads RAM) depends on the exact time */
#endif
    if (addr < 0x2000) ram[addr & 0x7ff] = d;
    else if (addr >= 0x4000 && addr < 0x4018 && addr != 0x4014 && addr != 0x4016) apu_write(stamp, (u8)(addr & 0x1f), d);
    else if (addr < 0x4000) {
        open_bus = d;
        switch (addr & 7) {
        case 0: ctrl = d; t = (t & ~0xc00) | ((d & 3) << 10); nmi_eval(stamp + 1); break;
        case 1: mask = d; break;
        case 3: oamaddr = d; break;
        case 4: flush_pending(); oam[oamaddr++] = d; oam_cnt_valid = 0; break;
        case 5: if (!wlatch) { t = (t & ~0x1f) | (d >> 3); finex = d & 7; } else t = (t & ~0x73e0) | ((d & 7) << 12) | ((d >> 3) << 5); wlatch ^= 1; break;
        case 6: if (!wlatch) { t = (t & 0xff) | ((d & 0x3f) << 8); } else { t = (t & 0xff00) | d; v = t; } wlatch ^= 1; break;
        case 7: ppu_wr(v, d); v += (ctrl & 4) ? 32 : 1; break;
        }
    }
    else if (addr == 0x4014) {
        u16 base = (u16)(d << 8);
        u32 c = stamp + 1; extra = 1; if ((c + (u32)epoch_cycles) & 1) { c++; extra++; }
        flush_pending(); oam_cnt_valid = 0;
        if (oamaddr == 0 && base < 0x2000) { const u32 *sp = (const u32 *)(ram + (base & 0x700)); u32 *dp = (u32 *)oam; for (int i = 0; i < 64; i++) dp[i] = sp[i]; }
        else for (int i = 0; i < 256; i++) oam[(oamaddr + i) & 255] = base < 0x2000 ? ram[(base + i) & 0x7ff] : 0;
        extra += 512;
    }
    else if (addr == 0x4016) { strobe = d & 1; if (strobe) { pad_shift[0] = pad_state[0]; pad_shift[1] = pad_state[1]; } }
    if (due_dirty) reschedule();
    return extra;
}
void c_set_pad(u8 p) { pad_state[0] = p; }
void c_reset(void) {
    for (int i = 0; i < 2048; i++) ram[i] = (i & 1) ? 0xff : 0x00;
    memset(nt, 0, sizeof nt); memset(pal, 0, sizeof pal); memset(oam, 0, sizeof oam);
    ctrl = mask = status = oamaddr = rbuf = wlatch = finex = open_bus = 0; v = t = 0; dots_done = 0; fbase = 0; p_scan = 0; p_dot = 0; frame_no = 0; nmi_pending = 0; nmi_line_prev = 0;
    reschedule();
}
u32 c_frame_no(void) { return frame_no; }

/* ------------------------------------------------------------------ fused busy-wait loops (exact closed forms, see tools/xl6502.py) */
/* run the model ahead to just after the sprite-0 hit of this frame (never beyond it); returns 1 if the hit is now known */
static int ahead_to_hit(void) {
    while (!(status & 0x40)) {
        int scan = p_scan, dot = p_dot;
        if (scan >= 240) return 0;
        int h = (ctrl & 0x20) ? 16 : 8, s0 = (int)oam[0] + 1;
        if (scan >= s0 + h) return 0;                                    /* sprite 0 is behind us: no hit in this frame */
        u32 line_start = dots_done - dot, stop = line_start + 341;
        if (scan < s0 - 1 || (scan == s0 - 1 && dot == 0)) {              /* nothing can happen before the first line of sprite 0: skip whole lines at once */
            if (dot == 0) { stop = fbase + (u32)(s0 > 240 ? 240 : s0) * 341; ppu_run_to(stop); continue; }
        }
        int on = rend_on();
        if (on && dot < 257) {                              /* probe the rest of this line for the hit before committing the segment */
            hit_scan(scan, dot, 341, 0);
            if (hit_x_found >= 0) { u32 hd = line_start + hit_x_found + 1; if (hd + 1 < stop) stop = hd + 1; }
        }
        ppu_run_to(stop);
    }
    return 1;
}
/* "L: LDA $2002 / AND #mask / B<cc> L" executed from stamp T0 (cycle count at the LDA).  whilenz: loop while (v & mask) != 0 (BNE) else while == 0 (BEQ).
 * limit: cycle of the next event the translated code has to poll for (the loop is not fused past it).  Returns the cycles consumed; *val = A at exit. */
u32 c_fuse_poll(u32 T0, u32 maskv, u32 whilenz, u32 limit, u32 *val, u32 *exited) {
    u32 j = 0; *exited = 0;
    for (;;) {
        u32 top = T0 + 9 * j;                              /* start of iteration j (LDA); reads at top+4; iterations take 9 cycles, the last one 8 */
        if (limit != 0x7fffffffu && top + 6 >= limit) return 9 * j;      /* an event is due inside this iteration: the translated code runs it instruction by instruction */
        u32 v = c_rd(top + 4, 0x2002) & maskv;
        int exitp = whilenz ? (v == 0) : (v != 0);
        if (exitp) { *val = v; *exited = 1; return 9 * j + 8; }
        u32 dotE = ~0u;                                    /* dot at which the awaited change happens */
        if (maskv == 0x40 && !whilenz) { if (ahead_to_hit()) dotE = fbase + live_hit_dot; }
        else if (maskv == 0x40 && whilenz) { dotE = fbase + 311 * 341 + 1; if (dotE < dots_done) dotE += FD; }
        else if (maskv == 0x80 && !whilenz) { dotE = fbase + 241 * 341 + 1; if (dotE < dots_done) dotE += FD; }
        u32 jn = j + 1;
        if (dotE != ~0u) {
            u32 target = cycle_of_dot(dotE);                 /* first read stamp that sees the change */
            if (target > top + 4 + 9) jn = (target - T0 - 4 + 8) / 9;
        }
        if (limit != 0x7fffffffu) { u32 maxj = limit > T0 + 7 ? (limit - T0 - 7) / 9 + 1 : 0; if (jn > maxj) jn = maxj > j + 1 ? maxj : j + 1; }
        j = jn;
    }
}
/* nested counting delay loop  LDY #n / DEY / BNE -3 / DEX / BNE -7  with X in x: cycles consumed (X ends 0, Y ends 0) */
u32 c_delay_cycles(u32 x, u32 n) { if (x == 0) x = 256; if (n == 0) n = 256; return x * (5 * n + 6) - 1; }

/* main-thread idle loop  C1D8: JSR D326 (64-bit shift register RNG at $18-$1F) / LDA $48 / BMI C1D8 / BEQ C1D8 : whole iterations while the loop condition holds
 * and no event is due.  S = 6502 stack pointer.  Returns the cycles consumed (0: nothing fused); rt.idle_ccr = ccr after the last fused iteration, rt.idle_a = A. */
u32 c_idle(u32 now, u32 limit, u32 S) {
    u8 t48 = ram[0x48];
    if (!(t48 & 0x80) && t48 != 0) return 0;                          /* the loop would exit */
    u32 hi = ((u32)ram[0x18] << 24) | ((u32)ram[0x19] << 16) | ((u32)ram[0x1a] << 8) | ram[0x1b];
    u32 lo = ((u32)ram[0x1c] << 24) | ((u32)ram[0x1d] << 16) | ((u32)ram[0x1e] << 8) | ram[0x1f];
    u32 base = 6 + 3 + 2 + 3 + 3 + 2 + 3 + 2 + 3 + 40 + 6 + 3 + ((t48 & 0x80) ? 3 : 5);      /* one iteration without the extra cycle of the SEC path */
    u32 room = (limit == 0x7fffffffu) ? 0xffffffffu : limit - now;                                /* iterations must end before the event */
    u32 used = 0, n = 0, last_b18 = 0, c = 0;
    for (;;) {
        u32 x = ((hi >> 25) ^ (hi >> 17)) & 1;
        u32 cyc = base + x;
        if (used + cyc >= room) break;
        last_b18 = hi >> 24;
        c = lo & 1; lo = (lo >> 1) | (hi << 31); hi = (hi >> 1) | (x << 31);
        used += cyc; n++;
    }
    if (n) {
        ram[0] = (u8)(last_b18 & 2);
        ram[0x18] = (u8)(hi >> 24); ram[0x19] = (u8)(hi >> 16); ram[0x1a] = (u8)(hi >> 8); ram[0x1b] = (u8)hi;
        ram[0x1c] = (u8)(lo >> 24); ram[0x1d] = (u8)(lo >> 16); ram[0x1e] = (u8)(lo >> 8); ram[0x1f] = (u8)lo;
        ram[0x100 + (S & 255)] = 0xC1; ram[0x100 + ((S - 1) & 255)] = 0xDA;   /* the JSR's return address stays in the stack page */
        rt.idle_a = t48; rt.idle_ccr = (t48 & 0x80 ? 8 : 0) | (t48 == 0 ? 4 : 0) | (c ? 0x11 : 0);
    }
    return used;
}
u32 c_fuse_poll_w(u32 T0, u32 maskv, u32 whilenz, u32 limit) {
    u32 val, exited; u32 cyc = c_fuse_poll(T0, maskv, whilenz, limit, &val, &exited);
    rt.fuse_val = val; rt.fuse_exited = exited; return cyc;
}
