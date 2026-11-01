/* audio.c - APU channel state (rt/apu.c) -> Paula.  Each NES channel is a looping hardware waveform on one Paula channel:
 *   pulse 1 -> AUD0 (left), pulse 2 -> AUD1 (right), triangle -> AUD2 (right), noise -> AUD3 (left)
 * Pulse: 8 samples per duty cycle, Paula period = (t+1) * 2*3546895/1662607.  Triangle: 32 samples (16/8 for high pitches).
 * Noise: the 15-bit LFSR sequence (or the 93 step one) as a 32767 byte loop, decimated for periods below Paula's 124 colour clock minimum.
 * Volumes follow the nonlinear mixer's relative levels (pulse 15 : triangle 15 : noise 15 = 0.149 : 0.246 : 0.174).  The register
 * values of a frame are applied once per frame (envelope steps inside a frame are not heard individually).  $4011 (DMC level) is not played.
 * Paula cannot mix to mono: the channels are panned as above. */
#include <stdint.h>
#include "platform.h"
#include "../rt/rt.h"
#define R16(o) (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o) (*(volatile uint32_t *)(0xDFF000u + (o)))
#define AUD(ch, o) (0xA0 + (ch) * 0x10 + (o))
#define DMACON 0x096
static int8_t wpulse[4][8] __attribute__((section(".chipbuf"), aligned(4)));
static int8_t wtri32[32] __attribute__((section(".chipbuf"), aligned(4)));
static int8_t wtri16[16] __attribute__((section(".chipbuf"), aligned(4)));
static int8_t wtri8[8] __attribute__((section(".chipbuf"), aligned(4)));
static int8_t wsil[4] __attribute__((section(".chipbuf"), aligned(4)));
static int8_t lfsr_long[32768] __attribute__((section(".chipbuf"), aligned(4)));      /* 32767 used (+1 pad: whole words) */
static int8_t lfsr_short[96] __attribute__((section(".chipbuf"), aligned(4)));
static int8_t noise_dec[32768] __attribute__((section(".chipbuf"), aligned(4)));
static unsigned cur_noise_key = ~0u;
static const uint8_t duty_seq[4][8] = { {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
uint32_t aud_clamped;                                  /* number of times a period had to be clamped to Paula's minimum */

void audio_init(void)
{
    for (int d = 0; d < 4; d++) for (int i = 0; i < 8; i++) wpulse[d][i] = duty_seq[d][i] ? 127 : -128;
    for (int i = 0; i < 32; i++) { int v = i < 16 ? 15 - i : i - 16; wtri32[i] = (int8_t)(v * 17 - 128); }   /* 15..0..15 -> +127..-128 */
    for (int i = 0; i < 16; i++) wtri16[i] = wtri32[i * 2];
    for (int i = 0; i < 8; i++) wtri8[i] = wtri32[i * 4];
    uint32_t l = 1;
    for (int i = 0; i < 32767; i++) { lfsr_long[i] = (l & 1) ? -128 : 127; uint32_t fb = (l ^ (l >> 1)) & 1; l = (l >> 1) | (fb << 14); }
    lfsr_long[32767] = lfsr_long[0];
    l = 1;
    for (int i = 0; i < 93; i++) { lfsr_short[i] = (l & 1) ? -128 : 127; uint32_t fb = (l ^ (l >> 6)) & 1; l = (l >> 1) | (fb << 14); }
    for (int ch = 0; ch < 4; ch++) { R32(AUD(ch, 0)) = (uint32_t)wsil; R16(AUD(ch, 4)) = 2; R16(AUD(ch, 6)) = 400; R16(AUD(ch, 8)) = 0; }
    R16(DMACON) = 0x8200 | 0x000F;                    /* DMAEN + AUD0-3 */
}
static void setch(int ch, const void *w, unsigned bytes, unsigned per, unsigned vol)
{
    if (per < 124) { per = 124; aud_clamped++; }
    if (per > 65535) per = 65535;
    R32(AUD(ch, 0)) = (uint32_t)w; R16(AUD(ch, 4)) = (uint16_t)(bytes / 2); R16(AUD(ch, 6)) = (uint16_t)per; R16(AUD(ch, 8)) = (uint16_t)vol;
}
/* period in Paula clocks for 'cyc' CPU cycles per sample: cyc * 3546895 / 1662607 (rounded) */
static unsigned cpu2paula(unsigned cyc) { return (cyc * 139810u + 32768u) >> 16; }     /* 3546895/1662607 in 16.16 (cyc < 30000) */

void plat_apu(const struct ApuOut *o, uint32_t frame)
{
    (void)frame;
    for (int c = 0; c < 2; c++) {                                    /* pulse */
        unsigned v = o->vol[c], t = o->per[c];
        if (!v || t < 8) { setch(c, wsil, 4, 400, 0); continue; }
        setch(c, wpulse[o->duty[c]], 8, cpu2paula(2 * (t + 1)), (v * 39) / 15);
    }
    if (!o->vol[2] || o->per[2] < 2) setch(2, wsil, 4, 400, 0);       /* triangle */
    else {
        unsigned t = o->per[2] + 1, p = cpu2paula(t);                 /* 32 samples per cycle: one sample per (t+1) cycles */
        if (p >= 124) setch(2, wtri32, 32, p, 64);
        else if (p * 2 >= 124) setch(2, wtri16, 16, p * 2, 64);
        else setch(2, wtri8, 8, p * 4, 64);
    }
    if (!o->vol[3]) setch(3, wsil, 4, 400, 0);                       /* noise */
    else {
        unsigned P = o->per[3], p = cpu2paula(P), m = 1;
        while (p * m < 124) m++;
        const int8_t *w = o->noise_short ? lfsr_short : lfsr_long; unsigned bytes = o->noise_short ? 93 : 32768;
        if (m > 1 && !o->noise_short) {
            unsigned key = m;
            if (key != cur_noise_key) { cur_noise_key = key; unsigned idx = 0; for (unsigned i = 0; i < 32767; i++) { noise_dec[i] = lfsr_long[idx]; idx += m; if (idx >= 32767) idx -= 32767; } noise_dec[32767] = noise_dec[0]; }
            w = noise_dec;
        }
        if (o->noise_short) bytes = 94;
        setch(3, w, bytes, p * m, (o->vol[3] * 45) / 15);
    }
}
