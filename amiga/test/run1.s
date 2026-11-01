	.text
	.globl run1
| void run1(u32 fn, struct R *r)   struct R { d0,d1,d2,a2,a5,ccr,flat }
run1:
	movem.l %d2-%d7/%a2-%a6,-(%sp)
	move.l 48(%sp),%d7
	move.l 52(%sp),%a6
	move.l 24(%a6),%a0
	move.l 0(%a6),%d0
	move.l 4(%a6),%d1
	move.l 8(%a6),%d2
	move.l 12(%a6),%a2
	move.l 16(%a6),%a5
	move.l 20(%a6),%d4
	move.l %d7,%a1
	move.l %a6,-(%sp)
	move.w %d4,%ccr
	jsr (%a1)
	move.w %ccr,%d4
	move.l (%sp)+,%a6
	move.l %d0,0(%a6)
	move.l %d1,4(%a6)
	move.l %d2,8(%a6)
	move.l %a2,12(%a6)
	move.l %a5,16(%a6)
	moveq #0,%d5
	move.w %d4,%d5
	move.l %d5,20(%a6)
	movem.l (%sp)+,%d2-%d7/%a2-%a6
	rts
