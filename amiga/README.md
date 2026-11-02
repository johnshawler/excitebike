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
correctly.  Sound: APU model (pulse/triangle/noise, frame counter) drives Paula directly.  Speed is below 100 % (see "Speed").

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
| + log-free skipped frames, cheaper PPU stepping, up to 7 of 8 frames skipped; **racing with opponents, timer running** | 77.2 % (624 game frames / 808 ticks; 546 of them not displayed, i.e. about 6-7 displayed frames per second at worst) |
| + sprite-0 look-ahead in whole-line steps, closed-form line stepping, long-word OAM DMA, cheaper overflow check (core 52k -> 33k instructions per frame) | **100.0 %** (808 game frames / 808 ticks in a race; about 73 % of the frames are not displayed, i.e. roughly 13 displayed frames per second) |
| + dual-playfield display (PF1 scrolled background, PF2 sprite layer, 8 planes), cheaper cell and sprite drawing, batched RNG idle loop | logic 100 %; displayed frames in the repeatable race benchmark (recorded input movie, `make EXTRA=-DDEMO DEMOPAD=...`): about 12-16 % in the busiest part of a race with three opponents, more in lighter scenes |

**Honest status of the 50 fps goal:** game logic and audio run at 50 Hz (100 %), but the display does *not* reach 50 fps in busy race scenes.
Benchmarks in FS-UAE (stock A1200, 14 MHz 68020, code in fast RAM): logic alone needs about 82 % of a frame, a fully rendered frame about 190 % of a frame
(sprite layer drawing and chip-RAM writes are the largest part), so only a fraction of the frames can be shown. Tried and not kept: drawing the sprite
layer with the blitter (cookie-cut blits; same total time, the blitter is not faster than the CPU here), FMODE 1 (32-bit fetch: about 17 % faster rendering
but BPLCON1 delays of 16-31 pixels behave differently in FS-UAE than modelled, so the well-defined FMODE 0 is used). Nothing has been run on real hardware.
Benchmark switches (platform `EXTRA=`): `-DHUD` counters, `-DFR_PROF` timing, `-DDEMO` movie input, `-DSKIPMODE=1|2` render always / almost never.
Instructions per frame (qemu count, production build, frames rendered): ~103,000 before the last round; with rendering skipped the core needs ~33,000: translated 6502 code 13 k, PPU line model 5 k, I/O 5 k, rest 10 k.

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

## Sound (rt/apu.c, platform/audio.c)
* The APU model replays every cycle-stamped register write with the PAL frame counter (envelopes, linear counter, length counters, sweeps) and gives the channel state at each frame boundary; the register write sequence it consumes is identical to MAME's over 3000 frames (see above).
* Paula plays looping hardware waveforms (pulse duty tables, 32/16/8 step triangle, 15-bit and 93-step LFSR noise loop, decimated for fast noise periods): no CPU time per sample, no resampling.  Check against MAME's audio: dominant frequencies agree at the sampled times (e.g. 121 / 182 / 488 Hz at the same game states; see test/fsuae/snd.sh, AUDIO_OUT recording).
* Limits: panned (pulse1 L, pulse2 R, triangle R, noise L) instead of mono; volume and period are applied once per frame (envelope steps inside a frame are not heard); the DMC level ($4011) is not played; pulse/triangle periods that would need Paula periods below 124 are clamped or use shorter waveforms; the nonlinear NES mixer is approximated by per-channel gains.

## Not done yet
Further speed work (target 100 %), regression battery in FS-UAE (boot, pause, soak), real-hardware tests, final docs.
