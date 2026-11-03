/* nes_main.c - Excitebike (Europe) on the Amiga 1200: glue between the platform layer and the NES runtime (rt/nes_rt.c) */
#include <stdint.h>
#include "platform.h"
#include "../rt/rt.h"
#include "nespal.h"

extern const u8 rom_prg[], rom_chr[];
extern void *dispatch_tab[16384];
extern void exb_run(u8 *ram, const u8 *prg, struct RT *rt, void **disp);
extern void c2p_pair(const uint8_t *src, uint8_t *dst);
uint16_t c2p_uni_words[256][8] __attribute__((aligned(4)));

static uint32_t pal32[256], palf[256];
extern u8 rt_frame_fast, rt_skip_next; extern u8 *fr_bmp, *fr_priv, *fr_spr, *fr_var, *fr_tmp[2], *fr_bmp_chip, *fr_priv_chip; extern void fd_disarm(void); extern void fr_palette(u8 out[32]);
extern void audio_init(void); extern void fd_show_slow(void); extern void fd_show_fast(void); extern int fd_mode_now(void);
static uint8_t chip_bmp[248 * 400 + 16] __attribute__((section(".chipbuf"), aligned(16)));
static uint8_t chip_spr[240 * 192 + 32] __attribute__((section(".chipbuf"), aligned(16)));
static uint8_t bmp_fast[248 * 400 + 16] __attribute__((aligned(16))), priv_fast[4 * 400 + 16] __attribute__((aligned(16)));   /* the CPU's working copies (fast RAM) of the background bitmap and the private lines */
static uint8_t chip_var[1024 * 192 + 256 + 8192 + 262144] __attribute__((section(".chipbuf"), aligned(16)));
static uint8_t chip_tmp[2][64 * 256] __attribute__((section(".chipbuf"), aligned(16)));
static uint8_t chip_priv[4 * 400 + 16] __attribute__((section(".chipbuf"), aligned(16)));
static uint8_t fast_pal_shadow[32]; static int pal_mode = -1;
#if defined(HUD) || defined(SCROLLTEST) || defined(MEMBENCH)
struct FrBand { uint16_t line0, nlines, x0, row0; uint8_t *base; };
extern struct FrBand fr_bands[]; extern int fr_nbands;
#endif
#ifdef PCPROF
/* program counter samples taken by the 50 Hz interrupts (isr.s hands over the interrupted PC): histogram over 256-byte blocks of the program */
static uint16_t pc_hist[640];
void plat_prof_pc(uint32_t pc) { uint32_t b = (pc - 0x200800u) >> 8; if (b < 640 && pc >= 0x200800u) { if (pc_hist[b] < 65535) pc_hist[b]++; } }
#endif
/* copies of the CPU's working bitmaps to the chip RAM bitmaps that are displayed */
static void wait_blitter(void) { (void)*(volatile uint16_t *)0xDFF002; while (*(volatile uint16_t *)0xDFF002 & 0x4000) { } }
void fr_bmp_copy_all(void)
{
    wait_blitter();
    const uint32_t *s = (const uint32_t *)fr_bmp; uint32_t *d = (uint32_t *)fr_bmp_chip;
    for (unsigned i = 0; i < 248 * 400 / 4; i++) d[i] = s[i];
}
void fr_priv_flush(int n)
{
    for (int k = 0; k < n; k++) for (int p = 0; p < 4; p++) {                 /* only the 40 bytes of each plane row that are shown */
        const uint32_t *s = (const uint32_t *)(fr_priv + k * 400 + p * 100); uint32_t *d = (uint32_t *)(fr_priv_chip + k * 400 + p * 100);
        for (int i = 0; i < 10; i++) d[i] = s[i];
    }
}
void c_dbg(u32 pc, u32 cyc, u32 a, u32 x, u32 y, u32 ccr) { (void)pc; (void)cyc; (void)a; (void)x; (void)y; (void)ccr; }
/* translated code reached something the static analysis excluded: show it (code 1 = fell through a branch assumed always taken, 2 = bad indirect jump target) */
void c_abort(int code, u32 cyc)
{
    (void)cyc;
    for (;;) { (*(volatile uint16_t *)0xDFF180) = code == 1 ? 0x0F00 : 0x00F0; }
}
volatile uint32_t exb_frames_shown, exb_frames_late;
/* time base for the profile of the renderer: colour clocks (227 per line, 312 lines per 50 Hz tick) */
#ifdef FR_PROF
uint32_t plat_timer(void)
{
    uint32_t t, v;
    do { t = plat_tick_count; v = *(volatile uint16_t *)0xDFF006; } while (t != plat_tick_count);
    uint32_t line = ((uint32_t)(*(volatile uint16_t *)0xDFF004 & 1) << 8) | (v >> 8);
    return t * (312 * 227) + line * 227 + (v & 0xFF);
}
#else
#define plat_timer() 0
#endif
extern uint32_t fr_prof[4], fr_prof_n, fr_prof_rl; static uint32_t prof_pf, prof_pf_n;

