# Excitebike (Europe, NES/PAL) -> Amiga 1200 (68020, AGA)  -  work in progress

Method: mechanical static translation of the 6502 code to 68020 + runtime models of the board (PPU, pads, APU registers), verified against a reference emulator, MAME and hardware-derived CPU test vectors.  The ROM is never committed (`*.nes` is ignored); supply `Excitebike (Europe).nes`.

* `FACTS.md`        facts page (identification, source hardware, approach, assumptions, open questions)
* `ref/`            reference emulator `nesref.c` (6502 bus-cycle accurate, dot-accurate PPU), MAME trace script, check script
* `tools/`          opcode table, CFG discovery, translator `xl6502.py`, palette (MAME's PAL palette), pad/pixel tools
* `rt/`             target-independent runtime (`nes_rt.c`: lazy PPU/NMI scheduler/I/O) and asm glue (`rt6502.s`)
* `test/`           per-instruction tests (SingleStepTests) and whole-game harness under qemu-m68k (`build_nes.sh`, `main_nes.c`)
* `scripts/`        input scripts / pad files (movies converted from the FM2 files of the disassembly repo; used only as input for coverage)

## Verified so far (emulation only; nothing on real hardware)
| check | result |
|---|---|
| reference CPU vs SingleStepTests 65x02, all 151 official opcodes, cycle-exact bus log | 1,510,000 vectors, 0 failures |
| translator vs SingleStepTests (flat memory model, registers, N/Z/C, memory, cycles incl. page-cross) | 199,500 vectors (133 data-flow opcodes), 0 failures |
| reference vs MAME `nespal` over 1500 frames (menus) | nametables, palette, OAM, PPU/APU/pad write sequence identical; RAM identical except RNG bytes |
| reference vs MAME on gameplay (3000 frames) | all 153,242 register writes identical; pixels identical (RGB) on every sampled frame until the RNG-driven opponent state diverges (frame ~1160-1200); frame 502 (forced blank) differs: MAME draws a picture, the register trace and this reference do not |
| translated game (qemu-m68k) vs reference | byte-identical full trace (every cycle-stamped PPU/APU/pad access, nametables, palette, OAM, all 2 KB RAM incl. stack) and pixel frames: 9000 frames menu script, 3000 frames race, 4 x 19000 frames movie input |
| dynamic coverage of the static CFG (5 movies) | 4557 of 5570 instructions executed; nothing executed outside the static set; all indirect jump targets are known |

## Known differences / limits
* RNG (`$18-$1F`) depends on the CPU/PPU power-on phase and the exact NMI instant; MAME uses another phase, so opponent behaviour differs from MAME (not from hardware: the phase of real hardware is arbitrary).
* V flag not modelled (no BVC/BVS/PHP/PLP in this ROM); I/D constant.  Mid-line `$2006/$2007` writes during rendering are not modelled (never happen in this ROM's traces).
* 1013 static instructions were not executed by any test movie (only the per-instruction tests cover them).

## Not done yet
APU/sound, Amiga platform layer (boot, AGA display, input, Paula), performance measurement/optimisation, FS-UAE tests (needs a Kickstart 3.1 ROM from the user), ADF, final docs.
