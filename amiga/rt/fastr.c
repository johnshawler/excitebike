/* fastr.c - fast frame renderer (included by nes_rt.c).
 *
 * The background is kept as a planar bitmap of the two nametables side by side (512 px) plus a copy of nametable 0 at 512..767 (so that a
 * 256 px window never wraps), 5 interleaved bit planes.  Colour index per pixel (5 bits): bit0,1 = pattern pixel, bit2,3 = palette number, bit4 = sprite.
 * The display shows the bitmap through hardware scroll: one "band" per run of lines with the same scroll; lines whose fine X scroll changes in the
 * middle of the line (the game does this at the two status-bar splits) get a private 256 px line.  Sprites are drawn into the bitmap rows (or the
 * private lines) in OAM priority order and removed again next frame by redrawing the cells under them.
 * Applicability is checked for every frame (fr_build); if a frame does not fit, the caller replays the segment log with the exact slow renderer.
 * Exactness is verified by reconstructing the frame from the planes (fr_reconstruct) and comparing it with the slow renderer (qemu harness, FR_VERIFY).
 */
#define FR_BW   100                        /* bytes per plane row: 800 px (768 + guard) */
#define FR_ROWB (FR_BW * 5)
#define FR_ROWS 240
#define FR_MAXBANDS 16
#define FR_MAXPRIV  4
u8 *fr_bmp;                                /* FR_ROWS * FR_ROWB bytes, set by the platform */
u8 *fr_priv;                               /* FR_MAXPRIV private lines of FR_ROWB bytes each */
struct FrBand { u16 line0, nlines, x0, row0; u8 *base; };
struct FrBand fr_bands[FR_MAXBANDS]; int fr_nbands;
static u8 fr_cell_dirty[30 * 64]; static u8 fr_row_dirty[30]; static u8 fr_all_dirty = 1; static u8 fr_bmp_table = 0xff;
static u8 fr_have_bmp;
u32 fr_stat_ok, fr_stat_fallback, fr_stat_cells;

/* ---- background cells ------------------------------------------------------------------------------------------------ */
static void fr_nt_write(u16 a) {                       /* a: PPU address $2000-$3EFF */
    int nti = (a >> 10) & 1, off = a & 0x3ff;
    if (off < 960) { fr_cell_dirty[(off >> 5) * 64 + nti * 32 + (off & 31)] = 1; fr_row_dirty[off >> 5] = 1; }
    else { int o = off - 960, r0 = (o >> 3) * 4, c0 = (o & 7) * 4; for (int r = r0; r < r0 + 4 && r < 30; r++) { fr_row_dirty[r] = 1; for (int c = c0; c < c0 + 4; c++) fr_cell_dirty[r * 64 + nti * 32 + c] = 1; } }
}
static void fr_cell_draw(int col, int row) {
    int nti = col >> 5, cx = col & 31;
    u8 id = nt[nti][row * 32 + cx], at = nt[nti][960 + (row >> 2) * 8 + (cx >> 2)];
    int pal = (at >> (((row & 2) << 1) | (cx & 2))) & 3;
    const u8 *c = chr + (fr_bmp_table ? 0x1000 : 0) + id * 16;
    for (int r = 0; r < 8; r++) {
        u8 lo = c[r], hi = c[r + 8], nz = lo | hi;
        u8 *d = fr_bmp + (row * 8 + r) * FR_ROWB + col;
        d[0] = lo; d[FR_BW] = hi; d[2 * FR_BW] = (pal & 1) ? nz : 0; d[3 * FR_BW] = (pal & 2) ? nz : 0; d[4 * FR_BW] = 0;
        if (col < 32) { d[64] = lo; d[FR_BW + 64] = hi; d[2 * FR_BW + 64] = (pal & 1) ? nz : 0; d[3 * FR_BW + 64] = (pal & 2) ? nz : 0; d[4 * FR_BW + 64] = 0; }
    }
    fr_stat_cells++;
}
static void fr_cells_update(u8 table) {
    if (!fr_have_bmp || table != fr_bmp_table) { fr_bmp_table = table; fr_all_dirty = 1; fr_have_bmp = 1; }
    if (fr_all_dirty) { for (int r = 0; r < 30; r++) for (int c = 0; c < 64; c++) fr_cell_draw(c, r); memset(fr_cell_dirty, 0, sizeof fr_cell_dirty); memset(fr_row_dirty, 0, sizeof fr_row_dirty); fr_all_dirty = 0; return; }
    for (int r = 0; r < 30; r++) { if (!fr_row_dirty[r]) continue; fr_row_dirty[r] = 0; for (int c = 0; c < 64; c++) if (fr_cell_dirty[r * 64 + c]) { fr_cell_dirty[r * 64 + c] = 0; fr_cell_draw(c, r); } }
}

