/* apu.c - 2A07 APU model (PAL): register writes with CPU-cycle stamps are replayed in order together with the frame counter
 * (envelopes, linear counter, length counters, sweeps); the result at every frame boundary is handed to the platform as channel state
 * (period, volume, duty), which drives Paula directly (platform/audio.c).  No samples are computed here.
 * Included by nes_rt.c.  Tables: length table and PAL noise periods from the 2A07 documentation / MAME nes_apu.
 */
static const u8 apu_len[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14, 12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const u16 apu_noise_pal[16] = { 4,8,14,30,60,88,118,148,188,236,354,472,708,944,1890,3778 };
struct ApuCh { u8 reg[4]; u8 len, env_start, env_div, env_decay, enabled, sw_div, sw_reload, lin, lin_reload; };
static struct ApuCh ach[4];
static u8 apu_dac, apu_mode5, apu_irq_inhibit;
static u32 apu_next_tick, apu_tick_idx;               /* cycle of the next frame counter step and its index */
static u32 apu_cyc_done;                              /* everything before this cycle has been applied */
struct ApuEv { u32 cyc; u8 reg, val; };
static struct ApuEv apu_ev[1024]; static int apu_nev, apu_evpos;
static u8 apu_retrig[4];

static void apu_quarter(void) {
    for (int c = 0; c < 4; c++) {
        struct ApuCh *a = &ach[c];
        if (c == 2) {                                   /* triangle linear counter */
            if (a->lin_reload) a->lin = a->reg[0] & 0x7f; else if (a->lin) a->lin--;
            if (!(a->reg[0] & 0x80)) a->lin_reload = 0;
            continue;
        }
        int vreg = c == 3 ? 0 : 0;
        u8 r0 = a->reg[vreg];
        if (a->env_start) { a->env_start = 0; a->env_decay = 15; a->env_div = r0 & 15; }
        else if (a->env_div == 0) { a->env_div = r0 & 15; if (a->env_decay) a->env_decay--; else if (r0 & 0x20) a->env_decay = 15; }
        else a->env_div--;
    }
}
static int sweep_target(int c) {
    struct ApuCh *a = &ach[c]; int per = a->reg[2] | ((a->reg[3] & 7) << 8), d = per >> (a->reg[1] & 7);
    if (a->reg[1] & 8) return per - d - (c == 0 ? 1 : 0);
    return per + d;
}
static void apu_half(void) {
    for (int c = 0; c < 4; c++) {
        struct ApuCh *a = &ach[c];
        int halt = c == 2 ? (a->reg[0] & 0x80) : (a->reg[0] & 0x20);
        if (a->len && !halt) a->len--;
        if (c < 2) {
            int per = a->reg[2] | ((a->reg[3] & 7) << 8);
            if (a->sw_div == 0 && (a->reg[1] & 0x80) && (a->reg[1] & 7) && per >= 8 && sweep_target(c) <= 0x7ff) { int t = sweep_target(c); a->reg[2] = (u8)t; a->reg[3] = (u8)((a->reg[3] & ~7) | ((t >> 8) & 7)); }
            if (a->sw_div == 0 || a->sw_reload) { a->sw_div = (a->reg[1] >> 4) & 7; a->sw_reload = 0; } else a->sw_div--;
        }
    }
}
static void apu_tick(void) {                             /* PAL frame counter: 4 step 8313/16627/24939/33253, 5 step adds 41565 */
    static const u8 q4[4] = { 1, 1, 1, 1 }, h4[4] = { 0, 1, 0, 1 }; (void)q4;
    int st = apu_tick_idx;
    if (!apu_mode5) { apu_quarter(); if (h4[st & 3]) apu_half(); apu_tick_idx = (st + 1) & 3; }
    else { if (st != 3) apu_quarter(); if (st == 1 || st == 4) apu_half(); apu_tick_idx = st == 4 ? 0 : st + 1; }
}
static u32 apu_step_cycles(void) {
    static const u16 s4[4] = { 8313, 8314, 8312, 8314 }, s5[5] = { 8313, 8314, 8312, 8314 /* step 3 does nothing */, 8312 };
    return apu_mode5 ? s5[apu_tick_idx % 5] : s4[apu_tick_idx & 3];
}
static void apu_apply(const struct ApuEv *e) {
    int r = e->reg, v = e->val;
    if (r < 0x10) {
        int c = r >> 2, k = r & 3; struct ApuCh *a = &ach[c];
        if (c == 2 && k == 1) return;
        if (c == 3 && (k == 1)) return;
        a->reg[k] = (u8)v;
        if (k == 1 && c < 2) a->sw_reload = 1;
        if (k == 3) {                                           /* length load */
            if (a->enabled) a->len = apu_len[v >> 3];
            if (c < 2 || c == 3) a->env_start = 1;
            if (c == 2) a->lin_reload = 1;
            apu_retrig[c] = 1;
        }
    } else if (r == 0x11) apu_dac = (u8)(v & 0x7f);
    else if (r == 0x15) { for (int c = 0; c < 4; c++) { ach[c].enabled = (v >> c) & 1; if (!ach[c].enabled) ach[c].len = 0; } }
    else if (r == 0x17) {
        apu_mode5 = (v >> 7) & 1; apu_irq_inhibit = (v >> 6) & 1;
        apu_tick_idx = 0; apu_next_tick = e->cyc + 8313 + 3;
        if (apu_mode5) { apu_quarter(); apu_half(); }
    }
}
void apu_write(u32 cyc, u8 reg, u8 val) {
    if (apu_nev == 1024) return;
    apu_ev[apu_nev].cyc = cyc; apu_ev[apu_nev].reg = reg; apu_ev[apu_nev].val = val; apu_nev++;
}
static void apu_rebase(u32 amt) { for (int i = apu_evpos; i < apu_nev; i++) apu_ev[i].cyc -= amt; apu_next_tick -= amt; apu_cyc_done -= amt; }
/* apply everything up to 'cyc' and describe the state */
void apu_run_to(u32 cyc, struct ApuOut *o) {
    if (!apu_next_tick) apu_next_tick = 8313 + 7;
    for (;;) {
        int have_ev = apu_evpos < apu_nev && apu_ev[apu_evpos].cyc <= cyc;
        if (have_ev && apu_ev[apu_evpos].cyc <= apu_next_tick) { apu_apply(&apu_ev[apu_evpos]); apu_evpos++; continue; }
        if (apu_next_tick <= cyc) { apu_tick(); apu_next_tick += apu_step_cycles(); continue; }
        if (have_ev) { apu_apply(&apu_ev[apu_evpos]); apu_evpos++; continue; }
        break;
    }
    if (apu_evpos == apu_nev) apu_evpos = apu_nev = 0;
    apu_cyc_done = cyc;
    memset(o, 0, sizeof *o);
    for (int c = 0; c < 2; c++) {
        struct ApuCh *a = &ach[c]; int per = a->reg[2] | ((a->reg[3] & 7) << 8);
        int on = a->len && per >= 8 && sweep_target(c) <= 0x7ff;
        o->per[c] = (u16)per; o->duty[c] = a->reg[0] >> 6;
        o->vol[c] = on ? ((a->reg[0] & 0x10) ? (a->reg[0] & 15) : a->env_decay) : 0;
    }
    { struct ApuCh *a = &ach[2]; int per = a->reg[2] | ((a->reg[3] & 7) << 8); o->per[2] = (u16)per; o->vol[2] = (a->len && a->lin && per >= 2) ? 15 : 0; }
    { struct ApuCh *a = &ach[3]; o->per[3] = apu_noise_pal[a->reg[2] & 15]; o->noise_short = a->reg[2] >> 7;
      o->vol[3] = a->len ? ((a->reg[0] & 0x10) ? (a->reg[0] & 15) : a->env_decay) : 0; }
    o->dac = apu_dac;
    for (int c = 0; c < 4; c++) { o->retrig[c] = apu_retrig[c]; apu_retrig[c] = 0; }
}
