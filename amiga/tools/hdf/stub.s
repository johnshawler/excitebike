| AmigaDOS entry: takes over the machine and starts the game payload that follows this code (same hand-over as the ADF bootblock).
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
    move.l  %d6,%a0             | the package starts with the stage 2 loader (platform/stage2.s): it finds fast RAM and relocates the game there
    jmp     (%a0)

    .balign 4
payload_len:
    .long   0
payload:
