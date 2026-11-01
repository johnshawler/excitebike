#!/bin/sh
# builds the whole-game test binary for qemu-m68k:  $O/exb   usage: build_nes.sh rom.nes
set -e
H=$(cd $(dirname $0) && pwd); R=$H/..; O=${OUT:-/tmp/exbnes}; mkdir -p $O
ROM=$1; CFG=${CFG:-$O/cfg.json}; SEEDS=${SEEDS:-$R/tools/seeds_ind.txt}
python3 -I $R/tools/cfg6502.py "$ROM" --seeds $R/tools/seeds_ind.txt --always $R/tools/always_taken.txt --skip E6B7 > $CFG
python3 -I $R/tools/xl6502.py "$ROM" $CFG $SEEDS $O/prog.s ${TRACE:+x --trace}
CC=m68k-linux-gnu-gcc
$CC -m68020 -c -x assembler -o $O/prog.o $O/prog.s
$CC -m68020 -c -x assembler -o $O/rt6502.o $R/rt/rt6502.s
$CC -m68020 -O2 -static -g ${TRACEFLAG--DRT_TRACE} ${VERIFY-"-DFR_VERIFY"} -o $O/exb $H/main_nes.c $R/rt/nes_rt.c $O/prog.o $O/rt6502.o -Wall -Wno-unused 2>&1 | grep -v "GNU-stack\|deprecated\|^$" || true
ls -l $O/exb
