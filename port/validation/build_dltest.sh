#!/bin/bash
# build_dltest.sh — cross-compile the loader probe (and its 4 KB-aligned control) + extract the shim.
#   docker run --rm --network none -v "$PWD":/work ab-port bash /work/port/validation/build_dltest.sh
set -e
CC=$(ls /opt/android-ndk-*/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android26-clang | head -1)
SRC=/work/port/validation/src/dltest.c
# The 16 KB build uses the SAME flag the shipped shim uses. The 4 KB build is deliberately built
# WITHOUT it: it is the negative control that shows the 16 KB kernel enforces.
"$CC" -O1 -Wl,-z,max-page-size=16384 -o /work/out/dltest16k "$SRC"
"$CC" -O1                            -o /work/out/dltest4k  "$SRC"
cd /tmp && rm -rf dlx && mkdir dlx && cd dlx
unzip -o -q /work/out/angrybirds-8.0.3-x86shim-release.apk "lib/x86_64/*"
cp lib/x86_64/libAngryBirdsClassic.so /work/out/shim_x86_64.so
for b in dltest16k dltest4k; do
    echo "  $b LOAD p_align: $(readelf -lW /work/out/$b | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')"
done
echo "  shim  LOAD p_align: $(readelf -lW /work/out/shim_x86_64.so | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')"
