/* fastr.c - fast frame renderer (included by nes_rt.c).
 *
 * The background is kept as a planar bitmap of the two nametables side by side (512 px) plus a copy of nametable 0 at 512..767 (so that a
 * 256 px window never wraps), 4 interleaved bit planes (playfield 1).  Colour index per pixel (4 bits): bit0,1 = pattern pixel, bit2,3 = palette number.
 * Sprites live in a second playfield (dual playfield mode, screen coordinates, 4 planes, index = palette*4 + pixel, 0 = transparent).
 * The display shows the bitmap through hardware scroll: one "band" per run of lines with the same scroll; lines whose fine X scroll changes in the
 * middle of the line (the game does this at the two status-bar splits) get a private 256 px line.  Sprites are drawn into the bitmap rows (or the
 * private lines) in OAM priority order and removed again next frame by redrawing the cells under them.
 * Applicability is checked for every frame (fr_build); if a frame does not fit, the caller replays the segment log with the exact slow renderer.
 * Exactness is verified by reconstructing the frame from the planes (fr_reconstruct) and comparing it with the slow renderer (qemu harness, FR_VERIFY).
 */
#define FR_BW   100                        /* bytes per plane row: 800 px (768 + guard) */
#define FR_ROWB (FR_BW * 4)
#define SL_BW   48                         /* sprite layer: 4 planes of 384 px (256 + guard) per line, screen coordinates.  Two groups of two planes:
                                            * plane 0, 1 interleaved line by line (line l: plane 0 at l*96, plane 1 at l*96+48), then planes 2, 3 the same way at +SL_GRP.
                                            * Every group is a stack of rows of SL_BW bytes, so one blit can draw the two planes of a sprite. */
#define SL_ROWB (SL_BW * 2)
#define SL_GRP  (240 * SL_ROWB)
#define FR_ROWS 240
#define FR_MAXBANDS 16
#define FR_MAXPRIV  4
u8 *fr_bmp;                                /* FR_ROWS * FR_ROWB bytes, set by the platform */
u8 *fr_priv;                               /* FR_MAXPRIV private lines of FR_ROWB bytes each */
u8 *fr_spr;                                /* 240 * SL_ROWB bytes: sprite playfield (colour index 0 = transparent), the buffer shown by the last rendered frame */
u8 *fr_var;                                /* platform (chip RAM, FR_VARBYTES): tile variants for the blitter, then the constant palette-plane rows */
u8 *fr_tmp[2];                             /* platform (chip RAM, 64 * 192 bytes each): sources of sprites that need per-row masks, one pool per frame parity */
#define FR_VARSZ 192                       /* mask rows (16 x 4 bytes), the same again (so that the second job of a sprite finds its A pointer where the first left it), low/high plane rows */
#define FR_SVAR  (1024 * FR_VARSZ + 256)   /* sprite variants of one pattern table (tile, flips) and the four constant palette-plane patterns */
#define FR_CMASK (2 * 64 * 64)             /* cell masks: 64 copies of the 32-row mask for even and for odd columns */
#define FR_CVAR  (2048 * 128)              /* cell data of one pattern table: (tile, palette, column parity), twice in a row */
#define FR_VARBYTES (FR_SVAR + FR_CMASK + FR_CVAR)
struct FrBand { u16 line0, nlines, x0, row0; u8 *base; };
struct BlJob { u8 *d; const u8 *a, *b; u8 rows, sh, wid, kind, tag; };   /* kind 0: sprite cookie cut, 1: clear, 2: background cell cookie cut; wid = width in words (1 or 2) */
u8 *fr_bmp_chip, *fr_priv_chip;            /* platform: the displayed copies of fr_bmp / fr_priv (chip RAM); fr_bmp / fr_priv themselves are the CPU's working copies */
struct FrBand fr_bands[FR_MAXBANDS + 2]; int fr_nbands;
static u8 fr_cell_dirty[30 * 64]; static u8 fr_row_dirty[30]; static u8 fr_all_dirty = 1; static u8 fr_bmp_table = 0xff;
static u8 fr_have_bmp;
#if defined(FR_VERIFY) || defined(FR_STATS)
#define ST(x) x
#else
#define ST(x)
#endif                                      /* statistics: only in verifying / statistics builds (every counter is a read-modify-write in slow memory) */
u32 fr_stat_cpuspr, fr_stat_behind, fr_stat_ovfframes, fr_stat_cell_bad;
#define behind_dbg(a) ((a) & 0x20)
u32 fr_stat_ok, fr_stat_fallback, fr_stat_cells, fr_stat_sprites, fr_stat_rows, fr_stat_drawn;

/* ---- background cells ------------------------------------------------------------------------------------------------ */
static void fr_nt_write(u16 a) {                       /* a: PPU address $2000-$3EFF */
    int nti = (a >> 10) & 1, off = a & 0x3ff;
    if (off < 960) { fr_cell_dirty[(off >> 5) * 64 + nti * 32 + (off & 31)] = 1; fr_row_dirty[off >> 5] = 1; }
    else { int o = off - 960, r0 = (o >> 3) * 4, c0 = (o & 7) * 4; for (int r = r0; r < r0 + 4 && r < 30; r++) { fr_row_dirty[r] = 1; for (int c = c0; c < c0 + 4; c++) fr_cell_dirty[r * 64 + nti * 32 + c] = 1; } }
}
static u8 fr_exp[2][256][24], fr_zero8[8]; static u8 fr_exp_init;         /* per tile and pattern table: low plane, high plane, "any bit" plane (8 rows each) */
static void fr_exp_build(void) {
    for (int t = 0; t < 2; t++) for (int id = 0; id < 256; id++) {
        const u8 *c = chr + t * 0x1000 + id * 16; u8 *e = fr_exp[t][id];
        for (int r = 0; r < 8; r++) { e[r] = c[r]; e[8 + r] = c[r + 8]; e[16 + r] = c[r] | c[r + 8]; }
    }
    fr_exp_init = 1;
}
#define FR_CELL_ROW(d, r, e, s2, s3, o) do { (d)[(r) * FR_ROWB + (o)] = (e)[r]; (d)[(r) * FR_ROWB + FR_BW + (o)] = (e)[8 + (r)]; \
    (d)[(r) * FR_ROWB + 2 * FR_BW + (o)] = (s2)[r]; (d)[(r) * FR_ROWB + 3 * FR_BW + (o)] = (s3)[r]; } while (0)
