#!/bin/bash
# emu_16k_pagesize.sh — does the deliverable actually LOAD on a 16 KB-page Android?
#
# WHY THIS EXISTS
# ---------------
# This was an open gap, and an honest one: OPEN_FINDINGS recorded that the shim and its payloads are
# linked with `-Wl,-z,max-page-size=16384` and that the alignment was verified in the ELF program
# headers, "verified at the mechanism and inferred at the outcome", because no 16 KB page-size image
# existed on this host.
#
# One is available now — `system-images;android-36.1;google_apis_ps16k;x86_64` — so the inference can
# become a run. It matters because the failure mode is not subtle: on a 16 KB-page kernel the loader
# REJECTS a shared object whose segments are only 4 KB-aligned. `dlopen` fails and the app dies at
# launch. This project has already shipped one bug of that exact shape — the shim linked without
# `-lm` ran fine on API 25 and would have died on the A56 with `cannot locate symbol "sin"` — and it
# was caught by running on a newer image, not by reading headers.
#
# THE PREMISE IS ASSERTED FIRST. If the guest reports a 4 KB page size, every result below is
# meaningless: the run would "pass" while testing nothing, which is this project's most expensive
# recurring defect. So the page size is read from the guest and the script exits non-zero if it is
# not 16384 — a missing measurement must not read as a good one.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-16k bash /work/port/validation/emu_16k_pagesize.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_wincheck.sh"
source "$(dirname "$0")/lib_selfhash.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk   # the SHIPPING configuration
PKG=com.rovio.angrybirds
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/pagesize16k.txt"; : >"$LOG"
ABLOG="$OUT/pagesize16k_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }

say "== boot the 16 KB-page image =="
# -feature -ModemSimulator: the android-36.1 image starts a modem simulator that cannot come up in
# a `--network none` container —
#     Unable to connect character device modem: address resolution failed for 127.0.0.1:42895
# — and the device then sits at `offline` forever, so `adb wait-for-device` blocks with no output.
# The older tiers never hit this because their system images do not start one. Network isolation is
# not negotiable here (see the network policy), so the simulator is switched off instead.
emulator -avd "${ABSHIM_AVD:-ab16k}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -feature -ModemSimulator -memory 2048 -partition-size 6144 \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
# Bounded wait: `adb wait-for-device` blocks indefinitely when the device is stuck offline, which is
# exactly what happened — twenty minutes of silence that looked like a slow boot.
for i in $(seq 1 60); do
    D=$(adb devices 2>/dev/null | grep -c "emulator-.*device$")
    [ "${D:-0}" -ge 1 ] && break
    sleep 5
done
if [ "${D:-0}" -lt 1 ]; then
    say "[FAIL] no device came online within 300s — adb says: $(adb devices 2>&1 | tr '\n' ' ')"
    tail -6 /tmp/emu.log 2>/dev/null | sed 's/^/    /' | tee -a "$LOG"
    adb emu kill; exit 1
fi
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1

say
say "== 0. THE PREMISE: the guest kernel's page size =="
# getconf is the direct answer; /proc/self/smaps KernelPageSize is the cross-check. Two independent
# sources, because the AVD being *named* ps16k proves nothing about the kernel that actually booted.
PS=$(adb shell getconf PAGE_SIZE 2>/dev/null | tr -d '\r')
KPS=$(adb shell "grep -m1 KernelPageSize /proc/self/smaps" 2>/dev/null | tr -d '\r' | awk '{print $2, $3}')
say "  getconf PAGE_SIZE          : ${PS:-<none>}"
say "  /proc/self/smaps KernelPage: ${KPS:-<none>}"
if [ "$PS" != "16384" ]; then
    say "  [FAIL] this guest is NOT 16 KB-page (got '${PS:-nothing}'). Everything below would be a"
    say "         4 KB result wearing a 16 KB label, so the run stops here rather than reporting one."
    adb emu kill; exit 1
fi
say "  [ OK ] 16384-byte pages confirmed from inside the guest"

say
say "== 1. install the shipping APK =="
install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }

adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
# -T 1 = start from NOW, not from the head of the buffer. Without it the first attempt captured
# 1,663,267 lines of boot backlog and the capture never reached the app launch at all: the file ended
# at the moment install finished, and the run then reported "the app is not running" on evidence that
# had not been collected. `adb logcat -c` did not clear this image's buffer.
adb logcat -T 1 -s abshim > "$ABLOG" 2>/dev/null &
# Unfiltered too — a loader rejection is logged by ART, not by our shim — but restricted to the
# priorities that matter, or the stream is again dominated by system chatter.
adb logcat -T 1 '*:W' > "$OUT/pagesize16k_full.txt" 2>/dev/null &
sleep 3
MONKEY=$(adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 2>&1 | tr -d '\r')
say "  monkey said: ${MONKEY:-<nothing>}"
# Confirm it STARTED before waiting ten minutes for frames, and say so if it did not (emu_soak.sh
# learned the same lesson: a silent nine-minute wait that ends in "no pid" carries no clue why).
for i in $(seq 1 12); do
    P0=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r'); [ -n "$P0" ] && break; sleep 5
done
if [ -z "$P0" ]; then
    say "  [FAIL] the app did not start within 60s on a 16 KB-page kernel."
    say "  last activity/crash lines:"
    adb logcat -d '*:E' 2>/dev/null | grep -aiE "rovio|dlopen|UnsatisfiedLink|AndroidRuntime|not .*aligned" \
        | tail -8 | sed 's/^/    /' | tee -a "$LOG"
    adb emu kill; exit 1
fi
say "  started as pid $P0"

say
say "== 2. does the shim LOAD, and does the engine run =="
for s in $(seq 1 130); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 8
settle_frames "$ABLOG" 200 200
# Written under a NEUTRAL name. The first version wrote PROOF_16k_pagesize.png unconditionally, and
# the run that produced it never launched the app — a zero-byte file called PROOF_. The claim gate
# caught it ("every proof on disk is described in the index"), which is the check working as
# intended. A capture only earns the PROOF_ prefix once the run it came from actually passed, so the
# rename happens at the end, on success.
adb exec-out screencap -p > "$OUT/pagesize16k_screen.png" 2>/dev/null

say
say "== RESULTS =="
PID=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
FRAME=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
CTOR=$(grep -ac 'ctor' "$ABLOG" 2>/dev/null)
say "  guest page size : $PS"
say "  alive pid       : ${PID:-<none>}"
say "  frames reached  : ${FRAME:-<none>}"
say "  h_fatal         : $(h_fatal_report "$ABLOG")"
say "  shim log lines  : $(wc -l <"$ABLOG")"

# The specific failure a 4 KB-aligned library produces on this kernel.
# Restricted to OUR libraries. The first version counted every dlopen failure on the device and
# reported 366 — all of them system apps probing optional libraries like libtextclassifier3_jni_agsa,
# none of them ours. A check that fires on unrelated system noise is a check that always fails.
DL=$(grep -aiE "dlopen failed|not .*aligned|cannot locate symbol|UnsatisfiedLinkError" "$OUT/pagesize16k_full.txt" 2>/dev/null \
     | grep -aciE "rovio|libAngryBirds|libengine32|abshim")
say "  loader errors   : $DL  (dlopen/alignment/UnsatisfiedLinkError in UNFILTERED logcat)"
[ "${DL:-0}" -eq 0 ] || { say "  [FAIL] the loader complained — see pagesize16k_full.txt"; FAIL=1; }

[ -n "$PID" ] || { say "  [FAIL] the app is not running on a 16 KB-page kernel"; FAIL=1; }
[ -n "$FRAME" ] && [ "$FRAME" -ge 601 ] || { say "  [FAIL] never reached frame[601] — the engine did not render"; FAIL=1; }
HF=$(grep -ac 'h_fatal' "$ABLOG"); [ "${HF:-0}" -eq 0 ] || { say "  [FAIL] h_fatal on a 16 KB-page kernel"; FAIL=1; }
win_check "$OUT/PROOF_16k_pagesize.png" >/dev/null 2>&1   # informational: reaching a level is enough here

# Promote the capture to PROOF_ only if everything above held.
if [ "$FAIL" -eq 0 ] && [ -s "$OUT/pagesize16k_screen.png" ]; then
    cp "$OUT/pagesize16k_screen.png" "$OUT/PROOF_16k_pagesize.png"
    say "  capture promoted to PROOF_16k_pagesize.png (the run passed)"
else
    say "  capture left as pagesize16k_screen.png — this run did not earn a PROOF_ name"
fi

say
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
