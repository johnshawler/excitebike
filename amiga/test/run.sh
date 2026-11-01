#!/bin/sh
# usage: run.sh sst_dir [tests_per_opcode]   -> builds and runs the per-instruction tests under qemu-m68k
set -e
H=$(cd $(dirname $0) && pwd); O=${OUT:-/tmp/exbtests}; mkdir -p $O
python3 -I $H/gen_tests.py $1 $O ${2:-60}
m68k-linux-gnu-gcc -m68020 -static -O1 -o $O/tests $H/harness.c $H/run1.s $O/tests.s -Wno-all 2>&1 | grep -v "GNU-stack\|^$" || true
qemu-m68k $O/tests $O/tests.txt
