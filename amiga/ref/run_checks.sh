#!/bin/sh
# Step-1 verification of ref/nesref.c.  Needs: ROM path ($ROM), MAME (nespal), SingleStepTests 65x02 json files in $SST.
# 1) CPU: every official opcode against SingleStepTests (hardware-derived, cycle exact bus log)
# 2) System: 1500 frames (30 s, scripted play from frame 500) compared with MAME: nametables, palette, OAM RAM, RAM (except RNG $18-$1F and $00, stack page) and the PPU/APU register write sequence
set -e
ROM=${ROM:-exb.nes}; SST=${SST:-sst}; HERE=$(cd $(dirname $0) && pwd); T=${T:-/tmp/exbchk}; mkdir -p $T
gcc -O2 -w -o $T/nesref $HERE/nesref.c
rm -f $T/sst.res
for f in $SST/*.json; do python3 -I $HERE/../tools/sst2txt.py $f $T/x.txt; $T/nesref --sst $T/x.txt >> $T/sst.res; done
echo "SST: $(grep -c ' 0 failures' $T/sst.res) of $(wc -l < $T/sst.res) opcode files pass"
PATH=$PATH:/usr/games START_FRAME=500 TRACE_OUT=$T/mame.txt PIX_OUT=$T/mame.pix mame nespal -cart $ROM -video none -sound none -nothrottle -seconds_to_run 30 -autoboot_script $HERE/mame_trace.lua >/dev/null 2>&1
PIX_OUT=$T/my.pix $T/nesref $ROM 1500 500 $T/my.txt
echo "traces in $T (compare with scratch scripts cmp2.py / pixel mapping in VERIFICATION.md)"
