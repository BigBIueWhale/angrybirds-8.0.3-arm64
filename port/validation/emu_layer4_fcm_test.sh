#!/bin/bash
# emu_layer4_fcm_test.sh — the DIFFERENTIAL test for de-phone-home layer 4, on a GMS emulator.
#
# WHY THIS EXISTS
# ---------------
# Layer 4 is `manifest_firebase_off.py` injecting `firebase_messaging_auto_init_enabled=false`. It
# guards the one phone-home layers 1-3 cannot close: **FCM registration is performed by Google Play
# Services on the app's behalf**, so it never needs the app's own INTERNET permission, and stripping
# that permission does not stop it.
#
# Every other emulator tier here runs an AOSP image with NO GMS, where Firebase never initialises at
# all — so layer 4 was asserted from the manifest and exercised by nothing. The A56 has GMS.
# `Dockerfile.ab-emu-gms` reproduces that condition.
#
# WHY IT IS DIFFERENTIAL
# ----------------------
# "We looked and saw no FCM registration" is worth very little on its own: it is equally consistent
# with the kill-switch working and with nothing ever having tried. So this runs BOTH builds:
#
#   CONTROL  angrybirds-8.0.3-x86shim-fbcontrol.apk   — identical build, layer 4 REMOVED
#   SHIPPED  angrybirds-8.0.3-x86shim-release.apk     — layer 4 present
#
# and requires the control to show Firebase/FCM activity that the shipped build does not. If the
# control is quiet too, the test proves nothing and says so rather than reporting a pass.
#
#   docker build -f port/docker/Dockerfile.ab-emu-gms -t ab-emu-gms port/docker
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-gms bash /work/port/validation/emu_layer4_fcm_test.sh
#
# NOTE ON `--network none`: the emulator has no route out, so no token can actually be *fetched*.
# That is fine and is not what is being measured — the question is whether the app's Firebase
# messaging component AUTO-INITIALISES and tries. An attempt is visible in logcat either way.
set +e
source "$(dirname "$0")/lib_install.sh"
( sleep 3000; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
OUT=/work/reports/shots; mkdir -p "$OUT"
LOG="$OUT/layer4_fcm${ABSHIM_OUTPFX:+_$ABSHIM_OUTPFX}.txt"; : >"$LOG"
say(){ echo "$@" | tee -a "$LOG"; }
FAIL=0
ok(){  printf "  [ OK ] %s\n" "$1" | tee -a "$LOG"; }
bad(){ printf "  [FAIL] %s\n" "$1" | tee -a "$LOG"; FAIL=1; }

CONTROL=/work/out/angrybirds-8.0.3-x86shim-fbcontrol.apk
SHIPPED=/work/out/angrybirds-8.0.3-x86shim-release.apk
for f in "$CONTROL" "$SHIPPED"; do
  [ -f "$f" ] || { say "FATAL: missing $f"; say "  build it: ABSHIM_FIREBASE_CONTROL=1 bash port/build_apk_x86_release.sh"; exit 1; }
done

# AVD overridable so this runs on the API 36 tier too (the A56's actual OS). Both images are
# google_apis, which is the requirement - the test is meaningless without GMS.
AVD="${ABSHIM_AVD:-abgms}"
say "== boot WITH Google Play Services (AVD $AVD) =="
emulator -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 240); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 20
say "  GMS present: $(adb shell pm list packages 2>/dev/null | grep -c com.google.android.gms)  (must be >=1)"
adb shell pm list packages 2>/dev/null | grep -q com.google.android.gms \
  || { bad "no GMS on this AVD - wrong image; this test is meaningless without it"; say DONE; adb emu kill; exit 1; }

# Firebase/FCM signals. Deliberately broad: the point is to detect ANY auto-init or registration
# attempt, and the exact tags differ between GMS revisions.
PAT='FirebaseApp|FirebaseMessaging|FirebaseInstanceId|FirebaseInstallations|firebase-iid|Firebase-Messaging|gcm_register|GCM registration|Registering with GMS|MessagingAnalytics'
# The COUNT of Firebase lines is a poor verdict: both arms log the same two benign
# "FirebaseApp failed to initialize / initialization unsuccessful" lines, because the emulator has no
# google-services config. Comparing 3-vs-2 would pass on what is essentially noise. The signal that
# actually distinguishes them is the TOKEN REGISTRATION ATTEMPT - the phone-home layer 4 exists to
# prevent - which appears only in the control:
#     "SERVICE_NOT_AVAILABLE. Will retry token retrieval"
# So the verdict asserts on that specifically, in both directions.
TOKPAT='token retrieval|getToken|Registering with GMS|gcm_register|GCM registration|registration token'