/* ---- per-line description ---------------------------------------------------------------------------------------------- */
struct FrLine { u16 x0, row, x1, xs; u8 special; };    /* special: x < xs uses x0, x >= xs uses x1 (fine X changed in mid-line) */
static struct FrLine fr_line[240];
static u8 fr_rev[256]; static u8 fr_rev_init;
static u8 fr_mask_spr_left, fr_spr_table;

/* build fr_line[] from the segment log; returns 0 if this frame needs the exact renderer */
static inline u16 fr_vinc(u16 vv) {
    if ((vv & 0x7000) != 0x7000) return vv + 0x1000;
    vv &= ~0x7000; int y = (vv & 0x3e0) >> 5;
    if (y == 29) { y = 0; vv ^= 0x800; } else if (y == 31) y = 0; else y++;
    return (u16)((vv & ~0x3e0) | (y << 5));
}
static int fr_build(const struct Seg *log, int n) {
    static u8 seen[240];
    memset(seen, 0, sizeof seen);
    int have_first = 0; u8 ctrl0 = 0;
    static struct { u16 x; u8 finex; } last[240];
    for (int i = 0; i < n; i++) {
        const struct Seg *r = &log[i];
        if (r->scan >= 240) continue;
        if ((r->mask & 0xE1) || (r->mask & 0x1A) != 0x1A) return 0;      /* bg+sprites on, bg left column shown, no grey/emphasis */
        if (!have_first) { have_first = 1; ctrl0 = r->ctrl; fr_mask_spr_left = r->mask & 4; }
        if ((r->ctrl & 0x38) != (ctrl0 & 0x38) || (r->mask & 4) != fr_mask_spr_left) return 0;
        if (r->ctrl & 0x20) return 0;                                    /* 8x16 sprites */
        if (r->nl) {                                                     /* run of complete lines: tile coordinates evolve by the vertical increment */
            u16 hh = r->v & 0x41f, vv = r->v & 0x7be0, hhn = r->t & 0x41f;
            for (int k = 0; k < r->nl; k++) {
                int line = r->scan + k, cy = (vv >> 5) & 31; if (cy >= 30) return 0;
                u16 xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8));
                struct FrLine *L = &fr_line[line];
                L->x0 = (u16)((xh + r->finex) & 511); L->row = (u16)(cy * 8 + ((vv >> 12) & 7)); L->special = 0; L->xs = 256;
                seen[line] = 1; last[line].finex = r->finex; last[line].x = xh;
                vv = fr_vinc((u16)vv); hh = hhn;
            }
            continue;
        }
        int xa = r->d0 < 1 ? 0 : r->d0 - 1, xb = r->d1 > 257 ? 256 : r->d1 - 1; if (xb <= xa) continue;
        int hh = r->v & 0x41f, vv = r->v & 0x7be0;
        int cy = (vv >> 5) & 31; if (cy >= 30) return 0;
        u16 x0 = (u16)((((hh & 31) << 3) + (((hh >> 10) & 1) << 8) + r->finex) & 511), row = (u16)(cy * 8 + ((vv >> 12) & 7));
        struct FrLine *L = &fr_line[r->scan];
        u16 xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8));          /* horizontal start of the line in the nametable space, without fine X */
        if (!seen[r->scan]) { seen[r->scan] = 1; L->x0 = x0; L->row = row; L->special = 0; L->xs = 256; last[r->scan].finex = r->finex; last[r->scan].x = xh; }
        else {
            if (row != L->row || xh != last[r->scan].x) return 0;                  /* the tile coordinates changed inside the line */
            if (r->finex != last[r->scan].finex) {                                 /* fine X scroll written in the middle of the line */
                if (L->special) return 0;
                L->special = 1; L->xs = (u16)xa; L->x1 = x0; last[r->scan].finex = r->finex;
            }
        }
    }
    for (int l = 0; l < 240; l++) if (!seen[l]) return 0;
    /* rows must be unique (otherwise two lines would share bitmap pixels) */
    static u8 used[240]; memset(used, 0, sizeof used);
    for (int l = 0; l < 240; l++) { if (fr_line[l].special) continue; if (used[fr_line[l].row]) return 0; used[fr_line[l].row] = 1; }
    return 1;
}

