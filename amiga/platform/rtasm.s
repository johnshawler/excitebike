| rtasm.s - 32x32->32 multiply for the 68000 (libgcc's may use 68020 mulu.l)
    .text
    .globl  __mulsi3
__mulsi3:                       | uint32 __mulsi3(uint32 a, uint32 b); a=4(sp) b=8(sp)
    move.w  4(%sp),%d0          | ah
    mulu.w  10(%sp),%d0         | ah*bl
    move.w  6(%sp),%d1          | al
    mulu.w  8(%sp),%d1          | al*bh
    add.w   %d1,%d0
    swap    %d0
    clr.w   %d0                 | (ah*bl + al*bh) << 16
    move.w  6(%sp),%d1
    mulu.w  10(%sp),%d1         | al*bl
    add.l   %d1,%d0
    rts
