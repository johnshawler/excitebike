	.section .rodata
	.globl rom_prg, rom_chr
	.balign 4
rom_prg:
	.incbin "rom.bin", 0, 16384
rom_chr:
	.incbin "rom.bin", 16384, 8192
	.globl demo_pad, demo_end
demo_pad:
	.incbin "demo.bin"
demo_end:
	.balign 4
