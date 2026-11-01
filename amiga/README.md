# Excitebike (Europe, NES/PAL) -> Amiga 1200 (68020, AGA)  -  work in progress

Method: mechanical static translation of the 6502 code to 68020 + runtime models of the board (PPU, pads, APU registers), verified against a reference emulator, MAME and hardware-derived CPU test vectors.  The ROM is never committed (`*.nes` is ignored); supply `Excitebike (Europe).nes`.

* `FACTS.md`        facts page (identification, source hardware, approach, assumptions, open questions)
* `ref/`            reference emulator `nesref.c` (6502 bus-cycle accurate, dot-accurate PPU), MAME trace script, check script
* `tools/`          opcode table, CFG discovery, translator `xl6502.py`, palette (MAME's PAL palette), pad/pixel tools
* `rt/`             target-independent runtime (`nes_rt.c`: lazy PPU/NMI scheduler/I/O) and asm glue (`rt6502.s`)
* `test/`           per-instruction tests (SingleStepTests) and whole-game harness under qemu-m68k (`build_nes.sh`, `main_nes.c`)
* `scripts/`        input scripts / pad files (movies converted from the FM2 files of the disassembly repo; used only as input for coverage)

## Status (work in progress; nothing tested on real hardware)
The game runs on an emulated A1200 (FS-UAE, Kickstart 3.1, 2 MB chip + 2 MB fast RAM): title, menus, track select and the race itself are displayed
correctly.  **Sound is not implemented yet.**  Speed is below 100 % (see "Speed").

Build:  `cd platform && make ROM=/path/to/Excitebike\ \(Europe\).nes`  ->  `platform/build/exb_a1200.adf` (the ADF contains the game's code translated from the
user's ROM and the ROM data itself; it is a private build artefact, do not distribute).  `make ... EXTRA=-DHUD` adds the measurement overlay
(game frames / real 50 Hz ticks / skipped frames as hex digits in the first 8 lines).  Requires a stock A1200 with 2 MB of fast RAM at $200000 (the loader shows a yellow screen
otherwise).  Controls: joystick port 2 (fire = A / accelerate, 2nd button = B / turbo), Enter = Start, Space = Select.

## Speed (measured in FS-UAE, method: HUD counters in two screenshots; `test/fsuae/play.sh`)
| state | game frames per real 50 Hz tick |
|---|---|
| first working build (per-pixel renderer) | 1.8 % |
| fused loops + exact line model + fast renderer | 40.7 % (race start, 290 game frames / 712 ticks) |
| + display frame skipping when behind real time (at most 3 of 4) | 59.3 % (420 / 708) |
Instructions per frame (qemu count, production build, race frames): ~103,000: renderer set-up/sprites 40 k, PPU line model 19 k, translated 6502 code 13 k, background cell redraw 7 k, I/O 9 k, rest 15 k.

## Verified so far
| check | result |
|---|---|
| reference CPU vs SingleStepTests 65x02, all 151 official opcodes, cycle-exact bus log | 1,510,000 vectors, 0 failures |
| translator vs SingleStepTests (flat memory model, registers, N/Z/C, memory, cycles incl. page-cross) | 199,500 vectors (133 data-flow opcodes), 0 failures |
| reference vs MAME `nespal` over 1500 frames (menus) | nametables, palette, OAM, PPU/APU/pad write sequence identical; RAM identical except RNG bytes |
| reference vs MAME on gameplay (3000 frames) | all 153,242 register writes identical; pixels identical (RGB) on every sampled frame until the RNG-driven opponent state diverges (frame ~1160-1200); frame 502 (forced blank) differs: MAME draws a picture, the register trace and this reference do not |
| translated game (qemu-m68k) vs reference, every cycle-stamped PPU/APU/pad write, nametables, palette, OAM, all 2 KB RAM incl. stack, pixel frames | identical: 9000 frames menu script, 3000 frames race, 4 x 19000 frames movie input (read polling of $2002 not compared since the busy-wait loops are fused) |
| fused busy-wait loops (status polls, delay loops, RNG idle loop) | exact closed forms, same comparison |
| fast renderer (planar nametable bitmap + hardware-scroll bands + software sprites) vs the exact slow renderer, every displayed frame | 0 mismatching frames over 4 x 19000 frames (99.5 % of frames on the fast path; the rest uses the exact slow renderer), also with 3 of 4 frames skipped |
| dynamic coverage of the static CFG (5 movies) | 4557 of 5570 instructions executed; nothing executed outside the static set; all indirect jump targets are known |
Test scripts: `ref/run_checks.sh`, `test/run.sh` (per-instruction), `test/build_nes.sh` (whole game), `tools/cmp_mame.py`; inputs `scripts/*.txt|pad`.

## Known differences / limits
* RNG (`$18-$1F`) depends on the CPU/PPU power-on phase and the exact NMI instant; MAME uses another phase, so opponent behaviour differs from MAME (not from hardware: the phase of real hardware is arbitrary).
* V flag not modelled (no BVC/BVS/PHP/PLP in this ROM); I/D constant.  Mid-line `$2006/$2007` writes during rendering are not modelled (never happen in this ROM's traces).
* The fast renderer needs: bg+sprites enabled, 8x8 sprites, a constant bg pattern table, mask $1A/$1E, all lines covered by at most 16 scroll bands, at most 4 lines with a mid-line fine-X change; other frames use the exact (slow) renderer.  Colour emphasis / greyscale are only handled by the slow path (the ROM never uses them).
* The slow renderer shows 64 NES colours through a 6-plane c2p screen; frames that need it (transitions, boot) run at a few frames per second.
* 1013 static instructions were not executed by any test movie (only the per-instruction tests cover them).
* Real-time pacing uses the CIA-B timer at the NES PAL frame rate (50.0070 Hz); display frames are skipped while behind.

## Not done yet
APU/sound (Paula), further speed work (target 100 %), regression battery in FS-UAE (boot, pause, soak), real-hardware tests, final docs.
