#!/bin/bash
# CONTINUOUS-AUDIO CAPTURE (headless): the emulator's qemu links libpulse but a bare container has
# no PulseAudio server -> AudioTrack never drains -> mixData stops after ~8 (the "silent headless"
# artifact). Here we run a headless PulseAudio null sink; the emulator routes audio to it (drains at
# real time -> continuous mixData), and we capture the sink's .monitor to a WAV. Two decisive checks:
#  (1) mixData >> 8  => the mixer runs CONTINUOUSLY (buffer drains).
#  (2) WAV has non-zero amplitude (sox stat) => the game emits ACTUAL SOUND (not silence).
# PulseAudio runs as a non-root user with an anonymous unix socket; the root emulator connects via
# PULSE_SERVER. Everything is a Unix socket -> safe under --network none (no network port bound).
set +e
source "$(dirname "$0")/lib_metrics.sh"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/audiocap.txt"; : >"$LOG"
ABLOG="$OUT/audiocap_abshim.txt"; : >"$ABLOG"
WAV="$OUT/audio_capture.wav"; rm -f "$WAV"
say(){ echo "$@" | tee -a "$LOG"; }

say "== start headless PulseAudio (run as the pkg's 'pulse' user, null sink, anonymous unix socket) =="
SOCK=/tmp/pa/native.sock
mkdir -p /tmp/pa && chown pulse:pulse /tmp/pa
cat > /tmp/pa/start.sh <<EOF
export XDG_RUNTIME_DIR=/tmp/pa HOME=/tmp/pa
pulseaudio --exit-idle-time=-1 --disallow-exit --high-priority=false --realtime=false -n \
  --load="module-null-sink sink_name=vsink rate=44100 channels=2 sink_properties=device.description=vsink" \
  --load="module-native-protocol-unix auth-anonymous=1 socket=$SOCK" \
  --daemonize=true
EOF
chown pulse /tmp/pa/start.sh; chmod +x /tmp/pa/start.sh
su pulse -s /bin/bash /tmp/pa/start.sh >/tmp/pa/pa.log 2>&1
sleep 4
export PULSE_SERVER="unix:$SOCK"
say "  pactl info: $(pactl info 2>&1 | grep -iE 'Server Name|Default Sink' | tr '\n' ' ' )"
say "  sinks:   $(pactl list short sinks 2>&1 | tr '\n' '|')"
say "  sources: $(pactl list short sources 2>&1 | tr '\n' '|')"
[ -S "$SOCK" ] || { say "  PA socket MISSING — pa.log:"; tail -5 /tmp/pa/pa.log | tee -a "$LOG"; }

say "== boot emulator WITH audio routed to PulseAudio (PULSE_SERVER set) =="
( sleep 1500; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null; pkill -9 parecord 2>/dev/null ) &
emulator -avd abtest -no-window -no-boot-anim -no-snapshot -accel on \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 150); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 8
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
adb push /work/out/angrybirds-8.0.3-x86shim-audio.apk /data/local/tmp/ab.apk >/dev/null 2>&1
adb shell pm install -r -d /data/local/tmp/ab.apk 2>&1 | grep -q Success && say "install=ok" || { say "install FAIL"; say DONE; adb emu kill; exit 0; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 32M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== wait for render (frame[601]+), then CAPTURE the null-sink monitor while playing =="
for s in $(seq 1 130); do sleep 5; fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG"|tail -1|grep -oE '[0-9]+'); [ -n "$fn" ]&&[ "$fn" -ge 601 ]&&break; done
MB=$(marker_report "$ABLOG" 'nativeMixData ENABLED')
say "  card at frame[$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG"|tail -1)]  mixData-before-capture=$MB"
parecord --device=vsink.monitor --format=s16le --rate=44100 --channels=2 "$WAV" 2>/tmp/pa/rec.log &
RECPID=$!
say "  capturing ~45s while driving input..."
adb shell input tap 390 266; sleep 6; adb shell input tap 390 266; sleep 8
adb shell input swipe 207 118 110 150 700; sleep 7
adb shell input swipe 207 118 122 140 700; sleep 7
sleep 16
kill $RECPID 2>/dev/null; sleep 1
say "== RESULTS (continuous-audio capture) =="
say "  install:        ok"
say "  mixData total:  $(marker_report "$ABLOG" 'nativeMixData ENABLED')  (>>8 => CONTINUOUS mixing, PulseAudio drained the buffer)"
say "  h_fatal:        $(h_fatal_report "$ABLOG")   stack_chk: $(marker_report "$ABLOG" stack_chk_fail)"
say "  final frame:    $(grep -aoE 'frame\[[0-9]+\]' "$ABLOG"|tail -1)"
say "  WAV bytes:      $(stat -c%s "$WAV" 2>/dev/null || echo 0)"
say "  --- sox stat (Maximum/Minimum amplitude & RMS non-zero => ACTUAL SOUND, not silence) ---"
sox "$WAV" -n stat 2>&1 | grep -iE 'Samples read|Length|RMS.*amplitude|Maximum amplitude|Minimum amplitude|Mean.*norm' | tee -a "$LOG"
say DONE
adb emu kill >/dev/null 2>&1
pkill -9 pulseaudio 2>/dev/null