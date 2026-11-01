#!/bin/sh
# usage: cmp_hash.sh pad frames   -> md5 of the full trace (all cycle-stamped I/O + per-frame dumps) from the reference and the translated build
S=/tmp/claude-0/-home-user/64edc795-f3ed-5b3b-8638-3e16a442eba1/scratchpad
export INPUT_PAD=$1
R=$($S/nesref "$S/rom/Excitebike (Europe).nes" $2 0 /dev/stdout | grep -v "^[RA] " | md5sum | cut -c1-32)
T=$(qemu-m68k /tmp/exbnes/exb "$S/rom/Excitebike (Europe).nes" $2 0 /dev/stdout | grep -v "^[RA] " | md5sum | cut -c1-32)
if [ "$R" = "$T" ]; then echo "$(basename $1) $2 frames: IDENTICAL $R"; else echo "$(basename $1) $2 frames: DIFFERENT ref=$R xl=$T"; fi
