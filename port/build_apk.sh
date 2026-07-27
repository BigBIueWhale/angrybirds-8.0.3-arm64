#!/bin/bash
# Reproducible conversion: 32-bit Angry Birds 8.0.3 APK  ->  arm64-v8a APK (emulation shim).
# Run inside the `ab-port` Docker image (has NDK + unicorn arm64 libs):
#   docker run --rm --network none -v "$PWD":/work ab-port bash /work/port/build_apk.sh
#
# Output: /work/out/angrybirds-8.0.3-arm64.apk  (installs on the AArch64-only A56).
# STATUS: the shim boots the unmodified 32-bit engine under ARM32->ARM64 emulation and the
# engine RENDERS (validated draws>0, menu graphics loaded, on the x86-shim-in-real-ART rig
# which is the proven proxy for the arm64 build). Built -DABSHIM_RELEASE (diagnostic uc-hooks
# gated off; the functional neut/guard/de-phonehome/S2/CLREX/UAF-quarantine keepers stay).
set -e
NDK=/opt/android-ndk-r26d
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang
UNI="/opt/unicorn/b/libunicorn-static.a /opt/unicorn/b/libunicorn-common.a /opt/unicorn/b/libarm-softmmu.a"
IN=/work/apks/com.rovio.angrybirds@8.0.3.apk
ENGINE=/work/work803/libv7/libAngryBirdsClassic.so

# Inputs (decompressed APK + extracted engine) are not tracked in git; prepare them from the
# committed .xz so every documented direct invocation works from a fresh clone.
. /work/port/prepare_inputs.sh
REPO=/work prepare_inputs || exit 1
OUTDIR=/work/out; mkdir -p "$OUTDIR"
WORK=/tmp/apkwork

# Optional AUDIO-enabled variant: `ABSHIM_AUDIO=1 bash build_apk.sh` compiles the shim with
# -DABSHIM_AUDIO (the cont.121 nested-gt re-entrancy fix + nativeMixData enabled) and writes a
# SEPARATE APK (…-arm64-audio.apk), so the default — silent, validated, bit-reproducible — is
# completely untouched. (A specific hash used to be quoted here; it went stale two rebuilds ago.
# The current one lives in RELEASE_NOTES.md, which is regenerated alongside the artifact.)
# With the env unset, AUDIO_FLAG expands to NO token (identical
# clang args) and OUT_APK is the default name => the default build stays BYTE-IDENTICAL.
# The audio variant is EXPERIMENTAL: crash-free + full playthrough+win validated on the x86 proxy,
# but continuous audio playback is only verifiable on real audio HW (the A56). See ONDEVICE.md.
AUDIO_FLAG=""; OUT_APK="angrybirds-8.0.3-arm64.apk"
if [ "${ABSHIM_AUDIO:-0}" = 1 ]; then AUDIO_FLAG="-DABSHIM_AUDIO"; OUT_APK="angrybirds-8.0.3-arm64-audio.apk"; fi

# Hermetic: the ab-port image bakes in apksigner/zipalign/zip/unzip/keytool (see
# port/docker/Dockerfile.ab-port). This conversion does NO network of its own — it is
# meant to run fully offline (docker run --network none). Fail loudly if a tool is
# missing (a stale/wrong image), rather than silently reaching out to apt.
missing=""
for t in apksigner zipalign zip unzip keytool; do command -v "$t" >/dev/null 2>&1 || missing="$missing $t"; done
[ -n "$missing" ] && { echo "FATAL: image is missing build tools:$missing" >&2
  echo "       rebuild it: docker build -t ab-port -f port/docker/Dockerfile.ab-port ." >&2; exit 1; }

echo "== 1/5 build arm64 shim (.so) — hardened modular implementation =="
S=/work/port/shim/src
# regenerate the 72 JNI thunks from the engine exports + DEX shorties (reproducible)
DEX=/tmp/classes.dex; (cd /tmp && unzip -o -q "$IN" classes.dex 2>/dev/null || true)
[ -f "$DEX" ] && python3 /work/port/shim/gen_thunks.py "$ENGINE" "$DEX" > "$S/jni_thunks.gen.c" && echo "   regenerated $(grep -c JNIEXPORT "$S/jni_thunks.gen.c") thunks"
MODS="cpu loader dispatch sched jni_passthrough jni_entry jni_thunks.gen jni_argbuild galloc elf32 ctype_tables marshal format utf handle_table fdtable bridge_gl bridge_asset bridge_libc bridge_file"
SRCS=""; for m in $MODS; do SRCS="$SRCS $S/$m.c"; done
# -Wl,-z,max-page-size=16384: 16 KB-align the ELF LOAD segments so the shim dlopen()s on 16 KB-page
# devices (Android 15+ / newer arm64 SoCs like the A56's Exynos 1580) as well as 4 KB ones — a
# 4 KB-aligned .so fails to load on a 16 KB-page device, and 16 KB alignment is harmless on 4 KB.
# NDK r26d defaults to 4 KB; NDK r27+ made 16 KB the default. Same class of modern-Android launch
# fix as -lm. (The 32-bit engine payload is data the shim maps itself, so only the shim needs this.)
$CC -shared -fPIC -O2 -DABSHIM_RELEASE $AUDIO_FLAG -Wno-unused -I/opt/unicorn/include -I"$S" $SRCS \
  -Wl,--start-group $UNI -Wl,--end-group -llog -landroid -lGLESv2 -lEGL -lm -ldl \
  -Wl,-z,max-page-size=16384 -o /tmp/shim.so