/* NES controller bits: 0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right */
void plat_pad(u32 frame)
{
#ifdef DEMO                                  /* benchmark: replay a recorded input movie (repeatable load, no keyboard) */
    { extern const uint8_t demo_pad[], demo_end[]; c_set_pad(frame < (u32)(demo_end - demo_pad) ? demo_pad[frame] : 0); return; }
#endif
    (void)frame;
    uint16_t in = plat_joy_bits(); uint8_t b = 0;
    if (in & PIN_FIRE)  b |= 0x01;           /* red button: A = accelerate */
    if (in & PIN_BTN2)  b |= 0x02;           /* blue button: B = turbo */
    if (in & PIN_UP)    b |= 0x10;
    if (in & PIN_DOWN)  b |= 0x20;
    if (in & PIN_LEFT)  b |= 0x40;
    if (in & PIN_RIGHT) b |= 0x80;
    if (plat_key_down(KEY_RETURN)) b |= 0x08;  /* Enter = Start */
    if (plat_key_down(KEY_SPACE))  b |= 0x04;  /* Space = Select */
    c_set_pad(b);
}

/* 8x8 font for the speed overlay (0-9 A-F) */
static const uint8_t hexfont[16][8] = {
 {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},{0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0},{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},
 {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0},{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},{0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},{0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},
 {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},{0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},{0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0},
 {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0},{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0},{0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0},{0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0}};
