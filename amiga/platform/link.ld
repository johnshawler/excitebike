/* Linker script: Excitebike for the A1200 (2 MB chip + 2 MB fast RAM at $200000).
 *   fast RAM:  0x200800  .text .rodata .data (= payload on disk, loaded by the bootblock)   .bss   stack   tables
 *   chip RAM:  0x000000-0x0003FF vectors, 0x000200 bootblock relocator (boot time only), 0x100000 .chipbuf (display, copper, audio)
 */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
PROVIDE(_stack_size = 0x4000);
SECTIONS
{
    . = 0x200800;
    _payload_base = .;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    }
    _load_end = .;
    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    }
    .stack (NOLOAD) : ALIGN(4) {
        . += _stack_size;
        _stack_top = .;
    }
    ASSERT(_stack_top <= 0x400000, "image does not fit in 2 MB of fast RAM")
    .chipbuf 0x100000 (NOLOAD) : ALIGN(16) {
        *(.chipbuf)
        . = ALIGN(4);
    }
    ASSERT(. <= 0x1F0000, "chip buffers too large")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
