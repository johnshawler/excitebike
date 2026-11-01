/* runs the per-instruction tests under qemu-m68k: loads vectors, executes the translated function, compares registers, N/Z/C, memory, cycles */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
extern u32 ttab[];
struct R { u32 d0, d1, d2, a2, a5, ccr; u8 *flat; };
extern void run1(u32 fn, struct R *r);
static u8 flat[65536];
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "r"); char name[64]; long total = 0, fail = 0; unsigned n = 0;
    unsigned pc, s, a, x, y, p, cnt;
    while (fscanf(f, " T %63[^|]|", name) == 1) {
        static unsigned ia[64], iv[64];
        if (fscanf(f, "%u %u %u %u %u %u %u", &pc, &s, &a, &x, &y, &p, &cnt) != 7) return 2;
        for (unsigned i = 0; i < cnt; i++) if (fscanf(f, "%u %u", &ia[i], &iv[i]) != 2) return 2;
        memset(flat, 0, sizeof flat); for (unsigned i = 0; i < cnt; i++) flat[ia[i]] = iv[i];
        struct R r; r.d0 = a; r.d1 = x; r.d2 = y; r.a2 = (u32)(flat + 0x100 + s); r.a5 = 0; r.flat = flat;
        r.ccr = ((p & 0x80) ? 8 : 0) | ((p & 2) ? 4 : 0) | ((p & 1) ? 0x11 : 0);
        run1(ttab[n], &r);
        unsigned fpc, fs, fa, fx, fy, fp, nf; if (fscanf(f, "%u %u %u %u %u %u %u", &fpc, &fs, &fa, &fx, &fy, &fp, &nf) != 7) return 2;
        char why[160] = ""; int bad = 0;
        unsigned S = (r.a2 - (u32)flat - 0x100) & 255;
        unsigned P = ((r.ccr & 8) ? 0x80 : 0) | ((r.ccr & 4) ? 2 : 0) | ((r.ccr & 1) ? 1 : 0);
        if ((r.d0 & 255) != fa || (r.d1 & 255) != fx || (r.d2 & 255) != fy || S != fs) { bad = 1; sprintf(why, "regs a%02x x%02x y%02x s%02x vs a%02x x%02x y%02x s%02x", r.d0 & 255, r.d1 & 255, r.d2 & 255, S, fa, fx, fy, fs); }
        else if ((r.d1 >> 8) || (r.d2 >> 8)) { bad = 1; sprintf(why, "upper bits of X/Y dirty %x %x", r.d1, r.d2); }
        else if ((P & 0x83) != (fp & 0x83)) { bad = 1; sprintf(why, "flags %02x vs %02x (N Z C)", P & 0x83, fp & 0x83); }
        for (unsigned i = 0; i < nf; i++) { unsigned aa, vv; if (fscanf(f, "%u %u", &aa, &vv) != 2) return 2; if (!bad && flat[aa] != vv) { bad = 1; sprintf(why, "mem[%04x]=%02x vs %02x", aa, flat[aa], vv); } }
        unsigned nc; if (fscanf(f, "%u", &nc) != 1) return 2;
        for (unsigned i = 0; i < nc; i++) { unsigned aa, vv, rw; if (fscanf(f, "%u %u %u", &aa, &vv, &rw) != 3) return 2; }
        if (!bad && r.a5 != nc) { bad = 1; sprintf(why, "cycles %u vs %u", r.a5, nc); }
        total++; if (bad) { if (fail < 25) printf("FAIL %s: %s\n", name, why); fail++; }
        n++;
    }
    printf("%ld tests, %ld failures\n", total, fail);
    return fail != 0;
}
