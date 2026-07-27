#!/bin/bash
# Build an x86_64 variant of the emulation-shim APK — SAME shim source + SAME Unicorn commit,
# just compiled for x86_64-android instead of arm64. Purpose: install it in the x86 Android
# emulator (which has a REAL ART + app lifecycle) to validate the shim END-TO-END, including the
# engine's C++ boot that the fake-JVM host harness cannot drive. Since Unicorn's ARM32 emulation
# is deterministic across host arch (proven: bit-identical heap on x86 and arm64), an x86 shim
# rendering in the emulator ==> the arm64 shim renders on the A56.
#   docker run --rm --network none -v "$PWD":/work ab-port bash /work/port/build_apk_x86.sh
set -e
NDK=/opt/android-ndk-r26d
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android24-clang
IN=/work/apks/com.rovio.angrybirds@8.0.3.apk
ENGINE=/work/work803/libv7/libAngryBirdsClassic.so
OUTDIR=/work/out; mkdir -p "$OUTDIR"
WORK=/tmp/apkwork_x86
UB=/work/port/build/uni-x86   # x86_64-android Unicorn build — PERSISTED in the repo build/ dir so
                              # re-runs skip the (one-time) Unicorn rebuild (offline: src in /opt/unicorn)

echo "== 0/5 build Unicorn 2.1.4 for x86_64-android (UNICORN_ARCH=arm) — offline, one-time =="
if [ ! -f "$UB/libunicorn-static.a" ]; then
  cmake -S /opt/unicorn -B "$UB" \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-24 \
    -DUNICORN_ARCH=arm -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release >/tmp/ucfg.log 2>&1
  cmake --build "$UB" -j"$(nproc)" >/tmp/ubuild.log 2>&1 || { echo "UNICORN x86_64 BUILD FAILED"; tail -20 /tmp/ubuild.log; exit 1; }
fi
UNI="$UB/libunicorn-static.a $UB/libunicorn-common.a $UB/libarm-softmmu.a"
ls $UNI >/dev/null && echo "   x86_64 unicorn libs OK"

echo "== 1/5 build x86_64 shim (.so) — SAME source as arm64 =="
S=/work/port/shim/src
DEX=/tmp/classes.dex; (cd /tmp && unzip -o -q "$IN" classes.dex 2>/dev/null || true)
[ -f "$DEX" ] && python3 /work/port/shim/gen_thunks.py "$ENGINE" "$DEX" > "$S/jni_thunks.gen.c" && echo "   regenerated $(grep -c JNIEXPORT "$S/jni_thunks.gen.c") thunks"
MODS="cpu loader dispatch sched jni_passthrough jni_entry jni_thunks.gen jni_argbuild galloc elf32 ctype_tables marshal format utf handle_table fdtable bridge_gl bridge_asset bridge_libc bridge_file"
SRCS=""; for m in $MODS; do SRCS="$SRCS $S/$m.c"; done
$CC -shared -fPIC -O2 -Wno-unused -I/opt/unicorn/include -I"$S" $SRCS \
  -Wl,--start-group $UNI -Wl,--end-group -llog -landroid -lGLESv2 -lEGL -lm -ldl -Wl,-z,max-page-size=16384 -o /tmp/shim_x86.so
echo "   arch: $(readelf -h /tmp/shim_x86.so 2>/dev/null | sed -n 's/.*Machine: *//p') (must be X86-64)"
echo "   exports: $(readelf --dyn-syms -W /tmp/shim_x86.so 2>/dev/null | grep -cE ' (Java_com_rovio_|JNI_OnLoad$)') JNI entry points"

echo "== 2/5 unpack + de-phone-home =="
rm -rf "$WORK"; mkdir -p "$WORK"; (cd "$WORK" && unzip -q "$IN")
python3 /work/port/depermission.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml" || true
# Layer 4 (GMS-mediated): disable Firebase Cloud Messaging auto-init (parity with build_apk.sh).
python3 /work/port/manifest_firebase_off.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml"

echo "== 3/5 swap ABIs: x86_64 shim in; 32-bit engine as emulated payload; strip other ABIs =="
cd "$WORK"
rm -rf lib/armeabi-v7a lib/x86 lib/armeabi lib/arm64-v8a lib/x86_64
mkdir -p lib/x86_64
cp /tmp/shim_x86.so  lib/x86_64/libAngryBirdsClassic.so
cp "$ENGINE"         lib/x86_64/libengine32.so
mkdir -p /tmp/aux && (cd /tmp/aux && unzip -o -q "$IN" 'lib/armeabi-v7a/*.so' 2>/dev/null || true)
for b in js adcolony; do f=/tmp/aux/lib/armeabi-v7a/lib$b.so; [ -f "$f" ] && cp "$f" lib/x86_64/lib${b}32.so; done

# Inject the level-script VFS manifest `data/script_paths.json`. It is NOT bundled in the APK
# (normally runtime-staged); without it AAssetManager_open('data/script_paths.json') fails ->
# io::IOException -> the scene loader can't map any script -> JSON ParseError -> HANG. The engine
# reads it as PLAINTEXT JSON (host harness served plaintext `{}`). Start minimal `{}` (valid empty
# table) to unblock the failed-open; real name->path mappings are layered on once the schema is set.
mkdir -p assets/data
python3 /work/port/gen_script_paths.py "$WORK/assets" > assets/data/script_paths.json
echo "   injected assets/data/script_paths.json ($(wc -c < assets/data/script_paths.json) bytes, $(python3 -c 'import json;print(len(json.load(open("'"$WORK"'/assets/data/script_paths.json"))))' 2>/dev/null) keys)"

echo "== 4/5 repack + align =="
rm -f META-INF/*.RSA META-INF/*.SF META-INF/*.MF 2>/dev/null || true
rm -f /tmp/uns_x86.apk /tmp/al_x86.apk
(cd "$WORK" && zip -q -r -X /tmp/uns_x86.apk .)
zipalign -f -p 4 /tmp/uns_x86.apk /tmp/al_x86.apk

echo "== 5/5 sign =="
KS=/tmp/debug.ks
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
  -alias d -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=AngryBirdsShim" >/dev/null 2>&1
apksigner sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --out "$OUTDIR/angrybirds-8.0.3-x86shim.apk" /tmp/al_x86.apk
echo "DONE -> $OUTDIR/angrybirds-8.0.3-x86shim.apk"
unzip -l "$OUTDIR/angrybirds-8.0.3-x86shim.apk" | grep -E 'lib/x86_64|classes.dex'
