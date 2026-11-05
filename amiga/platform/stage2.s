| stage2.s - second stage loader (position independent).  First bytes of the package that the bootblock / AmigaDOS stub brings into memory.
| Entered in SUPERVISOR mode with all interrupts and DMA off and the caches off, D6 = address of this package, A6 = ExecBase (still intact).
| 1. Walk ExecBase->MemList for the best non-chip memory block (highest priority, then largest) that is big enough: the A1200's fast RAM may be at
|    $200000 (trapdoor expansion) or anywhere else (accelerator board).  Nothing in the OS is used, the lists are only read.
| 2. Copy the game image and its relocation table there, add (new base - link base) to every absolute address listed in the table, jump to the image.
| The copy/relocation loop runs from chip RAM at $200 (the source may overlap the destination; the copy runs in the right direction).
| Failure: screen turns yellow (no suitable fast RAM).
    .text
    bra.w   s2
    .long   0                       | (header at offset 8)
hdr:                                | offset 8
game_off:   .long 0                 | byte offset of the game image in the package
game_len:   .long 0                 | its length (multiple of 4)
rel_off:    .long 0                 | byte offset of the relocation table (offsets of 32-bit words, relative to the start of the image)
rel_cnt:    .long 0
link_base:  .long 0                 | address the image was linked for
min_size:   .long 0                 | bytes needed from the new base (image, bss, stack)
s2:
    move.l  %d6,%a5
    move.l  322(%a6),%a0            | ExecBase->MemList.lh_Head
    sub.l   %a3,%a3                 | best MemHeader
    moveq   #-128,%d4               | its priority
    moveq   #0,%d5                  | its size
10: move.l  (%a0),%a1               | ln_Succ
    move.l  %a1,%d0
    beq.s   20f                     | end of the list
    move.w  14(%a0),%d0             | mh_Attributes
    btst    #1,%d0                  | MEMF_CHIP: no
    bne.s   19f
    btst    #2,%d0                  | MEMF_FAST: required
    beq.s   19f
    move.l  24(%a0),%d1             | mh_Upper
    sub.l   20(%a0),%d1             | - mh_Lower
    cmp.l   8+20(%a5),%d1
    bcs.s   19f                     | too small
    move.b  9(%a0),%d2              | ln_Pri
    cmp.b   %d4,%d2
    blt.s   19f
    bgt.s   18f
    cmp.l   %d5,%d1
    bls.s   19f
18: move.l  %a0,%a3
    move.b  %d2,%d4
    move.l  %d1,%d5
19: move.l  %a1,%a0
    bra.s   10b
20: move.l  %a3,%d0
    beq.s   fail
    move.l  20(%a3),%d0             | mh_Lower
    add.l   #0xFFF,%d0
    and.l   #-4096,%d0
    add.l   #0x800,%d0              | new image base (same low bits as the link address)
    move.l  24(%a3),%d1
    sub.l   %d0,%d1                 | room above it
    cmp.l   8+20(%a5),%d1
    bcs.s   fail
    move.l  %d0,%a4                 | a4 = new base
    sub.l   8+16(%a5),%d0
    move.l  %d0,%d3                 | d3 = delta
    lea     copier(%pc),%a0
    lea     0x200.w,%a1
    moveq   #(copier_end-copier)/2-1,%d0
21: move.w  (%a0)+,(%a1)+
    dbra    %d0,21b
    move.l  8+0(%a5),%d0            | game_off
    lea     0(%a5,%d0.l),%a0        | src = image
    move.l  %a4,%a1                 | dst
    move.l  8+8(%a5),%d1            | rel_off
    sub.l   %d0,%d1                 | table offset inside the copied block
    lea     0(%a4,%d1.l),%a2        | table in the destination
    move.l  8+12(%a5),%d2           | rel_cnt
    move.l  %d1,%d7
    move.l  %d2,%d0
    lsl.l   #2,%d0
    add.l   %d0,%d7                 | d7 = bytes to copy (image + table)
    move.l  %d2,%d1                 | d1 = count
    move.l  %d7,%d0
    lsr.l   #2,%d0                  | d0 = longs
    jmp     0x200.w
fail:
    move.w  #0x0FF0,%d1
1:  move.w  %d1,0xDFF180
    bra.s   1b

copier:                             | runs at $200: a0 src, a1 dst, a2 table (in dst), a4 base, d0 longs, d1 count, d3 delta, d7 bytes
    cmp.l   %a0,%a1
    bhi.s   3f
2:  move.l  (%a0)+,(%a1)+
    subq.l  #1,%d0
    bne.s   2b
    bra.s   6f
3:  lea     0(%a0,%d7.l),%a0
    lea     0(%a1,%d7.l),%a1
4:  move.l  -(%a0),-(%a1)
    subq.l  #1,%d0
    bne.s   4b
6:  tst.l   %d1
    beq.s   8f
7:  move.l  (%a2)+,%d2
    add.l   %d3,0(%a4,%d2.l)
    subq.l  #1,%d1
    bne.s   7b
8:  move.l  #0x0808,%d0             | clear the caches
    movec   %d0,%cacr
    jmp     (%a4)
copier_end:
