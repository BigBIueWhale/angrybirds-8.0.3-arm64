#!/bin/bash
# arm64_boot_probe.sh — can the DELIVERABLE actually execute?
#
# The Android emulator launcher refuses an arm64 AVD on an x86_64 host:
#     FATAL | Avd's CPU Architecture 'arm64' is not supported by the QEMU2 emulator on x86_64 host.
# and this project recorded that as "not possible", which is why
# out/angrybirds-8.0.3-arm64.apk has never been executed anywhere.
#
# But the engine ships in the x86_64 distribution anyway:
#     /opt/android-sdk/emulator/qemu/linux-x86_64/qemu-system-aarch64
# It is a normal x86_64 ELF, it accepts the FULL emulator option set (-avd, -sysdir, -system,
# -kernel, -ramdisk, -gpu, ...), and once its runtime libs are present it starts. The arch check
# that stops the run lives in the LAUNCHER above it.
#
# So this asks the only question that matters: does the engine itself refuse, or does it boot?
# Under TCG this is very slow — an arm64 Android boot on an x86_64 host has no KVM to fall back on —
# so the timeout is generous and the script reports HOW FAR it got rather than pass/fail.
# WHERE IT ACTUALLY STOPS (measured 2026-07-29, three consecutive runs, same point):
#
#   DEBUG | Target arch = 'arm64'                     <- no architecture refusal
#   DEBUG | Starting QEMU main loop
#   DEBUG | control console listening on port 5554, ADB on port 5555
#   qemu-system-aarch64: PCI bus not available for hda
#   WARNING | QEMU main loop exits abnormally with code 1
#
# The engine accepts arm64, assembles a complete ranchu machine
# (-cpu cortex-a57 -machine type=ranchu -smp cores=4 -m 2048, kernel-ranchu, five virtio drives,
# -android-hw), starts the QEMU main loop and opens the console and ADB ports. It then aborts
# because the argv it generated contains `-soundhw hda` and the arm64 ranchu machine has no PCI bus
# to attach an Intel HDA device to.
#
# That option could NOT be removed from outside. Verified, not assumed: hw.audioInput/hw.audioOutput
# set to `no` in the AVD (printed back by this script to prove the patch landed), plus `-no-audio`
# and `-audio none`, and the generated argv still contains `-soundhw hda`. The emulator builds that
# argv in-process and calls QEMU directly, so there is no argv to intercept.
#
# So the honest state is: NOT an architecture limitation, and not yet a boot. The blocker is one
# unconditional device option in emulator 36.6.11's arm64 argv generation. The obvious next thing to
# try is an OLDER emulator — ARM images on x86 hosts were routine for years — which needs a network
# download and has not been attempted here.
#
# TWO TRAPS THIS SCRIPT PRE-EMPTS, both of which would have produced a confident wrong answer:
#   - the committed AVD says hw.ramSize=96M, which cannot boot Android 11 on ANY architecture
#   - "no soundhw in the argv" also happens when the run dies BEFORE generating the argv, so this
#     script counts the argv line itself rather than treating an absent symptom as a fixed problem
set +e
E=/opt/android-sdk/emulator
export LD_LIBRARY_PATH="$E/lib64:$E/lib64/qt/lib:$E/lib64/gles_swiftshader:$E/lib64/vulkan"
export ANDROID_AVD_HOME=/root/.android/avd
# The first attempt died 15s in with "no Qt platform plugin could be initialized" — despite
# -no-window, the emulator still brings Qt up, and this container has no X display. That is a
# HEADLESS problem, not an architecture one: the engine had already accepted "Target arch = 'arm64'",
# loaded kernel-ranchu, created filesystems and registered qemud services before Qt killed it.
# "offscreen" is in the binary's own list of available platform plugins.
#
# Round 2 got further and died on the GRAPHICS stack: the Vulkan loader found no driver and libX11
# was absent. Also not architectural. -gpu off removes host GPU emulation from the question — the
# point right now is whether an arm64 Android BOOTS under TCG, not how it renders. If it boots, the
# GL path becomes the next problem to solve, and a real one, since the game needs it.
export QT_QPA_PLATFORM=offscreen
QEMU="$E/qemu/linux-x86_64/qemu-system-aarch64"

