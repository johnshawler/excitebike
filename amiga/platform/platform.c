/*
 * platform.c - Amiga 1200 (AGA, 68EC020) hardware layer: reset, display, Copper, timing, input, audio.
 * Derived from the A500 layer (platform/platform.c); the CIA tick, keyboard, joystick and Paula parts are unchanged.
 * See platform.h for the API.
 */
#include "platform.h"

/* ---------------------------------------------------------------- registers */
#define R16(o)   (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o)   (*(volatile uint32_t *)(0xDFF000u + (o)))
#define CIAA(r)  (*(volatile uint8_t *)(0xBFE001u + (unsigned)(r) * 0x100u))
#define CIAB(r)  (*(volatile uint8_t *)(0xBFD000u + (unsigned)(r) * 0x100u))
/* CIA register numbers */
#define C_PRA 0x0
#define C_PRB 0x1
#define C_DDRA 0x2
#define C_DDRB 0x3
#define C_TALO 0x4
#define C_TAHI 0x5
#define C_TBLO 0x6
#define C_TBHI 0x7
#define C_TOD0 0x8
#define C_TOD1 0x9
#define C_TOD2 0xA
#define C_SDR  0xC
#define C_ICR  0xD
#define C_CRA  0xE
#define C_CRB  0xF

#define DMACONR  0x002
#define VPOSR    0x004
#define VHPOSR   0x006
#define JOY1DAT  0x00C
#define POTGOR   0x016
#define POTGO    0x034
#define DENISEID 0x07C
#define COP1LC   0x080
#define COPJMP1  0x088
#define DIWSTRT  0x08E
#define DIWSTOP  0x090
#define DDFSTRT  0x092
#define DDFSTOP  0x094
#define DMACON   0x096
#define CLXCON   0x098
#define INTENA   0x09A
#define INTREQ   0x09C
#define INTREQR  0x01E
#define ADKCON   0x09E
#define AUDx(ch,o) (0x0A0 + (ch) * 0x10 + (o))
#define BPLCON0  0x100
#define BPLCON1  0x102
#define BPLCON2  0x104
#define BPLCON3  0x106
#define BPL1MOD  0x108
#define BPL2MOD  0x10A
#define BPLCON4  0x10C
#define CLXCON2  0x10E
#define SPR0PT   0x120
#define SPRx(n,o) (0x140 + (n) * 8 + (o))
#define COLOR00  0x180
#define DIWHIGH  0x1E4
#define FMODE    0x1FC

#ifndef PLAT_POTGO_INIT
#define PLAT_POTGO_INIT 0xFF00
#endif

/* AGA display programming.  PLAT_FMODE: bitplane fetch width, 0 = 16 bit (every bitplane DMA slot is used: the CPU is shut out of chip RAM
 * during the display window, ~57 % speed in FS-UAE) or 1 = 32 bit (half the slots, ~82 %; default).  A 64-bit mode (FMODE 3) was
 * tried and not pursued: its fetch start must be aligned to 32 colour clocks, which cannot place a 256-pixel picture at the centred
 * position DIWSTRT 0xA1 without a wider bitmap; with 32-bit fetch the display already takes half the DMA slots. */
