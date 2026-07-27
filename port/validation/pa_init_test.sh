set +e
mkdir -p /tmp/pa && chown pulse:pulse /tmp/pa
cat > /tmp/pa/start.sh <<'PA'
export XDG_RUNTIME_DIR=/tmp/pa HOME=/tmp/pa
pulseaudio --exit-idle-time=-1 --disallow-exit --high-priority=false --realtime=false -n \
  --load="module-null-sink sink_name=vsink rate=44100 channels=2" \
  --load="module-native-protocol-unix auth-anonymous=1 socket=/tmp/pa/native.sock" --daemonize=true
PA
chown pulse /tmp/pa/start.sh; chmod +x /tmp/pa/start.sh
su pulse -s /bin/bash /tmp/pa/start.sh >/tmp/pa/pa.log 2>&1
sleep 4
export PULSE_SERVER="unix:/tmp/pa/native.sock" QEMU_PA_SERVER="unix:/tmp/pa/native.sock" QEMU_AUDIO_DRV=pa
export XDG_RUNTIME_DIR=/tmp/pa
pactl info >/dev/null 2>&1   # ensure root cookie/connection exists before emulator
( sleep 180; adb emu kill 2>/dev/null; pkill -9 -f qemu 2>/dev/null ) &
echo "booting emulator with -audio pa ..."
emulator -avd abtest -no-window -no-boot-anim -no-snapshot -accel on -audio pa \
         -gpu swiftshader_indirect -partition-size 6144 -wipe-data >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 60); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = 1 ] && break; sleep 2; done
sleep 6
echo "=== pa init result ==="
grep -iE "Could not init .pa|audio.*pa|pulse" /tmp/emu.log | head -4
echo "  (absence of 'Could not init pa' => success)"
echo "=== sink-inputs (emulator audio stream present => routed) ==="
pactl list short sink-inputs 2>&1 | head
echo "=== sink state (RUNNING => audio flowing) ==="
pactl list short sinks 2>&1
adb emu kill >/dev/null 2>&1; pkill -9 pulseaudio 2>/dev/null
