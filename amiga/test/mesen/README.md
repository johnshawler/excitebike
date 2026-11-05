# Cross-check against Mesen 2 (headless)

Mesen 2.1.1 (Linux x64 release) runs headless with `--testRunner script.lua rom.nes` under Xvfb.  Environment that worked here:
`DISPLAY=:N` (Xvfb), `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` (otherwise the native core aborts with std::bad_cast in std::regex),
a `~/.config/Mesen2/settings.json` created once by the first-run wizard, and `Debug.ScriptWindow.AllowIoOsAccess = true` in it.  Lua stops with `emu.stop(code)`.

* `dump.lua`: replays a pad movie (`PADFILE`, one byte per frame, bits A B Select Start Up Down Left Right) and writes, at every NMI entry
  (`NMIPC` = NMI vector), FNV-1a hashes of CPU RAM (stack page skipped), nametable RAM and palette (sprite-palette mirrors folded).
* `ref/nesref` with `RAM_INIT_ZERO=1 DUMP_AT_NMI=1 PPU_PHASE=<n>` writes the same dumps from the reference model.
  The CPU/PPU power-on alignment is arbitrary on hardware; `PPU_PHASE` 27 (and 28, 187, ...) reproduces Mesen's default.
* `cmp.py ref.txt mesen.txt` compares per frame.

Result (Excitebike Europe, PAL, three input movies, 3000-6000 frames each): CPU RAM and nametables equal at every NMI entry (2997 of 2997 / 5997 of 5997 twice);
palette equal except the first 94 frames (power-on palette contents before the game writes it).
