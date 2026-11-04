| AmigaDOS entry: takes over the machine and starts the game payload that follows this code (same hand-over as the ADF bootblock).
    .equ    PAYLOAD_BASE, 0x200800
    .equ    RELOC_ADDR,   0x200
    .text
start:
    move.l  4.w,%a6
    move.w  296(%a6),%d0        | AttnFlags: refuse 68040/68060
    andi.w  #0x88,%d0
    beq.s   ok
    moveq   #20,%d0
    rts
ok:
    lea     payload(%pc),%a0
    move.l  %a0,%d6
    move.l  payload_len(%pc),%d7
    lea     super(%pc),%a5
    jsr     -30(%a6)            | Supervisor - never returns
fail_nofast:
    move.w  #0x0FF0,%d1
    move.w  #0x7FFF,0xDFF096
    move.w  #0x7FFF,0xDFF09A
1:  move.w  %d1,0xDFF180
    bra.s   1b
super:
    move.w  #0x2700,%sr
    lea     0x400.w,%sp
    move.w  #0x7FFF,0xDFF09A
    move.w  #0x7FFF,0xDFF09C
    move.b  0xDFF002,%d0
2:  btst    #6,0xDFF002
    bne.s   2b
    move.w  #0x7FFF,0xDFF096
    move.w  #0x7FFF,0xDFF09C
    move.l  #0x0808,%d0
    movec   %d0,%cacr
    move.l  #0x200000,%a0
    move.l  #0x3FFFFC,%a1
    move.l  #0xA5C3E187,%d0
    move.l  %d0,(%a0)
    not.l   %d0
    move.l  %d0,(%a1)
    not.l   %d0
    cmp.l   (%a0),%d0
    bne     fail_nofast
    not.l   %d0
    cmp.l   (%a1),%d0
    bne     fail_nofast
    lea     reloc(%pc),%a0
    lea     RELOC_ADDR.w,%a1
    moveq   #(reloc_end-reloc)/2-1,%d0
3:  move.w  (%a0)+,(%a1)+
    dbra    %d0,3b
    move.l  %d6,%a0
    move.l  #PAYLOAD_BASE,%a1
    move.l  %d7,%d0
    lsr.l   #2,%d0
    jmp     RELOC_ADDR.w
reloc:
    cmp.l   %a0,%a1
    bhi.s   2f
1:  move.l  (%a0)+,(%a1)+
    subq.l  #1,%d0
    bne.s   1b
    jmp     PAYLOAD_BASE
2:  lea     0(%a0,%d7.l),%a0
    lea     0(%a1,%d7.l),%a1
3:  move.l  -(%a0),-(%a1)
    subq.l  #1,%d0
    bne.s   3b
    jmp     PAYLOAD_BASE
reloc_end:
    .balign 4
payload_len:
    .long   0
payload:
