#!/bin/bash
# arm64_real_run.sh — boot a REAL arm64 Android and run the REAL arm64 deliverable on it.
#
# WHY THIS EXISTS
# ---------------
# Every playthrough in this project is an x86 proxy. `out/angrybirds-8.0.3-arm64.apk` — the thing the
# user actually receives — has never been executed anywhere: it is built, signed, aligned and
# statically audited, and the only arm64 execution on record is a qemu-user engine load plus 125
# constructors, with no Android, no ART, no JNI and no GL.
#
# R17 established that the "arm64 cannot be emulated on this host" story is about the emulator
# LAUNCHER, not about emulation: the engine binary itself accepts arm64 and assembles a ranchu
# machine. It then died on `-soundhw hda`, because ranchu-arm64 has no PCI bus. Four further
# blockers were behind it, each hiding the next:
#
#   1. -soundhw hda                     Intel HDA is PCI-only. Not removable from outside: the AVD's
#                                       hw.audioInput/Output=no ARE honoured (the argument is
#                                       "hda:input=off,output=off") but the device is added anyway.
#                                       Fixed by patching the 8-byte option token to "-pidfile",
#                                       which is exactly the same length and validates nothing about
#                                       its argument. See port/tools/patch_emulator_arm64.py.
#   2. 11x virtio_input_multi_touch_pci Same PCI problem. NOT a binary patch — those literals are the
#                                       registered device type names, and renaming them collides with
#                                       the MMIO types of the same name that this binary also
#                                       registers. Driven by the AVD instead: hw.screen=touch.
#   3. virtio-wifi-pci                  Same again; removed by the emulator's own feature flag,
#                                       -feature -VirtioWifi.
#   4. SIGSEGV in setupSubWindow        With every PCI device gone the machine assembled, and the
#                                       crash moved to the HOST window path — Qt building an
#                                       EmulatorQtWindow with uninitialised geometry
#                                       ("Negative sizes (-469898510,-939797020)") despite
#                                       -no-window. Fixed with -no-skin -qt-hide-window
#                                       (+ showDeviceFrame=no).
#
# With all five applied the arm64 kernel boots and Android userspace starts. This script takes it the
# rest of the way: wait for boot_completed, install the REAL arm64 APK, launch it, and report.
#
# GPU MODE IS NOT COSMETIC HERE. A first attempt used `-gpu off`, reasoning that rendering was not
# the question yet. It boots the kernel and runs init, and then never completes, and the log says
# why in as many words:
#     init: Control message: Could not find 'android.hardware.graphics.composer@2.1::IComposer/default'
# No composer means no SurfaceFlinger, which means boot_completed never arrives however long you
# wait. 20,268 init lines and 30 minutes of TCG established that the patient way.
#
# EXPECT THIS TO BE SLOW. There is no KVM for an arm64 guest on an x86_64 host, so the whole system
# runs under TCG, and the app itself then runs a 32-bit ARM engine under Unicorn INSIDE that. Timeouts
# are large on purpose; the script reports how far it got rather than pretending a timeout is a
# verdict.
#
#   python3 port/tools/patch_emulator_arm64.py <in> <out>     # produce the patched engine
#   docker run --rm --network none -w /tmp -v "$PWD":/work \
#       -v /path/qsa_patched.bin:/opt/android-sdk/emulator/qemu/linux-x86_64/qemu-system-aarch64:ro \
#       ab-qemu-probe bash /work/port/validation/arm64_real_run.sh
set +e
E=/opt/android-sdk/emulator
export LD_LIBRARY_PATH="$E/lib64:$E/lib64/qt/lib:$E/lib64/gles_swiftshader:$E/lib64/vulkan"
export QT_QPA_PLATFORM=offscreen
export ANDROID_AVD_HOME=/root/.android/avd
# This script was self-contained (it sourced nothing) until assert_no_absorbed_faults was wired in
# below. prose_as_code.py caught the omission before any arm64 run did: an unsourced helper is a
# word in command position that resolves to nothing, which is the same class of defect as prose.
source "$(dirname "$0")/lib_metrics.sh"   # h_fatal_report / assert_no_absorbed_faults
QEMU="$E/qemu/linux-x86_64/qemu-system-aarch64"
APK=${ABSHIM_APK:-/work/out/angrybirds-8.0.3-arm64.apk}
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/arm64_real_run.txt"; : >"$LOG"
BOOTLOG="$OUT/arm64_real_boot.txt"; : >"$BOOTLOG"
say(){ echo "$@" | tee -a "$LOG"; }
FAIL=0

