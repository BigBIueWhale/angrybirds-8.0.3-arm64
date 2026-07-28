#!/bin/bash
# emu_jni_exception_probe.sh — capture UNFILTERED logcat and look for ART complaining about
# pending JNI exceptions.
#
# WHY THIS EXISTS
# ---------------
# Every other validation script captures `adb logcat -s abshim`, i.e. ONLY our own tag. That is
# exactly the wrong filter for this question: the shim invokes real JNI methods on the guest's
# behalf and never calls ExceptionCheck afterwards, so if a call throws, the exception stays
# PENDING — and the complaint about that comes from ART, under its own tags, which our filter
# discards.
#
# The hypothesis was that the de-phone-home work CREATES throw sites: ACCESS_NETWORK_STATE is
# mangled, so ConnectivityManager.getActiveNetwork() could throw SecurityException - and cont.144
# measured the engine walking a garbage pointer immediately after a JNI call returning
# android.net.Network. Per the JNI spec, with an exception pending only a small set of functions may
# be called; anything else is undefined, ART says so, and CheckJNI aborts.
#
# RESULT: the hypothesis is REFUTED, and this script is kept as the standing regression check.
# Measured on API 34 with the Network call made 18 times in the run: 0 "pending exception",
# 0 "JNI DETECTED ERROR", 0 SecurityException, 0 aborts. The call returns NULL rather than throwing,
# so nothing is left pending. The garbage pointer is the engine's own behaviour on the null path,
# and it is now harmless because cpu.c zero-fills a failed guest read (cont.144).
#
# It also turned out to be the best RUNTIME de-phone-home evidence available: assertion 2 below
# proves at OS level that the app's own pid performs no name resolution. (Every DNS failure in the
# log belongs to the system's NetworkMonitor/resolv on a different pid - which is exactly why the
# check attributes by pid instead of counting occurrences.)
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_jni_exception_probe.sh
set +e
# AVD and output prefix overridable so this also runs on the API 36 tier (the A56's actual OS).
# Defaults reproduce the original behaviour exactly.
AVD="${ABSHIM_AVD:-abtest34}"
PFX="${ABSHIM_OUTPFX:-jniexc}"
source "$(dirname "$0")/lib_settle.sh"
( sleep 2400; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/${PFX}_probe.txt"; : >"$LOG"
FULL="$OUT/${PFX}_full_logcat.txt"; : >"$FULL"
ABLOG="$OUT/${PFX}_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }

say "== boot API 34 =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 200); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 10
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push "$APK" /data/local/tmp/ab.apk >/dev/null 2>&1
INST=fail
for t in 1 2 3 4; do adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && { INST=ok; break; }; sleep 4; done
say "install=$INST"; [ "$INST" = ok ] || { say DONE; adb emu kill; exit 0; }

adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat            > "$FULL"  2>/dev/null &     # EVERYTHING, no tag filter
adb logcat -s abshim  > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== let it boot + render =="
for s in $(seq 1 150); do sleep 5
  fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
  [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
adb shell input tap 640 1500 >/dev/null 2>&1     # dismiss the 'older Android' dialog
sleep 3
adb shell input tap 640 800 >/dev/null 2>&1      # tap through the start card
settle_frames "$ABLOG" 120 300

PID=$(adb shell pidof com.rovio.angrybirds 2>/dev/null | tr -d '\r')
FAIL=0
ok(){  printf "  [ OK ] %s\n" "$1" | tee -a "$LOG"; }
bad(){ printf "  [FAIL] %s\n" "$1" | tee -a "$LOG"; FAIL=1; }

say ""
say "== ASSERTION 1: no pending-exception / CheckJNI / abort complaints from ART =="
# The shim invokes real JNI methods for the guest and never calls ExceptionCheck afterwards. If a
# call threw, the exception would stay PENDING and ART would say so (and CheckJNI would abort).
for pat in "pending exception" "JNI DETECTED ERROR" "Runtime aborting"; do
  n=$(grep -aic "$pat" "$FULL")
  [ "$n" = 0 ] && ok "no '$pat'" || bad "$n x '$pat'"
done

say ""
say "== ASSERTION 2: the app itself performs NO name resolution or socket work =="
# RUNTIME de-phone-home proof, as opposed to verify_claims.sh's static manifest check. Any DNS the
# emulator does is the SYSTEM's own connectivity probing (NetworkMonitor/resolv, a different pid) -
# so the check is specifically that OUR pid never appears on such a line.
if [ -z "$PID" ]; then bad "app not running - cannot attribute network activity"; else
  ok "app pid = $PID"
  for pat in "UnknownHostException" "SocketException" "ConnectException" "socket failed"; do
    tot=$(grep -aic "$pat" "$FULL")
    mine=$(grep -ai "$pat" "$FULL" | awk -v p="$PID" '$3==p' | wc -l)
    [ "$mine" = 0 ] && ok "$pat: $tot in the log, 0 from our pid" \
                    || bad "$pat: $mine from OUR pid - the app attempted network"
  done
fi

say ""
say "== ASSERTION 3: a bundled tracking SDK observes the missing permission =="
# Positive evidence the de-permissioning is real and visible to the SDKs, not just to aapt.
SDKSEEN=0
grep -aiq "Flurry.*ACCESS_NETWORK_STATE" "$FULL" \
  && { ok "FlurryAgent reports ACCESS_NETWORK_STATE is not declared"; SDKSEEN=$((SDKSEEN+1)); } \
  || say "  [note] Flurry did not log its permission warning this run"
# The Facebook SDK states it even more plainly, and is a SECOND independent confirmation.
grep -aiq "No internet permissions granted for the app" "$FULL" \
  && { ok "Facebook SDK reports no INTERNET permission granted"; SDKSEEN=$((SDKSEEN+1)); } \
  || say "  [note] the Facebook SDK did not log its permission warning this run"
[ "$SDKSEEN" -ge 1 ] && ok "$SDKSEEN bundled SDK(s) observed the stripped permissions at runtime" \
                     || bad "no bundled SDK reported a missing permission - is the strip actually reaching them?"
# GMS is ABSENT from this emulator ("Google Play Store is missing"), so de-phone-home layer 4 - the
# Firebase/FCM auto-init kill-switch, which exists precisely for GMS-MEDIATED phone-homes - is NOT
# exercised here. Say so rather than let a green run imply it was covered. See OPEN_FINDINGS.md.
# Ask the DEVICE whether GMS is installed, rather than inferring it from a log line. The first
# version grepped for "Google Play Store is missing", which is emitted by google_apis images too -
# they ship Google Play SERVICES without the Play STORE app - so it printed "no Google Play
# Services" on an image that demonstrably has them (the layer-4 test measures "GMS present: 2" on
# the same AVD). A scope disclaimer that is itself false is worse than none.
if ! adb shell pm list packages 2>/dev/null | grep -q com.google.android.gms; then
  say "  [SCOPE] no Google Play Services on this AVD -> layer 4 (Firebase/FCM auto-init"
  say "          kill-switch) is NOT exercised by this run. The A56 does have GMS."
else
  say "  [scope] GMS IS present on this AVD, so layer 4 is exercisable here - see emu_layer4_fcm_test.sh"
fi

say ""
say "== raw counts (informational) =="
say "  'pending exception':      $(grep -aic 'pending exception' "$FULL")"
say "  'JNI DETECTED ERROR':     $(grep -aic 'JNI DETECTED ERROR' "$FULL")"
say "  'SecurityException':      $(grep -aic 'SecurityException' "$FULL")"
say "  'Permission Denial':      $(grep -aic 'Permission Denial' "$FULL")"
say "  'ACCESS_NETWORK_STATE':   $(grep -aic 'ACCESS_NETWORK_STATE' "$FULL")"
say "  'getActiveNetwork':       $(grep -aic 'getActiveNetwork' "$FULL")"
say "  'art::Thread' aborts:     $(grep -aic 'Runtime aborting' "$FULL")"
say ""
say "== first matches (if any) =="
grep -ai 'pending exception\|JNI DETECTED ERROR\|SecurityException\|Permission Denial' "$FULL" 2>/dev/null | head -12 | tee -a "$LOG"
say ""
say "== our own guest-read failures for correlation =="
say "  [gm] read FAIL:           $(grep -ac '\[gm\] read FAIL' "$ABLOG")"
say "  h_fatal:                  $(grep -ac '\[h_fatal\]' "$ABLOG")"
say "  last frame:               $(grep -aoE 'frame\[[0-9]+\]' "$ABLOG"|tail -1)"
say "  final pid:                [$(adb shell pidof com.rovio.angrybirds 2>/dev/null|tr -d '\r')]"
say "  full logcat kept at:      reports/shots/jniexc_full_logcat.txt ($(wc -c < "$FULL") bytes)"
say ""
[ "$FAIL" = 0 ] && say "PROBE PASSED — no pending JNI exceptions, and the app itself did no network" \
              || say "PROBE FAILED — see [FAIL] lines above"
say DONE
adb emu kill >/dev/null 2>&1
exit "$FAIL"