/* ---- background cells: the CPU keeps the exact bitmap (fr_bmp, fast RAM: the sprite code reads it) and queues one blit per changed cell that copies the cell into the displayed
 * bitmap (fr_bmp_chip).  A cell is 8 pixels = one byte wide in 4 planes x 8 rows; the planes are 100 bytes apart and the rows 400, so the 32 plane rows of a cell are
 * 100 bytes apart: one blit of 32 rows, one word wide, with a half-word mask (cookie cut: A = byte mask, B = cell data placed in that byte). */
#define FR_MAXCJOBS 400
static struct BlJob fr_cjobs[FR_MAXCJOBS]; int fr_ncjobs; u32 fr_dbg[8];   /* fr_dbg: bulk copies, lists lost, cell jobs, sprite jobs, ... (HUD) */
static u8 fr_bulk, fr_cj_ovf, fr_cm_idx[2], fr_cvar_ok[2048], fr_cvar_tbl = 0xff, fr_cm_init;
__attribute__((weak)) void fr_bmp_copy_all(void) { memcpy(fr_bmp_chip, fr_bmp, 240 * FR_ROWB); }      /* platform: waits for the blitter first */
__attribute__((weak)) void fr_priv_flush(int n) { (void)n; }
__attribute__((weak)) int fr_jobs_executed(void) { return 1; }
static const u8 *fr_cell_var(int id, int pal, int par);
static void fr_cell_job(int col, int row, int id, int pal) {
    if (fr_bulk) return;
    if (fr_ncjobs + 2 > FR_MAXCJOBS) { fr_cj_ovf = 1; return; }
    int par = col & 1; const u8 *v = fr_cell_var(id, pal, par);
    u8 *d = fr_bmp_chip + row * 8 * FR_ROWB + (col & ~1);
    const u8 *m = fr_var + FR_SVAR + par * (64 * 64);
    struct BlJob *j = &fr_cjobs[fr_ncjobs++]; j->kind = 2; j->rows = 32; j->sh = 0; j->wid = 1; j->d = d; j->tag = (u8)par;
    j->a = m + (fr_cm_idx[par]++ & 63) * 64; j->b = v;
    if (col < 32) { struct BlJob *k = &fr_cjobs[fr_ncjobs++]; *k = *j; k->d = d + 64; k->a = m + (fr_cm_idx[par]++ & 63) * 64; k->b = v + 64; }
}
static const u8 *fr_cell_var(int id, int pal, int par) {
    u8 *base = fr_var + FR_SVAR + FR_CMASK;
    if (fr_cvar_tbl != fr_bmp_table) { fr_cvar_tbl = fr_bmp_table; fr_dbg[7]++; memset(fr_cvar_ok, 0, sizeof fr_cvar_ok); }
    int idx = (id * 4 + pal) * 2 + par; u8 *v = base + idx * 128;
    if (!fr_cvar_ok[idx]) {
        const u8 *e = fr_exp[fr_bmp_table][id]; u8 t[128]; memset(t, 0, sizeof t);
        for (int r = 0; r < 8; r++) {
            u8 nz = e[16 + r], pv[4] = { e[r], e[8 + r], (pal & 1) ? nz : 0, (pal & 2) ? nz : 0 };
            for (int p = 0; p < 4; p++) { int i = r * 4 + p; t[i * 2 + (par ? 1 : 0)] = pv[p]; t[64 + i * 2 + (par ? 1 : 0)] = pv[p]; }
        }
        const u32 *sp = (const u32 *)t; u32 *dp = (u32 *)v; for (int i = 0; i < 32; i++) dp[i] = sp[i];
        fr_cvar_ok[idx] = 1;
    }
    return v;
}
static void fr_cell_masks_init(void) {
    if (fr_cm_init) return; fr_cm_init = 1;
    u8 *m = fr_var + FR_SVAR; for (int par = 0; par < 2; par++) for (int c = 0; c < 64; c++) for (int i = 0; i < 32; i++) { m[par * 4096 + c * 64 + i * 2] = par ? 0x00 : 0xFF; m[par * 4096 + c * 64 + i * 2 + 1] = par ? 0xFF : 0x00; }
}
static void fr_cell_draw(int col, int row) {
    int nti = col >> 5, cx = col & 31;
    u8 id = nt[nti][row * 32 + cx], at = nt[nti][960 + (row >> 2) * 8 + (cx >> 2)];
    int pal = (at >> (((row & 2) << 1) | (cx & 2))) & 3;
    const u8 *e = fr_exp[fr_bmp_table][id];
    const u8 *s2 = (pal & 1) ? e + 16 : fr_zero8, *s3 = (pal & 2) ? e + 16 : fr_zero8;
    u8 *d = fr_bmp + row * 8 * FR_ROWB + col;
    if (col < 32) {
        FR_CELL_ROW(d, 0, e, s2, s3, 0); FR_CELL_ROW(d, 1, e, s2, s3, 0); FR_CELL_ROW(d, 2, e, s2, s3, 0); FR_CELL_ROW(d, 3, e, s2, s3, 0);
        FR_CELL_ROW(d, 4, e, s2, s3, 0); FR_CELL_ROW(d, 5, e, s2, s3, 0); FR_CELL_ROW(d, 6, e, s2, s3, 0); FR_CELL_ROW(d, 7, e, s2, s3, 0);
        FR_CELL_ROW(d, 0, e, s2, s3, 64); FR_CELL_ROW(d, 1, e, s2, s3, 64); FR_CELL_ROW(d, 2, e, s2, s3, 64); FR_CELL_ROW(d, 3, e, s2, s3, 64);
        FR_CELL_ROW(d, 4, e, s2, s3, 64); FR_CELL_ROW(d, 5, e, s2, s3, 64); FR_CELL_ROW(d, 6, e, s2, s3, 64); FR_CELL_ROW(d, 7, e, s2, s3, 64);
    } else {
        FR_CELL_ROW(d, 0, e, s2, s3, 0); FR_CELL_ROW(d, 1, e, s2, s3, 0); FR_CELL_ROW(d, 2, e, s2, s3, 0); FR_CELL_ROW(d, 3, e, s2, s3, 0);
        FR_CELL_ROW(d, 4, e, s2, s3, 0); FR_CELL_ROW(d, 5, e, s2, s3, 0); FR_CELL_ROW(d, 6, e, s2, s3, 0); FR_CELL_ROW(d, 7, e, s2, s3, 0);
    }
    ST(fr_stat_cells++;) fr_cell_job(col, row, id, pal);
}
static void fr_cells_update(u8 table) {
    if (!fr_exp_init) fr_exp_build();
#ifdef EXPCHK
    { extern void c_abort(int, u32); for (int t = 0; t < 2; t++) for (int id = 0; id < 256; id++) for (int r = 0; r < 8; r++) { const u8 *c = chr + t * 0x1000 + id * 16; if (fr_exp[t][id][r] != c[r] || fr_exp[t][id][8 + r] != c[r + 8] || fr_exp[t][id][16 + r] != (c[r] | c[r + 8])) c_abort(2, 0); } }
#endif
    fr_cell_masks_init();
    if (fr_jobs_executed()) { fr_ncjobs = 0; fr_cj_ovf = 0; }                      /* (jobs of a list that never ran stay queued) */
    if (!fr_have_bmp || table != fr_bmp_table) { fr_bmp_table = table; fr_all_dirty = 1; fr_have_bmp = 1; }
    if (fr_all_dirty) {                                                              /* everything: draw the working copy and copy it to the display bitmap */
        fr_bulk = 1; for (int r = 0; r < 30; r++) for (int c = 0; c < 64; c++) fr_cell_draw(c, r); fr_bulk = 0;
        memset(fr_cell_dirty, 0, sizeof fr_cell_dirty); memset(fr_row_dirty, 0, sizeof fr_row_dirty); fr_all_dirty = 0;
        fr_dbg[0]++; fr_bmp_copy_all(); fr_ncjobs = 0; fr_cj_ovf = 0; return;
    }
    for (int r = 0; r < 30; r++) { if (!fr_row_dirty[r]) continue; fr_row_dirty[r] = 0; for (int c = 0; c < 64; c++) if (fr_cell_dirty[r * 64 + c]) { fr_cell_dirty[r * 64 + c] = 0; fr_cell_draw(c, r); } }
    if (fr_cj_ovf) { fr_dbg[0]++; fr_bmp_copy_all(); fr_ncjobs = 0; fr_cj_ovf = 0; }
}

