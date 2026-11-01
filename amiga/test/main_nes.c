/* qemu-m68k harness for the whole translated game: runs N frames with the scripted input of ref/nesref.c and writes the same trace/dump format */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rt/rt.h"
extern u8 *fr_bmp, *fr_priv; extern u32 stat_fr_bad, fr_stat_ok, fr_stat_fallback, fr_stat_cells; extern u8 fr_enabled; extern u32 stat_segs, stat_syncs;
extern void exb_run(u8 *ram, const u8 *prg, struct RT *rt, void **disp);
extern void *dispatch_tab[16384];
static FILE *tr, *pixf; static int maxframes, start_frame;
FILE *dbg; void c_dbg(u32 pc, u32 cyc, u32 a, u32 x, u32 y, u32 ccr) { if (dbg) fprintf(dbg, "B %u %04X A%02X X%02X Y%02X F%02X\n", cyc, pc, a & 255, x & 255, y & 255, ccr); }
void c_abort(int code, u32 cyc) { fprintf(stderr, "ABORT %d at cycle %u frame %u\n", code, cyc, c_frame_no()); if (tr) fclose(tr); exit(3); }
void plat_trace(char kind, u32 frame, u32 cyc, u16 addr, u8 val) { if (tr) fprintf(tr, "%c %u %llu %04X %02X\n", kind, frame, (unsigned long long)(c_epoch() + cyc), addr, val); }
static void hexline(const char *k, u32 f, const u8 *p, int n) { fprintf(tr, "%s %u ", k, f); for (int i = 0; i < n; i++) fprintf(tr, "%02X", p[i]); fputc('\n', tr); }
static struct { long from, to; int mask, period, on; } scr[64]; static int nscr = -1;
static void load_script(void) {
    nscr = 0; const char *fn = getenv("INPUT_SCRIPT"); if (!fn) return;
    FILE *f = fopen(fn, "r"); if (!f) { perror(fn); exit(1); } char line[256];
    while (fgets(line, sizeof line, f)) { if (line[0] == '#' || line[0] == '\n') continue; long a, b; int m, p, o;
        if (sscanf(line, "%ld %ld %x %d %d", &a, &b, &m, &p, &o) == 5 && nscr < 64) { scr[nscr].from = a; scr[nscr].to = b; scr[nscr].mask = m; scr[nscr].period = p; scr[nscr].on = o; nscr++; } }
    fclose(f);
}
static u8 *padfile; static long padlen = -1;
static u8 pad_for(long f) {
    long s = start_frame; u8 b = 0;
    if (padlen == -1) { padlen = 0; const char *fn = getenv("INPUT_PAD"); if (fn) { FILE *pf = fopen(fn, "rb"); if (!pf) { perror(fn); exit(1); } padfile = malloc(1 << 20); padlen = fread(padfile, 1, 1 << 20, pf); fclose(pf); } }
    if (padlen > 0) return f < padlen ? padfile[f] : 0;
    if (nscr == -1) load_script();
    if (nscr > 0) { for (int i = 0; i < nscr; i++) if (f >= scr[i].from && f < scr[i].to && (scr[i].period == 0 || (f % scr[i].period) < scr[i].on)) b |= scr[i].mask; return b; }
    if (f >= s && f < s + 4) b |= 0x08;
    if (f >= s + 10 && (f % 7) != 0) b |= 0x01;
    if (f >= s + 300 && (f % 40) < 20) b |= 0x80;
    if (f >= s + 200 && (f % 97) < 5) b |= 0x10;
    if (f >= s + 260 && (f % 89) < 5) b |= 0x20;
    if (f >= s + 120 && (f % 3) == 0) b |= 0x02;
    return b;
}
void plat_pad(u32 f) { c_set_pad(pad_for(f)); }
void plat_frame(u32 f) {
    if (pixf && ((f >= 495 && f <= 530) || (f % 10 == 0 && f >= 100))) { fputc(f & 255, pixf); fputc(f >> 8, pixf); fwrite(frame_pix, 1, sizeof frame_pix, pixf); }
    if (tr) {
        u8 full[4096]; for (int i = 0; i < 4096; i++) full[i] = nt[(i >> 10) & 1][i & 0x3ff];
        hexline("NT", f, full, 4096); hexline("PAL", f, pal, 32); hexline("OAMRAM", f, ram + 0x200, 256); hexline("RAM", f, ram, 2048);
    }
    if ((int)f >= maxframes) { if (tr) fclose(tr); if (pixf) fclose(pixf); fprintf(stderr, "segments %u syncs %u frames %u; fast renderer: %u frames ok, %u fallback, %u cells drawn, %u mismatching frames\n", stat_segs, stat_syncs, f, fr_stat_ok, fr_stat_fallback, fr_stat_cells, stat_fr_bad); exit(0); }
}
static u8 fr_bmp_store[240 * 500], fr_priv_store[4 * 500];
void plat_fr_report(u32 frame, int bad, int first) { fprintf(stderr, "FAST RENDERER MISMATCH frame %u: %d pixels, first at line %d x %d\n", frame, bad, first / 256, first % 256); }
int main(int argc, char **argv) {
    fr_bmp = fr_bmp_store; fr_priv = fr_priv_store; if (getenv("NOFAST")) fr_enabled = 0;
    if (argc < 4) { fprintf(stderr, "usage: exb rom.nes frames start_frame [trace] [pix]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); static u8 rom[24592]; if (!f || fread(rom, 1, sizeof rom, f) != sizeof rom) return 1; fclose(f);
    prg = rom + 16; chr = rom + 16 + 16384; maxframes = atoi(argv[2]); start_frame = atoi(argv[3]);
    if (argc > 4) tr = fopen(argv[4], "w");
    if (argc > 5) pixf = fopen(argv[5], "wb");
    if (getenv("DBG")) dbg = fopen(getenv("DBG"), "w");
    c_reset();
    exb_run(ram, prg, &rt, dispatch_tab);
    return 0;
}
