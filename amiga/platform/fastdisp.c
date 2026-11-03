/* fastdisp.c - Amiga 1200 (AGA) display driver for the fast renderer (rt/fastr.c): one copper list per frame, dual playfield (4+4 bitplanes) through hardware scroll.
 * See rt/fastr.c for the bitmap layout.  The picture is a set of bands (runs of lines with the same scroll); the Copper changes the bitplane
 * pointers and the scroll (BPLCON1) at the end of the line before each band starts.  16-bit fetch with one extra leading word (DDFSTRT one slot earlier)
 * gives scrolling by 0..15 pixels: pixel x0 of the bitmap shows at screen x 0 when the pointer is word w-(r==0) and the delay is (16-r)&15 (x0 = 16w + r). */
#include <stdint.h>
#include "platform.h"

#define R16(o)   (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o)   (*(volatile uint32_t *)(0xDFF000u + (o)))
#define COP1LC 0x080
#define BPLCON0 0x100
#define BPLCON1 0x102
#define BPLCON2 0x104
#define BPL1MOD 0x108
#define BPL2MOD 0x10A
#define DIWSTRT 0x08E
#define DIWSTOP 0x090
#define DDFSTRT 0x092
#define DDFSTOP 0x094
#define DIWHIGH 0x1E4
#define FMODE   0x1FC
#define BPL1PT  0x0E0

#define FR_BW 100
#define FR_ROWB (FR_BW * 4)
#define SL_BW 48
#define SL_ROWB (SL_BW * 2)
#define SL_GRP (240 * SL_ROWB)
struct FrBand { uint16_t line0, nlines, x0, row0; uint8_t *base; };
extern struct FrBand fr_bands[]; extern int fr_nbands;
extern uint8_t *fr_bmp, *fr_spr, *fr_bmp_chip, *fr_priv, *fr_priv_chip;
static inline uint32_t chip_addr(const uint8_t *b)          /* working copy (fast RAM) -> displayed copy (chip RAM); anything else is already in chip RAM */
{
    if (b >= fr_bmp && b < fr_bmp + 240 * FR_ROWB) return (uint32_t)(fr_bmp_chip + (b - fr_bmp));
    if (b >= fr_priv && b < fr_priv + 4 * FR_ROWB) return (uint32_t)(fr_priv_chip + (b - fr_priv));
    return (uint32_t)b;
}
extern int fr_njobs, fr_ncjobs; extern uint32_t fr_dbg[8]; extern uint32_t *fr_bl_buf; extern uint32_t fr_bl_len;
/* The sprite layer is drawn by the blitter from a Copper program (see rt/fastr.c): the display list jumps (COP2LC / COPJMP2) into the program at the top of the
 * frame, the program runs the blits one after the other during the vertical blank and jumps back.  It must run only once: afterwards the jump is patched to
 * point at the next instruction (fd_disarm). */
static uint32_t bl_buf[2][8192] __attribute__((section(".chipbuf"), aligned(4)));
static unsigned bl_par;
static uint16_t *armed_hi, *armed_lo; static uint32_t armed_ret, armed_vbl; static int pend, dead;

static uint16_t cop[2][1024] __attribute__((section(".chipbuf"), aligned(8)));
static unsigned cop_cur;
static int fd_mode = -1;                       /* 0 slow (chunky c2p buffer), 1 fast (hardware scroll bands) */

static uint16_t *cp;
static inline void mv(unsigned reg, unsigned val) { *(uint32_t *)cp = ((uint32_t)(reg & 0xFFFF) << 16) | (val & 0xFFFF); cp += 2; }   /* one 32-bit store per Copper instruction (chip RAM accesses are expensive) */
static void wait_line(unsigned vpos, unsigned hpos)
{
    static unsigned passed255;
    if (vpos > 255 && !passed255) { *(uint32_t *)cp = 0xFFDFFFFEu; cp += 2; passed255 = 1; }
    if (vpos <= 255) passed255 = 0;
    *(uint32_t *)cp = ((uint32_t)(((vpos & 0xFF) << 8) | (hpos & 0xFE) | 1) << 16) | 0xFFFE; cp += 2;
}
#if defined(EXPMODE) && EXPMODE >= 1
#define EXPA 1
#else
#define EXPA 0
#endif
static unsigned hi_cur[4];
/* dual playfield: PF1 (odd bitplanes 1,3,5,7) = scrolled background, PF2 (even planes 2,4,6,8) = sprite layer in screen coordinates.
 * 16-bit fetch (FMODE 0) with one extra leading word: bitmap pixel x0 = 16w + r shows at screen x 0 when the PF1 pointer is word w-(r==0) and the
 * PF1 delay is (16-r)&15 (BPLCON1 bits 3..0); PF2 has no delay and its pointer starts one word before the line.
 * (FMODE 1 with 32-bit fetch needs scroll delays up to 31; FS-UAE shows a 32 pixel jump for delays >= 16 there, so the well-defined 0..15 range of
 *  FMODE 0 is used.) */