/* ---- per-frame description: bands ----------------------------------------------------------------------------------------- */
static u8 fr_rev[256]; static u8 fr_rev_init;
static u8 fr_mask_spr_left, fr_spr_table;
static u8 fr_lineband[240];                  /* band index of every line */
static int fr_special_xs[FR_MAXBANDS + 2];   /* private line: x < xs uses xa, x >= xs uses xb (bitmap x of the window start) */
static u16 fr_special_xa[FR_MAXBANDS + 2], fr_special_xb[FR_MAXBANDS + 2], fr_special_row[FR_MAXBANDS + 2];
static u8 fr_band_special[FR_MAXBANDS + 2];
static int fr_ovf; int fr_why;

static inline u16 fr_vinc(u16 vv) {
    if ((vv & 0x7000) != 0x7000) return vv + 0x1000;
    vv &= ~0x7000; int y = (vv & 0x3e0) >> 5;
    if (y == 29) { y = 0; vv ^= 0x800; } else if (y == 31) y = 0; else y++;
    return (u16)((vv & ~0x3e0) | (y << 5));
}
/* append n lines starting at 'line' that show bitmap row 'row' (consecutive rows) at window start x0; merges with the previous band when it continues it */
static void fr_band_add(int line, int n, int x0, int row) {
    if (fr_nbands) {
        struct FrBand *p = &fr_bands[fr_nbands - 1];
        if (!fr_band_special[fr_nbands - 1] && p->x0 == x0 && p->line0 + p->nlines == line && (p->row0 + p->nlines) % 240 == row) { p->nlines += n; return; }
    }
    if (fr_nbands >= FR_MAXBANDS) { fr_ovf = 1; return; }
    struct FrBand *b = &fr_bands[fr_nbands]; b->line0 = line; b->nlines = n; b->x0 = x0; b->row0 = row; b->base = fr_bmp + row * FR_ROWB;
    fr_band_special[fr_nbands] = 0; fr_nbands++;
}
/* a line whose fine X scroll changes: gets a private line (planes composed later) */
static void fr_band_special_add(int line, int row, int xa, int xb, int xs) {
    if (fr_nbands >= FR_MAXBANDS) { fr_ovf = 1; return; }
    struct FrBand *b = &fr_bands[fr_nbands]; b->line0 = line; b->nlines = 1; b->x0 = 0; b->row0 = row; b->base = 0;
    fr_band_special[fr_nbands] = 1; fr_special_xa[fr_nbands] = xa; fr_special_xb[fr_nbands] = xb; fr_special_xs[fr_nbands] = xs; fr_special_row[fr_nbands] = row; fr_nbands++;
}

