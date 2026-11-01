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
u8 *fr_clean;                              /* same layout as fr_bmp, background only (fast RAM): used to remove last frame's sprites */
struct FrBand { u16 line0, nlines, x0, row0; u8 *base; };
struct FrBand fr_bands[FR_MAXBANDS + 2]; int fr_nbands;
static u8 fr_cell_dirty[30 * 64]; static u8 fr_row_dirty[30]; static u8 fr_all_dirty = 1; static u8 fr_bmp_table = 0xff;
static u8 fr_have_bmp;
u32 fr_stat_ok, fr_stat_fallback, fr_stat_cells, fr_stat_sprites, fr_stat_rows, fr_stat_drawn;

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
    u8 m2 = (pal & 1) ? 0xFF : 0, m3 = (pal & 2) ? 0xFF : 0;
    u8 *d = fr_bmp + row * 8 * FR_ROWB + col, *q = fr_clean + row * 8 * FR_ROWB + col;
    for (int r = 0; r < 8; r++, d += FR_ROWB, q += FR_ROWB) {
        u8 lo = c[r], hi = c[r + 8], nz = lo | hi;
        u8 v2 = nz & m2, v3 = nz & m3;
        d[0] = lo; d[FR_BW] = hi; d[2 * FR_BW] = v2; d[3 * FR_BW] = v3; d[4 * FR_BW] = 0;
        q[0] = lo; q[FR_BW] = hi; q[2 * FR_BW] = v2; q[3 * FR_BW] = v3; q[4 * FR_BW] = 0;
        if (col < 32) { d[64] = lo; d[FR_BW + 64] = hi; d[2 * FR_BW + 64] = v2; d[3 * FR_BW + 64] = v3; d[4 * FR_BW + 64] = 0;
                        q[64] = lo; q[FR_BW + 64] = hi; q[2 * FR_BW + 64] = v2; q[3 * FR_BW + 64] = v3; q[4 * FR_BW + 64] = 0; }
    }
    fr_stat_cells++;
}
static void fr_cells_update(u8 table) {
    if (!fr_have_bmp || table != fr_bmp_table) { fr_bmp_table = table; fr_all_dirty = 1; fr_have_bmp = 1; }
    if (fr_all_dirty) { for (int r = 0; r < 30; r++) for (int c = 0; c < 64; c++) fr_cell_draw(c, r); memset(fr_cell_dirty, 0, sizeof fr_cell_dirty); memset(fr_row_dirty, 0, sizeof fr_row_dirty); fr_all_dirty = 0; return; }
    for (int r = 0; r < 30; r++) { if (!fr_row_dirty[r]) continue; fr_row_dirty[r] = 0; for (int c = 0; c < 64; c++) if (fr_cell_dirty[r * 64 + c]) { fr_cell_dirty[r * 64 + c] = 0; fr_cell_draw(c, r); } }
}