#ifndef FD_HISHIFT
#define FD_HISHIFT 12            /* BPLCON1 bit of delay bit 4 (PF1H6/PF1H7 are bits 12/13) */
#endif
static void set_planes(const struct FrBand *b, int first)
{
#ifdef FD_FMODE1
    unsigned r = b->x0 & 31, w = b->x0 >> 5;
    int P = (int)w - (r == 0 ? 1 : 0); unsigned d = (32 - r) & 31; unsigned unit = 4;
    unsigned con1 = (d & 15) | (((d >> 4) & 3) << FD_HISHIFT);
#ifdef FD_PM1
    if (d >= 16) P -= 1;
#endif
#else
    unsigned r = b->x0 & 15, w = b->x0 >> 4;
    int P = (int)w - (r == 0 ? 1 : 0); unsigned d = (16 - r) & 15; unsigned unit = 2;
    unsigned con1 = d;
#endif
    for (unsigned p = 0; p < 4; p++) {
        uint32_t a = chip_addr(b->base) + p * FR_BW + (uint32_t)(P * unit);
        if (first || (a >> 16) != hi_cur[p]) { mv(BPL1PT + p * 8, a >> 16); hi_cur[p] = a >> 16; }
        mv(BPL1PT + p * 8 + 2, a & 0xFFFF);
    }
    mv(BPLCON1, con1);
}
static void set_sprite_planes(void)
{
#ifdef FD_FMODE1
    unsigned g = 4;
#else
    unsigned g = 2;
#endif
    for (unsigned p = 0; p < 4; p++) {
        uint32_t a = (uint32_t)fr_spr + (p >> 1) * SL_GRP + (p & 1) * SL_BW - g;
        mv(BPL1PT + p * 8 + 4, a >> 16); mv(BPL1PT + p * 8 + 6, a & 0xFFFF);
    }
}
static void header(int fast)
{
    unsigned vs = plat_vstart, ve = plat_vstart + PLAT_H;
#ifdef FD_FMODE1
    mv(FMODE, 0x0001); mv(DDFSTRT, fast ? 0x0030 : 0x0040); mv(DDFSTOP, 0x00B0);
    unsigned fw = 36;
#else
    mv(FMODE, fast ? 0x0000 : 0x0001); mv(DDFSTRT, 0x0040); mv(DDFSTOP, fast ? 0x00C0 : 0x00B0);
    unsigned fw = 34;
#endif
    mv(DIWSTRT, ((vs & 0xFF) << 8) | 0xA1); mv(DIWSTOP, ((ve & 0xFF) << 8) | 0xA1); mv(DIWHIGH, 0x2100);
    mv(BPLCON2, fast ? 0x0040 : 0);
    mv(BPL1MOD, fast ? FR_ROWB - fw : PLAT_STRIDE - PLAT_PLANE_STRIDE); mv(BPL2MOD, fast ? SL_ROWB - fw : PLAT_STRIDE - PLAT_PLANE_STRIDE);
    mv(BPLCON0, fast ? 0x0611 : 0x0211);
}
static void finish(void)
{
    *(uint32_t *)cp = 0xFFFFFFFEu; cp += 2;
    R32(COP1LC) = (uint32_t)cop[cop_cur];       /* the Copper restarts from COP1LC at the next vertical blank */
    cop_cur ^= 1;
}
static void list_replaced(void);
void fd_show_slow(void)
{
    if (!fr_bl_buf) fr_bl_buf = bl_buf[0];
    list_replaced();
    cp = cop[cop_cur]; header(0);
    uint32_t base = (uint32_t)plat_front();
    for (unsigned p = 0; p < PLAT_DEPTH; p++) { mv(BPL1PT + p * 4, (base + p * PLAT_PLANE_STRIDE) >> 16); mv(BPL1PT + p * 4 + 2, (base + p * PLAT_PLANE_STRIDE) & 0xFFFF); }
    mv(BPLCON1, 0);
    finish(); fd_mode = 0;
}
/* has the last list of blitter jobs run (or will it certainly, before a list installed now starts)? */
int fr_jobs_executed(void)
{
    if (dead) return 0;
    if (!pend) return 1;
    return plat_vbl_count != armed_vbl;
}
void fd_disarm(void)                            /* called every frame: the jobs of the last list have run once the vertical blank has passed */
{
    if (pend && !dead && plat_vbl_count != armed_vbl) {
        *armed_hi = (uint16_t)(armed_ret >> 16); *armed_lo = (uint16_t)armed_ret;      /* the jump now goes to the next instruction */
        pend = 0;
    }
}
static void list_replaced(void)                 /* a new list is about to be installed: a list that has not run is lost */
{
    if (pend && plat_vbl_count == armed_vbl) { dead = 1; fr_dbg[1]++; }
    pend = 0;
}
static void wait_not_at_top(void)               /* COP1LC is latched at the start of the vertical blank: do not write it right there */
{
    for (;;) { uint16_t v = R16(0x004), h = R16(0x006); unsigned line = ((v & 1) << 8) | (h >> 8); if (line > 8) break; }
}
void fd_show_fast(void)
{
    list_replaced();
    cp = cop[cop_cur]; header(1); set_sprite_planes();
    for (int k = 0; k < fr_nbands; k++) {
        const struct FrBand *b = &fr_bands[k];
        if (k) wait_line(plat_vstart + b->line0 - 1, 0xC8);
        set_planes(b, k == 0);
        if (k == 0 && (fr_njobs || fr_ncjobs) && !EXPA) {
            uint32_t *bl = fr_bl_buf; uint32_t ret = (uint32_t)(cp + 6); uint32_t n = fr_bl_len;
            bl[n] = (0x084u << 16) | (ret >> 16); bl[n + 1] = (0x086u << 16) | (ret & 0xFFFF); bl[n + 2] = (0x08Au << 16);      /* COP2LC = ret, COPJMP2 */
            fr_dbg[4] = n; bl_par ^= 1; fr_bl_buf = bl_buf[bl_par];
            armed_hi = cp + 1; armed_lo = cp + 3; armed_ret = ret;
            mv(0x084, (uint32_t)bl >> 16); mv(0x086, (uint32_t)bl & 0xFFFF); mv(0x08A, 0);
            armed_vbl = plat_vbl_count; pend = 1; dead = 0;
        }
    }
    if (!pend) dead = 0;
    wait_not_at_top();
    finish(); fd_mode = 1;
}
int fd_mode_now(void) { return fd_mode; }