# NB: run_one is invoked inside $( ), i.e. a SUBSHELL, so a bad() call in here PRINTS but cannot
# set FAIL in the parent - the same subshell trap that once let verify_claims.sh print [FAIL] and
# still exit 0. What actually propagates is the "-1" sentinel on stdout, which the verdict below
# checks explicitly. Do not add a bad() here and assume it fails the run.
run_one(){   # $1=label  $2=apk  $3=outfile
  local label="$1" apk="$2" out="$3"
  say ""
  say "== $label =="
  adb uninstall com.rovio.angrybirds >/dev/null 2>&1
  # see lib_install.sh — was a bare retry that could not distinguish "package service not up
  # yet" from "the APK was rejected", and reported the first as the second.
  local inst=fail
  if install_apk "$apk" 4; then inst=ok; fi
  say "  install=$inst"
  [ "$inst" = ok ] || { bad "$label failed to install"; return 1; }
  # Let the package manager finish tearing down the PREVIOUS install's task before launching.
  # Without this the freshly started process is killed ~11 ms in with
  # "Killing <pid> (adj -10000): remove task" - which produced a silent app, zero Firebase lines,
  # and a PASS that meant nothing. A zero is only evidence if the subject was actually alive.
  adb shell am force-stop com.rovio.angrybirds >/dev/null 2>&1
  sleep 12
  adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
  adb logcat > "$out" 2>/dev/null &
  local LPID=$!
  local alive="" try
  for try in 1 2 3; do
    adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    local w
    for w in $(seq 1 20); do
      sleep 3
      alive=$(adb shell pidof com.rovio.angrybirds 2>/dev/null | tr -d '\r')
      [ -n "$alive" ] && break
    done
    [ -n "$alive" ] && break
    say "  launch attempt $try did not leave a live process; retrying"
  done
  if [ -z "$alive" ]; then
    kill $LPID 2>/dev/null
    bad "$label never stayed running - this run measures NOTHING"
    echo "-1"; return 1
  fi
  say "  running as pid $alive"
  # Firebase auto-init happens at ContentProvider time, i.e. essentially at process start. Give it
  # a generous window anyway so a slow GMS handshake is not mistaken for absence.
  sleep 90
  # LIVENESS PRECONDITION: the shim must have logged, otherwise the app was not really executing
  # and a Firebase count of 0 is meaningless rather than reassuring.
  local ab; ab=$(grep -ac 'abshim' "$out")
  kill $LPID 2>/dev/null
  say "  abshim log lines: $ab  (proves the app actually executed)"
  [ "$ab" -gt 0 ] || { bad "$label produced no abshim output - the app did not really run"; echo "-1"; return 1; }
  local n; n=$(grep -aEc "$PAT" "$out")
  local tk; tk=$(grep -aEc "$TOKPAT" "$out")
  say "  Firebase/FCM log lines: $n   (of which TOKEN-REGISTRATION attempts: $tk)"
  grep -aE "$PAT" "$out" | head -6 | sed 's/^/    /' | tee -a "$LOG"
  echo "$tk"
}

CN=$(run_one "CONTROL (layer 4 REMOVED - must show Firebase activity)" "$CONTROL" "$OUT/layer4_control_logcat.txt" | tail -1)
SN=$(run_one "SHIPPED (layer 4 present - must be quiet)"               "$SHIPPED" "$OUT/layer4_shipped_logcat.txt" | tail -1)

say ""
say "== VERDICT =="
say "  control token-registration attempts: ${CN:-0}"
say "  shipped token-registration attempts: ${SN:-0}"
if [ "${CN:-0}" -lt 0 ] || [ "${SN:-0}" -lt 0 ]; then
  bad "at least one arm failed to run - the comparison is void (see above)"
elif [ "${CN:-0}" -eq 0 ]; then
  bad "the CONTROL never attempted token registration - this run proves NOTHING about layer 4"
  say "        (do not read the shipped build's silence as success; the experiment did not fire)"
elif [ "${SN:-0}" -eq 0 ]; then
  ok "control attempted token registration ${CN}x, shipped 0x - layer 4 demonstrably prevents it"
else
  bad "shipped still attempted token registration ${SN}x - layer 4 is NOT preventing the phone-home"
fi
say ""
[ "$FAIL" = 0 ] && say "LAYER 4 TEST PASSED" || say "LAYER 4 TEST FAILED"
say DONE
adb emu kill >/dev/null 2>&1
exit "$FAIL"
