#!/bin/bash
# run_tests.sh — host unit tests for the mode-agnostic core (Audit 09 blueprint).
# These modules are pure logic over the mem-ops interface (src/memops.h) and are
# validated on x86 with NO arm64 runtime. Every test runs under ASan+UBSan with
# -fno-sanitize-recover so any UB/OOB aborts non-zero.
#
#   bash port/shim/test/run_tests.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
CC="${CC:-cc}"
CFLAGS="-Wall -Wextra -O2 -fsanitize=address,undefined -fno-sanitize-recover=all -I$SRC"
OUT="/tmp/abshim-tests"; mkdir -p "$OUT"
# real engine binary for the loader test (classification is checked against it)
export ABSHIM_ENGINE_SO="$(cd "$HERE/../../.." && pwd)/work803/libv7/libAngryBirdsClassic.so"

# test name -> source files (space-separated, relative to test/ or src/)
run() {  # $1=name  $2...=extra srcs
  local name="$1"; shift
  echo "== $name =="
  $CC $CFLAGS "$HERE/test_$name.c" "$@" -o "$OUT/test_$name"
  "$OUT/test_$name"
  echo
}

run galloc       "$SRC/galloc.c"
run marshal      "$SRC/marshal.c"
run format       "$SRC/format.c" "$SRC/marshal.c"
run handle_table "$SRC/handle_table.c"
run jni_arg      "$SRC/jni_argbuild.c" "$SRC/marshal.c" "$SRC/handle_table.c"
run elf32        "$SRC/elf32.c"
run utf          "$SRC/utf.c"
run ctype_tables "$SRC/ctype_tables.c"
run fdtable      "$SRC/fdtable.c"
# Phase A (mode-agnostic core) COMPLETE — 9/9 modules.

# ---- Phase B device-layer tests (need Unicorn; emulate ARM32 on x86) ----
UNI="${ABSHIM_UNICORN:-$HERE/../vendor/unicorn}"
if [ -f "$UNI/lib/libunicorn.a" ]; then
  DEV_SRCS="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c $SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c $SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c"
  echo "== boot (device: load real engine into Unicorn) =="
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_boot.c" \
     "$SRC/cpu.c" "$SRC/loader.c" "$SRC/galloc.c" "$SRC/elf32.c" "$SRC/ctype_tables.c" \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_boot"
  "$OUT/test_boot"
  echo
  echo "== ctors (device: execute init_array under emulation) =="
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_ctors.c" $DEV_SRCS \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_ctors"
  "$OUT/test_ctors"
  echo
  echo "== longjmp (device: setjmp/longjmp + cpu_run stop/restart) =="
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_longjmp.c" $DEV_SRCS \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_longjmp"
  "$OUT/test_longjmp"
  echo
  echo "== sched (device: GEL green-thread scheduler — create/join/cond/preempt) =="
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_sched.c" $DEV_SRCS \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_sched"
  timeout 60 "$OUT/test_sched"
  echo
  echo "== libc (device: bridge_libc soft-float/int ABI — math/net-hardfail/strtol) =="
  # bridge_libc.c depends on the GEL scheduler (sched_errno_addr/sched_sleep/g_sched), so link the
  # full device src set (same as ctors) rather than a minimal list that omits sched.c + its deps.
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_libc.c" $DEV_SRCS \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_libc"
  "$OUT/test_libc"
  echo
  echo "== file (device: bridge_file stdio FILE round-trip)  =="
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_file.c" \
     "$SRC/bridge_file.c" "$SRC/cpu.c" "$SRC/galloc.c" "$SRC/marshal.c" "$SRC/format.c" \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_file"
  "$OUT/test_file"
  echo
  echo "== nativeInit (device probe: drive JNI passthrough to the JVM boundary) =="
  cc -Wall -Wextra -O2 -DRTLD_DEFAULT=0 -I"$SRC" -I"$UNI/include" "$HERE/test_native_init.c" $DEV_SRCS "$SRC/jni_passthrough.c" \
     "$UNI/lib/libunicorn.a" -lpthread -lm -o "$OUT/test_ninit"
  "$OUT/test_ninit"
  echo
else
  echo "== boot: SKIP (set ABSHIM_UNICORN to a dir with include/ + lib/libunicorn.a) =="
fi

echo "== coverage (every engine UND FUNC bridged? — see RUNTIME_BRIDGES.md) =="
python3 "$HERE/coverage_check.py"    # HARD GATE: every engine UND FUNC must resolve to a bridge
echo

echo "ALL MODULE TESTS PASSED"
