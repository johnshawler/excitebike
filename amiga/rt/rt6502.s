| rt6502.s - assembler glue between the translated 6502 code and nes_rt.c   (68020, GNU as)
| Conventions (see tools/xl6502.py):  d0=A d1=X d2=Y  d4,d5,d6 scratch  a0=RAM a1=PRG a2=6502 SP pointer a3=struct RT a4=dispatch table a5=cycle counter
| I/O stubs: d5 = cycle offset of the access relative to a5, d6 = value (writes) / result (reads), d4 = address (generic stubs)
	.text
	.even
	.macro RDSTUB name, addr
	.globl \name
\name:
	movem.l %d0-%d1/%a0-%a1,-(%sp)
	add.l %a5,%d5
	move.l #\addr,-(%sp)
	move.l %d5,-(%sp)
	jsr c_rd
	addq.l #8,%sp
	move.l %d0,%d6
	movem.l (%sp)+,%d0-%d1/%a0-%a1
	rts
	.endm
	.macro WRSTUB name, addr
	.globl \name
\name:
	movem.l %d0-%d1/%a0-%a1,-(%sp)
	add.l %a5,%d5
	and.l #0xff,%d6
	move.l %d6,-(%sp)
	move.l #\addr,-(%sp)
	move.l %d5,-(%sp)
	jsr c_wr
	lea 12(%sp),%sp
	adda.l %d0,%a5
	movem.l (%sp)+,%d0-%d1/%a0-%a1
	rts
	.endm
	.irp n,0,1,2,3,4,5,6,7
	RDSTUB rt_rd_200\n, 0x200\n
	WRSTUB rt_wr_200\n, 0x200\n
	.endr
	.irp n,0,1,2,3,4,5,6,7,8,9,a,b,c,d,e,f
	RDSTUB rt_rd_400\n, 0x400\n
	WRSTUB rt_wr_400\n, 0x400\n
	RDSTUB rt_rd_401\n, 0x401\n
	WRSTUB rt_wr_401\n, 0x401\n
	.endr
	.globl rt_rd_gen
rt_rd_gen:
	movem.l %d0-%d1/%a0-%a1,-(%sp)
	add.l %a5,%d5
	and.l #0xffff,%d4
	move.l %d4,-(%sp)
	move.l %d5,-(%sp)
	jsr c_rd
	addq.l #8,%sp
	move.l %d0,%d6
	movem.l (%sp)+,%d0-%d1/%a0-%a1
	rts
	.globl rt_wr_gen
rt_wr_gen:
	movem.l %d0-%d1/%a0-%a1,-(%sp)
	add.l %a5,%d5
	and.l #0xffff,%d4
	and.l #0xff,%d6
	move.l %d6,-(%sp)
	move.l %d4,-(%sp)
	move.l %d5,-(%sp)
	jsr c_wr
	lea 12(%sp),%sp
	adda.l %d0,%a5
	movem.l (%sp)+,%d0-%d1/%a0-%a1
	rts

| d4.w = 6502 PC of the instruction about to execute, d5.w = saved ccr (valid when the flags were live).  Returns with the flags intact.
	.globl rt_nmi_deliver
rt_nmi_deliver:
	movem.l %d0-%d1/%a0-%a1,-(%sp)
	move.l %d4,%d3
	move.l %d5,%d6
	move.l %a5,-(%sp)
	jsr c_event
	addq.l #4,%sp
	tst.l %d0
	beq.s 9f
	| NMI: push PCH, PCL, P and enter the handler
	moveq #0x24,%d4
	btst #3,%d6
	beq.s 1f
	or.b #0x80,%d4
1:	btst #2,%d6
	beq.s 2f
	or.b #0x02,%d4
2:	btst #0,%d6
	beq.s 3f
	or.b #0x01,%d4
3:	move.b %d4,-2(%a2)
	move.w %d3,%d5
	lsr.w #8,%d5
	move.b %d5,(%a2)
	move.b %d3,-1(%a2)
	lea -3(%a2),%a2
	lea 7(%a5),%a5
	jsr c_nmi_taken
	movem.l (%sp)+,%d0-%d1/%a0-%a1
	jsr L_C23B
	rts
9:	movem.l (%sp)+,%d0-%d1/%a0-%a1
	move.w %d6,%ccr
	rts

	.globl rt_rti
rt_rti:
	move.b 1(%a2),%d4
	lea 3(%a2),%a2
	moveq #0,%d5
	btst #7,%d4
	beq.s 1f
	or.b #8,%d5
1:	btst #1,%d4
	beq.s 2f
	or.b #4,%d5
2:	btst #0,%d4
	beq.s 3f
	or.b #0x11,%d5
3:	move.w %d5,%ccr
	rts

	.globl rt_bad_flow
rt_bad_flow:
	moveq #1,%d0
	bra.s rt_die
	.globl rt_bad_jump
rt_bad_jump:
	moveq #2,%d0
rt_die:
	move.l %a5,-(%sp)
	move.l %d0,-(%sp)
	jsr c_abort

| void exb_run(u8 *ram, const u8 *prg, struct RT *rt, void **dispatch)   - never returns
	.globl exb_run
exb_run:
	move.l 4(%sp),%a0
	move.l 8(%sp),%a1
	move.l 12(%sp),%a3
	move.l 16(%sp),%a4
	lea 0x1fd(%a0),%a2
	suba.l %a5,%a5
	addq.l #7,%a5
	moveq #0,%d0
	moveq #0,%d1
	moveq #0,%d2
	move.w #0,%ccr
	jmp L_C184

| debug: log the entry of every translated block (xl6502 --trace): d4.w = block address
	.globl rt_dbg_block
rt_dbg_block:
	move.w %ccr,-(%sp)
	movem.l %d0-%d7/%a0-%a6,-(%sp)
	move.l 66(%sp),%d4
	and.l #0xffff,%d4
	moveq #0,%d6
	move.w 60(%sp),%d6
	and.l #0x1f,%d6
	move.l %d6,-(%sp)
	move.l %d2,-(%sp)
	move.l %d1,-(%sp)
	move.l %d0,-(%sp)
	move.l %a5,-(%sp)
	move.l %d4,-(%sp)
	jsr c_dbg
	lea 24(%sp),%sp
	movem.l (%sp)+,%d0-%d7/%a0-%a6
	move.w (%sp)+,%ccr
	rts
