#!/bin/bash
# arm64_cross_test.sh — the arm64 ABI validation, CROSS-compiled on x86 and executed under qemu-user.
#
# WHY THIS EXISTS
# ---------------
# `arm64_unicorn_test.sh` does the same validation but builds everything *inside* an emulated arm64
# container — the compiler itself runs under qemu. That takes hours and needs network, so in
# practice it was almost never run: its `/scratch/uni-arm64` cache had disappeared and the only
# arm64 execution in the project had fallen behind a whole session of shim changes (the galloc
# size-class fix, the guest->host boundary hardening, the loader path bounding).
#
# Nothing about the target requires an emulated compiler. The target is arm64 **Linux**, so a cross
# toolchain on x86 produces the same binaries in minutes and only the *executables* need qemu, which
# is what qemu-user is for. Unicorn is pre-cross-built into the `ab-arm64x` image, pinned to the
# same commit the shipped shim uses, so this script runs fully offline.
#
# WHAT IT PROVES (and what it does not)
# -------------------------------------
# It loads the real 32-bit engine and executes its 125 C++ static constructors **on AArch64** — the
# ABI the A56 uses. There is no Android, no ART, no JNI, no GL and no frames here: it validates the
# ABI and the ARM32 emulation on it, nothing above that. The shipped APK still has never been
# executed anywhere (see port/OPEN_FINDINGS.md — the Android emulator refuses arm64 images on an
# x86_64 host).
#
#   docker build -f port/docker/Dockerfile.ab-arm64x -t ab-arm64x port/docker
#   docker run --rm --network none -v "$PWD":/work -w /work ab-arm64x \
#       bash /work/port/validation/arm64_cross_test.sh
set +e
CC=aarch64-linux-gnu-gcc
U="${UNICORN_BUILD:-/opt/unicorn-b}"
USRC="${UNICORN_SRC:-/opt/unicorn-src}"
REPO=/work
SRC="$REPO/port/shim/src"
T="$REPO/port/shim/test"
FAIL=0
ok(){  printf "  [ OK ] %s\n" "$1"; }
bad(){ printf "  [FAIL] %s\n" "$1"; FAIL=1; }

echo "host arch: $(uname -m)  (building AArch64 binaries with a cross toolchain, running them under qemu-user)"
command -v $CC >/dev/null 2>&1 || { echo "FATAL: $CC missing - build/run inside ab-arm64x"; exit 1; }
# Run the cross-built binaries through qemu EXPLICITLY with the cross sysroot. They are dynamically
# linked against /lib/ld-linux-aarch64.so.1, which does not exist at that path in this container, so
# relying on binfmt gives "Could not open '/lib/ld-linux-aarch64.so.1'". -L points qemu at the
# toolchain's sysroot instead.
QEMU="$(command -v qemu-aarch64-static || command -v qemu-aarch64) -L /usr/aarch64-linux-gnu"
[ -n "$(command -v qemu-aarch64-static || command -v qemu-aarch64)" ] || { echo "FATAL: no qemu-aarch64"; exit 1; }
echo "qemu: $QEMU"

# The engine is not tracked in git; prepare it exactly like every other entry point.
. "$REPO/port/prepare_inputs.sh"
REPO="$REPO" prepare_inputs || exit 1
export ABSHIM_ENGINE_SO="$REPO/work803/libv7/libAngryBirdsClassic.so"

UL=$(ls "$U"/libunicorn*.a 2>/dev/null | tr '\n' ' ')
[ -n "$UL" ] || { echo "FATAL: no cross-built unicorn in $U"; exit 1; }
echo "unicorn: $UL"
echo "pinned commit: $(cat /IMAGE_VERSIONS 2>/dev/null | head -1)"

echo
echo "== build test_boot (load the real engine into Unicorn) for AArch64 =="
$CC -Wall -O2 -D_GNU_SOURCE -iquote "$SRC" -I"$USRC/include" "$T/test_boot.c" \
    "$SRC/cpu.c" "$SRC/loader.c" "$SRC/galloc.c" "$SRC/elf32.c" "$SRC/ctype_tables.c" \
    -Wl,--start-group $UL -Wl,--end-group -lpthread -lm -o /tmp/t_boot 2>/tmp/boot.err
if [ -x /tmp/t_boot ]; then
  file /tmp/t_boot | grep -qi "ARM aarch64" && ok "test_boot is an AArch64 binary" || bad "test_boot is not AArch64: $(file /tmp/t_boot)"
  echo "  -- running under qemu-user --"
  timeout 600 $QEMU /tmp/t_boot 2>&1 | tail -6 | sed 's/^/    /'
  [ "${PIPESTATUS[0]}" = 0 ] && ok "test_boot ran on AArch64" || bad "test_boot failed on AArch64"
else
  bad "test_boot did not link"; head -12 /tmp/boot.err | sed 's/^/    /'
fi

echo
echo "== build test_ctors (execute the 125 C++ static ctors under emulation) for AArch64 =="
DEV="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c $SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c $SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c"
$CC -Wall -O2 -iquote "$SRC" -I"$USRC/include" -D_GNU_SOURCE -DRTLD_DEFAULT=0 "$T/test_ctors.c" $DEV \
    -Wl,--start-group $UL -Wl,--end-group -lpthread -lm -ldl -o /tmp/t_ctors 2>/tmp/ctors.err