#ifndef PLAT_FMODE
#define PLAT_FMODE 1
#endif
#define BPLCON0_8PL  0x0211       /* BPU3 (bit 4) = 8 planes with BPU2..0 = 0, COLOR (bit 9), lores, ECSENA (bit 0: enables BRDRBLNK in BPLCON3) */
#define BPLCON3_BASE 0x1020       /* BANK 0, PF2OF 100 (second playfield colours from 16), LOCT 0, SPRES lores, BRDRBLNK (border blanked: always black) */
#define BPLCON3_LOCT 0x0200
#define BPLCON4_BASE 0x0011       /* default: no bitplane XOR, sprite colour banks 1 (unused, sprites are off) */
#if PLAT_FMODE == 0
#define FMODE_VAL 0x0000
#define DDF_STRT  0x0048
#define DDF_STOP  0x00C0
#define BPLCON1_VAL 0x0000
#elif PLAT_FMODE == 1
#define FMODE_VAL 0x0001
#define DDF_STRT  0x0040
#define DDF_STOP  0x00B0
#define BPLCON1_VAL 0x0000
#else
#error PLAT_FMODE must be 0 or 1
#endif
#ifdef PLAT_DDFSTRT                         /* experiments: override the fetch window and the horizontal scroll */
#undef DDF_STRT
#define DDF_STRT PLAT_DDFSTRT
#endif
#ifdef PLAT_DDFSTOP
#undef DDF_STOP
#define DDF_STOP PLAT_DDFSTOP
#endif
#ifdef PLAT_BPLCON1
#undef BPLCON1_VAL
#define BPLCON1_VAL PLAT_BPLCON1
#endif

/* DMACON / INTENA bits */
#define SETBIT   0x8000
#define DMA_ALL  0x7FFF
#define DMAEN    0x0200
#define BPLEN    0x0100
#define COPEN    0x0080
#define BLTEN    0x0040
#define INT_PORTS 0x0008
#define INT_VERTB 0x0020
#define INT_AUD0  0x0080
#define INT_EXTER 0x2000
#define INT_INTEN 0x4000

/* ---------------------------------------------------------------- tick timing
 * E-clock = colour clock / 5: PAL 3546895/5 = 709379 Hz, NTSC 3579545/5 = 715909 Hz.
 * tick period = 39*267/625000 s = 10413/625000 s exactly.
 *   PAL : 709379*10413/625000 = 11818 + 513527/625000 E cycles = 11818.8216
 *   NTSC: 715909*10413/625000 = 11927 + 385417/625000 E cycles = 11927.6167
 * A CIA timer with latch L has period L+1 cycles.  Each underflow the ISR adds
 * `frac` to an accumulator; when it passes DEN the next period is base+1 else base.
 * Long-term mean period is therefore exactly base + frac/DEN (error 0, in E units). */
#define TICK_DEN 3325214u              /* NES PAL frame = 33247.5 CPU cycles at 1662607 Hz: E-clock periods of one frame */
static const struct { uint16_t base; uint32_t frac; } tick_parm[2] = {
    { 14316, 605331 },   /* [0] NTSC */
    { 14185, 1996015 },  /* [1] PAL  */
};

/* ------------------------------------------------------------------- state */
volatile uint32_t plat_tick_count, plat_vbl_count, plat_flip_count, plat_tick_scanline;
uint8_t  plat_is_pal;
uint16_t plat_lines;
uint16_t plat_vstart;
volatile uint8_t  plat_keymap[16];
volatile uint8_t  plat_last_key, plat_last_event;
volatile uint32_t plat_key_events;

PlatCopper plat_cop __attribute__((aligned(8), section(".chipbuf")));

static uint8_t plat_buf[PLAT_BUF_SIZE] __attribute__((section(".chipbuf"), aligned(16)));
static uint16_t null_sprite[8] __attribute__((aligned(8), section(".chipbuf")));   /* all zero = end of sprite */
static uint16_t silence[2]     __attribute__((aligned(4), section(".chipbuf")));   /* 4 zero bytes for audio */

/* 256-colour palette shadow: the high and low nibbles of every channel, as written to the colour registers with LOCT = 0 / 1 */
static uint16_t pal_hi[256], pal_lo[256];
static volatile uint8_t pal_pending;
static volatile uint8_t pal_banks;           /* banks of 32 colours that the next vertical blank has to load */

static uint16_t tick_base;
static uint32_t tick_frac, tick_acc;

static volatile uint8_t  kq_head, kq_tail;
static uint8_t  kq[32];
static volatile uint8_t snd_state[4];       /* 0 idle, 1 start pending, 2 playing last pass, 3 looping */