/* ---- per-frame description: bands ----------------------------------------------------------------------------------------- */
static u8 fr_rev[256]; static u8 fr_rev_init;
static u8 fr_mask_spr_left, fr_spr_table;
static u8 fr_lineband[240];                  /* band index of every line */
static int fr_special_xs[FR_MAXBANDS + 2];   /* private line: x < xs uses xa, x >= xs uses xb (bitmap x of the window start) */
static u16 fr_special_xa[FR_MAXBANDS + 2], fr_special_xb[FR_MAXBANDS + 2], fr_special_row[FR_MAXBANDS + 2];
static u8 fr_band_special[FR_MAXBANDS + 2];
static int fr_ovf;

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
static int fr_build(const struct Seg *log, int n) {
    int have_first = 0; u8 ctrl0 = 0;
    int next_line = 0;                        /* the next line that has to start */
    int cur_line = -1, cov = 0;               /* line being assembled from partial segments, x covered so far */
    u16 cur_row = 0, cur_xh = 0, cur_x0 = 0, cur_finex = 0; int cur_special = 0, cur_xs = 0, cur_x1 = 0;
    fr_nbands = 0; fr_ovf = 0;
#define FLUSH_CUR() do { if (cur_line >= 0) { if (cov != 256) return 0; \
        if (cur_special) fr_band_special_add(cur_line, cur_row, cur_x0, cur_x1, cur_xs); else fr_band_add(cur_line, 1, cur_x0, cur_row); cur_line = -1; } } while (0)
    for (int i = 0; i < n; i++) {
        const struct Seg *r = &log[i];
        if (r->scan >= 240) continue;
        if ((r->mask & 0xE1) || (r->mask & 0x1A) != 0x1A) return 0;      /* bg+sprites on, bg left column shown, no grey/emphasis */
        if (!have_first) { have_first = 1; ctrl0 = r->ctrl; fr_mask_spr_left = r->mask & 4; }
        if ((r->ctrl & 0x38) != (ctrl0 & 0x38) || (r->mask & 4) != fr_mask_spr_left) return 0;
        if (r->ctrl & 0x20) return 0;                                    /* 8x16 sprites */
        if (r->nl) {                                                     /* run of complete lines */
            FLUSH_CUR();
            if (r->scan != next_line) return 0;
            u16 vv = r->v & 0x7be0; int cy = (vv >> 5) & 31; if (cy >= 30) return 0;
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
            if (r->scan != next_line || xa != 0) return 0;
            u16 hh = r->v & 0x41f, vv = r->v & 0x7be0; int cy = (vv >> 5) & 31; if (cy >= 30) return 0;
            cur_line = r->scan; cur_row = (u16)(cy * 8 + ((vv >> 12) & 7)); cur_xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8));
            cur_finex = r->finex; cur_x0 = (u16)((cur_xh + r->finex) & 511); cur_special = 0; cur_xs = 256; cur_x1 = 0; cov = xb; next_line = cur_line + 1;
        } else {
            if (xa != cov) return 0;
            u16 hh = r->v & 0x41f, vv = r->v & 0x7be0; int cy = (vv >> 5) & 31;
            u16 xh = (u16)(((hh & 31) << 3) + (((hh >> 10) & 1) << 8));
            if ((u16)(cy * 8 + ((vv >> 12) & 7)) != cur_row || xh != cur_xh) return 0;       /* the tile coordinates changed inside the line */
            if (r->finex != cur_finex) {                                                       /* fine X scroll written in the middle of the line */
                if (cur_special) return 0;
                cur_special = 1; cur_xs = xa; cur_x1 = (xh + r->finex) & 511; cur_finex = r->finex;
            }
            cov = xb;
        }
    }
    FLUSH_CUR();
    if (next_line != 240 || fr_ovf) return 0;
    /* rows must be unique (otherwise two lines would share bitmap pixels) */
    static u8 used[240 + 8]; memset(used, 0, 240);
    for (int k = 0; k < fr_nbands; k++) {
        if (fr_band_special[k]) { if (used[fr_special_row[k]]) return 0; used[fr_special_row[k]] = 1; continue; }
        const struct FrBand *b = &fr_bands[k]; int r0 = b->row0, nn = b->nlines;
        for (int j = 0; j < nn; j++) { int rr = r0 + j; if (rr >= 240) rr -= 240; if (used[rr]) return 0; used[rr] = 1; }
    }
    return 1;
}

/* ---- sprites ----------------------------------------------------------------------------------------------------------- */
static u8 fr_claim[240][32]; static u8 fr_claim_gen[240]; static u8 fr_gen;
static struct { u16 row, bx; } fr_spr_hist[64 * 8 + 8]; static u16 fr_spr_hist_n;
static inline u32 get8(const u8 *p, int bitpos) { int i = bitpos >> 3, s = bitpos & 7; return ((((u32)p[i] << 8) | p[i + 1]) >> (8 - s)) & 0xFF; }
static inline void or8(u8 *p, int bitpos, u32 bits) { int i = bitpos >> 3, s = bitpos & 7; u32 v = (bits << 8) >> s; p[i] |= (u8)(v >> 8); p[i + 1] |= (u8)v; }
static inline void put8m(u8 *p, int bitpos, u32 mask8, u32 data8) {
    int i = bitpos >> 3, s = bitpos & 7; u32 m = (mask8 << 8) >> s, d = ((data8 & mask8) << 8) >> s;
    p[i] = (u8)((p[i] & ~(m >> 8)) | (d >> 8)); p[i + 1] = (u8)((p[i + 1] & ~m) | d);
}
static u8 *fr_priv_ptr[FR_MAXBANDS + 2];

