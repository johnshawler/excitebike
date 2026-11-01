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

static uint32_t pal32[256];
void c_dbg(u32 pc, u32 cyc, u32 a, u32 x, u32 y, u32 ccr) { (void)pc; (void)cyc; (void)a; (void)x; (void)y; (void)ccr; }
/* translated code reached something the static analysis excluded: show it (code 1 = fell through a branch assumed always taken, 2 = bad indirect jump target) */
void c_abort(int code, u32 cyc)
{
    (void)cyc;
    for (;;) { (*(volatile uint16_t *)0xDFF180) = code == 1 ? 0x0F00 : 0x00F0; }
}
volatile uint32_t exb_frames_shown, exb_frames_late;

/* NES controller bits: 0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right */
void plat_pad(u32 frame)
{
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
volatile uint32_t exb_game_frame;
/* called at the instruction boundary after the frame's last visible line (frame_pix complete) */
void plat_frame(u32 f)
{
    exb_game_frame = f;
    uint8_t *screen = (uint8_t *)plat_front();
    static uint32_t base_tick; static int started;
    if (!started) { started = 1; base_tick = plat_tick_count; }
    int behind = (int)(plat_tick_count - base_tick) - (int)f;
    if (behind > 2 && (f & 3)) { exb_frames_late++; return; }   /* running late: convert only every 4th frame */
#ifdef HUD
    hud_hex(8, 8, f, 5); hud_hex(8, 16, plat_tick_count - base_tick, 5); hud_hex(8, 24, exb_frames_late, 5);
#endif
    for (unsigned y = 0; y < PLAT_H; y++) {
        const uint8_t *src = &frame_pix[y][0]; uint8_t *dst = screen + y * PLAT_STRIDE;
        for (unsigned k = 0; k < 16; k++) c2p_pair(src + k * 16, dst + k * 2);
    }
    exb_frames_shown++;
    while ((int)f - (int)(plat_tick_count - base_tick) > 1) { }             /* never run faster than the 50 Hz of the original */
}

void main(void)
{
    for (unsigned v = 0; v < 256; v++) for (unsigned p = 0; p < 8; p++) c2p_uni_words[v][p] = ((v >> p) & 1) ? 0xFFFF : 0;
    for (unsigned i = 0; i < 256; i++) pal32[i] = nes_pal_rgb[i & 63];
    plat_set_palette256(pal32);
    prg = rom_prg; chr = rom_chr;
    c_reset();
    exb_run(ram, prg, &rt, dispatch_tab);
}
