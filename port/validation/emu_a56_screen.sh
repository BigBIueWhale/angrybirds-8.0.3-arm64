#!/bin/bash
# emu_a56_screen.sh — run at the A56's ACTUAL screen geometry, because that is one of the very few
# device attributes this host can genuinely reproduce.
#
# WHY THIS EXISTS
# ---------------
# Every screenshot in this repo is 640x320 — a 2.0 aspect at a resolution no phone has had in a
# decade. The Galaxy A56 is 1080x2340 (2.167) at ~385 dpi. Three separate things were argued from
# the small screen rather than measured at the real one:
#
#   * that the game is not letterboxed. `resizeableActivity="false"` is declared in the manifest
#     (Rovio's, unmodified), and apps targeting < API 26 get a 1.86 max-aspect cap; this APK targets
#     26 precisely so that cap does not apply. That is a reading of the platform rules, not a result.
#   * that screen size cannot route the engine to unexercised art (OPEN_FINDINGS R11). There is only
#     one gameplay asset tier, but the SPLASH tier is chosen by aspect and the 1024x768_splash set
#     has never been loaded in any run.
#   * that the largest texture uploaded is 2000x1991. A 3.7x larger framebuffer is exactly the sort
#     of thing that could change a render-target allocation.
#
# The emulator can be told to use an arbitrary skin, so all three become measurable here instead of
# waiting for the device. What this still does NOT cover is the driver — SwiftShader, not
# Mali/Xclipse (R9/R11).
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_a56_screen.sh
set +e
source "$(dirname "$0")/lib_settle.sh"
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_selfhash.sh"
source "$(dirname "$0")/lib_wincheck.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-shaders.apk     # the dumps build: needed for [tex-dim]
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/a56screen.txt"; : >"$LOG"
ABLOG="$OUT/a56screen_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0
W=1080; H=2340; DPI=${A56_DPI:-420}                     # A56: 1080x2340, ~385ppi; 420 is the nearest bucket

[ -f "$APK" ] || { say "[FAIL] missing $APK — build with port/build_apk_x86_shaders.sh"; exit 1; }
say "  measuring: $(basename "$APK")  sha256 $(sha256sum "$APK" | cut -c1-16)…"
say "  target geometry: ${W}x${H} @ ${DPI}dpi  (Galaxy A56, aspect $(awk -v w=$W -v h=$H 'BEGIN{printf "%.3f", h/w}'))"

say "== boot at the A56's geometry =="
emulator -avd "${ABSHIM_AVD:-abtest34}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect -skin "${W}x${H}" >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
# `-skin WxH` already sets the display; the first version ALSO forced `wm size`/`wm density` here and
# that was the bug. Both trigger a display reconfiguration, and installing + launching into it meant
# the app never started at all: the shim log came back empty and the screenshot was the notification
# shade. `wm size` also answered with an EMPTY string while the reconfiguration was in flight, so the
# confirmation check reported "the resolution did not take" about a device that was, in fact, already
# 1080x2340 — the screenshot proved it. Set nothing; poll until the display settles and report it.
SZ=""; for i in $(seq 1 30); do
    SZ=$(adb shell wm size 2>/dev/null | tr -d '\r')
    case "$SZ" in *"${W}x${H}"*) break ;; esac
    sleep 2
done
# DENSITY IS NOT COSMETIC HERE. The first version of this script left the AVD's 160, and at
# 1080x2340 that makes smallestWidth 1080dp — which Android classifies as an XLARGE TABLET. This APK
# ships res/drawable-large-* and res/drawable-xlarge-* buckets, so that run silently exercised the
# TABLET resource path, not the phone path the A56 takes. "Ran at the A56's geometry" was true of the
# pixels and false of the configuration. At 420 (the bucket for the A56's ~385ppi) smallestWidth is
# 411dp — a normal phone.
#
# Set it AFTER the resolution has settled and poll until it takes: doing this at boot, together with
# a wm size, is what previously stopped the app from launching at all.
adb shell wm density "$DPI" >/dev/null 2>&1
DN=""; for i in $(seq 1 30); do
    DN=$(adb shell wm density 2>/dev/null | tr -d '\r')
    case "$DN" in *"Override density: $DPI"*|*"Physical density: $DPI"*) break ;; esac
    sleep 2