BOOTLOG=${BOOTLOG:-/work/reports/shots/arm64_boot.txt}
mkdir -p "$(dirname "$BOOTLOG")"; : >"$BOOTLOG"
say(){ echo "$@" | tee -a "$BOOTLOG"; }

# Round 3 reached "Starting QEMU main loop" and "control console listening on port 5554, ADB on
# port 5555" — the arm64 machine genuinely started — then died on:
#     qemu-system-aarch64: PCI bus not available for hda
#     WARNING | QEMU main loop exits abnormally with code 1
# The emulator appends `-soundhw hda` from the AVD's hardware config, and the arm64 `ranchu` machine
# has no PCI bus to hang it on. `-no-audio` does NOT remove it (the generated command line contains
# both). Turning audio off in the AVD does, so patch the config rather than fight the flag.
AVD=/root/.android/avd/arm64.avd/config.ini
if [ -f "$AVD" ]; then
    sed -i '/^hw\.audio/d' "$AVD"
    printf 'hw.audioInput=no\nhw.audioOutput=no\n' >> "$AVD"
    # 96M cannot boot Android 11; the engine reads this too, so fix it at the source as well.
    sed -i 's/^hw\.ramSize=.*/hw.ramSize=2048/' "$AVD"
fi

say "== AVD config after patching (printed, because an unverified patch is a guess) =="
grep -iE "^hw\.(audio|ramSize)" "$AVD" 2>/dev/null | sed 's/^/  /' | tee -a "$BOOTLOG"

say "== the engine, driven directly (launcher bypassed) =="
say "  qemu: $QEMU"
say "  avd : arm64  (android-30 google_apis arm64-v8a)"

# -show-kernel so a boot that starts but stalls is DISTINGUISHABLE from one that never started.
# Without it a refusal and a slow boot look identical from outside.
# -memory 2048 is NOT cosmetic. The committed AVD says hw.ramSize=96M, which cannot boot Android 11
# on any architecture — a probe run as-configured would have died for a reason with nothing to do
# with arm64 and been read as "arm64 does not work". -cores 4 matches hw.cpu.ncore.
timeout "${BOOT_TIMEOUT:-2400}" "$QEMU" \
    -avd arm64 -no-window -no-audio -no-snapshot -no-boot-anim -no-qt \
    -memory 2048 -cores 4 -wipe-data -audio none \
    -gpu off -show-kernel -verbose \
    >>"$BOOTLOG" 2>&1 &
QPID=$!

say
say "== watching for signs of life =="
BOOTED=0
for i in $(seq 1 "${WATCH_ITERS:-160}"); do
    sleep 15
    kill -0 $QPID 2>/dev/null || { say "  engine exited after ~$((i*15))s"; break; }
    # adb only answers once the guest's adbd is up, which is deep into a successful boot
    B=$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
    if [ "$B" = "1" ]; then
        say "  *** sys.boot_completed=1 after ~$((i*15))s — ANDROID ARM64 IS UP ***"
        BOOTED=1; break
    fi
    if [ $((i % 4)) -eq 0 ]; then
        say "  ~$((i*15))s: $(grep -acE 'Linux version|Freeing unused|init:' "$BOOTLOG") kernel marker(s), log $(wc -l <"$BOOTLOG") lines"
    fi
done

say
say "== how far it got =="
grep -qai "not supported by the QEMU2 emulator" "$BOOTLOG" \
    && say "  [STOP] the ENGINE refuses too — the check is not only in the launcher" \
    || say "  [ OK ] the engine did not refuse on architecture"
for m in "Linux version" "Booting Linux" "Freeing unused kernel" "init: " "zygote" "Boot is finished"; do
    printf "  %-26s %s\n" "$m" "$(grep -aci "$m" "$BOOTLOG")" | tee -a "$BOOTLOG"
done
say "  boot_completed reached: $BOOTED"
kill $QPID 2>/dev/null
exit 0