if [ -x /tmp/t_ctors ]; then
  file /tmp/t_ctors | grep -qi "ARM aarch64" && ok "test_ctors is an AArch64 binary" || bad "test_ctors is not AArch64"
  echo "  -- running under qemu-user (heavy: ARM32 emulated on emulated AArch64) --"
  OUT=$(timeout 1800 $QEMU /tmp/t_ctors 2>&1); RC=$?
  echo "$OUT" | tail -8 | sed 's/^/    /'
  [ "$RC" = 0 ] || bad "test_ctors exited $RC on AArch64"
  if echo "$OUT" | grep -qE "125/125 constructors ran CLEAN"; then
    ok "125/125 C++ constructors ran CLEAN on AArch64"
  else
    bad "did not see '125/125 constructors ran CLEAN' - the arm64 ABI path is NOT validated"
  fi
else
  bad "test_ctors did not link"; head -12 /tmp/ctors.err | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# The REST of the device suite, on AArch64.
#
# Until now the arm64 side ran only boot + ctors, while the x86 suite ran seven device tests. The
# ones it was missing are precisely the architecture-SENSITIVE ones:
#   longjmp  setjmp/longjmp across cpu_run stop/restart - jmp_buf layout is per-ABI
#   sched    the green-thread scheduler, which is built on that same mechanism
#   libc     the soft-float / integer ABI bridges (arg passing differs sharply from x86)
#   file     stdio FILE* round-trip through guest memory
#   ninit    JNI passthrough marshalling up to the JVM boundary
# Running these on AArch64 is the whole point of having a cross toolchain that finishes in minutes.
# ---------------------------------------------------------------------------
run_dev(){   # $1=name  $2=extra srcs  $3=timeout
  local name="$1" extra="$2" tmo="$3"
  echo
  echo "== $name (device test) on AArch64 =="
  $CC -Wall -O2 -iquote "$SRC" -I"$USRC/include" -D_GNU_SOURCE -DRTLD_DEFAULT=0 "$T/test_$name.c" $extra \
      -Wl,--start-group $UL -Wl,--end-group -lpthread -lm -ldl -o "/tmp/t_$name" 2>"/tmp/$name.err"
  if [ ! -x "/tmp/t_$name" ]; then bad "$name did not link"; head -8 "/tmp/$name.err" | sed 's/^/    /'; return; fi
  local o rc
  o=$(timeout "$tmo" $QEMU "/tmp/t_$name" 2>&1); rc=$?
  echo "$o" | tail -5 | sed 's/^/    /'
  [ "$rc" = 0 ] && ok "$name passed on AArch64" || bad "$name exited $rc on AArch64"
}
FILEB="$SRC/bridge_file.c $SRC/cpu.c $SRC/galloc.c $SRC/marshal.c $SRC/format.c"
run_dev longjmp     "$DEV"                        600
run_dev sched       "$DEV"                        900
run_dev libc        "$DEV"                        900
run_dev file        "$FILEB"                      600
run_dev native_init "$DEV $SRC/jni_passthrough.c" 900

# ---------------------------------------------------------------------------
# Mode-agnostic core modules, on AArch64. These are pure logic over the mem-ops interface and are
# normally validated on x86 under ASan+UBSan. Sanitizers are omitted here because they are
# unreliable under qemu-user; what this adds over the x86 run is the AArch64 code generation itself
# — alignment and integer-width assumptions that x86 happens to tolerate.
# ---------------------------------------------------------------------------
echo
echo "== mode-agnostic module tests on AArch64 (no sanitizers: unreliable under qemu-user) =="
run_mod(){   # $1=name  $2...=srcs
  local name="$1"; shift
  $CC -Wall -O2 -iquote "$SRC" -D_GNU_SOURCE "$T/test_$name.c" "$@" -lpthread -lm -o "/tmp/m_$name" 2>/dev/null
  if [ ! -x "/tmp/m_$name" ]; then bad "module $name did not link"; return; fi
  if timeout 600 $QEMU "/tmp/m_$name" >/dev/null 2>&1; then ok "module $name"; else bad "module $name FAILED on AArch64"; fi
}
run_mod galloc            "$SRC/galloc.c"
run_mod galloc_quarantine "$SRC/galloc.c"
run_mod marshal           "$SRC/marshal.c"
run_mod format            "$SRC/format.c" "$SRC/marshal.c"
run_mod handle_table      "$SRC/handle_table.c"
run_mod jni_arg           "$SRC/jni_argbuild.c" "$SRC/marshal.c" "$SRC/handle_table.c"
run_mod elf32             "$SRC/elf32.c"
run_mod utf               "$SRC/utf.c"
run_mod ctype_tables      "$SRC/ctype_tables.c"
run_mod fdtable           "$SRC/fdtable.c"

echo
[ "$FAIL" = 0 ] && echo "ARM64 CROSS TEST PASSED" || echo "ARM64 CROSS TEST FAILED"
echo "ARM64_EMULATION_TEST_DONE"
exit "$FAIL"
