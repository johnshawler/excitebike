| crt0.s - payload entry (A1200).  Linked first (section .text.start) at 0x800.
| Entered from the bootblock relocator in SUPERVISOR mode with interrupts masked, or via plat_restart().  Never returns.
| Uses nothing from the OS.  68020: VBR := 0 (the vectors are installed at chip address 0), caches: instruction cache cleared and
| enabled, data cache (68030) off - the custom chips/CIAs must never be cached.
    .section .text.start,"ax"
    .globl  _start
_start:
    move.w  #0x2700,%sr             | supervisor, all interrupts masked
    moveq   #0,%d0
    movec   %d0,%vbr
    move.l  #0x0809,%d0             | CACR: clear I-cache (CI, bit 3), clear D-cache (CD, bit 11, 68030 only), enable I-cache (EI, bit 0)
    movec   %d0,%cacr
    lea     _stack_top,%sp
    jsr     plat_hw_reset           | quiesce all chips (no BSS use before here)
    | zero .bss
    lea     _bss_start,%a0
    lea     _bss_end,%a1
1:  cmp.l   %a0,%a1
    bls.s   2f
    clr.l   (%a0)+
    bra.s   1b
2:
    | install the 64 exception vectors at 0x000000 (chip RAM; OVL is 0)
    lea     plat_vectors,%a0
    sub.l   %a1,%a1
    moveq   #63,%d0
3:  move.l  (%a0)+,(%a1)+
    dbra    %d0,3b
    jsr     plat_init               | detect standard, display, CIA, IRQs
    jsr     main
4:  bra.s   4b                      | main() returned: idle forever