/* ---- sprites ----------------------------------------------------------------------------------------------------------- */
static u8 fr_claim[240][32];
static u16 fr_spr_rows; static struct { u16 row, bx; } fr_spr_hist[64 * 8 + 8]; static u16 fr_spr_hist_n;
static inline u32 get8(const u8 *p, int bitpos) { int i = bitpos >> 3, s = bitpos & 7; return ((((u32)p[i] << 8) | p[i + 1]) >> (8 - s)) & 0xFF; }
static inline void or8(u8 *p, int bitpos, u32 bits) { int i = bitpos >> 3, s = bitpos & 7; u32 v = (bits << 8) >> s; p[i] |= (u8)(v >> 8); p[i + 1] |= (u8)v; }
static inline void put8m(u8 *p, int bitpos, u32 mask8, u32 data8) {
    int i = bitpos >> 3, s = bitpos & 7; u32 m = (mask8 << 8) >> s, d = ((data8 & mask8) << 8) >> s;
    p[i] = (u8)((p[i] & ~(m >> 8)) | (d >> 8)); p[i + 1] = (u8)((p[i + 1] & ~m) | d);
}
static u8 *fr_rowptr(int l) { return fr_line[l].special ? 0 : fr_bmp + fr_line[l].row * FR_ROWB; }
static u8 *fr_priv_ptr[240]; static int fr_npriv;

static void fr_sprites(void) {
    static u8 cnt[240]; memset(cnt, 0, sizeof cnt); memset(fr_claim, 0, sizeof fr_claim);
    fr_spr_hist_n = 0;
    for (int i = 0; i < 64; i++) {
        int y = oam[i * 4]; if (y >= 239) continue;
        u8 tid = oam[i * 4 + 1], attr = oam[i * 4 + 2]; int sx = oam[i * 4 + 3];
        const u8 *c = chr + (fr_spr_table ? 0x1000 : 0) + tid * 16;
        for (int r = 0; r < 8; r++) {
            int l = y + 1 + r; if (l > 239) break;
            if (cnt[l]++ >= 8) continue;
            int rr = (attr & 0x80) ? 7 - r : r; u32 lo = c[rr], hi = c[rr + 8];
            if (attr & 0x40) { lo = fr_rev[lo]; hi = fr_rev[hi]; }
            u32 om = lo | hi; if (!om) continue;
            /* visible columns: x in [sx, sx+7], x <= 255, x >= 8 unless the left column of sprites is enabled */
            u32 cm = 0xFF; if (sx + 7 > 255) cm &= 0xFF << (sx + 7 - 255) & 0xFF;
            if (!fr_mask_spr_left && sx < 8) { int k = 8 - sx; if (k >= 8) continue; cm &= 0xFF >> k; }
            om &= cm; if (!om) continue;
            u8 *claim = fr_claim[l]; u32 cl = get8(claim, sx); u32 nw = om & ~cl; or8(claim, sx, om);       /* claims even when the pixel ends up hidden */
            if (!nw) continue;
            u8 *rowp; int x0;
            if (fr_line[l].special) { rowp = fr_priv_ptr[l]; x0 = 0; } else { rowp = fr_bmp + fr_line[l].row * FR_ROWB; x0 = fr_line[l].x0; }
            int bx = x0 + sx;                                           /* bitmap x of the sprite's first pixel */
            u32 vm = nw;
            if (attr & 0x20) {                                          /* behind the background: only where the background pixel is transparent */
                u32 bgnz = get8(rowp, bx) | 0; u32 b1 = get8(rowp + FR_BW, bx); bgnz = (bgnz | b1) & 0xFF;
                /* background opaque pixels outside the visible clip do not exist here (left column of the background is shown) */
                vm &= ~bgnz;
            }
            if (!vm) continue;
            put8m(rowp, bx, vm, lo); put8m(rowp + FR_BW, bx, vm, hi);
            put8m(rowp + 2 * FR_BW, bx, vm, (attr & 1) ? 0xFF : 0); put8m(rowp + 3 * FR_BW, bx, vm, (attr & 2) ? 0xFF : 0); put8m(rowp + 4 * FR_BW, bx, vm, 0xFF);
            if (!fr_line[l].special) { fr_spr_hist[fr_spr_hist_n].row = fr_line[l].row; fr_spr_hist[fr_spr_hist_n].bx = (u16)bx; fr_spr_hist_n++; }
        }
    }
}
static void fr_sprites_restore(void) {                  /* mark the cells under last frame's sprites dirty */
    for (int k = 0; k < fr_spr_hist_n; k++) {
        int row = fr_spr_hist[k].row >> 3, c0 = fr_spr_hist[k].bx >> 3, c1 = (fr_spr_hist[k].bx + 7) >> 3;
        if (row < 30) fr_row_dirty[row] = 1;
        for (int c = c0; c <= c1; c++) { int cc = c >= 64 ? c - 64 : c; if (cc < 64 && row < 30) fr_cell_dirty[row * 64 + cc] = 1; }
    }
    fr_spr_hist_n = 0;
}