done
sleep 5                                            # let the reconfiguration finish before installing
say "  ${SZ:-<wm size returned nothing>}"; say "  ${DN:-<wm density returned nothing>}"
SWDP=$(awk -v w="$W" -v d="$DPI" 'BEGIN{printf "%d", w/(d/160)}')
say "  smallestWidth ${SWDP}dp -> $([ "$SWDP" -ge 600 ] && echo 'TABLET resource bucket (WRONG for the A56)' || echo 'phone resource bucket (what the A56 uses)')"
case "$DN" in
    *"$DPI"*) say "  [ OK ] density is $DPI, so the phone resource bucket is the one being exercised" ;;
    *) say "  [FAIL] density never became $DPI — this would test the tablet configuration instead"
       FAIL=1 ;;
esac
case "$SZ" in
    *"${W}x${H}"*) say "  [ OK ] the device really is at the A56's resolution" ;;
    *) say "  [FAIL] the display never settled at ${W}x${H} — everything below would describe the"
       say "         wrong screen, so this run proves nothing about the A56's geometry."
       FAIL=1 ;;
esac
# Boot leaves a notification shade / dialogs over the launcher on this image; collapse before
# launching, or `monkey` starts the game behind them and the taps below hit the wrong window.
adb shell cmd statusbar collapse >/dev/null 2>&1
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1

adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1
install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p com.rovio.angrybirds -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

say "== play =="
for s in $(seq 1 130); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
# Taps are in DEVICE pixels, so the usual 390,266 is meaningless here. The game runs LANDSCAPE on a
# portrait panel, so the touch surface is W_l x H_l = H x W (2340x1080) — getting that backwards
# sends every tap to the wrong half of the screen.
#
# Driving it to a WIN at this geometry is the point, not decoration: touch events reach the engine
# through the shim's JNI path, and nothing had ever exercised that mapping at anything other than
# 640x320. A successful slingshot drag here IS the test of it.
#
# Fractions read off PROOF_21 rather than guessed: the tutorial card's confirm button sits at
# ~(0.526, 0.657) of the landscape frame, and the slingshot in the level behind it is left-of-centre
# and low.
WL=$H; HL=$W                                     # landscape touch surface
tapf(){ adb shell input tap $(awk -v a="$1" -v b="$WL" 'BEGIN{printf "%d", a*b}') \
                            $(awk -v a="$2" -v b="$HL" 'BEGIN{printf "%d", a*b}') >/dev/null 2>&1; }
dragf(){ adb shell input swipe $(awk -v a="$1" -v b="$WL" 'BEGIN{printf "%d", a*b}') \
                               $(awk -v a="$2" -v b="$HL" 'BEGIN{printf "%d", a*b}') \
                               $(awk -v a="$3" -v b="$WL" 'BEGIN{printf "%d", a*b}') \
                               $(awk -v a="$4" -v b="$HL" 'BEGIN{printf "%d", a*b}') 700 >/dev/null 2>&1; }
say "  touch surface: ${WL}x${HL} (landscape)"
# Fractions taken from the coordinates that RELIABLY win on the 640x320 rig, not estimated off a
# screenshot. The first attempt eyeballed them from PROOF_21 and put the drag at y=0.58 when the
# working rig drags at y=0.369 — far too low, so it grabbed nothing and the run never scored.
#   tap  (390,266)/640x320 -> (0.609, 0.831)
#   drag (207,118)->(110,150) -> (0.323,0.369) -> (0.172,0.469)
# Card timing also differs at this resolution (everything is slower), so the confirm tap is repeated
# rather than fired twice and hoped for.
for i in 1 2 3 4 5; do tapf 0.609 0.831; sleep 5; done
dismiss_system_dialogs                           # the fullscreen notice can reappear after a card
for i in 1 2 3 4; do dragf 0.323 0.369 0.172 0.469; sleep 9; done
settle_frames "$ABLOG" 300 300

say
say "== RESULTS =="
say "  h_fatal: $(h_fatal_report "$ABLOG")"
FN=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
if [ -n "$FN" ] && [ "$FN" -ge 601 ]; then
    say "  [ OK ] renders at the A56's geometry (frame[$FN])"
else
    say "  [FAIL] did not reach frame[601] at this geometry"; FAIL=1
fi

