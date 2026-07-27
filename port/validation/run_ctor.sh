#!/bin/bash
# Fresh render-harness run focused on the ctor 0x6f370 non-return: full [mark]/[pc]/SCHED_DBG.
set -e
SRC=/shim/src; UNI=/shim/vendor/unicorn
DEV_SRCS="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c \
$SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c \
$SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c $SRC/jni_passthrough.c \
$SRC/jni_argbuild.c $SRC/utf.c"
echo "== compiling =="
cc -Wall -O2 -rdynamic -iquote "$SRC" -I"$UNI/include" -D_GNU_SOURCE \
   /shim/test/test_boot_render.c $DEV_SRCS "$UNI/lib/libunicorn.a" \
   -lEGL -lGLESv2 -lpthread -lm -ldl -o /tmp/tbr 2>&1 | grep -iE 'error|undefined' | head -20 || true
export ABSHIM_ASSET_DIR=/work803/assets
export ABSHIM_ENGINE_SO=/work803/libv7/libAngryBirdsClassic.so
export ABSHIM_RENDER_OUT=/tmp/frame.ppm
mkdir -p /tmp/abfiles
cp /work803/seedfiles/*.lua /tmp/abfiles/ 2>/dev/null || true   # NO foreign fusion.registry / ad-ID (device-keyed)
echo "seeded files:"; ls -la /tmp/abfiles/
export ABSHIM_FILES_DIR=/tmp/abfiles
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless
export ABSHIM_LOG=1
export SCHED_DBG=1
export ABSHIM_FRAMES=2
export ABSHIM_ASSET_TRACE=1
echo "== running =="
timeout 300 /tmp/tbr > /out/ctor.log 2>&1; echo "exit=$?"
echo "=== [mark] (which scene-ctor addrs reached) ==="
grep -E 'reached engine|^\[mark\]' /out/ctor.log
echo "=== [pc] (F path) ==="
grep -E '^\[pc\]' /out/ctor.log | head -40
echo "=== [ring2]/[hfwd] (deepest blocks) ==="
grep -A40 -E '^\[ring2\]|^\[hfwd\]' /out/ctor.log | head -80
echo "=== SCHED_DBG tail (last 30 loop lines) ==="
grep -E '^\[loop|-> e=|pend_blk|pcnow=' /out/ctor.log | tail -30
echo "=== harness progression ==="
grep -E 'nativeInit|nativeResume|nativeResize|frame ' /out/ctor.log | head