static void fr_sprites(void) {
    static u8 cnt[240]; memset(cnt, 0, sizeof cnt);
    if (++fr_gen == 0) { memset(fr_claim_gen, 0, sizeof fr_claim_gen); fr_gen = 1; }
    fr_spr_hist_n = 0;
    for (int l = 0, k = 0; k < fr_nbands; k++) { int e = fr_bands[k].line0 + fr_bands[k].nlines; for (; l < e; l++) fr_lineband[l] = (u8)k; }
    for (int i = 0; i < 64; i++) {
        int y = oam[i * 4]; if (y >= 239) continue;
        u8 tid = oam[i * 4 + 1], attr = oam[i * 4 + 2]; int sx = oam[i * 4 + 3];
        const u8 *c = chr + (fr_spr_table ? 0x1000 : 0) + tid * 16;
        /* visible columns: x in [sx, sx+7], x <= 255, x >= 8 unless the left column of sprites is enabled */
        u32 cm = 0xFF; if (sx + 7 > 255) cm &= (0xFF << (sx + 7 - 255)) & 0xFF;
        if (!fr_mask_spr_left && sx < 8) { int k = 8 - sx; if (k >= 8) cm = 0; else cm &= 0xFF >> k; }
        int hflip = attr & 0x40, vflip = attr & 0x80, behind = attr & 0x20; fr_stat_sprites++;
        for (int r = 0; r < 8; r++) {
            int l = y + 1 + r; if (l > 239) break;
            if (cnt[l]++ >= 8) continue;
            if (!cm) continue;
            fr_stat_rows++;
            int rr = vflip ? 7 - r : r; u32 lo = c[rr], hi = c[rr + 8];
            if (hflip) { lo = fr_rev[lo]; hi = fr_rev[hi]; }
            u32 om = (lo | hi) & cm; if (!om) continue;
            u8 *claim = fr_claim[l];
            if (fr_claim_gen[l] != fr_gen) { memset(claim, 0, 32); fr_claim_gen[l] = fr_gen; }
            u32 cl = get8(claim, sx); u32 nw = om & ~cl; or8(claim, sx, om);       /* claims even when the pixel ends up hidden */
            if (!nw) continue;
            int bi = fr_lineband[l]; const struct FrBand *bd = &fr_bands[bi]; u8 *rowp; int x0;
            if (fr_band_special[bi]) { rowp = fr_priv_ptr[bi]; x0 = 0; } else { rowp = bd->base + (l - bd->line0) * FR_ROWB; x0 = bd->x0; }
            int bx = x0 + sx;                                           /* bitmap x of the sprite's first pixel */
            u32 vm = nw;
            if (behind) vm &= ~(get8(rowp, bx) | get8(rowp + FR_BW, bx));   /* behind the background: only where the background pixel is transparent */
            if (!vm) continue;
            fr_stat_drawn++;
            {   int s = bx & 7; u8 *pp = rowp + (bx >> 3); u32 m16 = (vm << 8) >> s, nm = ~m16 & 0xFFFF;
                u32 w;
                w = (u32)((pp[0] << 8) | pp[1]); w = (w & nm) | (((lo & vm) << 8) >> s); pp[0] = (u8)(w >> 8); pp[1] = (u8)w; pp += FR_BW;
                w = (u32)((pp[0] << 8) | pp[1]); w = (w & nm) | (((hi & vm) << 8) >> s); pp[0] = (u8)(w >> 8); pp[1] = (u8)w; pp += FR_BW;
                w = (u32)((pp[0] << 8) | pp[1]); w = (w & nm) | ((attr & 1) ? m16 : 0); pp[0] = (u8)(w >> 8); pp[1] = (u8)w; pp += FR_BW;
                w = (u32)((pp[0] << 8) | pp[1]); w = (w & nm) | ((attr & 2) ? m16 : 0); pp[0] = (u8)(w >> 8); pp[1] = (u8)w; pp += FR_BW;
                w = (u32)((pp[0] << 8) | pp[1]); w = w | m16; pp[0] = (u8)(w >> 8); pp[1] = (u8)w; }
            if (!fr_band_special[bi]) { int rowb = bd->row0 + (l - bd->line0); if (rowb >= 240) rowb -= 240; fr_spr_hist[fr_spr_hist_n].row = (u16)rowb; fr_spr_hist[fr_spr_hist_n].bx = (u16)bx; fr_spr_hist_n++; }
        }
    }
}
static void fr_sprites_restore(void) {                  /* put last frame's sprite rows back from the clean background */
    for (int k = 0; k < fr_spr_hist_n; k++) {
        int off = fr_spr_hist[k].row * FR_ROWB + (fr_spr_hist[k].bx >> 3);
        u8 *d = fr_bmp + off; const u8 *q = fr_clean + off;
        for (int p = 0; p < 5; p++) { int o = p * FR_BW; d[o] = q[o]; d[o + 1] = q[o + 1]; }
    }
    fr_spr_hist_n = 0;
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
    if (fr_ovf) { fr_all_dirty = 1; fr_stat_fallback++; return 0; }
    fr_sprites();
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
        const struct FrBand *bd = 0;
        for (int b = 0; b < fr_nbands; b++) if (l >= fr_bands[b].line0 && l < fr_bands[b].line0 + fr_bands[b].nlines) bd = &fr_bands[b];
        const u8 *rowp = bd->base + (l - bd->line0) * FR_ROWB; int x0 = bd->x0;
        for (int x = 0; x < 256; x++) {
            int bx = x0 + x, idx = 0;
            for (int p = 0; p < 5; p++) idx |= ((rowp[p * FR_BW + (bx >> 3)] >> (7 - (bx & 7))) & 1) << p;
            out[l][x] = pl[idx];
        }
    }
}