[ -f "$APK" ] || { say "  [FAIL] no APK at $APK"; say "DONE (FAIL=1)"; exit 1; }
say "== the artifact under test =="
say "  $APK"
say "  sha256 $(sha256sum "$APK" | cut -c1-64)"
# This must be the AArch64 shim, not an x86 proxy — the whole point of the run.
if unzip -l "$APK" 2>/dev/null | grep -q 'lib/arm64-v8a/'; then
    say "  [ OK ] contains lib/arm64-v8a/ (this is the real deliverable, not a proxy)"
else
    say "  [FAIL] no lib/arm64-v8a/ in this APK — wrong artifact, refusing to report an arm64 result"
    say "DONE (FAIL=1)"; exit 1
fi

# --- the AVD changes that remove the PCI devices (see the header) --------------------------------
# AVD is parameterised so the SAME script covers both arm64 tiers without a second copy:
#   arm64     android-30 (API 30) — boots, then stalls on the missing IComposer HIDL service (R39)
#   arm64_25  android-25 (API 25) — predates Treble, so SurfaceFlinger loads hwcomposer.ranchu.so
#             in-process and there is no composer SERVICE to be missing. Also the tier this
#             project's x86 validation used, so a result is directly comparable.
AVD="${ABSHIM_AVD:-arm64}"
A=/root/.android/avd/${AVD}.avd/config.ini
[ -f "$A" ] || { say "  [FAIL] no AVD config at $A (ABSHIM_AVD=$AVD)"; say "DONE (FAIL=1)"; exit 1; }
sed -i 's/^hw\.screen=.*/hw.screen=touch/'            "$A"
sed -i 's/^showDeviceFrame=.*/showDeviceFrame=no/'    "$A"
sed -i 's/^hw\.ramSize=.*/hw.ramSize=2048/'           "$A"   # 96M cannot boot Android 11 anywhere
sed -i '/^hw\.audio/d' "$A"; printf 'hw.audioInput=no\nhw.audioOutput=no\n' >> "$A"
# The camera HAL crash-loops under TCG (`vendor.camera-provider-2-4 exited 4 times before boot
# completed`), which retriggers sys.init.updatable_crashing over and over and burns emulation
# cycles this run cannot spare. Nothing here needs a camera.
sed -i 's/^hw\.camera\.back=.*/hw.camera.back=none/;s/^hw\.camera\.front=.*/hw.camera.front=none/' "$A"
# THE GPU MUST BE ENABLED IN THE AVD, not just on the command line. This AVD ships with
#     hw.gpu.enabled=no
# which overrides -gpu, so the guest never gets a graphics composer HAL, SurfaceFlinger cannot
# start, and the whole system enters a restart loop: measured 964 "Could not find
# ...IComposer/default" messages (one per second, indefinitely) with vendor.audio-hal restarted 135
# times, zygote 126, netd 126. The boot log looks busy the entire time, which is exactly why the
# init-line counter alone is a bad progress signal.
sed -i 's/^hw\.gpu\.enabled=.*/hw.gpu.enabled=yes/;s/^hw\.gpu\.mode=.*/hw.gpu.mode=swiftshader_indirect/' "$A"
say
say "== AVD, after patching (printed because an unverified patch is a guess) =="
grep -iE '^(hw\.screen|hw\.audio|hw\.ramSize|showDeviceFrame|hw\.gpu|hw\.camera)' "$A" | sed 's/^/  /' | tee -a "$LOG"

say
say "== boot arm64 Android on AVD '$AVD' (TCG, no KVM — this is slow) =="
timeout "${BOOT_TIMEOUT:-9000}" "$QEMU" \
    -avd "$AVD" -no-window -no-audio -no-snapshot -no-boot-anim -no-qt -no-skin -qt-hide-window \
    -memory 2048 -cores 4 -wipe-data -gpu swiftshader_indirect -show-kernel -verbose \
    -feature -VirtioWifi \
    >>"$BOOTLOG" 2>&1 &
QPID=$!

BOOTED=0
for i in $(seq 1 "${WATCH_ITERS:-300}"); do
    sleep 20
    kill -0 $QPID 2>/dev/null || { say "  engine exited after ~$((i*20))s"; break; }
    B=$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
    if [ "$B" = "1" ]; then
        say "  *** sys.boot_completed=1 after ~$((i*20))s — ARM64 ANDROID IS UP ***"
        BOOTED=1; break
    fi
    if [ $((i % 15)) -eq 0 ]; then
        say "  ~$((i*20))s: init lines $(grep -aci 'init:' "$BOOTLOG"), log $(wc -l <"$BOOTLOG") lines"
    fi