static void hud_hex(unsigned x, unsigned y, uint32_t v, unsigned digits)
{
    for (unsigned d = 0; d < digits; d++) {
        unsigned n = (v >> (4 * (digits - 1 - d))) & 15;
        for (unsigned r = 0; r < 8; r++) for (unsigned c = 0; c < 8; c++)
            frame_pix[y + r][x + d * 8 + c] = (hexfont[n][r] >> (7 - c)) & 1 ? 0x30 : 0x0F;
    }
}
volatile uint32_t exb_game_frame; static uint32_t base_tick_g; static int started;
/* called at the instruction boundary after the frame's last visible line */
void plat_frame(u32 f)
{
    uint32_t pf0 = plat_timer();
    fd_disarm();
    exb_game_frame = f;
    if (!started) { started = 1; base_tick_g = plat_tick_count; }
    int behind = (int)(plat_tick_count - base_tick_g) - (int)f;
    if (rt_frame_fast == 2) { exb_frames_late++; }                 /* frame not rendered (running late) */
    else if (rt_frame_fast) {                                  /* hardware scroll display: the planes are ready, only the Copper list has to follow */
        uint8_t pl[32]; fr_palette(pl);
        if (pal_mode != 1) { pal_mode = 1; for (int i = 0; i < 32; i++) fast_pal_shadow[i] = 0xFF; }
        unsigned changed = 0;
        for (int i = 0; i < 32; i++) if (pl[i] != fast_pal_shadow[i]) { fast_pal_shadow[i] = pl[i]; palf[i] = nes_pal_rgb[pl[i] & 63]; changed = 1; }
        if (changed) plat_set_palette_banks(palf, 1);
#ifdef HUD
        {   /* debug overlay: 16 lines at the top: game frame / real 50 Hz ticks / frames not displayed; below: average cost per rendered frame in colour clocks
             * (build, clear+cells, private lines, sprites).  One band of 16 lines, 4 planes. */
            static uint8_t hudbuf[48 * 400 + 16] __attribute__((section(".chipbuf"), aligned(16)));
            static uint32_t hud_last, hud_f;
            if (f - hud_last >= 64 || f < hud_last) { hud_last = f;
            for (int i = 0; i < 48 * 400 / 4; i++) ((uint32_t *)(hudbuf + 8))[i] = 0;
            uint32_t n = fr_prof_n ? fr_prof_n : 1;
            uint32_t pn = prof_pf_n ? prof_pf_n : 1; extern uint32_t fr_dbg[8]; uint32_t vals[23] = { f, plat_tick_count - base_tick_g, exb_frames_late, fr_prof[0] / n, fr_prof[1] / n, fr_prof[2] / n, fr_prof[3] / n, 0, fr_prof_rl / (hud_f ? hud_f : 1), prof_pf / pn, fr_dbg[0], fr_dbg[1], fr_dbg[2], fr_dbg[3], fr_dbg[4], 0, 0, 0, 0, 0, 0, 0, 0 };
#ifdef PCPROF
            { for (int t = 0; t < 4; t++) { int bi = -1; unsigned bc = 0; for (int i = 0; i < 640; i++) if (pc_hist[i] > bc) { bc = pc_hist[i]; bi = i; } if (bi >= 0) { vals[15 + t * 2] = (uint32_t)bi; vals[16 + t * 2] = bc; pc_hist[bi] = 0; } } for (int i = 0; i < 640; i++) pc_hist[i] = 0; }
#endif

            for (int w = 0; w < 23; w++) for (int d = 0; d < 5; d++) {
                unsigned nn = (vals[w] >> (4 * (4 - d))) & 15; int row = w >= 15 ? 32 + ((w - 15) / 4) * 8 : w >= 10 ? 24 : w >= 8 ? 16 : w >= 3 ? 8 : 0, col = (w >= 15 ? (w - 15) % 4 : w >= 10 ? w - 10 : w >= 8 ? w - 8 : w >= 3 ? w - 3 : w) * 6 + d;
                for (int l = 0; l < 8; l++) { uint8_t g = hexfont[nn][l]; uint8_t *h = hudbuf + 8 + (row + l) * 400; h[col] = g; h[100 + col] = g; h[200 + col] = g; h[300 + col] = g; }
            }
            fr_prof[0] = fr_prof[1] = fr_prof[2] = fr_prof[3] = 0; fr_prof_n = 0; fr_prof_rl = 0; prof_pf = prof_pf_n = 0; hud_f = 0;
            }
            hud_f++;
            struct FrBand nb[16]; int nbn = 0;
            nb[nbn].line0 = 0; nb[nbn].nlines = 48; nb[nbn].x0 = 0; nb[nbn].row0 = 0; nb[nbn].base = hudbuf + 8; nbn++;
            for (int k = 0; k < fr_nbands && nbn < 16; k++) {
                struct FrBand b = fr_bands[k]; if (b.line0 + b.nlines <= 48) continue;
                if (b.line0 < 48) { unsigned cut = 48 - b.line0; b.base += cut * 400; b.line0 = 48; b.nlines -= cut; }
                nb[nbn++] = b;
            }
            for (int k = 0; k < nbn; k++) fr_bands[k] = nb[k]; fr_nbands = nbn;
            palf[15] = 0xFFFFFF; plat_set_palette_banks(palf, 1);
        }
#endif
        fd_show_fast(); exb_frames_shown++;
    } else {
        if (behind > 2 && (f & 3)) { exb_frames_late++; return; }   /* running late: convert only every 4th frame */
        if (pal_mode != 0) { pal_mode = 0; plat_set_palette256(pal32); }
#ifdef HUD
        hud_hex(8, 8, f, 5); hud_hex(8, 16, plat_tick_count - base_tick_g, 5); hud_hex(8, 24, exb_frames_late, 5);
#endif
        uint8_t *screen = (uint8_t *)plat_front();
        for (unsigned y = 0; y < PLAT_H; y++) {
            const uint8_t *src = &frame_pix[y][0]; uint8_t *dst = screen + y * PLAT_STRIDE;
            for (unsigned k = 0; k < 16; k++) c2p_pair(src + k * 16, dst + k * 2);
        }
        fd_show_slow(); exb_frames_shown++;
    }
    {   static unsigned skipped;                               /* render the next frame only if we are not behind real time; never skip more than 3 in a row */
        int bh = (int)(plat_tick_count - base_tick_g) - (int)f;
        if (rt_frame_fast == 2) skipped++; else skipped = 0;
        rt_skip_next = (bh > 1 && skipped < 7) ? 1 : 0;
#if defined(SKIPMODE) && SKIPMODE == 1
        rt_skip_next = 0;                                       /* benchmark: render every frame (game speed shows the cost of a rendered frame) */
#elif defined(SKIPMODE) && SKIPMODE == 2
        rt_skip_next = ((f + 1) & 31) != 0;                     /* benchmark: render every 32nd frame only (game speed shows the cost of the game logic alone) */
#endif
    }
    prof_pf += plat_timer() - pf0; prof_pf_n++;
#if !(defined(SKIPMODE) && SKIPMODE == 2)
    while ((int)f - (int)(plat_tick_count - base_tick_g) > 1) { }
#endif             /* never run faster than the 50 Hz of the original */
}

