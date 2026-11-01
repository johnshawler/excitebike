#ifndef RT_H
#define RT_H
#include <stdint.h>
typedef int16_t s16; typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
struct RT { u32 ev_due; u32 rebase; u32 idle_a; u32 idle_ccr; u32 fuse_val; u32 fuse_exited; };                 /* offset 0: polled by translated main-thread code (cmpa.l ev_due(%a3),%a5) */
extern struct RT rt;
extern u8 ram[2048]; extern const u8 *prg, *chr; extern u8 nt[2][1024], pal[32], oam[256]; extern u8 frame_pix[240][256];
u32 c_rd(u32 stamp, u32 addr); u32 c_wr(u32 stamp, u32 addr, u32 val); u32 c_event(u32 now); void c_nmi_taken(void);
void c_set_pad(u8 p); void c_reset(void); u32 c_frame_no(void);
u32 c_fuse_poll(u32 T0, u32 maskv, u32 whilenz, u32 limit, u32 *val, u32 *exited); u32 c_idle(u32 now, u32 limit, u32 S); u64 c_epoch(void);
void plat_frame(u32 frame); void plat_pad(u32 frame); void plat_trace(char kind, u32 frame, u32 cyc, u16 addr, u8 val); 

struct ApuOut {
    u16 per[4];          /* timer register (11 bit) for pulse1, pulse2, triangle; noise: period in CPU cycles */
    u8 vol[4];           /* 0..15 (triangle: 15 when audible); 0 = silent */
    u8 duty[2];          /* pulse duty 0..3 */
    u8 noise_short;      /* noise mode bit */
    u8 dac;              /* $4011 level */
    u8 retrig[4];        /* set when the channel was (re)started inside this frame (length load) */
};
void plat_apu(const struct ApuOut *o, u32 frame);
#endif
