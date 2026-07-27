#!/bin/bash
# build + run the scheduler-faithful render harness inside ab-render (mesa GLES2, surfaceless).
set -e
SRC=/shim/src; UNI=/shim/vendor/unicorn
DEV_SRCS="$SRC/cpu.c $SRC/loader.c $SRC/dispatch.c $SRC/sched.c $SRC/galloc.c $SRC/elf32.c \
$SRC/ctype_tables.c $SRC/marshal.c $SRC/format.c $SRC/bridge_gl.c $SRC/bridge_asset.c \
$SRC/bridge_libc.c $SRC/bridge_file.c $SRC/handle_table.c $SRC/jni_passthrough.c \
$SRC/jni_argbuild.c $SRC/utf.c"
echo "== compiling test_boot_render (scheduler-faithful) =="
cc -Wall -O2 -rdynamic -iquote "$SRC" -I"$UNI/include" -D_GNU_SOURCE \
   /shim/test/test_boot_render.c $DEV_SRCS "$UNI/lib/libunicorn.a" \
   -lEGL -lGLESv2 -lpthread -lm -ldl -o /tmp/tbr 2>&1 | grep -iE 'error|warning: .*implicit|undefined' | head -20 || true
echo "== running =="
export ABSHIM_ASSET_DIR=/work803/assets
export ABSHIM_ENGINE_SO=/work803/libv7/libAngryBirdsClassic.so
export ABSHIM_RENDER_OUT=/tmp/frame.ppm
mkdir -p /tmp/abfiles                 # engine's filesDir (registry/save + VFS base)
export ABSHIM_FILES_DIR=/tmp/abfiles
cp /out/appdata/files/fusion.registry /out/appdata/files/*.lua /tmp/abfiles/ 2>/dev/null # seed staged
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=surfaceless
export ABSHIM_LOG=1
export ABSHIM_DET_URANDOM=1                    # forward the engine's OWN __android_log_print to stderr
export ABSHIM_FILE_TRACE=1 ABSHIM_ASSET_TRACE=1   # what does the engine touch during the frame loop?
timeout 600 /tmp/tbr > /out/render.log 2>&1; echo "exit=$?"
echo "--- activity AFTER nativeResume (the frame-loop region: is it loading or stuck?) ---"
awk '/nativeResume/{f=1} f' /out/render.log | grep -vE '^  frame' | head -40
echo "--- the ENGINE's own log lines (Framework/Lua/errors) + throws, in order ---"
grep -nE '\[engine:|^\[throw\]|Lua|lua|rror|xcept|ecrypt|arse|ssert|ANGRY|Framework|fusion|Fusion|onfig' /out/render.log | grep -vE '\[asset\]' | head -60
echo "--- distinct throws ---"; grep -E '^\[throw\]' /out/render.log | sed -E 's/ from engine.*//' | sort | uniq -c
echo "--- harness progression ---"; grep -E 'nativeConfig|nativeInit|nativeResize|nativeResume|frame |GL state|pbuffer sanity|framebuffer\(|assets OK' /out/render.log
if [ -f /tmp/frame.ppm ]; then cp /tmp/frame.ppm /out/frame.ppm 2>/dev/null && echo "frame copied"; fi