#ifdef MEMBENCH
/* time for 100 x 1024 long-word writes (move.l) to chip RAM, fast RAM and a read-modify-write on chip RAM, in 50 Hz ticks, shown as hex digits */
static uint8_t mb_lab[16 * 400 + 16] __attribute__((section(".chipbuf"), aligned(16)));
static uint32_t mb_chip[1024] __attribute__((section(".chipbuf"), aligned(16))); static uint32_t mb_fast[1024];
static void membench(void)
{
    for (int i = 0; i < 256; i++) palf[i] = 0; palf[1] = 0xFFFFFF; plat_set_palette_banks(palf, 1);
    fr_nbands = 1; fr_bands[0].line0 = 0; fr_bands[0].nlines = 240; fr_bands[0].x0 = 0; fr_bands[0].row0 = 0; fr_bands[0].base = mb_lab + 8;
    fd_show_fast(); { uint32_t tw = plat_tick_count; while (plat_tick_count - tw < 3) { } }
    uint32_t t0 = plat_tick_count; for (int r = 0; r < 100; r++) for (int i = 0; i < 1024; i++) mb_chip[i] = i; uint32_t a = plat_tick_count - t0;
    t0 = plat_tick_count; for (int r = 0; r < 100; r++) for (int i = 0; i < 1024; i++) mb_fast[i] = i; uint32_t b = plat_tick_count - t0;
    t0 = plat_tick_count; for (int r = 0; r < 100; r++) for (int i = 0; i < 1024; i++) mb_chip[i] += 1; uint32_t c = plat_tick_count - t0;
    t0 = plat_tick_count; for (int r = 0; r < 100; r++) for (int i = 0; i < 1024; i++) mb_fast[i] += 1; uint32_t d = plat_tick_count - t0;
    uint32_t v[4] = { a, b, c, d };
    for (int i = 0; i < 16 * 400; i++) mb_lab[8 + i] = 0;
    for (int w = 0; w < 4; w++) for (int dg = 0; dg < 5; dg++) { unsigned n = (v[w] >> (4 * (4 - dg))) & 15; for (int l = 0; l < 8; l++) mb_lab[8 + l * 400 + w * 6 + dg] = hexfont[n][l]; }
    fr_nbands = 1; fr_bands[0].line0 = 0; fr_bands[0].nlines = 240; fr_bands[0].x0 = 0; fr_bands[0].row0 = 0; fr_bands[0].base = mb_lab + 8;
    fd_show_fast(); for (;;) { }
}
#endif
#ifdef SCROLLTEST
/* test pattern: a bar at bitmap pixels 80..87 (plane 0) and one at 336..343 (plane 1), shown with different scroll values; the scroll value is
 * written as hex digits in a separate band at the top, the bar position in the screenshot is compared with 80 - x0 */
