#!/bin/bash
# build_apk_x86_shaders.sh — release configuration plus ONLY the glShaderSource dump.
#
# WHY THIS EXISTS
# ---------------
# The full diagnostic build logs every fopen (~3800 lines/run) on top of ~350 shader chunks, and that
# was heavy enough that the game never reached gameplay: the capture stalled at frame[1] while the
# app cycled its DNS retry loop. Screening only the boot-time shaders would be a real result over an
# unrepresentative sample — the uber-shader's variants are selected by #define, so gameplay compiles
# more of them.
#
# -DABSHIM_RELEASE -DABSHIM_SHADERDUMP gives release speed with just the dump. NOT a deliverable.
# build_apk_x86_shaders.sh — a MEASUREMENT-ONLY variant: release configuration plus the perf timers.
#
# WHY THIS EXISTS
# ---------------
# The perf instrumentation is normally non-release, so measuring it on the diagnostic build
# overstates the emulation's share: that build also samples heap checks. This builds with
# -DABSHIM_RELEASE (so none of the heavy diagnostics are present) plus -DABSHIM_PERF (so only the
# two timers are), giving a number representative of what actually ships.
#
# Output: out/angrybirds-8.0.3-x86shim-shaders.apk - NOT a deliverable, and not signed for release.
# The shipped APKs never define ABSHIM_PERF; verify by rebuilding and comparing hashes.
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
# Prepare untracked inputs from the committed .xz (fresh-clone safe) — see prepare_inputs.sh
. /work/port/prepare_inputs.sh
REPO=/work prepare_inputs || exit 1
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
$CC -shared -fPIC -O2 -DABSHIM_RELEASE -DABSHIM_SHADERDUMP -Wno-unused -I/opt/unicorn/include -I"$S" $SRCS \
  -Wl,--start-group $UNI -Wl,--end-group -llog -landroid -lGLESv2 -lEGL -lm -ldl -Wl,-z,max-page-size=16384 -o /tmp/shim_x86.so
echo "   arch: $(readelf -h /tmp/shim_x86.so 2>/dev/null | sed -n 's/.*Machine: *//p') (must be X86-64)"
echo "   exports: $(readelf --dyn-syms -W /tmp/shim_x86.so 2>/dev/null | grep -cE ' (Java_com_rovio_|JNI_OnLoad$)') JNI entry points"
# A build that emits a shim with no JNI entry points is a build that produces an APK which dies at
# launch with UnsatisfiedLinkError, and until now it would have done so QUIETLY. `gen_thunks.py`'s
# output is redirected with `>`, which truncates the generated file BEFORE the script runs, and the
# `[ -f "$DEX" ] && python3 ... && echo` chain does not trip `set -e` when a command inside it fails
# (set -e exempts non-final commands of an && list). So a failure there leaves an empty
# jni_thunks.gen.c in the source tree, the compile succeeds because an empty C file is valid, and the
# count is only PRINTED. Same shape as depermission.py reporting "0 neutralised" as a result.
# Assert instead. This only adds a failure path: when it passes the artifact is byte-identical.
JNIN=$(readelf --dyn-syms -W /tmp/shim_x86.so 2>/dev/null | grep -cE ' (Java_com_rovio_|JNI_OnLoad$)')
[ "${JNIN:-0}" -ge 8 ] || { echo "FATAL: shim exports only ${JNIN:-0} JNI entry points (expected ~73)."; \
    echo "       jni_thunks.gen.c is $(wc -c < "$S/jni_thunks.gen.c" 2>/dev/null) bytes — if that is 0,"; \
    echo "       gen_thunks.py failed and '>' had already truncated it. Refusing to emit an APK that"; \
    echo "       would die at launch with UnsatisfiedLinkError."; exit 1; }

echo "== 2/5 unpack + de-phone-home =="
rm -rf "$WORK"; mkdir -p "$WORK"; (cd "$WORK" && unzip -q "$IN")
python3 /work/port/depermission.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml" || true
# ABSHIM_FIREBASE_CONTROL=1 skips the layer-4 injection and renames the output, so the layer-4
# test has a control to compare against. Never set for a normal build; with the env unset the
# default output is byte-identical.
X86R_APK="angrybirds-8.0.3-x86shim-shaders.apk"
if [ "${ABSHIM_FIREBASE_CONTROL:-0}" = 1 ]; then
  echo "   !! CONTROL BUILD: skipping manifest_firebase_off.py (layer 4 DISABLED on purpose)"
  X86R_APK="angrybirds-8.0.3-x86shim-fbcontrol.apk"
else
  python3 /work/port/manifest_firebase_off.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml"
fi

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
# An EMPTY or unparseable script_paths.json is not a cosmetic problem: the engine opens it through
# AAssetManager, and per the comment above a failed open cascades to io::IOException -> the scene
# loader maps no script -> JSON ParseError -> HANG at boot. gen_script_paths.py writing 0 keys would
# produce exactly that, and the count was only printed. Assert it parses and is non-empty.
python3 - "$WORK/assets/data/script_paths.json" <<'PYCHK' || { echo "FATAL: script_paths.json is empty/unparseable — the engine would hang at boot."; exit 1; }
import json,sys
# The real file is a LIST of ~2035 asset paths, not an object. A first version of this guard asserted
# isinstance(d, dict) and would have failed EVERY build — caught only by running it against the real
# generated file rather than against the shape I assumed from the "{}" mentioned in the comment above
# (that "{}" was an early bootstrap placeholder, long superseded). Accept either container, require
# non-empty, and require the entries to look like asset paths.
d = json.load(open(sys.argv[1]))
if not isinstance(d, (list, dict)) or len(d) == 0:
    sys.exit(1)
items = d if isinstance(d, list) else list(d)
sys.exit(0 if all(isinstance(x, str) for x in items[:50]) else 1)
PYCHK

echo "== 4/5 repack + align =="
rm -f META-INF/*.RSA META-INF/*.SF META-INF/*.MF 2>/dev/null || true
rm -f /tmp/uns_x86.apk /tmp/al_x86.apk
# Deterministic zip, matching build_apk.sh: normalise every entry mtime to the fixed zip epoch
# (1980-01-01Z) so rebuilds are BYTE-identical. Without this the freshly-built lib/*.so carry
# build-time mtimes and two builds of identical source differ — measured: the x86 proxy was NOT
# reproducible, which matters because every play-validation runs on these proxies, so a PROOF
# could not be tied to an exact binary. TZ=UTC pins the DOS-time conversion.
find "$WORK" -exec touch -d @315532800 {} +
(cd "$WORK" && TZ=UTC zip -q -r -X /tmp/uns_x86.apk .)
zipalign -f -p 4 /tmp/uns_x86.apk /tmp/al_x86.apk

echo "== 5/5 sign =="
# Use the SAME committed keystore as the arm64 build. /tmp/debug.ks was wiped by --rm every
# run, so keytool minted a FRESH RANDOM KEY per build: two builds of identical source produced
# different signatures and different hashes, i.e. the x86 proxies were not reproducible at all.
# That matters because every play-validation runs on these, so a PROOF could not be pinned to a
# specific binary. A fixed signer also lets a rebuilt proxy update-install over the previous one
# in the emulator instead of forcing an uninstall.
KS=/work/port/debug.ks
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
  -alias d -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=AngryBirdsShim" >/dev/null 2>&1
apksigner sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --out "$OUTDIR/$X86R_APK" /tmp/al_x86.apk
echo "DONE -> $OUTDIR/$X86R_APK"
unzip -l "$OUTDIR/$X86R_APK" | grep -E 'lib/x86_64|classes.dex'
