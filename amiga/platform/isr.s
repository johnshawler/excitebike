| isr.s - exception/interrupt vector table and register-saving stubs.
| ISR bodies are C functions (plat_isr_*_c in platform.c).
    .text
    .macro  ISR name, cfunc
    .globl  \name
\name:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    jsr     \cfunc
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rte
    .endm

    | profiling variant: also hands the interrupted PC to plat_prof_pc (weak no-op in platform.c)
    .macro  ISRP name, cfunc
    .globl  \name
\name:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    move.l  18(%sp),-(%sp)
    jsr     plat_prof_pc
    addq.l  #4,%sp
    jsr     \cfunc
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rte
    .endm

    ISRP isr_vbl,  plat_isr_vbl_c       | level 3  VERTB
    ISRP isr_ciab, plat_isr_ciab_c      | level 6  CIA-B (game tick)
    ISRP isr_ports, plat_isr_ports_c     | level 2  CIA-A (keyboard)
    ISR isr_aud,   plat_isr_audio_c     | level 4  Paula audio

isr_null:                               | levels 1,5,7 / spurious: just return
    rte

| Fatal exception: stop everything, paint COLOR00 with a colour per class and
| hang.  (Copper DMA is switched off so the copper cannot repaint it.)
    .macro  CRASH name, colour
\name:
    move.w  #\colour,%d0
    bra     crash_common
    .endm
    CRASH crash_bus,    0x0F0F          | bus error / address error: magenta
    CRASH crash_ill,    0x0F00          | illegal instruction: red
    CRASH crash_div,    0x0FF0          | divide by zero / chk / trapv: yellow
    CRASH crash_priv,   0x00FF          | privilege violation / trace / line A,F: cyan
    CRASH crash_other,  0x0F80          | everything else: orange
crash_common:
    move.w  #0x7FFF,0xDFF09A            | INTENA off
    move.w  #0x0080,0xDFF096            | copper DMA off
    move.w  %d0,0xDFF180                | COLOR00
1:  move.w  %d0,0xDFF180
    bra.s   1b

    .section .rodata
    .globl  plat_vectors
    .align  2
plat_vectors:
    .long   _stack_top                  | 0  initial SSP
    .long   _start                      | 1  initial PC
    .long   crash_bus, crash_bus        | 2,3 bus error, address error
    .long   crash_ill                   | 4  illegal
    .long   crash_div                   | 5  divide by zero
    .long   crash_div, crash_div        | 6,7 CHK, TRAPV
    .long   crash_priv                  | 8  privilege
    .long   crash_priv                  | 9  trace
    .long   crash_priv, crash_priv      | 10,11 line A / line F
    .rept   12                          | 12..23 reserved
    .long   crash_other
    .endr
    .long   isr_null                    | 24 spurious
    .long   isr_null                    | 25 level 1
    .long   isr_ports                   | 26 level 2
    .long   isr_vbl                     | 27 level 3
    .long   isr_aud                     | 28 level 4
    .long   isr_null                    | 29 level 5
    .long   isr_ciab                    | 30 level 6
    .long   isr_null                    | 31 level 7
    .rept   32                          | 32..63 TRAP #0-15 + reserved
    .long   crash_other
    .endr
