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
  # The point of the whole exercise: all 125 constructors must run clean ON AArch64.
  if echo "$OUT" | grep -qE "125/125 constructors ran CLEAN"; then
    ok "125/125 C++ constructors ran CLEAN on AArch64"
  else
    bad "did not see '125/125 constructors ran CLEAN' - the arm64 ABI path is NOT validated"
  fi
else
  bad "test_ctors did not link"; head -12 /tmp/ctors.err | sed 's/^/    /'
fi

echo
[ "$FAIL" = 0 ] && echo "ARM64 CROSS TEST PASSED" || echo "ARM64 CROSS TEST FAILED"
echo "ARM64_EMULATION_TEST_DONE"
exit "$FAIL"