echo "   arch: $(readelf -h /tmp/shim.so 2>/dev/null | sed -n 's/.*Machine: *//p') (must be AArch64)"
echo "   exports: $(readelf --dyn-syms -W /tmp/shim.so 2>/dev/null | grep -cE ' (Java_com_rovio_|JNI_OnLoad$)') JNI entry points"

echo "== 2/5 unpack original APK + strip network/tracking permissions (no phone-home) =="
rm -rf "$WORK"; mkdir -p "$WORK"; (cd "$WORK" && unzip -q "$IN")
python3 /work/port/depermission.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml" || true
# Layer 4 (GMS-mediated): disable Firebase Cloud Messaging auto-init so it can't register a device
# token via Google Play Services (which would phone home even without the app's INTERNET permission).
python3 /work/port/manifest_firebase_off.py "$WORK/AndroidManifest.xml" "$WORK/AndroidManifest.xml"

echo "== 3/5 swap ABIs (arm64 shim in; 32-bit engine kept as extractable payload; other ABIs stripped) =="
cd "$WORK"
rm -rf lib/armeabi-v7a lib/x86 lib/armeabi lib/arm64-v8a
mkdir -p lib/arm64-v8a
cp /tmp/shim.so                       lib/arm64-v8a/libAngryBirdsClassic.so   # the shim (same soname)
cp "$ENGINE"                          lib/arm64-v8a/libengine32.so            # original 32-bit engine (data, emulated)
mkdir -p /tmp/aux && (cd /tmp/aux && unzip -o -q "$IN" 'lib/armeabi-v7a/*.so' 2>/dev/null || true)
for b in js adcolony; do f=/tmp/aux/lib/armeabi-v7a/lib$b.so; [ -f "$f" ] && cp "$f" lib/arm64-v8a/lib${b}32.so; done

# Inject the level-script VFS manifest data/script_paths.json (NOT bundled in the APK; normally
# runtime-staged). Without it the scene loader can't map scripts -> IOException/JSON errors -> the
# scene ctor hangs (draws=0). Generated from the APK's own scripts; read as PLAINTEXT JSON. Kept in
# sync with build_apk_x86.sh. NB schema still being finalised (see memory UPDATE 93) — regenerate here.
mkdir -p assets/data
python3 /work/port/gen_script_paths.py "$WORK/assets" > assets/data/script_paths.json
echo "   injected assets/data/script_paths.json ($(wc -c < assets/data/script_paths.json) bytes)"

echo "== 4/5 strip old signature, repack, zipalign =="
rm -f META-INF/*.RSA META-INF/*.SF META-INF/*.MF 2>/dev/null || true
rm -f /tmp/unsigned.apk /tmp/aligned.apk
# Deterministic zip: normalize every entry's mtime to the fixed zip epoch (1980-01-01Z) so rebuilds
# are BYTE-identical, not merely component-identical. Without this, freshly-created lib/*.so carry
# build-time mtimes that differ per run. (Entry order from `zip -r .` is already stable: the same
# input filenames yield the same fs hash order across runs.) TZ=UTC pins the DOS-time conversion.
find "$WORK" -exec touch -d @315532800 {} +
(cd "$WORK" && TZ=UTC zip -q -r -X /tmp/unsigned.apk .)
zipalign -f -p 4 /tmp/unsigned.apk /tmp/aligned.apk

echo "== 5/5 debug-sign (installable / 'unsigned'-equivalent) =="
# Persist the throwaway debug key IN THE REPO (not /tmp, which a --rm container wipes each run) so
# EVERY rebuild signs with the SAME identity. This makes the signer reproducible and — crucially —
# lets a rebuilt APK update-install OVER a prior install (Android refuses to update across a
# different signer; a random key per build would force uninstall + save-data loss). Standard
# Android-style debug key (password 'android', CN=AngryBirdsShim); generated once, then committed.
KS=/work/port/debug.ks
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
  -alias d -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=AngryBirdsShim" >/dev/null 2>&1
apksigner sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --out "$OUTDIR/$OUT_APK" /tmp/aligned.apk

echo "DONE -> $OUTDIR/$OUT_APK"
ls -la "$OUTDIR/$OUT_APK"
echo "arm64 payload:"; unzip -l "$OUTDIR/$OUT_APK" | grep -E 'lib/arm64|classes.dex' | head
