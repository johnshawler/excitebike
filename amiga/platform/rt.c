/*
 * rt.c - tiny 68000-safe runtime (no libc, no libgcc needed).
 * The cross libgcc may contain 68020 mulu.l/divu.l; these replace it.
 */
#include <stddef.h>
#include <stdint.h>
#include "platform.h"

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--) *dp++ = (uint8_t)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

/* __mulsi3 is in rtasm.s (plain C would be compiled into a call to itself) */

static uint32_t udivmod(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return 0xFFFFFFFFu; }
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint32_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint32_t __udivsi3(uint32_t n, uint32_t d) { return udivmod(n, d, NULL); }
uint32_t __umodsi3(uint32_t n, uint32_t d) { uint32_t r; udivmod(n, d, &r); return r; }
int32_t  __divsi3(int32_t n, int32_t d)
{
    int neg = 0;
    if (n < 0) { n = -n; neg ^= 1; }
    if (d < 0) { d = -d; neg ^= 1; }
    uint32_t q = udivmod((uint32_t)n, (uint32_t)d, NULL);
    return neg ? -(int32_t)q : (int32_t)q;
}
int32_t  __modsi3(int32_t n, int32_t d)
{
    int neg = n < 0;
    uint32_t r;
    if (n < 0) n = -n;
    if (d < 0) d = -d;
    udivmod((uint32_t)n, (uint32_t)d, &r);
    return neg ? -(int32_t)r : (int32_t)r;
}

/* a*b/c with a 64-bit intermediate (result must fit in 32 bits; c != 0) */
uint32_t plat_muldiv(uint32_t a, uint32_t b, uint32_t c)
{
    uint16_t al = (uint16_t)a, ah = (uint16_t)(a >> 16);
    uint16_t bl = (uint16_t)b, bh = (uint16_t)(b >> 16);
    uint32_t ll = (uint32_t)al * bl, lh = (uint32_t)al * bh, hl = (uint32_t)ah * bl, hh = (uint32_t)ah * bh;
    uint32_t mid = lh + hl;                       /* may carry */
    uint32_t midc = (mid < lh) ? 1u : 0u;
    uint32_t lo = ll + (mid << 16);
    uint32_t hi = hh + (mid >> 16) + (midc << 16) + (lo < ll ? 1u : 0u);
    uint32_t q = 0, r = 0;
    if (c == 0) return 0xFFFFFFFFu;
    for (int i = 63; i >= 0; i--) {
        uint32_t bit = (i >= 32) ? (hi >> (i - 32)) & 1 : (lo >> i) & 1;
        uint32_t top = r >> 31;
        r = (r << 1) | bit;
        if (top || r >= c) { r -= c; if (i < 32) q |= (uint32_t)1 << i; }
    }
    return q;
}