static uint8_t st_lab[16 * 400 + 16] __attribute__((section(".chipbuf"), aligned(16)));
static void scrolltest(void)
{
    for (unsigned r = 0; r < 240; r++) for (unsigned j = 0; j < 96; j++) { fr_bmp_chip[r * 400 + j] = (j == 10) ? 0xFF : 0; fr_bmp_chip[r * 400 + 100 + j] = (j == 42) ? 0xFF : 0; fr_bmp_chip[r * 400 + 200 + j] = fr_bmp_chip[r * 400 + 300 + j] = 0; }
    for (int i = 0; i < 256; i++) palf[i] = 0;
    palf[1] = 0xFFFFFF; palf[2] = 0xFF0000; palf[3] = 0xFFFF00; plat_set_palette_banks(palf, 1);
    static const int xs[] = { 0, 1, 7, 8, 15, 16, 17, 24, 31, 32, 33, 40, 47, 48, 63, 64, 65, 79, 80, 81, 95, 96, 97, 100, 111, 112, 113, 127, 128, 129, 200, 255 };
    for (unsigned k = 0; ; k = (k + 1) % 32) {
        for (int i = 0; i < 16 * 400; i++) st_lab[8 + i] = 0;
        for (int d = 0; d < 3; d++) { unsigned n = ((unsigned)xs[k] >> (4 * (2 - d))) & 15; for (int l = 0; l < 8; l++) st_lab[8 + l * 400 + d] = hexfont[n][l]; }
        fr_nbands = 2; fr_bands[0].line0 = 0; fr_bands[0].nlines = 8; fr_bands[0].x0 = 0; fr_bands[0].row0 = 0; fr_bands[0].base = st_lab + 8;
        fr_bands[1].line0 = 8; fr_bands[1].nlines = 232; fr_bands[1].x0 = xs[k]; fr_bands[1].row0 = 8; fr_bands[1].base = fr_bmp + 8 * 400;
        fd_show_fast();
        uint32_t t = plat_tick_count; while (plat_tick_count - t < 60) { }
    }
}
#endif
void main(void)
{
    for (unsigned v = 0; v < 256; v++) for (unsigned p = 0; p < 8; p++) c2p_uni_words[v][p] = ((v >> p) & 1) ? 0xFFFF : 0;
    for (unsigned i = 0; i < 256; i++) pal32[i] = nes_pal_rgb[i & 63];
    plat_set_palette256(pal32); pal_mode = 0;
    fr_bmp = bmp_fast + 8; fr_priv = priv_fast + 8; fr_bmp_chip = chip_bmp + 8; fr_priv_chip = chip_priv + 8; fr_spr = chip_spr + 16; fr_var = chip_var; fr_tmp[0] = chip_tmp[0]; fr_tmp[1] = chip_tmp[1];
    *(volatile uint16_t *)0xDFF02E = 2;                          /* COPCON: the Copper may write the blitter registers */
#ifdef SCROLLTEST
    scrolltest();
#endif
#ifdef MEMBENCH
    membench();
#endif
    fd_show_slow(); audio_init();
    prg = rom_prg; chr = rom_chr;
    c_reset();
    exb_run(ram, prg, &rt, dispatch_tab);
}
