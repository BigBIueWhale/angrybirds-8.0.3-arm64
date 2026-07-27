#!/bin/bash
# Reproducible build + arm64-emulated smoke test of the shim's C loader.
# Run inside the `ab-port` Docker image:
#   docker run --rm -v "$PWD":/work ab-port bash /work/port/shim/build_test.sh
set -e
NDK=/opt/android-ndk-r26d
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang
UNI="/opt/unicorn/b/libunicorn-static.a /opt/unicorn/b/libunicorn-common.a /opt/unicorn/b/libarm-softmmu.a"
SHIM=/work/port/shim/abshim.c
ENGINE=/work/work803/libv7/libAngryBirdsClassic.so

command -v qemu-aarch64-static >/dev/null 2>&1 || {
  echo "== installing qemu-user-static (outbound only, no ports published) =="
  apt-get update -qq >/dev/null 2>&1
  apt-get install -y -qq qemu-user-static >/dev/null 2>&1
}

echo "== compile test binary (static, -DSHIM_TEST) =="
$CC -DSHIM_TEST -static -O2 -Wno-unused -I/opt/unicorn/include "$SHIM" $UNI -ldl -o /work/port/shim/abtest
echo "   built: $(file /work/port/shim/abtest | cut -d, -f1-2)"

echo "== run the C loader under qemu-aarch64 =="
qemu-aarch64-static /work/port/shim/abtest "$ENGINE"
