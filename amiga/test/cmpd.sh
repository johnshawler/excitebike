#!/bin/sh
# production-like build (no register trace): compare per-frame dumps (nametables, palette, OAM, RAM) and the pixel dump with the reference
S=/tmp/claude-0/-home-user/64edc795-f3ed-5b3b-8638-3e16a442eba1/scratchpad
N=$1
R=$($S/nesref "$S/rom/Excitebike (Europe).nes" $N 0 /dev/stdout | grep -v "^[WRA] " | md5sum | cut -c1-32)
T=$(qemu-m68k /tmp/exbnes_nt/exb "$S/rom/Excitebike (Europe).nes" $N 0 /dev/stdout 2>/dev/null | grep -v "^[WRA] " | md5sum | cut -c1-32)
if [ "$R" = "$T" ]; then echo "$N frames (dumps only, production build): IDENTICAL ($R)"; else echo "$N frames: DIFFERENT ref=$R xl=$T"; fi
