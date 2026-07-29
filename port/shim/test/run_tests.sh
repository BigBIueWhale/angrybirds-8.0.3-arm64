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

# Neither the raw input APK nor the extracted engine is tracked in git (only the xz-compressed
# APK is), so on a FRESH CLONE this file did not exist and the suite died late, inside
# coverage_check.py, with a raw Python traceback about readelf exiting 1 — giving no hint that the
# real problem was a missing input. Prepare it here, using only what this image has: xz is present,
# unzip is NOT, so the APK member is extracted with python3's zipfile.
if [ ! -f "$ABSHIM_ENGINE_SO" ]; then
  REPO="$(cd "$HERE/../../.." && pwd)"
  APK="$REPO/apks/com.rovio.angrybirds@8.0.3.apk"
  echo "== preparing test input: engine not extracted yet =="
  if [ ! -f "$APK" ] && [ -f "$APK.xz" ]; then
    command -v xz >/dev/null 2>&1 || { echo "FATAL: need xz to decompress $APK.xz"; exit 1; }
    xz -dk "$APK.xz"
  fi
  [ -f "$APK" ] || { echo "FATAL: no input APK at $APK (nor .xz) — cannot run the suite"; exit 1; }
  mkdir -p "$(dirname "$ABSHIM_ENGINE_SO")"
  python3 - "$APK" "$ABSHIM_ENGINE_SO" <<'PYEOF'
import sys, zipfile, shutil
apk, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk) as z, open(dest, "wb") as out:
    with z.open("lib/armeabi-v7a/libAngryBirdsClassic.so") as src:
        shutil.copyfileobj(src, out)
PYEOF
  [ -s "$ABSHIM_ENGINE_SO" ] || { echo "FATAL: engine extraction produced nothing"; exit 1; }
  echo "   extracted $(stat -c%s "$ABSHIM_ENGINE_SO") bytes"
fi

# test name -> source files (space-separated, relative to test/ or src/)
run() {  # $1=name  $2...=extra srcs
  local name="$1"; shift
  echo "== $name =="
  $CC $CFLAGS "$HERE/test_$name.c" "$@" -o "$OUT/test_$name"
  "$OUT/test_$name"
  echo
}

run galloc       "$SRC/galloc.c"
# galloc_check() vs a QUARANTINED heap. Production enables quarantine (cpu.c:69) but the test
# above runs with it OFF, so this combination was never covered — including by the 450k-op
# torture. Added after the live shim reported galloc_check == -5 permanently and the allocator
# had to be cleared of suspicion before blaming the guest.
run galloc_quarantine "$SRC/galloc.c"
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

  # WHY THIS ASSERTION EXISTS — an invariant the pthread bridges silently depend on.
  #
  # dispatch.c gates 13 pthread handlers on HAVE_SCH(d) = (d->sch && sched_current(d->sch)).
  # sched_init() memsets the scheduler, so S->cur is NULL until a green thread actually runs, and
  # dispatch_run_init_array() calls cpu_call() DIRECTLY rather than through the scheduler. So for the
  # whole ctor phase sched_current() is NULL and those handlers take their fallback:
  #
  #     h_mlock/h_munlock/h_cwait  -> return 0   ("locked", without locking)
  #     h_pjoin                    -> writes 0 to the retval and returns 0 WITHOUT WAITING
  #
  # Single-threaded that is all semantically correct, and measured today it IS single-threaded: zero
  # pthread_create calls across all 125 constructors. But h_pcreate gates on `!d->sch` only — NOT on
  # HAVE_SCH — so it really does create a thread whenever the scheduler object exists, including
  # during ctors. If a future engine build or shim change ever spawns a thread from a constructor,
  # the guest would get a real thread plus a join that reports it already finished, silently.
  #
  # So the thing that makes the fallback safe is pinned here rather than left to luck. ABSHIM_LOG=1
  # makes h_pcreate log every creation; the ctor phase must produce none.
  ABSHIM_LOG=1 "$OUT/test_ctors" > "$OUT/ctors_pthread.log" 2>&1
  # `|| true` on BOTH counts, because this file runs under `set -e` and `grep -c` exits 1 when the
  # count is zero — i.e. the HEALTHY case would have aborted the whole suite. Written without it
  # first, which is a check that breaks precisely when it should pass.
  NPC=$(grep -c '\[pthread_create\]' "$OUT/ctors_pthread.log" || true); NPC=${NPC:-0}
  NCT=$(grep -aoE '125/125' "$OUT/ctors_pthread.log" | tail -1 || true)
  if [ "$NPC" -eq 0 ] && [ "$NCT" = "125/125" ]; then
      echo "  [ OK ] no thread is created during the ctor phase ($NCT ctors), so the pthread"
      echo "         fallbacks that cannot lock or join are provably single-threaded"
  else
      echo "  [FAIL] ctor phase created $NPC thread(s) (ctors='$NCT'). While sched_current() is NULL"
      echo "         a guest pthread_join returns success WITHOUT waiting and mutex locks do not"
      echo "         lock — see the comment above this check in run_tests.sh"
      # exit, NOT a FAILED counter: this script signals failure through `set -e` and a single
      # "ALL MODULE TESTS PASSED" at the end, so incrementing a variable nothing reads would have
      # printed [FAIL] and still let the suite report success. It was written that way first.
      exit 1
  fi
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

# GL scratch-size contract. bridge_gl.c needs its whole dependency set to link, but the test only
# exercises gl_gsize3 - the guard that stops a guest-supplied width/height/count from overflowing
# into a scratch buffer smaller than what the driver is then told to read. Nothing tested the GL
# bridge before this; a near-miss regression there was caught by reading code, not by a test.
echo "== gl_sizes =="
if [ -f "$UNI/lib/libunicorn.a" ]; then
  $CC -w -O2 -fsanitize=address,undefined -fno-sanitize-recover=all -I"$SRC" \
      -D_GNU_SOURCE -DRTLD_DEFAULT=0 -I"$UNI/include" "$HERE/test_gl_sizes.c" \
      "$SRC/bridge_gl.c" "$SRC/cpu.c" "$SRC/galloc.c" "$SRC/marshal.c" "$SRC/format.c" \
      "$UNI/lib/libunicorn.a" -lpthread -lm -ldl -o "$OUT/test_gl_sizes" && "$OUT/test_gl_sizes"
else
  echo "  (skipped: needs ABSHIM_UNICORN for bridge_gl's dependencies)"
fi
echo

echo "== coverage (every engine UND FUNC bridged? — see RUNTIME_BRIDGES.md) =="
python3 "$HERE/coverage_check.py"    # HARD GATE: every engine UND FUNC must resolve to a bridge
echo

echo "ALL MODULE TESTS PASSED"
