#!/bin/sh
# compare W lines (cycle-stamped writes), dumps and pixel frames of the reference and the translated build: R lines are dropped (fused polling loops do not reproduce the skipped reads)
S=/tmp/claude-0/-home-user/64edc795-f3ed-5b3b-8638-3e16a442eba1/scratchpad
N=$1; shift
R=$($S/nesref "$S/rom/Excitebike (Europe).nes" $N 0 /dev/stdout | grep -v "^[RA] " | md5sum | cut -c1-32)
T=$(qemu-m68k /tmp/exbnes/exb "$S/rom/Excitebike (Europe).nes" $N 0 /dev/stdout | grep -v "^[RA] " | md5sum | cut -c1-32)
if [ "$R" = "$T" ]; then echo "$N frames: IDENTICAL ($R)"; else echo "$N frames: DIFFERENT ref=$R xl=$T"; fi