/* ---- frame ------------------------------------------------------------------------------------------------------------- */
static void fr_make_bands(void) {
    fr_nbands = 0; int npriv = 0;
    for (int l = 0; l < 240; ) {
        struct FrBand *b = &fr_bands[fr_nbands];
        if (fr_line[l].special) { b->line0 = l; b->nlines = 1; b->x0 = 0; b->row0 = 0; b->base = fr_priv + npriv * FR_ROWB; npriv++; fr_nbands++; l++; continue; }
        int l2 = l + 1;
        while (l2 < 240 && !fr_line[l2].special && fr_line[l2].x0 == fr_line[l].x0 && fr_line[l2].row == fr_line[l].row + (l2 - l)) l2++;
        b->line0 = l; b->nlines = l2 - l; b->x0 = fr_line[l].x0; b->row0 = fr_line[l].row; b->base = fr_bmp + fr_line[l].row * FR_ROWB; fr_nbands++; l = l2;
        if (fr_nbands >= FR_MAXBANDS - 1) { fr_nbands = FR_MAXBANDS + 1; return; }
    }
}
/* the private lines: background bits from the bitmap with the two fine X scrolls, as the PPU shifter would show them */
static void fr_private_lines(void) {
    fr_npriv = 0;
    for (int l = 0; l < 240; l++) {
        if (!fr_line[l].special) continue;
        if (fr_npriv >= FR_MAXPRIV) { fr_nbands = FR_MAXBANDS + 1; return; }
        u8 *dst = fr_priv + fr_npriv * FR_ROWB; fr_priv_ptr[l] = dst; fr_npriv++;
        const u8 *src = fr_bmp + fr_line[l].row * FR_ROWB;
        int xs = fr_line[l].xs, xa = fr_line[l].x0, xb = fr_line[l].x1;
        for (int p = 0; p < 4; p++) {                                   /* sprites are not in the bitmap yet: planes 0-3 of the background */
            u8 *d = dst + p * FR_BW; const u8 *s = src + p * FR_BW;
            for (int i = 0; i < 32; i++) {
                u32 a = get8(s, xa + 8 * i), b = get8(s, xb + 8 * i);
                int k = xs - 8 * i;                                      /* number of pixels of this byte that still use the first scroll */
                d[i] = (u8)(k >= 8 ? a : k <= 0 ? b : ((a & (0xFF00u >> k)) | (b & ~(0xFF00u >> k))) & 0xFF);
            }
            for (int i = 32; i < 40; i++) d[i] = 0;
        }
        memset(dst + 4 * FR_BW, 0, 40);
    }
}
/* returns 1 if the frame was prepared for hardware display (fr_bands, planes, palette via fr_palette), 0 if the exact renderer must be used */
int fr_render(const struct Seg *log, int n) {
    if (!fr_rev_init) { for (int i = 0; i < 256; i++) { u8 r = 0; for (int b = 0; b < 8; b++) r |= ((i >> b) & 1) << (7 - b); fr_rev[i] = r; } fr_rev_init = 1; }
    if (!fr_bmp || !fr_priv) return 0;
    if (!fr_build(log, n)) { fr_stat_fallback++; return 0; }
    fr_spr_table = (log[n - 1].ctrl & 8) ? 1 : 0;
    u8 bgt = 0; for (int i = n - 1; i >= 0; i--) if (log[i].scan < 240) { bgt = (log[i].ctrl & 0x10) ? 1 : 0; break; }
    fr_sprites_restore();
    fr_cells_update(bgt);
    fr_private_lines();
    if (fr_nbands > FR_MAXBANDS) { fr_all_dirty = 1; fr_stat_fallback++; return 0; }
    fr_sprites();
    fr_make_bands();
    if (fr_nbands > FR_MAXBANDS) { fr_stat_fallback++; return 0; }
    fr_stat_ok++;
    return 1;
}
/* palette of the fast display: NES colour (0..63) of each of the 32 indices */
void fr_palette(u8 out[32]) {
    for (int i = 0; i < 32; i++) {
        int px = i & 3, pn = (i >> 2) & 3, spr = i >> 4;
        out[i] = pal[px ? ((spr ? 0x10 : 0) + pn * 4 + px) : 0];
    }
}
/* the picture as the hardware would show it, as NES colour indices (for verification) */
void fr_reconstruct(u8 out[240][256]) {
    u8 pl[32]; fr_palette(pl);
    for (int l = 0; l < 240; l++) {
        const u8 *rowp; int x0 = 0; const struct FrBand *bd = 0;
        for (int b = 0; b < fr_nbands; b++) if (l >= fr_bands[b].line0 && l < fr_bands[b].line0 + fr_bands[b].nlines) bd = &fr_bands[b];
        rowp = bd->base + (l - bd->line0) * FR_ROWB; x0 = bd->x0;
        for (int x = 0; x < 256; x++) {
            int bx = x0 + x, idx = 0;
            for (int p = 0; p < 5; p++) idx |= ((rowp[p * FR_BW + (bx >> 3)] >> (7 - (bx & 7))) & 1) << p;
            out[l][x] = pl[idx];
        }
    }
}
