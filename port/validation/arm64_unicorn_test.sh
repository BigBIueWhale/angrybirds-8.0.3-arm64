#!/bin/bash
# Build Unicorn 2.1.4 (same commit as the APK) for arm64-Linux + run the shim's engine-load
# + ctor-execution tests ON the arm64 ABI (qemu-user). Validates the actual ARM32 emulation
# on an arm64 host — the closest thing to the A56 without the device.
# Unicorn is built into the PERSISTED /scratch/uni-arm64 so re-runs skip the (slow) rebuild.
set -e
export DEBIAN_FRONTEND=noninteractive
echo "host arch: $(uname -m)"
command -v cmake >/dev/null 2>&1 || { apt-get update -qq && apt-get install -y -qq build-essential cmake git pkg-config >/dev/null 2>&1; }
U=/scratch/uni-arm64
if [ ! -f "$U/b/libunicorn-static.a" ] && [ ! -f "$U/b/libunicorn.a" ]; then
  echo "== building Unicorn 2.1.4 for arm64 (UNICORN_ARCH=arm) — slow under nested qemu =="
  rm -rf "$U"; git clone -q https://github.com/unicorn-engine/unicorn "$U"
  git -C "$U" checkout -q 7c5db94191defc1e04a4f66f4eb1220903cba837
  cmake -S "$U" -B "$U/b" -DUNICORN_ARCH=arm -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=OFF \
        -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "$U/b" -j"$(nproc)" >/tmp/ubuild.log 2>&1 || { echo "UNICORN BUILD FAILED"; tail -20 /tmp/ubuild.log; exit 1; }
else
  echo "== reusing persisted arm64 Unicorn build =="
fi
ls "$U"/b/libunicorn*.a "$U"/b/libarm-softmmu.a 2>/dev/null && echo "unicorn arm64 libs present"
# link ALL produced static libs inside one --start-group (handles circular deps / any order)
UL=$(ls "$U"/b/libunicorn-static.a "$U"/b/libunicorn-common.a "$U"/b/libarm-softmmu.a "$U"/b/libunicorn.a 2>/dev/null | tr '\n' ' ')
echo "linking against: $UL"
SRC=/shim/src; T=/shim/test
export ABSHIM_ENGINE_SO=/work803/libv7/libAngryBirdsClassic.so
echo "== build + run test_boot (load real engine into Unicorn) ON ARM64 =="
gcc -Wall -O2 -iquote "$SRC" -I"$U/include" "$T/test_boot.c" \
    "$SRC/cpu.c" "$SRC/loader.c" "$SRC/galloc.c" "$SRC/elf32.c" "$SRC/ctype_tables.c" \
    -Wl,--start-group $UL -Wl,--end-group -lpthread -lm -o /tmp/t_boot 2>&1 | grep -iE 'error|undefined' | head -8 || true
[ -x /tmp/t_boot ] && /tmp/t_boot 2>&1 | tail -6 || echo "test_boot did not link"
echo "== build + run test_ctors (execute 125 C++ ctors under emulation) ON ARM64 — heavy =="
DEV="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c $SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c $SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c"
gcc -Wall -O2 -iquote "$SRC" -I"$U/include" -D_GNU_SOURCE "$T/test_ctors.c" $DEV \
    -Wl,--start-group $UL -Wl,--end-group -lpthread -lm -ldl -o /tmp/t_ctors 2>&1 | grep -iE 'error|undefined' | head -8 || true
[ -x /tmp/t_ctors ] && { timeout 900 /tmp/t_ctors 2>&1 | tail -8; } || echo "test_ctors did not link"
echo "ARM64_EMULATION_TEST_DONE"
