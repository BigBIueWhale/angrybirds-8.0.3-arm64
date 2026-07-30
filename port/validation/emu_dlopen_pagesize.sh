#!/bin/bash
# emu_dlopen_pagesize.sh — does the SHIM actually load on a 16 KB-page kernel?
#
# WHY THIS EXISTS
# ---------------
# R18 originally argued that because the APK installs on a 16 KB-page device, the alignment must be
# fine. That was wrong: Android rejects a misaligned library at dlopen, not at install. The claim
# needed the loader's answer, and the game cannot supply it on that image because it never launches
# there (R18, unrelated cause). So ask the loader directly, with a 20-line program.
#
# THREE MEASUREMENTS, because one is not interpretable on its own:
#   A  16 KB-aligned tester dlopens THE SHIM      — the actual question
#   B  16 KB-aligned tester dlopens system libc   — positive control: the tester works at all
#   C  4 KB-aligned tester                        — negative control: does this kernel ENFORCE?
#
# Without C, "A succeeded" would not establish that the kernel checks alignment; the run could be
# passing because nothing is enforced. Without B, a broken tester reads as a broken shim — which is
# exactly what happened first time round.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work -e AVD=ab16k ab-emu-16k bash /work/port/validation/emu_dlopen_pagesize.sh
#
# Run it on ab-emu-36 (AVD=ab36) too: there C must SUCCEED, which is what proves C's failure on the
# 16 KB image is the page size and not a broken binary.
set +e
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/dlopen_pagesize.txt"; : >"$LOG"
say(){ echo "$@" | tee -a "$LOG"; }
FAIL=0
for f in dltest16k dltest4k shim_x86_64.so; do
    [ -s "/work/out/$f" ] || { say "[FAIL] missing /work/out/$f — build it with build_dltest.sh"; exit 1; }
done

emulator -avd "${AVD:-ab16k}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -feature -ModemSimulator -memory 2048 -partition-size 6144 -gpu swiftshader_indirect >/tmp/e.log 2>&1 &
for i in $(seq 1 90); do sleep 5; [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; done
PS=$(adb shell getconf PAGE_SIZE 2>/dev/null | tr -d '\r')
say "  API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r')   guest PAGE_SIZE = ${PS:-<unknown>}"
[ -n "$PS" ] || { say "[FAIL] could not read the guest page size — nothing below would be interpretable"; adb emu kill; exit 1; }

for f in dltest16k dltest4k shim_x86_64.so; do adb push "/work/out/$f" "/data/local/tmp/$f" >/dev/null 2>&1; done
adb shell chmod 755 /data/local/tmp/dltest16k /data/local/tmp/dltest4k >/dev/null 2>&1

A=$(adb shell 'cd /data/local/tmp && ./dltest16k ./shim_x86_64.so' 2>&1 | tr -d '\r' | tail -1)
B=$(adb shell 'cd /data/local/tmp && ./dltest16k /system/lib64/libc.so' 2>&1 | tr -d '\r' | tail -1)
C=$(adb shell 'cd /data/local/tmp && ./dltest4k  /system/lib64/libc.so' 2>&1 | tr -d '\r' | tail -1)
say "  A  16 KB-aligned tester, dlopen THE SHIM    : $A"
say "  B  16 KB-aligned tester, dlopen system libc : $B"
say "  C  4 KB-aligned tester  (negative control)  : $C"

case "$B" in *DLOPEN-OK*) ;; *) say "  [FAIL] the tester itself does not work here — A means nothing"; FAIL=1 ;; esac
case "$A" in *DLOPEN-OK*) say "  [ OK ] the shim loads on a ${PS}-byte page kernel" ;;
             *)           say "  [FAIL] the shim did NOT load"; FAIL=1 ;; esac
if [ "$PS" = "16384" ]; then
    case "$C" in *DLOPEN-OK*) say "  [FAIL] a 4 KB-aligned binary ran on a 16 KB kernel — this image does NOT"
                              say "         enforce alignment, so A proves nothing about the shim"; FAIL=1 ;;
                 *) say "  [ OK ] enforcement confirmed: the 4 KB-aligned binary is refused here" ;; esac
else
    case "$C" in *DLOPEN-OK*) say "  [ OK ] 4 KB-aligned binary runs on a 4 KB kernel, as it must" ;;
                 *) say "  [FAIL] a 4 KB-aligned binary failed on a 4 KB kernel — the control is broken"; FAIL=1 ;; esac
fi
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
