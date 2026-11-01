/*
 * platform.h - Zoo Keeper Amiga 1200 platform layer (A1200 / AGA / 68EC020 / 2 MB chip RAM)
 * ============================================================================================
 * Hardware/boot layer only.  No OS is used after the bootblock hands over.  All code runs in 68020 SUPERVISOR mode, all RAM is chip RAM.
 * Same API as the A500 layer (platform/platform.h) where the game glue (amiga/main.c) uses it; differences:
 *   - CPU: 68EC020 (also 68020/68030): VBR := 0, instruction cache enabled, data cache off.
 *   - Display: AGA, 256x240, lores, 8 bitplanes (256 colours of 24 bit), INTERLEAVED planes: line y of plane p at base + y*256 + p*32,
 *     BPLxMOD = 224.  The whole 256-colour palette is loaded by the vertical-blank interrupt (CPU writes, LOCT two-pass); the border is blanked.
 *   - plat_set_palette256() loads the 256 colours; plat_set_palette()/plat_set_color() (RGB444) are kept for the error screens.
 *
 * DISPLAY: 256x240.  DIWSTRT/STOP horizontal 0xA1..0x1A1 (256 px).
 *     PAL : lines 52..291 (240 lines, centred in the 44..299 standard window)
 *     NTSC: lines 21..260 (first possible line on a 262-line frame; 240 lines)
 *
 * TIMING: identical to the A500 layer (game tick 60.0211 Hz from CIA-B timer A, Bresenham latch; PAL/NTSC by counting beam lines).
 *
 * INTERRUPT USE (do not re-program): level 2 CIA-A (keyboard SP), level 3 VERTB (palette load), level 4 Paula audio, level 6 CIA-B timer A.
 */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>

#define PLAT_W            256
#define PLAT_H            240
#define PLAT_DEPTH        8
#define PLAT_STRIDE       256                      /* bytes per line (all 8 planes interleaved) */
#define PLAT_PLANE_STRIDE 32                       /* bytes between the planes of one line */
#define PLAT_BUF_SIZE     (PLAT_STRIDE * PLAT_H)   /* 61440 */

#define PLAT_PAYLOAD_BASE 0x00000800u
#define PLAT_CHIP_TOP     0x00200000u

#ifndef PLAT_PAL_VSTART
#define PLAT_PAL_VSTART   52      /* first display line, PAL  (VPOS) */
#endif
#ifndef PLAT_NTSC_VSTART
#define PLAT_NTSC_VSTART  21      /* first display line, NTSC (VPOS) */
#endif

/* Game tick: 39 chars * 267 lines / 625 kHz */
#define PLAT_TICK_HZ_X10000  600211   /* 60.0211 Hz */
#define PLAT_TICK_US_X10000  166608   /* 16660.8 us */

/* Input bits returned by plat_input() / plat_joy_bits() */
#define PIN_UP      0x0001
#define PIN_DOWN    0x0002
#define PIN_LEFT    0x0004
#define PIN_RIGHT   0x0008
#define PIN_FIRE    0x0010   /* port-2 button 1 (CIA-A PRA bit 7) */
#define PIN_BTN2    0x0020   /* port-2 button 2 (POTGOR bit 14)   */
#define PIN_COIN    0x0040   /* key '5'      (keyboard only)      */
#define PIN_START1  0x0080   /* key '1'                           */
#define PIN_START2  0x0100   /* key '2'                           */
#define PIN_ESC     0x0200   /* Esc                               */
#define PIN_SPACE   0x0400   /* Space                             */
#define PIN_PAUSE   0x0800   /* key 'P'                           */
#define PIN_JOY_MASK 0x003F

/* Amiga raw keycodes (make code; break = code | 0x80 on the wire) */
#define KEY_1      0x01
#define KEY_2      0x02
#define KEY_5      0x05
#define KEY_P      0x19
#define KEY_SPACE  0x40
#define KEY_RETURN 0x44
#define KEY_ESC    0x45
#define KEY_UP     0x4C
#define KEY_DOWN   0x4D
#define KEY_RIGHT  0x4E
#define KEY_LEFT   0x4F
#define KEY_F1     0x50      /* F1..F10 = 0x50..0x59 */
#define KEY_F(n)   (0x4F + (n))
#define KEY_LSHIFT 0x60
#define KEY_RSHIFT 0x61
#define KEY_CTRL   0x63
#define KEY_LALT   0x64
#define KEY_RALT   0x65
#define KEY_LAMIGA 0x66
#define KEY_RAMIGA 0x67

