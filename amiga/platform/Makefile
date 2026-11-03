# Excitebike (Europe) for the Amiga 1200:   make ROM=/path/to/Excitebike\ \(Europe\).nes   ->  build/exb_a1200.adf
PREFIX ?= m68k-linux-gnu-
CC := $(PREFIX)gcc
LD := $(PREFIX)ld -z noexecstack --no-warn-rwx-segments
OBJCOPY := $(PREFIX)objcopy
HERE := $(CURDIR)
A := $(abspath ..)
B ?= $(HERE)/build
ROM ?= $(A)/exb.nes
CFLAGS := -MMD -MP -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -Wall -Wno-misleading-indentation -I$(HERE) -I$(B) -DA1200 -DFR_HWBLIT $(EXTRA)
ASFLAGS := -m68020
all: $(B)/exb_a1200.adf
$(B):
	mkdir -p $(B)
$(B)/cfg.json: $(ROM) $(A)/tools/cfg6502.py $(A)/tools/seeds_ind.txt $(A)/tools/always_taken.txt | $(B)
	python3 -I $(A)/tools/cfg6502.py $(ROM) --seeds $(A)/tools/seeds_ind.txt --always $(A)/tools/always_taken.txt --skip E6B7 > $@
$(B)/prog.s: $(B)/cfg.json $(A)/tools/xl6502.py $(A)/tools/m6502.py
	python3 -I $(A)/tools/xl6502.py $(ROM) $(B)/cfg.json $(A)/tools/seeds_ind.txt $@
$(B)/rom.bin: $(ROM) | $(B)
	python3 -I -c "d=open('$(ROM)','rb').read(); assert len(d)==24592 and d[:4]==b'NES\x1a'; open('$@','wb').write(d[16:])"
$(B)/nespal.h: $(A)/tools/palette.py | $(B)
	python3 -I -c "import sys; sys.path.insert(0,'$(A)/tools'); import palette; t=palette.table(0); open('$@','w').write('static const unsigned nes_pal_rgb[64] = {' + ','.join('0x%02X%02X%02X'%c for c in t) + '};\n')"
$(B)/%.o: $(B)/%.s
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/demo.bin: | $(B)
	if [ -n "$(DEMOPAD)" ]; then cp $(DEMOPAD) $@; else : > $@; fi
$(B)/romdata.o: romdata.s $(B)/rom.bin $(B)/demo.bin
	cd $(B) && $(CC) $(ASFLAGS) -x assembler -c $(HERE)/romdata.s -o $@
$(B)/rt6502.o: $(A)/rt/rt6502.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/c2p.o: c2p.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/isr.o: isr.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/rtasm.o: rtasm.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/crt0.o: crt0.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/nes_rt.o: $(A)/rt/nes_rt.c $(A)/rt/rt.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/nes_main.o: nes_main.c $(B)/nespal.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/plat.o: platform.c platform.h | $(B)
	$(CC) $(CFLAGS) -Wextra -DPLAT_FMODE=1 -c $< -o $@
$(B)/audio.o: audio.c platform.h ../rt/rt.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/fastdisp.o: fastdisp.c platform.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/rt.o: rt.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/boot.bin: boot.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $(B)/boot.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $@ $(B)/boot.o
	@test `stat -c %s $@` -le 1024 || (echo "bootblock > 1024 bytes"; false)
OBJS := $(B)/crt0.o $(B)/prog.o $(B)/rt6502.o $(B)/nes_rt.o $(B)/nes_main.o $(B)/romdata.o $(B)/c2p.o $(B)/fastdisp.o $(B)/audio.o $(B)/plat.o $(B)/isr.o $(B)/rt.o $(B)/rtasm.o
$(B)/exb.elf: $(OBJS) link.ld
	$(LD) -T link.ld -Map=$(B)/exb.map -o $@ $(OBJS)
$(B)/exb.bin: $(B)/exb.elf
	$(OBJCOPY) -O binary $< $@
$(B)/exb_a1200.adf: $(B)/exb.bin $(B)/boot.bin
	python3 mkdisk.py $@ $(B)/boot.bin $<
	@$(PREFIX)size $(B)/exb.elf
	@sha256sum $@
clean:
	rm -rf $(B)
.PHONY: all clean
-include $(wildcard $(B)/*.d)