/* build the bands from the segment log; returns 0 if this frame needs the exact renderer */
static int fr_fail(int line) { fr_why = line; return 0; }
static int fr_build(const struct Seg *log, int n) {
    int have_first = 0; u8 ctrl0 = 0;
    int next_line = 0;                        /* the next line that has to start */
    int cur_line = -1, cov = 0;               /* line being assembled from partial segments, x covered so far */
    u16 cur_row = 0, cur_xh = 0, cur_x0 = 0, cur_finex = 0; int cur_special = 0, cur_xs = 0, cur_x1 = 0;
    fr_nbands = 0; fr_ovf = 0;
#define FLUSH_CUR() do { if (cur_line >= 0) { if (cov != 256) return fr_fail(__LINE__); \
        if (cur_special) fr_band_special_add(cur_line, cur_row, cur_x0, cur_x1, cur_xs); else fr_band_add(cur_line, 1, cur_x0, cur_row); cur_line = -1; } } while (0)
    for (int i = 0; i < n; i++) {
        const struct Seg *r = &log[i];
        if (r->scan >= 240) continue;
        if ((r->mask & 0xE1) || (r->mask & 0x1A) != 0x1A) return fr_fail(__LINE__);      /* bg+sprites on, bg left column shown, no grey/emphasis */
        if (!have_first) { have_first = 1; ctrl0 = r->ctrl; fr_mask_spr_left = r->mask & 4; }
        if ((r->ctrl & 0x38) != (ctrl0 & 0x38) || (r->mask & 4) != fr_mask_spr_left) return fr_fail(__LINE__);
        if (r->ctrl & 0x20) return fr_fail(__LINE__);                                    /* 8x16 sprites */
        if (r->nl) {                                                     /* run of complete lines */
            FLUSH_CUR();
            if (r->scan != next_line) return fr_fail(__LINE__);
            u16 vv = r->v & 0x7be0; int cy = (vv >> 5) & 31; if (cy >= 30) return fr_fail(__LINE__);
            u16 hh = r->v & 0x41f, hhn = r->t & 0x41f;
            u16 xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8)), xhn = (u16)(((hhn & 31) << 3) + (((hhn >> 10) & 1) << 8));
            int row = cy * 8 + ((vv >> 12) & 7);
            fr_band_add(r->scan, 1, (xh + r->finex) & 511, row);
            if (r->nl > 1) { int row1 = row + 1; if (row1 == 240) row1 = 0; fr_band_add(r->scan + 1, r->nl - 1, (xhn + r->finex) & 511, row1); }
            next_line = r->scan + r->nl; continue;
        }
        int xa = r->d0 < 1 ? 0 : r->d0 - 1, xb = r->d1 > 257 ? 256 : r->d1 - 1; if (xb <= xa) continue;
        if (r->scan != cur_line) {
            FLUSH_CUR();
            if (r->scan != next_line || xa != 0) return fr_fail(__LINE__);
            u16 hh = r->v & 0x41f, vv = r->v & 0x7be0; int cy = (vv >> 5) & 31; if (cy >= 30) return fr_fail(__LINE__);
            cur_line = r->scan; cur_row = (u16)(cy * 8 + ((vv >> 12) & 7)); cur_xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8));
            cur_finex = r->finex; cur_x0 = (u16)((cur_xh + r->finex) & 511); cur_special = 0; cur_xs = 256; cur_x1 = 0; cov = xb; next_line = cur_line + 1;
        } else {
            if (xa != cov) return fr_fail(__LINE__);
            u16 hh = r->v & 0x41f, vv = r->v & 0x7be0; int cy = (vv >> 5) & 31;
            u16 xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8));
            if ((u16)(cy * 8 + ((vv >> 12) & 7)) != cur_row || xh != cur_xh) return fr_fail(__LINE__);       /* the tile coordinates changed inside the line */
            if (r->finex != cur_finex) {                                                       /* fine X scroll written in the middle of the line */
                if (cur_special) return fr_fail(__LINE__);
                cur_special = 1; cur_xs = xa; cur_x1 = (xh + r->finex) & 511; cur_finex = r->finex;
            }
            cov = xb;
        }
    }
    FLUSH_CUR();
    if (next_line != 240 || fr_ovf) return fr_fail(__LINE__);
    /* rows must be unique (otherwise two lines would share bitmap pixels): no two bands may cover the same bitmap row (cyclic intervals) */
    for (int i = 0; i < fr_nbands; i++) {
        int s1 = fr_band_special[i] ? fr_special_row[i] : fr_bands[i].row0, l1 = fr_band_special[i] ? 1 : fr_bands[i].nlines;
        if (l1 > 240) return fr_fail(__LINE__);
        for (int j = i + 1; j < fr_nbands; j++) {
            int s2 = fr_band_special[j] ? fr_special_row[j] : fr_bands[j].row0, l2 = fr_band_special[j] ? 1 : fr_bands[j].nlines;
            int d = s2 - s1; if (d < 0) d += 240; int e = s1 - s2; if (e < 0) e += 240;
            if (d < l1 || e < l2) return fr_fail(__LINE__);
        }
    }
    return 1;
}

/* ---- sprites ----------------------------------------------------------------------------------------------------------- */
static u8 fr_claim[240][32]; static u8 fr_claim_gen[240]; static u8 fr_gen;
static inline u32 get8(const u8 *p, int bitpos) { int i = bitpos >> 3, s = bitpos & 7; return ((((u32)p[i] << 8) | p[i + 1]) >> (8 - s)) & 0xFF; }
static inline void or8(u8 *p, int bitpos, u32 bits) { int i = bitpos >> 3, s = bitpos & 7; u32 v = (bits << 8) >> s; p[i] |= (u8)(v >> 8); p[i + 1] |= (u8)v; }
static u8 *fr_priv_ptr[FR_MAXBANDS + 2];

/* ---- the sprite layer is drawn by the blitter.  Every sprite becomes two "cookie cut" jobs, one per group of two planes:
 *     D = (A & B) | (~A & C)   A = mask of the pixels the sprite owns, B = bitplane data, C = D = the layer
 * with the shift x & 15 done by the blitter's barrel shifter; the previous frame's sprite rectangles are first cleared by D-only jobs.  The jobs are
 * collected in fr_jobs.  The host harness runs them with the exact software model below (so the picture is verified), the Amiga build turns them into a
 * Copper list that is run in the vertical blank (fr_jobs_encode), so the CPU never touches the sprite layer. */
#define FR_MAXJOBS 512
struct BlJob fr_jobs[FR_MAXJOBS]; int fr_njobs;
struct FrRect { u8 l0, rows, w, wid; };
static struct FrRect fr_rect_new[64], fr_rect_old[64], fr_rect_clr[64]; static int fr_nrect_new, fr_nrect_old;
static u8 fr_var_ok[256]; static u8 fr_svar_tbl = 0xff;
static const u8 *fr_consts;                           /* four 64-byte patterns: planes 2 and 3 all ones / all zeros for the four sprite palettes */
static u8 fr_tmp_par; static int fr_tmp_used;

static void fr_job_run(const struct BlJob *j) {                         /* software model of the blitter */
    u8 *d = j->d; const u8 *a = j->a, *b = j->b; int sh = j->sh;
    if (j->kind == 2) {                                                              /* background cell: 32 rows 100 bytes apart, one word wide */
        for (int r = 0; r < 32; r++, d += 100, a += 2, b += 2) { u32 A = (u32)a[0] << 8 | a[1], B = (u32)b[0] << 8 | b[1], C = (u32)d[0] << 8 | d[1], D = ((A & B) | (~A & C)) & 0xFFFF; d[0] = (u8)(D >> 8); d[1] = (u8)D; }
        return;
    }
    for (int r = 0; r < j->rows; r++, d += SL_BW, a += 4, b += 4) {
        if (j->kind) { d[0] = d[1] = 0; if (j->wid == 2) d[2] = d[3] = 0; continue; }
        if (j->wid == 1) {
            u32 A = ((u32)a[0] << 8 | a[1]) >> sh, B = ((u32)b[0] << 8 | b[1]) >> sh, C = (u32)d[0] << 8 | d[1], D = ((A & B) | (~A & C)) & 0xFFFF;
            d[0] = (u8)(D >> 8); d[1] = (u8)D;
        } else {
            u32 A = ((u32)a[0] << 24 | (u32)a[1] << 16) >> sh, B = ((u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3]) >> sh;
            u32 C = (u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3], D = (A & B) | (~A & C);
            d[0] = (u8)(D >> 24); d[1] = (u8)(D >> 16); d[2] = (u8)(D >> 8); d[3] = (u8)D;
        }
    }
}
void fr_jobs_run_sw(u8 *layer_delta_unused) { (void)layer_delta_unused; for (int i = 0; i < fr_njobs; i++) fr_job_run(&fr_jobs[i]); }


#ifdef FR_HWBLIT
/* Copper program for the jobs, written directly while the jobs are made: every instruction is one 32-bit word (register in the high half, value in the low half).
 * Pointers and modulos are only written when they differ from what the blitter holds after the previous job (a blit leaves every pointer advanced past the data it
 * used).  The platform adds the jump back to the display list. */
u32 *fr_bl_buf; u32 fr_bl_len;                         /* platform: buffer for this frame's program (chip RAM); length in words after fr_sprites */
static u32 *hw_o; static u32 hw_pa, hw_pb, hw_pc, hw_pd, hw_con0, hw_con1; static int hw_mode;
#define CM(reg, val) (*hw_o++ = ((u32)(reg) << 16) | ((u32)(val) & 0xFFFF))
#define BLT_WAIT 0x00010000u                  /* WAIT (0,0) with the blitter-finished bit clear: waits until the blitter is idle */
static void hw_begin(void) {
    hw_o = fr_bl_buf; hw_pa = hw_pb = hw_pc = hw_pd = 1; hw_con0 = hw_con1 = ~0u; hw_mode = -1;      /* pointers 1 = unknown */
    CM(0x044, 0xFFFF); CM(0x046, 0xFFFF);                                            /* first / last word masks: all ones (second source words are zero) */
}
static void hw_emit(const struct BlJob *j) {
    u32 d = (u32)j->d; int want = j->kind == 2 ? 8 : j->wid;
    if (want != hw_mode) {                                                           /* modulos: sprites: A and B rows are 4 bytes apart, the layer's rows 48; cells: 2 and 100 */
        hw_mode = want;
        if (want == 8) { CM(0x064, 0); CM(0x062, 0); CM(0x060, 98); CM(0x066, 98); }
        else { CM(0x064, 4 - 2 * want); CM(0x062, 4 - 2 * want); CM(0x060, SL_BW - 2 * want); CM(0x066, SL_BW - 2 * want); }
    }
    if (j->kind != 1) {
        u32 c0 = (u32)((j->sh << 12) | 0x0FCA), c1 = (u32)(j->sh << 12);
        if (c0 != hw_con0) { CM(0x040, c0); hw_con0 = c0; }
        if (c1 != hw_con1) { CM(0x042, c1); hw_con1 = c1; }
        u32 a = (u32)j->a, b = (u32)j->b;
        if ((a >> 16) != (hw_pa >> 16)) CM(0x050, a >> 16);
        if ((a & 0xFFFF) != (hw_pa & 0xFFFF)) CM(0x052, a);
        if ((b >> 16) != (hw_pb >> 16)) CM(0x04C, b >> 16);
        if ((b & 0xFFFF) != (hw_pb & 0xFFFF)) CM(0x04E, b);
        if ((d >> 16) != (hw_pc >> 16)) CM(0x048, d >> 16);
        if ((d & 0xFFFF) != (hw_pc & 0xFFFF)) CM(0x04A, d);
        if ((d >> 16) != (hw_pd >> 16)) CM(0x054, d >> 16);
        if ((d & 0xFFFF) != (hw_pd & 0xFFFF)) CM(0x056, d);
        if (j->kind == 2) { hw_pa = a + 64; hw_pb = b + 64; hw_pc = hw_pd = d + 32 * 100; }
        else { hw_pa = a + (u32)j->rows * 4; hw_pb = b + (u32)j->rows * 4; hw_pc = hw_pd = d + (u32)j->rows * SL_BW; }
    } else {
        if (hw_con0 != 0x0100) { CM(0x040, 0x0100); hw_con0 = 0x0100; }
        if (hw_con1 != 0) { CM(0x042, 0); hw_con1 = 0; }
        if ((d >> 16) != (hw_pd >> 16)) CM(0x054, d >> 16);
        if ((d & 0xFFFF) != (hw_pd & 0xFFFF)) CM(0x056, d);
        hw_pd = d + (u32)j->rows * SL_BW;
    }
    CM(0x058, ((u32)j->rows << 6) | j->wid);                                         /* BLTSIZE starts the blit */
    *hw_o++ = BLT_WAIT;
}
#endif
static inline void fr_job_add(u8 kind, u8 *d, const u8 *a, const u8 *b, int rows, int sh, int wid) {
#ifdef FR_HWBLIT
    struct BlJob j; j.kind = kind; j.d = d; j.a = a; j.b = b; j.rows = (u8)rows; j.sh = (u8)sh; j.wid = (u8)wid; hw_emit(&j); fr_njobs++;
#else
    struct BlJob *j = &fr_jobs[fr_njobs++]; j->kind = kind; j->d = d; j->a = a; j->b = b; j->rows = (u8)rows; j->sh = (u8)sh; j->wid = (u8)wid;
#endif
}
static void fr_sprites_begin(void) {                  /* clear what the last frame left in the layer */
    fr_njobs = 0;
#ifdef FR_HWBLIT
    hw_begin(); for (int i = 0; i < fr_ncjobs; i++) hw_emit(&fr_cjobs[i]);          /* the cells first */
#endif
    int n; const struct FrRect *src;
    if (fr_jobs_executed()) { n = fr_nrect_new; src = fr_rect_new; }        /* the layer holds last frame's sprites */
    else { n = fr_nrect_old; src = fr_rect_old; }                            /* last frame's jobs never ran: the layer still holds the frame before */
    for (int k = 0; k < n; k++) fr_rect_clr[k] = src[k];
    for (int k = 0; k < n; k++) {
        u8 *d = fr_spr + fr_rect_clr[k].l0 * SL_ROWB + fr_rect_clr[k].w * 2;
        fr_job_add(1, d, 0, 0, fr_rect_clr[k].rows * 2, 0, fr_rect_clr[k].wid); fr_job_add(1, d + SL_GRP, 0, 0, fr_rect_clr[k].rows * 2, 0, fr_rect_clr[k].wid);
    }
    for (int k = 0; k < n; k++) fr_rect_old[k] = fr_rect_clr[k];
    fr_nrect_old = n; fr_nrect_new = 0; fr_tmp_par ^= 1; fr_tmp_used = 0;
    if (!fr_consts) {                                                        /* constant rows of the palette planes */
        u8 *k = fr_var + 1024 * FR_VARSZ; fr_consts = k;
        for (int pn = 0; pn < 4; pn++) for (int r = 0; r < 8; r++) { memset(k + pn * 64 + r * 8, (pn & 1) ? 0xFF : 0, 4); memset(k + pn * 64 + r * 8 + 4, (pn & 2) ? 0xFF : 0, 4); }
    }
}
static void fr_job_sprite(int y, int sx, const u8 *A, const u8 *B01, const u8 *B23, int rows) {
    int w = sx >> 4, sh = sx & 15, wid = sh > 8 ? 2 : 1;
    u8 *d = fr_spr + (y + 1) * SL_ROWB + w * 2;
    fr_job_add(0, d, A, B01, rows * 2, sh, wid); fr_job_add(0, d + SL_GRP, rows == 8 ? A + 64 : A, B23, rows * 2, sh, wid);   /* (A + 64: where the blitter's A pointer stands after job 1 when all 8 rows were drawn) */
    struct FrRect *r = &fr_rect_new[fr_nrect_new++]; r->l0 = (u8)(y + 1); r->rows = (u8)rows; r->w = (u8)w; r->wid = (u8)wid;
}
static const u8 *fr_variant(int tbl, int tile, int fv) {
    if (fr_svar_tbl != tbl) { fr_svar_tbl = (u8)tbl; fr_dbg[6]++; memset(fr_var_ok, 0, sizeof fr_var_ok); }
    u8 *v = fr_var + (tile * 4 + fv) * FR_VARSZ;
    if (!(fr_var_ok[tile] & (1 << fv))) {
        const u8 *c = chr + tbl * 0x1000 + tile * 16;
        memset(v, 0, FR_VARSZ);
        for (int r = 0; r < 8; r++) {
            int rr = (fv & 2) ? 7 - r : r; u32 lo = c[rr], hi = c[rr + 8];
            if (fv & 1) { lo = fr_rev[lo]; hi = fr_rev[hi]; }
            v[r * 8] = v[r * 8 + 4] = v[64 + r * 8] = v[64 + r * 8 + 4] = (u8)(lo | hi); v[128 + r * 8] = (u8)lo; v[128 + r * 8 + 4] = (u8)hi;
        }
        fr_var_ok[tile] |= (u8)(1 << fv); fr_dbg[5]++;
    }
    return v;
}
/* sources with per-row masks: om = pixels that replace what is below, vm = pixels that are drawn (lo/hi are masked with vm).  Built in fast RAM, copied to chip RAM in long words:
 * 16 rows of A, the same again, 16 rows of low/high plane data, 16 rows of palette plane data (two pointers advance through the A table, see fr_job_sprite) */
