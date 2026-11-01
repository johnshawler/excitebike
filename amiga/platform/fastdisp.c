/* fastdisp.c - Amiga 1200 (AGA) display driver for the fast renderer (rt/fastr.c): one copper list per frame, 5 bitplanes through hardware scroll.
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
#define FR_ROWB (FR_BW * 5)
struct FrBand { uint16_t line0, nlines, x0, row0; uint8_t *base; };
extern struct FrBand fr_bands[]; extern int fr_nbands;
extern uint8_t *fr_bmp;

static uint16_t cop[2][1024] __attribute__((section(".chipbuf"), aligned(8)));
static unsigned cop_cur;
static int fd_mode = -1;                       /* 0 slow (chunky c2p buffer), 1 fast (hardware scroll bands) */

static uint16_t *cp;
static inline void mv(unsigned reg, unsigned val) { *cp++ = (uint16_t)reg; *cp++ = (uint16_t)val; }
static void wait_line(unsigned vpos, unsigned hpos)
{
    static unsigned passed255;
    if (vpos > 255 && !passed255) { *cp++ = 0xFFDF; *cp++ = 0xFFFE; passed255 = 1; }
    if (vpos <= 255) passed255 = 0;
    *cp++ = (uint16_t)(((vpos & 0xFF) << 8) | (hpos & 0xFE) | 1); *cp++ = 0xFFFE;
}
static unsigned hi_cur[5];
static void set_planes(const struct FrBand *b, int first)
{
    unsigned r = b->x0 & 15, w = b->x0 >> 4;
    int P = (int)w - (r == 0 ? 1 : 0); unsigned d = (16 - r) & 15;
    for (unsigned p = 0; p < 5; p++) {
        uint32_t a = (uint32_t)b->base + p * FR_BW + (uint32_t)(P * 2);
        if (first || (a >> 16) != hi_cur[p]) { mv(BPL1PT + p * 4, a >> 16); hi_cur[p] = a >> 16; }
        mv(BPL1PT + p * 4 + 2, a & 0xFFFF);
    }
    mv(BPLCON1, d | (d << 4));
}
static void header(int fast)
{
    unsigned vs = plat_vstart, ve = plat_vstart + PLAT_H;
    mv(FMODE, fast ? 0x0000 : 0x0001);
    mv(DDFSTRT, fast ? 0x0040 : 0x0040); mv(DDFSTOP, fast ? 0x00C0 : 0x00B0);
    mv(DIWSTRT, ((vs & 0xFF) << 8) | 0xA1); mv(DIWSTOP, ((ve & 0xFF) << 8) | 0xA1); mv(DIWHIGH, 0x2100);
    mv(BPLCON2, 0);
    mv(BPL1MOD, fast ? FR_ROWB - 34 : PLAT_STRIDE - PLAT_PLANE_STRIDE); mv(BPL2MOD, fast ? FR_ROWB - 34 : PLAT_STRIDE - PLAT_PLANE_STRIDE);
    mv(BPLCON0, fast ? 0x5201 : 0x0211);
}
static void finish(void)
{
    *cp++ = 0xFFFF; *cp++ = 0xFFFE;
    R32(COP1LC) = (uint32_t)cop[cop_cur];       /* the Copper restarts from COP1LC at the next vertical blank */
    cop_cur ^= 1;
}
void fd_show_slow(void)
{
    cp = cop[cop_cur]; header(0);
    uint32_t base = (uint32_t)plat_front();
    for (unsigned p = 0; p < PLAT_DEPTH; p++) { mv(BPL1PT + p * 4, (base + p * PLAT_PLANE_STRIDE) >> 16); mv(BPL1PT + p * 4 + 2, (base + p * PLAT_PLANE_STRIDE) & 0xFFFF); }
    mv(BPLCON1, 0);
    finish(); fd_mode = 0;
}
void fd_show_fast(void)
{
    cp = cop[cop_cur]; header(1);
    for (int k = 0; k < fr_nbands; k++) {
        const struct FrBand *b = &fr_bands[k];
        if (k) wait_line(plat_vstart + b->line0 - 1, 0xC8);
        set_planes(b, k == 0);
    }
    finish(); fd_mode = 1;
}
int fd_mode_now(void) { return fd_mode; }