/* ---------------------------------------------------------------- utilities */
static inline uint16_t irq_lock_vbl(void)
{   /* raise IPL mask to >= 3: blocks VERTB (level 3), CIA-B/ports still run */
    uint16_t old;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0300,%%sr" : "=d"(old) : : "memory", "cc");
    return old;
}
static inline uint16_t irq_lock_all6(void)
{   /* mask levels 1..6 (everything the platform uses) */
    uint16_t old;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0600,%%sr" : "=d"(old) : : "memory", "cc");
    return old;
}
static inline void irq_restore(uint16_t sr)
{
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

/* Read the current beam line (0..311) without tearing between VPOSR and VHPOSR. */
static uint16_t beam_line(void)
{
    for (;;) {
        uint16_t a = R16(VPOSR);
        uint16_t b = R16(VHPOSR);
        uint16_t c = R16(VPOSR);
        if (((a ^ c) & 1) == 0)
            return (uint16_t)(((a & 1) << 8) | (b >> 8));
    }
}

/* ---------------------------------------------------------------- hw reset  */
void plat_hw_reset(void)
{
    unsigned i;
    /* Let a Blitter operation that the OS left running finish while DMA is still on. */
    (void)R16(DMACONR);
    for (i = 0; i < 400000u && (R16(DMACONR) & 0x4000); i++) { }

    R16(INTENA) = DMA_ALL;                      /* all interrupts off */
    R16(INTREQ) = DMA_ALL;
    R16(DMACON) = DMA_ALL;                      /* all DMA off */
    R16(ADKCON) = DMA_ALL;                      /* no modulation, no precomp */
    R16(INTREQ) = DMA_ALL;
    R16(INTREQ) = DMA_ALL;

    for (i = 0; i < 4; i++) {                   /* silence Paula */
        R16(AUDx(i, 8)) = 0;                    /* AUDxVOL */
        R16(AUDx(i, 4)) = 1;                    /* AUDxLEN */
        R16(AUDx(i, 6)) = 124;                  /* AUDxPER */
    }
    for (i = 0; i < 8; i++) {                   /* disarm all sprites */
        R16(SPRx(i, 0)) = 0;                    /* POS  */
        R16(SPRx(i, 4)) = 0;                    /* DATA */
        R16(SPRx(i, 6)) = 0;                    /* DATB */
        R16(SPRx(i, 2)) = 0;                    /* CTL (last: disarms) */
    }
    /* AGA state the OS may have left behind: fetch mode, colour bank / LOCT / sprite resolution, bitplane XOR, collision control */
    R16(FMODE)   = 0;
    R16(BPLCON0) = 0x0200;                      /* 0 planes, colour burst on, lores, no lace, ECS/AGA features off */
    R16(BPLCON1) = 0;
    R16(BPLCON2) = 0;
    R16(BPLCON3) = BPLCON3_BASE;
    R16(BPLCON4) = BPLCON4_BASE;
    R16(CLXCON)  = 0;
    R16(CLXCON2) = 0;
    R16(BPL1MOD) = 0;
    R16(BPL2MOD) = 0;
    R16(COLOR00) = 0x0000;
    R16(POTGO)   = PLAT_POTGO_INIT;             /* see platform/README: pot pin configuration */

    /* CIA-A: no interrupts, stop timers, SP = input, OVL=0 (chip RAM at $0) */
    CIAA(C_ICR) = 0x7F;
    CIAA(C_CRA) = 0;
    CIAA(C_CRB) = 0;
    (void)CIAA(C_ICR);
    CIAA(C_DDRA) = 0x03;                        /* bit0 OVL, bit1 LED out; 6,7 = fire in */
    CIAA(C_PRA)  = (uint8_t)(CIAA(C_PRA) & ~0x01u);
    /* CIA-B: no interrupts, stop timers; floppy: deselect all, motor off */
    CIAB(C_ICR) = 0x7F;
    CIAB(C_CRA) = 0;
    CIAB(C_CRB) = 0;
    (void)CIAB(C_ICR);
    CIAB(C_DDRB) = 0xFF;
    CIAB(C_PRB)  = 0xFF;                        /* MTR=1(off) all /SELx=1 */
    CIAB(C_PRB)  = 0xF7;                        /* select DF0 with MTR=1: latches "motor off" */
    CIAB(C_PRB)  = 0xFF;                        /* deselect */
    R16(INTREQ) = DMA_ALL;
}

void plat_restart(void)
{
    extern void _start(void);
    __asm__ volatile("move.w #0x2700,%%sr\n\tjmp _start" : : : "memory");
    __builtin_unreachable();
}

/* ------------------------------------------------------- PAL/NTSC detection */
/* Count the lines of a complete frame by watching the beam counter: the highest
 * line number seen between two wraps is 311/312 on PAL and 261/262 on NTSC. */
static uint16_t measure_lines(void)
{
    uint16_t prev = beam_line(), cur, max = 0;
    unsigned wraps = 0;
    unsigned guard = 0;
    while (wraps < 2 && ++guard < 8000000u) {
        cur = beam_line();
        if (cur + 100u < prev) {                /* frame wrapped to line ~0 */
            wraps++;
            if (wraps == 1) max = 0;
        } else if (wraps == 1 && cur > max) {
            max = cur;
        }
        prev = cur;
    }
    return (uint16_t)(max + 1);                 /* lines per frame */
}

/* ------------------------------------------------------------ keyboard ISR  */
static void kbd_handshake(void)
{
    /* Pull SP low for >= 75 us: CRA bit 6 = output mode.  Wait 3 raster-line
     * changes (>= 128 us at 64 us/line) - independent of CPU speed or timers. */
    unsigned n = 0;
    uint8_t last = (uint8_t)(R16(VHPOSR) >> 8);
    CIAA(C_CRA) = (uint8_t)(CIAA(C_CRA) | 0x40);
    while (n < 3) {
        uint8_t l = (uint8_t)(R16(VHPOSR) >> 8);
        if (l != last) { last = l; n++; }
    }
    CIAA(C_CRA) = (uint8_t)(CIAA(C_CRA) & ~0x40);
}

void plat_isr_ports_c(void)
{
    uint8_t icr = CIAA(C_ICR);                  /* read clears CIA int */
    if (icr & 0x08) {                           /* SP: key byte received */
        uint8_t d = (uint8_t)~CIAA(C_SDR);      /* wire format: ~(code<<1 | up) */
        kbd_handshake();
        uint8_t code = (d >> 1) & 0x7F;
        uint8_t up   = d & 1;
        if (code < 0x78) {                      /* 0x78-0x7F = keyboard status codes */
            uint8_t mask = (uint8_t)(1u << (code & 7));
            uint8_t was  = plat_keymap[code >> 3] & mask;
            if (up) plat_keymap[code >> 3] &= (uint8_t)~mask;
            else { plat_keymap[code >> 3] |= mask; plat_last_key = code; }
            plat_last_event = (uint8_t)(code | (up ? 0x80 : 0));
            plat_key_events++;
            if (up ? (was != 0) : (was == 0)) {     /* real state change: queue it */
                uint8_t h = kq_head, nh = (uint8_t)((h + 1) & 31);
                if (nh != kq_tail) { kq[h] = plat_last_event; kq_head = nh; }
            }
        }
    }
    R16(INTREQ) = INT_PORTS;
}

int plat_key_get(void)
{
    if (kq_tail == kq_head) return -1;
    uint8_t t = kq_tail;
    int v = kq[t];
    kq_tail = (uint8_t)((t + 1) & 31);
    return v;
}

static inline uint32_t tod_read(void)
{
    uint32_t h = CIAB(C_TOD2);                  /* reading hi latches the counter */
    uint32_t m = CIAB(C_TOD1);
    uint32_t l = CIAB(C_TOD0);                  /* reading lo releases the latch */
    return (h << 16) | (m << 8) | l;
}

/* ---------------------------------------------------------------- tick ISR  */
void plat_isr_ciab_c(void)
{
    uint8_t icr = CIAB(C_ICR);                  /* read clears; do this BEFORE INTREQ clear */
    if (icr & 0x01) {                           /* timer A underflow */
        uint16_t p = tick_base;
        plat_tick_count++;
        plat_tick_scanline = tod_read();        /* diagnostic: beam time of this tick */
        tick_acc += tick_frac;
        if (tick_acc >= TICK_DEN) { tick_acc -= TICK_DEN; p++; }
        p--;                                    /* latch = period - 1 */
        CIAB(C_TALO) = (uint8_t)p;              /* running timer: only the latch changes */
        CIAB(C_TAHI) = (uint8_t)(p >> 8);
    }
    R16(INTREQ) = INT_EXTER;
}

/* ---------------------------------------------------------------- VBL ISR   */
/* Bitplane pointers live in the Copper list (reloaded every frame); the buffer never moves. */
static void apply_bpl_ptrs(void)
{
    uint32_t base = (uint32_t)plat_buf;
    for (unsigned p = 0; p < PLAT_DEPTH; p++) {
        uint32_t a = base + p * PLAT_PLANE_STRIDE;
        plat_cop.bpl[p][1] = (uint16_t)(a >> 16);
        plat_cop.bpl[p][3] = (uint16_t)a;
    }
}

__attribute__((weak)) void plat_vbl_hook(void) { }
__attribute__((weak)) void plat_prof_pc(uint32_t pc) { (void)pc; }
__attribute__((weak)) void plat_user_irq3(uint16_t req) { R16(INTREQ) = req; }

/* Load the 256 colour registers.  AGA colour registers are written in 32-colour banks (BPLCON3 BANK) in two passes: LOCT = 0 sets the
 * high nibbles of R, G and B, LOCT = 1 the low nibbles.  Runs in the vertical blank, before the display window starts. */
static void load_palette(unsigned banks)
{
    volatile uint16_t *col = (volatile uint16_t *)(0xDFF000u + COLOR00);
    for (unsigned bank = 0; bank < 8; bank++) {
        if (!(banks & (1u << bank))) continue;
        const uint16_t *hi = &pal_hi[bank * 32], *lo = &pal_lo[bank * 32];
        R16(BPLCON3) = (uint16_t)(BPLCON3_BASE | (bank << 13));
        for (unsigned i = 0; i < 32; i++) col[i] = hi[i];
        R16(BPLCON3) = (uint16_t)(BPLCON3_BASE | (bank << 13) | BPLCON3_LOCT);
        for (unsigned i = 0; i < 32; i++) col[i] = lo[i];
    }
    R16(BPLCON3) = BPLCON3_BASE;
}

void plat_isr_vbl_c(void)
{
    uint16_t other = (uint16_t)(R16(INTREQR) & 0x0050);     /* COPER | BLIT */
    if (other) plat_user_irq3(other);
    if (!(R16(INTREQR) & INT_VERTB)) return;
    R16(INTREQ) = INT_VERTB;
    plat_vbl_count++;
    plat_vbl_hook();                            /* (fast display: install the list prepared by the game, restart the Copper) */
    if (pal_pending) {
        uint8_t banks = pal_banks;
        pal_banks = 0; pal_pending = 0;         /* (a set_palette during the load re-arms it) */
        load_palette(banks);
    }
}

/* ---------------------------------------------------------------- audio ISR */
void plat_isr_audio_c(void)
{
    uint16_t req = R16(INTREQR);
    for (unsigned ch = 0; ch < 4; ch++) {
        uint16_t b = (uint16_t)(INT_AUD0 << ch);
        if (!(req & b)) continue;
        R16(INTREQ) = b;
        if (snd_state[ch] == 1) {               /* first reload: sample is now running */
            R32(AUDx(ch, 0)) = (uint32_t)silence;   /* next pass = silence */
            R16(AUDx(ch, 4)) = 1;
            snd_state[ch] = 2;
        } else if (snd_state[ch] == 2) {        /* sample finished: stop */
            R16(DMACON) = (uint16_t)(1u << ch);
            R16(INTENA) = b;
            R16(AUDx(ch, 8)) = 0;
            snd_state[ch] = 0;
        }
    }
}

void plat_audio_stop(unsigned ch)
{
    ch &= 3;
    R16(INTENA) = (uint16_t)(INT_AUD0 << ch);
    R16(DMACON) = (uint16_t)(1u << ch);
    R16(AUDx(ch, 8)) = 0;
    snd_state[ch] = 0;
    R16(INTREQ) = (uint16_t)(INT_AUD0 << ch);
}

void plat_audio_play(unsigned ch, const void *data, unsigned words,
                     unsigned period, unsigned vol, unsigned flags)
{
    ch &= 3;
    plat_audio_stop(ch);
    if (words == 0) return;
    if (period < 124) period = 124;
    if (vol > 64) vol = 64;
    R32(AUDx(ch, 0)) = (uint32_t)data;
    R16(AUDx(ch, 4)) = (uint16_t)words;
    R16(AUDx(ch, 6)) = (uint16_t)period;
    R16(AUDx(ch, 8)) = (uint16_t)vol;
    if (flags & PLAT_SND_LOOP) {
        snd_state[ch] = 3;
    } else {
        snd_state[ch] = 1;
        R16(INTENA) = (uint16_t)(SETBIT | (INT_AUD0 << ch));
    }
    R16(DMACON) = (uint16_t)(SETBIT | (1u << ch));
}

int  plat_audio_busy(unsigned ch) { return snd_state[ch & 3] != 0; }
void plat_audio_volume(unsigned ch, unsigned vol) { R16(AUDx(ch & 3, 8)) = (uint16_t)(vol > 64 ? 64 : vol); }

/* ---------------------------------------------------------------- video API */
void *plat_back(void)  { return plat_buf; }
void *plat_front(void) { return plat_buf; }

void plat_flip(void) { plat_wait_vbl(); }

void plat_wait_vbl(void)
{
    uint32_t c = plat_vbl_count;
    while (plat_vbl_count == c) { }
}

void plat_wait_tick(void)
{
    uint32_t c = plat_tick_count;
    while (plat_tick_count == c) { }
}

/* 0x00RRGGBB -> high nibbles (LOCT = 0 write) and low nibbles (LOCT = 1 write) as 0x0RGB words */
static inline void set_entry(unsigned i, uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    pal_hi[i] = (uint16_t)(((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
    pal_lo[i] = (uint16_t)(((r & 15) << 8) | ((g & 15) << 4) | (b & 15));
}

void plat_set_palette_banks(const uint32_t rgb[256], unsigned mask)
{
    uint16_t sr = irq_lock_vbl();               /* the ISR cannot run mid-copy */
    for (unsigned b = 0; b < 8; b++)
        if (mask & (1u << b)) for (unsigned i = b * 32; i < b * 32 + 32; i++) set_entry(i, rgb[i]);
    pal_banks |= (uint8_t)mask;
    pal_pending = 1;
    irq_restore(sr);
}

void plat_set_palette256(const uint32_t rgb[256]) { plat_set_palette_banks(rgb, 0xFF); }

void plat_set_palette(const uint16_t rgb[32])
{
    uint16_t sr = irq_lock_vbl();
    for (unsigned i = 0; i < 32; i++) { pal_hi[i] = rgb[i] & 0x0FFF; pal_lo[i] = rgb[i] & 0x0FFF; }
    pal_banks |= 1;
    pal_pending = 1;
    irq_restore(sr);
}

void plat_set_color(unsigned idx, uint16_t rgb)
{
    uint16_t sr = irq_lock_vbl();
    pal_hi[idx & 255] = rgb & 0x0FFF;
    pal_lo[idx & 255] = rgb & 0x0FFF;           /* 4-bit colour: low nibbles = high nibbles (0xF -> 0xFF) */
    pal_banks |= (uint8_t)(1u << ((idx & 255) >> 5));
    pal_pending = 1;
    irq_restore(sr);
}

void plat_clear_buffer(void *buf)
{
    uint32_t *p = (uint32_t *)buf;
    for (unsigned i = 0; i < PLAT_BUF_SIZE / 4; i++) p[i] = 0;
}

uint32_t plat_scanlines(void)
{
    uint16_t sr = irq_lock_all6();              /* the tick ISR reads TOD too: no interleaving */
    uint32_t v = tod_read();
    irq_restore(sr);
    return v;
}

/* -------------------------------------------------------------------- input */
uint16_t plat_joy_bits(void)
{
    uint16_t j = R16(JOY1DAT), b = 0;
    if (j & 0x0200) b |= PIN_LEFT;
    if (j & 0x0002) b |= PIN_RIGHT;
    if ((j ^ (j >> 1)) & 0x0001) b |= PIN_DOWN;     /* bit1 ^ bit0 */
    if ((j ^ (j >> 1)) & 0x0100) b |= PIN_UP;       /* bit9 ^ bit8 */
    if (!(CIAA(C_PRA) & 0x80)) b |= PIN_FIRE;       /* active low */
    if (!(R16(POTGOR) & 0x4000)) b |= PIN_BTN2;     /* DATRY, active low */
    return b;
}

uint16_t plat_input(void)
{
    uint16_t b = plat_joy_bits();
    if (plat_key_down(KEY_5))     b |= PIN_COIN;
    if (plat_key_down(KEY_1))     b |= PIN_START1;
    if (plat_key_down(KEY_2))     b |= PIN_START2;
    if (plat_key_down(KEY_ESC))   b |= PIN_ESC;
    if (plat_key_down(KEY_SPACE)) b |= PIN_SPACE;
    if (plat_key_down(KEY_P))     b |= PIN_PAUSE;
    return b;
}

/* --------------------------------------------------------------------- init */
void plat_init(void)
{
    unsigned i;

    /* 0. AGA required (Lisa: DENISEID low byte 0xF8).  Anything else: solid red screen. */
    if ((R16(DENISEID) & 0xFF) != 0xF8) {
        for (;;) R16(COLOR00) = 0x0F00;
    }

    /* 1. which video standard?  (display DMA is off; the beam counters run anyway) */
    plat_lines  = measure_lines();
    plat_is_pal = (plat_lines >= 286);
    plat_vstart = plat_is_pal ? PLAT_PAL_VSTART : PLAT_NTSC_VSTART;

    /* 2. display buffer (cleared), palette shadow (all black) and the Copper list */
    plat_clear_buffer(plat_buf);
    for (i = 0; i < 8; i++) {
        plat_cop.spr[i][0] = (uint16_t)(SPR0PT + i * 4);
        plat_cop.spr[i][1] = (uint16_t)((uint32_t)null_sprite >> 16);
        plat_cop.spr[i][2] = (uint16_t)(SPR0PT + i * 4 + 2);
        plat_cop.spr[i][3] = (uint16_t)(uint32_t)null_sprite;
    }
    plat_cop.wait[0] = (uint16_t)((PLAT_COP_SETUP_LINE << 8) | 1);   /* WAIT line, h = 0 */
    plat_cop.wait[1] = 0xFFFE;
    for (i = 0; i < PLAT_DEPTH; i++) {
        plat_cop.bpl[i][0] = (uint16_t)(0x0E0 + i * 4);
        plat_cop.bpl[i][2] = (uint16_t)(0x0E2 + i * 4);
    }
    apply_bpl_ptrs();
    plat_cop.end[0] = 0xFFFF;
    plat_cop.end[1] = 0xFFFE;
    pal_banks = 0xFF; pal_pending = 1;          /* load the (black) palette in the first vertical blank */

    /* 3. display window: 256x240, lores, 8 planes, interleaved */
    {
        unsigned vs = plat_vstart, ve = plat_vstart + PLAT_H;
        R16(DIWSTRT) = (uint16_t)(((vs & 0xFF) << 8) | 0xA1);   /* H = 0xA1 */
        R16(DIWSTOP) = (uint16_t)(((ve & 0xFF) << 8) | 0xA1);   /* H = 0x1A1 (bit8 implied) */
        R16(DIWHIGH) = 0x2100;                  /* H8 of DIWSTOP = 1, V8 of DIWSTOP = 1 (stop line 292 PAL / 261 NTSC), start lines < 256 */
        R16(DDFSTRT) = DDF_STRT;
        R16(DDFSTOP) = DDF_STOP;
        R16(FMODE)   = FMODE_VAL;
        R16(BPLCON0) = BPLCON0_8PL;
        R16(BPLCON1) = BPLCON1_VAL;
        R16(BPLCON2) = 0;
        R16(BPLCON3) = BPLCON3_BASE;
        R16(BPLCON4) = BPLCON4_BASE;
        R16(BPL1MOD) = PLAT_STRIDE - PLAT_PLANE_STRIDE;
        R16(BPL2MOD) = PLAT_STRIDE - PLAT_PLANE_STRIDE;
    }
    R32(COP1LC) = (uint32_t)&plat_cop;
    R16(COPJMP1) = 0;

    /* 4. CIA-B timer A = game tick; CIA-B TOD = scanline counter */
    {
        const unsigned t = plat_is_pal ? 1 : 0;
        tick_base = tick_parm[t].base;
        tick_frac = tick_parm[t].frac;
        tick_acc  = 0;
        uint16_t latch = (uint16_t)(tick_base - 1);
        CIAB(C_CRB)  = 0x00;                    /* TOD write mode (ALARM bit = 0) */
        CIAB(C_TOD2) = 0; CIAB(C_TOD1) = 0; CIAB(C_TOD0) = 0;   /* writing lo starts it */
        CIAB(C_TALO) = (uint8_t)latch;
        CIAB(C_TAHI) = (uint8_t)(latch >> 8);
        CIAB(C_CRA)  = 0x11;                    /* continuous, force-load, START */
        (void)CIAB(C_ICR);
        CIAB(C_ICR)  = 0x81;                    /* enable timer A interrupt */
    }
    /* 5. keyboard: release any pending handshake, enable SP interrupt */
    kbd_handshake();
    (void)CIAA(C_ICR);
#ifdef PROFILE                                  /* PC sampling at ~573 Hz, not commensurate with the game tick */
    CIAA(C_TALO) = 1237 & 255; CIAA(C_TAHI) = 1237 >> 8; CIAA(C_CRA) = 0x11;
    CIAA(C_ICR) = 0x89;
#else
    CIAA(C_ICR) = 0x88;
#endif

    /* 6. DMA + interrupts on */
    R16(INTREQ) = DMA_ALL;
    #ifdef PLAT_NODMA                              /* experiment: no bitplane DMA (measures the DMA share of the CPU slowdown) */
    R16(DMACON) = SETBIT | DMAEN | COPEN | BLTEN;
#else
    R16(DMACON) = SETBIT | DMAEN | BPLEN | COPEN | BLTEN;
#endif   /* sprite/audio/disk DMA stay off */
    R16(INTENA) = SETBIT | INT_INTEN | INT_EXTER | INT_VERTB | INT_PORTS;
    __asm__ volatile("move.w #0x2000,%%sr" : : : "memory");
}