done

if [ "$BOOTED" != 1 ]; then
    say "  [FAIL] never reached boot_completed"
    say "  kernel markers: $(grep -aciE 'Linux version|Booting Linux' "$BOOTLOG")   init lines: $(grep -aci 'init:' "$BOOTLOG")"
    say "  (that is how far it got — not a verdict on the APK, which never got to run)"
    kill $QPID 2>/dev/null; say "DONE (FAIL=1)"; exit 1
fi

say
say "== the guest really is arm64 (asked, not assumed) =="
ABI=$(adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')
ABILIST=$(adb shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')
REL=$(adb shell getprop ro.build.version.release 2>/dev/null | tr -d '\r')
SDK=$(adb shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')
say "  ro.product.cpu.abi     = ${ABI:-<unreadable>}"
say "  ro.product.cpu.abilist = ${ABILIST:-<unreadable>}"
say "  android ${REL:-?} (API ${SDK:-?})"
case "$ABI" in
  arm64*) say "  [ OK ] the guest CPU ABI is arm64 — this is not an x86 proxy" ;;
  *) say "  [FAIL] guest ABI is '$ABI', not arm64 — an arm64 claim here would be false"; FAIL=1 ;;
esac

say
say "== install the REAL arm64 APK =="
adb install -r "$APK" 2>&1 | tail -3 | sed 's/^/  /' | tee -a "$LOG"
if adb shell pm list packages 2>/dev/null | grep -q rovio; then
    say "  [ OK ] the arm64 APK installed on arm64 Android"
else
    say "  [FAIL] the arm64 APK did NOT install"; FAIL=1
    say "DONE (FAIL=$FAIL)"; kill $QPID 2>/dev/null; exit "$FAIL"
fi

say
say "== launch it =="
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
ABLOG="$OUT/arm64_real_abshim.txt"; : >"$ABLOG"
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for i in $(seq 1 "${RUN_ITERS:-90}"); do
    sleep 20
    P=$(adb shell pidof com.rovio.angrybirds 2>/dev/null | tr -d '\r')
    CT=$(grep -aoE 'init_array [0-9]+/125' "$ABLOG" 2>/dev/null | tail -1)
    FR=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1)
    [ $((i % 5)) -eq 0 ] && say "  ~$((i*20))s: pid=${P:-none} ctors=${CT:-none} frame=${FR:-none} ablog=$(wc -l <"$ABLOG")"
    [ -n "$FR" ] && break
done

say
say "== what the real arm64 build actually did =="
PID=$(adb shell pidof com.rovio.angrybirds 2>/dev/null | tr -d '\r')
CT=$(grep -aoE 'init_array [0-9]+/125' "$ABLOG" 2>/dev/null | tail -1)
FR=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null | tail -1)
HF=$(grep -ac 'h_fatal' "$ABLOG" 2>/dev/null)
# h_fatal alone is not a health signal — see assert_no_absorbed_faults in lib_metrics.sh (R41).
assert_no_absorbed_faults "${ABLOG}" || FAIL=$((FAIL+1))
say "  abshim log lines : $(wc -l <"$ABLOG")"
say "  constructors     : ${CT:-none}"
say "  frames           : ${FR:-none}"
say "  h_fatal          : ${HF:-0}"
say "  pid              : ${PID:-none}"
adb exec-out screencap -p > "$OUT/arm64_real_screen.png" 2>/dev/null
say "  capture          : arm64_real_screen.png ($(wc -c < "$OUT/arm64_real_screen.png" 2>/dev/null) bytes)"

[ -n "$PID" ] && say "  [ OK ] the process is alive on arm64" || { say "  [FAIL] no process"; FAIL=1; }
[ -n "$CT" ] && say "  [ OK ] the shim loaded and ran constructors on arm64: $CT" \
             || { say "  [FAIL] no constructor progress — the shim did not get going"; FAIL=1; }
[ "${HF:-0}" -eq 0 ] && say "  [ OK ] no h_fatal" || { say "  [FAIL] h_fatal on arm64"; FAIL=1; }

adb shell am force-stop com.rovio.angrybirds >/dev/null 2>&1
kill $QPID 2>/dev/null
say "DONE (FAIL=$FAIL)"
exit "$FAIL"