say
say "== letterboxing =="
adb shell screencap -p /sdcard/a56.png >/dev/null 2>&1
adb pull /sdcard/a56.png "$OUT/a56_screen.png" >/dev/null 2>&1
DIMS=$(python3 - "$OUT/a56_screen.png" <<'PY'
import sys, struct
d=open(sys.argv[1],'rb').read(33)
print("%dx%d" % struct.unpack('>II', d[16:24]) if d[:8]==b'\x89PNG\r\n\x1a\n' else "?")
PY
)
# Angry Birds is a LANDSCAPE game, so on a portrait 1080x2340 panel the captured frame is
# 2340x1080. That transposition is the correct outcome, not a mismatch — an earlier version printed
# "(expected 1080x2340)" beside it, which reads as a failure and is not one.
case "$DIMS" in
    "${W}x${H}"|"${H}x${W}") say "  screenshot is $DIMS — the panel is ${W}x${H}; landscape game, so a transposed capture is expected" ;;
    *) say "  [FAIL] screenshot is $DIMS, which is neither orientation of ${W}x${H} — wrong screen"; FAIL=1 ;;
esac
# A letterboxed app leaves uniform bars top and bottom. Sample rows near the edges and the middle:
# if the top/bottom rows are a single flat colour while the middle is not, the app is not filling.
python3 - "$OUT/a56_screen.png" <<'PY' 2>&1 | tee -a /work/reports/shots/a56screen.txt
import sys, zlib, struct
p=sys.argv[1]; d=open(p,'rb').read()
# minimal PNG decode (RGBA/RGB, non-interlaced) — enough to sample rows
pos=8; w=h=bd=ct=None; idat=b''
while pos < len(d):
    ln=struct.unpack('>I', d[pos:pos+4])[0]; typ=d[pos+4:pos+8]; data=d[pos+8:pos+8+ln]; pos+=12+ln
    if typ==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB', data[:10])
    elif typ==b'IDAT': idat+=data
raw=zlib.decompress(idat); ch={0:1,2:3,4:2,6:4}[ct]; stride=w*ch
# ONE pass. An earlier version re-decoded from row 0 for every sample, which at 1080x2340 is ~11M
# Python iterations for three rows — slow enough to look like a hang in the middle of a test run.
want={4: None, h//2: None, h-5: None}
prev=bytearray(stride); off=0
for y in range(h):
    f=raw[off]; line=bytearray(raw[off+1:off+1+stride]); off+=1+stride
    if f:
        for x in range(stride):
            a=line[x-ch] if x>=ch else 0; b=prev[x]; c=prev[x-ch] if x>=ch else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+((a+b)>>1))&255
            else:
                pp=a+b-c; pa=abs(pp-a); pb=abs(pp-b); pc=abs(pp-c)
                pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[x]=(line[x]+pr)&255
    if y in want: want[y]=bytes(line)
    prev=line
def flat(y):
    r=want[y]; px={r[i:i+ch] for i in range(0, stride, ch*17)}
    return len(px)<=1
top, mid, bot = flat(4), flat(h//2), flat(h-5)
print(f"    row 4 flat={top}   row {h//2} flat={mid}   row {h-5} flat={bot}")
if top and bot and not mid:
    print("    [WARN] uniform bars top AND bottom with content in the middle -> looks LETTERBOXED")
else:
    print("    [ OK ] no uniform top+bottom bar pattern -> the app is filling the screen")
PY

say
say "  largest texture uploaded at this geometry:"
grep -a '\[tex-dim\]' "$ABLOG" | tail -2 | sed 's/.*\[tex-dim\]/    [tex-dim]/' | tee -a "$LOG"
say "  splash tier opened (1024x768_splash is the one no 640x320 run ever loaded):"
grep -aoE '1024x[0-9]+_splash' "$ABLOG" | sort | uniq -c | sed 's/^/    /' | tee -a "$LOG"

say
say "== did the touch mapping actually work at this geometry? =="
win_check "$OUT/a56_screen.png"; WC=$?
case $WC in
  0) say "  [ OK ] WIN at 1080x2340 — slingshot drags reached the engine through the shim" ;;
  1) say "  (no win screen; the run still proves render+geometry, not input mapping)" ;;
  *) say "  (win check could not run — reported as unknown, not as a pass)" ;;
esac

say "  NOTE: geometry only. The driver here is still SwiftShader, not the A56's Mali/Xclipse."
selfhash_verify
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
