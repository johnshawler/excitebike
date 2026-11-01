| c2p.s - Zoo Keeper A1200: chunky -> 8 interleaved bitplanes, hand-written 68020 kernels used by display.c.
| The 16-pixel kernel (five merge steps, generated and verified against the bit-by-bit transpose; see DESIGN.md) is the same as the former inline asm.
|   c2p_pair(src, dst)          16 chunky bytes (4-byte aligned) -> 8 plane words at dst + 32 * p      (test hook, same code as the row loop)
|   c2p_row(src, dst, flags)    one VRAM row: flags = the 32 dirty-group bytes; every aligned pair of groups with a set flag is converted and its flags cleared
| C ABI: arguments on the stack, d2-d7 / a2-a6 preserved.
	.text
	.even
	.globl c2p_pair, c2p_row, c2p_uni_words
| a0 = src, a1 = dst (preserved), clobbers d0-d4 a3
.Lpair:
	move.l (%a0),%d0
	move.l %d0,%d1
	rol.l #8,%d1
	cmp.l %d0,%d1				| all four bytes of the first long equal?
	jbne .Lkern
	cmp.l 4(%a0),%d0
	jbne .Lkern
	cmp.l 8(%a0),%d0
	jbne .Lkern
	cmp.l 12(%a0),%d0
	jbne .Lkern
	and.w #255,%d0				| 16 pixels of one colour: each plane word is all ones or all zeros
	lsl.w #4,%d0
	lea c2p_uni_words,%a3
	lea 0(%a3,%d0.w),%a3
	move.w (%a3)+,(%a1)
	move.w (%a3)+,32(%a1)
	move.w (%a3)+,64(%a1)
	move.w (%a3)+,96(%a1)
	move.w (%a3)+,128(%a1)
	move.w (%a3)+,160(%a1)
	move.w (%a3)+,192(%a1)
	move.w (%a3)+,224(%a1)
	rts
.Lkern:
	move.l 12(%a0),%d0
	move.l 8(%a0),%d1
	move.l 4(%a0),%d2
	move.l (%a0),%d3
	move.l %d0,%d4
	lsr.l #8,%d4
	eor.l %d2,%d4
	and.l #0x00FF00FF,%d4
	eor.l %d4,%d2
	lsl.l #8,%d4
	eor.l %d4,%d0
	move.l %d1,%d4
	lsr.l #8,%d4
	eor.l %d3,%d4
	and.l #0x00FF00FF,%d4
	eor.l %d4,%d3
	lsl.l #8,%d4
	eor.l %d4,%d1
	move.l %d0,%d4
	lsr.l #4,%d4
	eor.l %d1,%d4
	and.l #0x0F0F0F0F,%d4
	eor.l %d4,%d1
	lsl.l #4,%d4
	eor.l %d4,%d0
	move.l %d2,%d4
	lsr.l #4,%d4
	eor.l %d3,%d4
	and.l #0x0F0F0F0F,%d4
	eor.l %d4,%d3
	lsl.l #4,%d4
	eor.l %d4,%d2
	swap %d0
	move.w %d0,%d4
	move.w %d1,%d0
	swap %d0
	move.w %d4,%d1
	swap %d2
	move.w %d2,%d4
	move.w %d3,%d2
	swap %d2
	move.w %d4,%d3
	move.l %d0,%d4
	lsr.l #2,%d4
	eor.l %d1,%d4
	and.l #0x33333333,%d4
	eor.l %d4,%d1
	lsl.l #2,%d4
	eor.l %d4,%d0
	move.l %d2,%d4
	lsr.l #2,%d4
	eor.l %d3,%d4
	and.l #0x33333333,%d4
	eor.l %d4,%d3
	lsl.l #2,%d4
	eor.l %d4,%d2
	move.l %d0,%d4
	lsr.l #1,%d4
	eor.l %d2,%d4
	and.l #0x55555555,%d4
	eor.l %d4,%d2
	lsl.l #1,%d4
	eor.l %d4,%d0
	move.l %d1,%d4
	lsr.l #1,%d4
	eor.l %d3,%d4
	and.l #0x55555555,%d4
	eor.l %d4,%d3
	lsl.l #1,%d4
	eor.l %d4,%d1
	move.w %d0,0(%a1)
	swap %d0
	move.w %d0,128(%a1)
	move.w %d1,64(%a1)
	swap %d1
	move.w %d1,192(%a1)
	move.w %d2,32(%a1)
	swap %d2
	move.w %d2,160(%a1)
	move.w %d3,96(%a1)
	swap %d3
	move.w %d3,224(%a1)
	rts

c2p_pair:
	movem.l %d2-%d4/%a3,-(%sp)
	move.l 20(%sp),%a0
	move.l 24(%sp),%a1
	jbsr .Lpair
	movem.l (%sp)+,%d2-%d4/%a3
	rts

c2p_row:
	movem.l %d2-%d4/%d7/%a2-%a3,-(%sp)
	move.l 28(%sp),%a0			| src (VRAM row)
	move.l 32(%sp),%a1			| dst (plane 0 of the screen line)
	move.l 36(%sp),%a2			| flags
	moveq #7,%d7				| eight longs of flags = 16 pairs
.Lq:
	tst.l (%a2)
	jbeq .Lskip
	tst.w (%a2)
	jbeq .Lsecond
	clr.w (%a2)
	jbsr .Lpair
.Lsecond:
	tst.w 2(%a2)
	jbeq .Lskip
	clr.w 2(%a2)
	lea 16(%a0),%a0
	addq.l #2,%a1
	jbsr .Lpair
	lea -16(%a0),%a0
	subq.l #2,%a1
.Lskip:
	lea 32(%a0),%a0
	addq.l #4,%a1
	addq.l #4,%a2
	dbra %d7,.Lq
	movem.l (%sp)+,%d2-%d4/%d7/%a2-%a3
	rts