static u8 fr_scr[256] __attribute__((aligned(4)));
static inline void fr_tmp_row(int r, u32 om, u32 vm, u32 lo, u32 hi, u32 m2, u32 m3) {
    u8 *t = fr_scr;
    t[r * 8] = t[r * 8 + 4] = t[64 + r * 8] = t[64 + r * 8 + 4] = (u8)om; t[128 + r * 8] = (u8)(lo & vm); t[128 + r * 8 + 4] = (u8)(hi & vm);
    t[192 + r * 8] = m2 ? (u8)vm : 0; t[192 + r * 8 + 4] = m3 ? (u8)vm : 0;
}
static int fr_oam_overflow(void);
static void fr_tmp_begin(void) { memset(fr_scr, 0, 256); }
static u8 *fr_tmp_end(void) { u8 *t = fr_tmp[fr_tmp_par] + fr_tmp_used * 256; fr_tmp_used++; const u32 *sp = (const u32 *)fr_scr; u32 *dp = (u32 *)t; for (int i = 0; i < 64; i++) dp[i] = sp[i]; return t; }
static const u8 *fr_bgrow(int l, int sx, int *bx) {      /* background bitmap row of line l and the bit position of screen x = sx */
    int bi = fr_lineband[l]; const struct FrBand *bd = &fr_bands[bi]; const u8 *rowp; int x0;
    if (fr_band_special[bi]) { rowp = fr_priv_ptr[bi]; x0 = 0; } else { rowp = bd->base + (l - bd->line0) * FR_ROWB; x0 = bd->x0; }
    *bx = x0 + sx; return rowp;
}
static void fr_sprites_fast(void) {                     /* no line has more than 8 sprites: painted from the lowest priority (highest index) to the highest */
    int have_lb = 0;
    for (int i = 63; i >= 0; i--) {
        int y = oam[i * 4]; if (y >= 239) continue;
        int sx = oam[i * 4 + 3];
        u32 cm = 0xFF; if (sx + 7 > 255) cm &= (0xFF << (sx + 7 - 255)) & 0xFF;
        if (!fr_mask_spr_left && sx < 8) { int k = 8 - sx; if (k >= 8) cm = 0; else cm &= 0xFF >> k; }
        if (!cm) continue;
        u8 attr = oam[i * 4 + 2]; u8 tile = oam[i * 4 + 1];
        ST(fr_stat_sprites++;)
        int r1 = 8; if (y + 9 > 240) r1 = 239 - y;
        int hflip = attr & 0x40, vflip = attr & 0x80, behind = attr & 0x20;
        if (!behind && cm == 0xFF) {                                              /* ordinary sprite: prepared tile variant */
            const u8 *v = fr_variant(fr_spr_table, tile, (hflip ? 1 : 0) | (vflip ? 2 : 0));
            fr_job_sprite(y, sx, v, v + 128, fr_consts + (attr & 3) * 64, r1); ST(fr_stat_rows += (u32)r1; fr_stat_drawn += (u32)r1;)
            continue;
        }
        ST(fr_stat_cpuspr++; if (behind) fr_stat_behind++;)                           /* behind the background or clipped: masks computed per row */
        const u8 *c = chr + (fr_spr_table ? 0x1000 : 0) + tile * 16; u32 m2 = attr & 1, m3 = attr & 2;
        if (behind && !have_lb) { have_lb = 1; for (int l = 0, k = 0; k < fr_nbands; k++) { int e = fr_bands[k].line0 + fr_bands[k].nlines; for (; l < e; l++) fr_lineband[l] = (u8)k; } }
        fr_tmp_begin();
        for (int r = 0; r < r1; r++) {
            int l = y + 1 + r, rr = vflip ? 7 - r : r; u32 lo = c[rr], hi = c[rr + 8];
            if (hflip) { lo = fr_rev[lo]; hi = fr_rev[hi]; }
            u32 om = (lo | hi) & cm; if (!om) continue;
            ST(fr_stat_rows++;) u32 vm = om;
            if (behind) { int bx; const u8 *rowp = fr_bgrow(l, sx, &bx); vm &= ~(get8(rowp, bx) | get8(rowp + FR_BW, bx)); }
            fr_tmp_row(r, om, vm, lo, hi, m2, m3); ST(fr_stat_drawn++;)
        }
        { u8 *t = fr_tmp_end(); fr_job_sprite(y, sx, t, t + 128, t + 192, r1); }
    }
}
static void fr_sprites_body(void) {
    fr_sprites_begin();
    if (!fr_oam_overflow()) { fr_sprites_fast(); return; }
    fr_stat_ovfframes++;
    static u8 cnt[240]; memset(cnt, 0, sizeof cnt);
    if (++fr_gen == 0) { memset(fr_claim_gen, 0, sizeof fr_claim_gen); fr_gen = 1; }
    for (int l = 0, k = 0; k < fr_nbands; k++) { int e = fr_bands[k].line0 + fr_bands[k].nlines; for (; l < e; l++) fr_lineband[l] = (u8)k; }
    for (int i = 0; i < 64; i++) {
        int y = oam[i * 4]; if (y >= 239) continue;
        u8 tid = oam[i * 4 + 1], attr = oam[i * 4 + 2]; int sx = oam[i * 4 + 3];
        const u8 *c = chr + (fr_spr_table ? 0x1000 : 0) + tid * 16;
        /* visible columns: x in [sx, sx+7], x <= 255, x >= 8 unless the left column of sprites is enabled */
        u32 cm = 0xFF; if (sx + 7 > 255) cm &= (0xFF << (sx + 7 - 255)) & 0xFF;
        if (!fr_mask_spr_left && sx < 8) { int k = 8 - sx; if (k >= 8) cm = 0; else cm &= 0xFF >> k; }
        if (!cm) { for (int r = 0; r < 8 && y + 1 + r <= 239; r++) cnt[y + 1 + r]++; continue; }      /* fully masked, but it still occupies one of the 8 slots of its lines */
        int hflip = attr & 0x40, vflip = attr & 0x80, behind = attr & 0x20; ST(fr_stat_sprites++;)
        u32 m2 = attr & 1, m3 = attr & 2; int any = 0; fr_tmp_begin();
        for (int r = 0; r < 8; r++) {
            int l = y + 1 + r; if (l > 239) break;
            if (cnt[l]++ >= 8) continue;
            ST(fr_stat_rows++;)
            int rr = vflip ? 7 - r : r; u32 lo = c[rr], hi = c[rr + 8];
            if (hflip) { lo = fr_rev[lo]; hi = fr_rev[hi]; }
            u32 om = (lo | hi) & cm; if (!om) continue;
            u8 *claim = fr_claim[l];
            if (fr_claim_gen[l] != fr_gen) { memset(claim, 0, 32); fr_claim_gen[l] = fr_gen; }
            u32 cl = get8(claim, sx); u32 nw = om & ~cl; or8(claim, sx, om);       /* claims even when the pixel ends up hidden */
            if (!nw) continue;
            u32 vm = nw;
            if (behind) { int bx; const u8 *rowp = fr_bgrow(l, sx, &bx); vm &= ~(get8(rowp, bx) | get8(rowp + FR_BW, bx)); }
            if (!vm) continue;
            ST(fr_stat_drawn++;)
            fr_tmp_row(r, vm, vm, lo, hi, m2, m3); any = 1;                      /* the pixels are disjoint from all others: no replacing needed */
        }
        if (any) { u8 *t = fr_tmp_end(); fr_job_sprite(y, sx, t, t + 128, t + 192, y + 8 <= 239 ? 8 : 239 - y); }
    }
}

/* ---- frame ------------------------------------------------------------------------------------------------------------- */
/* the private lines: background bits from the bitmap with the two fine X scrolls, as the PPU shifter would show them */
static void fr_private_lines(void) {
    int npriv = 0;
    for (int k = 0; k < fr_nbands; k++) {
        if (!fr_band_special[k]) continue;
        if (npriv >= FR_MAXPRIV) { fr_ovf = 1; return; }
        u8 *dst = fr_priv + npriv * FR_ROWB; fr_priv_ptr[k] = dst; fr_bands[k].base = dst; npriv++;
        const u8 *src = fr_bmp + fr_special_row[k] * FR_ROWB;
        int xs = fr_special_xs[k], xa = fr_special_xa[k], xb = fr_special_xb[k];
        for (int p = 0; p < 4; p++) {                                   /* sprites are not in the bitmap yet: planes 0-3 of the background */
            u8 *d = dst + p * FR_BW; const u8 *s = src + p * FR_BW;
            for (int i = 0; i < 32; i++) {
                u32 a = get8(s, xa + 8 * i), b = get8(s, xb + 8 * i);
                int kk = xs - 8 * i;                                      /* number of pixels of this byte that still use the first scroll */
                d[i] = (u8)(kk >= 8 ? a : kk <= 0 ? b : ((a & (0xFF00u >> kk)) | (b & ~(0xFF00u >> kk))) & 0xFF);
            }
            for (int i = 32; i < 40; i++) d[i] = 0;
        }
    }
    fr_priv_flush(npriv);
}
/* returns 1 if the frame was prepared for hardware display (fr_bands, planes, palette via fr_palette), 0 if the exact renderer must be used */
static void fr_sprites(void) {
    fr_sprites_body();
#ifdef FR_HWBLIT
    fr_bl_len = (u32)(hw_o - fr_bl_buf);
#endif
}
u32 fr_prof[4], fr_prof_n;
#ifdef FR_PROF
#define FR_T() plat_timer()
u32 plat_timer(void);
#else
#define FR_T() 0
#endif                             /* time spent per rendered frame (platform timer units): build, clear+cells, private lines, sprites */
int fr_render(const struct Seg *log, int n) {
    u32 t0 = FR_T(), t1 = 0, t2 = 0, t3 = 0;
    if (!fr_rev_init) { for (int i = 0; i < 256; i++) { u8 r = 0; for (int b = 0; b < 8; b++) r |= ((i >> b) & 1) << (7 - b); fr_rev[i] = r; } fr_rev_init = 1; }
    if (!fr_bmp || !fr_priv) return 0;
    if (!fr_build(log, n)) { fr_stat_fallback++; return 0; }
    t1 = FR_T();
    fr_spr_table = (log[n - 1].ctrl & 8) ? 1 : 0;
    u8 bgt = 0; for (int i = n - 1; i >= 0; i--) if (log[i].scan < 240) { bgt = (log[i].ctrl & 0x10) ? 1 : 0; break; }
#if !(defined(EXPMODE) && EXPMODE >= 3)
    fr_cells_update(bgt);
#endif
    t2 = FR_T();
    fr_private_lines();
    if (fr_ovf) { fr_all_dirty = 1; fr_stat_fallback++; return 0; }
    t3 = FR_T();
#if !(defined(EXPMODE) && EXPMODE >= 2)
    fr_sprites();
#else
    fr_njobs = 0;
#endif
    fr_dbg[2] = (u32)fr_ncjobs; fr_dbg[3] = (u32)fr_njobs;
#ifndef FR_HWBLIT
    for (int i = 0; i < fr_ncjobs; i++) {                                         /* run the cell jobs with the model and check them against the working copy */
        const struct BlJob *j = &fr_cjobs[i]; fr_job_run(j);
        for (int r = 0; r < 32; r++) { long off = (long)(j->d - fr_bmp_chip) + r * 100 + j->tag; if (fr_bmp_chip[off] != fr_bmp[off]) { fr_stat_cell_bad++; break; } }
    }
    fr_ncjobs = 0;
    fr_jobs_run_sw(0);                                      /* host harness: run the jobs with the software model */
#endif
    { u32 t4 = FR_T(); fr_prof[0] += t1 - t0; fr_prof[1] += t2 - t1; fr_prof[2] += t3 - t2; fr_prof[3] += t4 - t3; fr_prof_n++; }
    fr_stat_ok++;
    return 1;
}
/* palettes of the two playfields: NES colour (0..63) of each index (bg: 0..15, sprites: 16..31) */
void fr_palette(u8 out[32]) {
    for (int i = 0; i < 16; i++) { int px = i & 3; out[i] = pal[px ? i : 0]; }
    for (int i = 0; i < 16; i++) { int px = i & 3; out[16 + i] = pal[px ? 0x10 + i : 0]; }
}
/* the picture as the hardware would show it, as NES colour indices (for verification) */
void fr_reconstruct(u8 out[240][256]) {
    u8 pl[32]; fr_palette(pl);
    for (int l = 0; l < 240; l++) {
        const struct FrBand *bd = 0;
        for (int b = 0; b < fr_nbands; b++) if (l >= fr_bands[b].line0 && l < fr_bands[b].line0 + fr_bands[b].nlines) bd = &fr_bands[b];
        const u8 *rowp = bd->base + (l - bd->line0) * FR_ROWB; int x0 = bd->x0; const u8 *sp = fr_spr + l * SL_ROWB;
        for (int x = 0; x < 256; x++) {
            int bx = x0 + x, idx = 0, sidx = 0;
            for (int p = 0; p < 4; p++) { idx |= ((rowp[p * FR_BW + (bx >> 3)] >> (7 - (bx & 7))) & 1) << p; sidx |= ((sp[(p >> 1) * SL_GRP + (p & 1) * SL_BW + (x >> 3)] >> (7 - (x & 7))) & 1) << p; }
            out[l][x] = sidx ? pl[16 + sidx] : pl[idx];
        }
    }
}