/* ------------------------------------------------------- Copper list structure */
/* The Copper only reloads the sprite and bitplane pointers once per frame (line PLAT_COP_SETUP_LINE, before the display window).
 * The palette is written by the CPU in the vertical-blank interrupt.  Never write the live list directly. */
#define PLAT_COP_SETUP_LINE   14
typedef struct {
    uint16_t spr[8][4];
    uint16_t wait[2];
    uint16_t bpl[8][4];
    uint16_t end[2];
} PlatCopper;
extern PlatCopper plat_cop;      /* chip RAM, 4-aligned */

/* ------------------------------------------------------------------- state --- (all volatile; readable at any time) */
extern volatile uint32_t plat_tick_count;   /* game ticks since plat_init (60.0211 Hz) */
extern volatile uint32_t plat_vbl_count;    /* vertical blanks since plat_init */
extern volatile uint32_t plat_flip_count;   /* (unused, 0) */
extern volatile uint32_t plat_tick_scanline;/* CIA-B TOD sampled in the tick ISR (diagnostic) */
extern uint8_t  plat_is_pal;                /* 1 = PAL (>=286 lines/frame), 0 = NTSC */
extern uint16_t plat_lines;                 /* measured lines per frame (312/262) */
extern uint16_t plat_vstart;                /* display window first line in use */
extern volatile uint8_t plat_keymap[16];    /* 128-bit key-down bitmap (raw codes) */
extern volatile uint8_t plat_last_key;      /* raw code of last key pressed (make) */
extern volatile uint8_t plat_last_event;    /* last raw event: code | 0x80 if release */
extern volatile uint32_t plat_key_events;   /* number of key make/break events */

/* ------------------------------------------------------------------- system -*/
void plat_hw_reset(void);   /* called by crt0: quiesce all chips; safe from any hot state */
void plat_init(void);       /* called by crt0 once; sets up everything; enables IRQs */
void plat_restart(void) __attribute__((noreturn)); /* jump back to _start (full re-init) */

/* -------------------------------------------------------------------- video */
void    *plat_back(void);                /* single buffer: == plat_front() */
void    *plat_front(void);
void     plat_flip(void);                /* single buffer: waits for the next vertical blank */
void     plat_wait_vbl(void);
/* Palette: 256 x 0x00RRGGBB (8 bits per channel), loaded by the next vertical-blank interrupt (all 256 on the same frame). */
void     plat_set_palette256(const uint32_t rgb[256]);
/* the same for the banks of 32 colours whose bit is set in mask (entries 32*b .. 32*b+31 of rgb): only those are converted and loaded by the next vertical blank */
void     plat_set_palette_banks(const uint32_t rgb[256], unsigned mask);
/* RGB444 helpers for the error screens: colour registers 0..31 */
void     plat_set_palette(const uint16_t rgb[32]);
void     plat_set_color(unsigned idx, uint16_t rgb);
void     plat_clear_buffer(void *buf);   /* zero a PLAT_BUF_SIZE buffer (CPU) */
void     plat_user_irq3(uint16_t pending);

/* --------------------------------------------------------------------- time */
void     plat_wait_tick(void);
uint32_t plat_scanlines(void);

/* -------------------------------------------------------------------- input */
uint16_t plat_joy_bits(void);   /* PIN_UP|DOWN|LEFT|RIGHT|FIRE|BTN2 from port 2 */
uint16_t plat_input(void);      /* joystick bits | keyboard bits (COIN,START1/2,ESC,SPACE,PAUSE) */
#define  plat_key_down(code)  ((plat_keymap[(code) >> 3] >> ((code) & 7)) & 1)
int      plat_key_get(void);    /* pop event queue: -1 if empty, else raw code | 0x80 on release */

/* -------------------------------------------------------------------- audio */
/* Paula 8-bit sample playback, channels 0..3 (0,3 = left, 1,2 = right).  data in chip RAM, word aligned; `words` = length in 16-bit
 * words (1..65535); period >= 124; vol 0..64.  PLAT_SND_LOOP loops forever; otherwise plays once and the channel stops itself. */
#define PLAT_SND_LOOP 1
void     plat_audio_play(unsigned ch, const void *data, unsigned words, unsigned period, unsigned vol, unsigned flags);
void     plat_audio_stop(unsigned ch);
int      plat_audio_busy(unsigned ch);
void     plat_audio_volume(unsigned ch, unsigned vol);

uint32_t plat_muldiv(uint32_t a, uint32_t b, uint32_t c);  /* a*b/c, 64-bit intermediate */
#endif
