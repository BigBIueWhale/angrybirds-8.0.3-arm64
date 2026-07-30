#!/bin/bash
# emu_progression.sh — how many DISTINCT levels can it actually get through?
#
# WHY THIS EXISTS
# ---------------
# Multi-level progression is evidenced at exactly one step: PROOF_9 / progR_2_level2.png show a
# second level loading after a win. Nothing goes further, so "advances into the next level" is
# proven once and then generalised.
#
# emu_soak.sh made the gap concrete rather than theoretical. Twenty minutes of blind taps and swipes
# kept the renderer busy for 17 401 frames — and opened TWO distinct level files, both under
# data/levels/0_Tutorial/. It replayed the tutorial. Frames advancing is not levels advancing, and
# only a script that drives the win -> NEXT cycle deliberately can tell them apart.
#
# WHY THE FIRST VERSION FAILED — worth keeping, because the wrong lesson was nearly recorded.
# Five cycles produced ONE win and TWO distinct levels, and the obvious reading was "fixed
# coordinates cannot aim, so they cannot clear arbitrary levels". The SCREENSHOTS say otherwise:
# prog_5.png is empty sky with SCORE 0 and a live pause button. In this game a drag on empty space
# PANS THE CAMERA. Cycle 1's drag landed on the bird because the tutorial opens with the slingshot
# under it; once level 2 loaded, the same drag missed, panned the view right, and every later cycle
# panned further until the slingshot was off-screen entirely. Nothing to do with aiming and nothing
# to do with the port — the harness had scrolled away from the game.
#
# So each cycle now RESETS THE CAMERA first (rightward swipes pan back left; the camera clamps at
# the level edge, so over-panning is safe) and captures a pre-shot frame, which is what makes the
# difference between "the drag missed" and "the drag was nowhere near the level" visible next time.
#
# That capture immediately earned itself. With the camera fixed, the run STILL won only once — and
# prog_2_pre.png shows why: level 2, framed correctly, slingshot and loaded bird at (152,183), two
# spares behind it, pig towers to the right. The harness was dragging from (207,118), open sky well
# above the slingshot. The "slingshot drag" every script in this project shares is a fixed screen
# coordinate that happens to sit on the tutorial's bird; nothing holds it there when the level
# changes. So the anchor is now FOUND per shot (find_bird.py) instead of assumed, and the proven
# pull vector is applied relative to it.
#
# WHAT IT MEASURES
#   distinct data/levels/<episode>/<name> files opened  — the honest progression metric, straight
#                                                         from the asset bridge rather than inferred
#   wins confirmed from pixels                          — lib_wincheck, per cycle
#   h_fatal                                             — must stay 0 across every level transition
#
# The level-end transition is where this port's deepest bug lived (a session-long std::string UAF
# that killed the process on the results screen), so repeating it N times is a direct regression
# test of the thing that was hardest to fix.
#
#   docker run --rm --network none --device /dev/kvm --group-add "$(getent group kvm|cut -d: -f3)" \
#       -v "$PWD":/work ab-emu-34 bash /work/port/validation/emu_progression.sh
#
# LEVELS=8 tries more cycles. Default 5 keeps a full run near 15 minutes.
set +e
source "$(dirname "$0")/lib_settle.sh"
# Refuse to run while another evidence run is in flight: 50 scripts share reports/shots and
# two concurrent runs BLEND their logs into numbers that look normal and mean nothing.
source "$(dirname "$0")/lib_runlock.sh"
acquire_run_lock "$(basename "$0")" || exit 1
source "$(dirname "$0")/lib_dialogs.sh"
source "$(dirname "$0")/lib_metrics.sh"
source "$(dirname "$0")/lib_install.sh"
source "$(dirname "$0")/lib_wincheck.sh"
source "$(dirname "$0")/lib_selfhash.sh"
source "$(dirname "$0")/lib_camera.sh"

APK=/work/out/angrybirds-8.0.3-x86shim-release.apk
PKG=com.rovio.angrybirds
WANT="${LEVELS:-5}"
OUT=/work/reports/shots; mkdir -p "$OUT"; LOG="$OUT/progression.txt"; : >"$LOG"
ABLOG="$OUT/progression_abshim.txt"; : >"$ABLOG"
say(){ echo "$@" | tee -a "$LOG"; }
selfhash_begin
FAIL=0

[ -f "$APK" ] || { say "[FAIL] missing $APK"; exit 1; }

say "== boot =="
emulator -avd "${ABSHIM_AVD:-abtest34}" -no-window -no-audio -no-boot-anim -no-snapshot -accel on \
    -gpu swiftshader_indirect >/tmp/emu.log 2>&1 &
adb wait-for-device
for i in $(seq 1 120); do [ "$(adb shell getprop sys.boot_completed 2>/dev/null|tr -d '\r')" = "1" ] && break; sleep 5; done
say "  android $(adb shell getprop ro.build.version.release 2>/dev/null|tr -d '\r') (API $(adb shell getprop ro.build.version.sdk 2>/dev/null|tr -d '\r'))"
adb shell settings put global airplane_mode_on 1 >/dev/null 2>&1

install_apk "$APK" 4 2>&1 | tee -a "$LOG"
adb shell pm list packages 2>/dev/null | grep -q rovio || { say "[FAIL] install"; adb emu kill; exit 1; }
adb logcat -c >/dev/null 2>&1; adb logcat -G 64M >/dev/null 2>&1
adb logcat -s abshim > "$ABLOG" 2>/dev/null &
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

for s in $(seq 1 110); do sleep 5
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$ABLOG" 2>/dev/null|tail -1|grep -oE '[0-9]+')
    [ -n "$fn" ] && [ "$fn" -ge 601 ] && { say "  rendering at ~$((s*5))s frame[$fn]"; break; }
done
dismiss_system_dialogs
adb shell input tap 390 266; sleep 4; adb shell input tap 390 266; sleep 10

levels_seen(){ grep -aoE 'data/levels/[A-Za-z0-9_]+/[A-Za-z0-9_]+' "$ABLOG" 2>/dev/null | sort -u | wc -l; }

say
say "== drive win -> NEXT, $WANT times =="
WINS=0
for n in $(seq 1 "$WANT"); do
    # Each shot: put the view back on the slingshot, SEE where the bird is, then drag from the bird.
    #
    # Both halves are needed and each was found by looking at a capture rather than at a log.
    # Without the pan the view walks off the level; with the pan but a fixed anchor, the drag starts
    # at (207,118) — which is where the tutorial's bird sits and where level 2 has open sky, three
    # centimetres above the slingshot. Five cycles of that produced one win and a confident, wrong
    # conclusion about aiming.
    SLING=""                       # the slingshot, latched per cycle — see below
    for shot in 1 2 3; do
        pan_and_capture "$OUT/prog_${n}_s${shot}.png"     # lib_camera.sh

        # 1. Stop shooting once the level is already won. Without this the harness keeps firing at
        # the results screen, and find_bird.py obligingly reports the win banner as a bird: cycle 1
        # shot 3 logged "bird seen at (242,145)", exactly the false positive that file's header
        # documents. Harmless to the game, but it writes a false line into the record.
        if win_check "$OUT/prog_${n}_s${shot}.png" >/dev/null 2>&1; then
            say "    cycle $n shot $shot: already won — skipping the remaining shots"
            break
        fi

        # 2. No bird visible almost always means an in-game tutorial CARD is covering the scene,
        # not that the bird is missing. Each new tutorial level opens with one, and prog_3_s3.png
        # caught level 3 (Tutorial_chuck_niko) sitting behind the "tap to launch" card — dimmed
        # scene, orange checkmark — while the harness shot at it and the cycle was scored a loss.
        # (390,266) is this rig's established dismiss point, used at launch by every playthrough
        # script. Tap it, look again, and only then give up.
        if ! python3 /work/port/validation/find_bird.py "$OUT/prog_${n}_s${shot}.png" >/dev/null 2>&1; then
            adb shell input tap 390 266 >/dev/null 2>&1; sleep 4
            adb exec-out screencap -p > "$OUT/prog_${n}_s${shot}.png" 2>/dev/null
            say "    cycle $n shot $shot: no bird visible — dismissed a card and looked again"
        fi

        BX=207; BY=118; AIM=fixed
        if B=$(python3 /work/port/validation/find_bird.py "$OUT/prog_${n}_s${shot}.png" 2>/dev/null); then
            BX=${B% *}; BY=${B#* }; AIM=seen
            # 3. LATCH the first good anchor of the cycle. The slingshot does not move within a
            # level and every bird loads onto the same spot, so re-finding per shot buys nothing and
            # can lose: cycle 2 shot 3 reported (236,213) while the slingshot was at (151,185) — it
            # had locked onto a spare bird walking up, or one still in flight. The finder returns the
            # highest bird-sized red blob and says nothing about what that blob IS, so a spare bird
            # is exactly as convincing to it as the loaded one.
            if [ -z "$SLING" ]; then
                SLING="$BX $BY"
            else
                LX=${SLING% *}; LY=${SLING#* }
                DIFF=$(( (BX-LX)*(BX-LX) + (BY-LY)*(BY-LY) ))
                if [ "$DIFF" -gt 1600 ]; then      # further than 40 px from the latched slingshot
                    say "    cycle $n shot $shot: found ($BX,$BY) far from the slingshot ($LX,$LY) — using the slingshot"
                    BX=$LX; BY=$LY; AIM=latched
                fi
            fi
        elif [ -n "$SLING" ]; then
            # Still nothing, but this cycle already knows where the slingshot is. Reusing that beats
            # falling back to a constant borrowed from a different level, which is the original bug.
            BX=${SLING% *}; BY=${SLING#* }; AIM=latched
        fi
        # The pull is the vector proven by PROOF_2..PROOF_9 (dx -97, dy +32), applied at wherever
        # the bird actually is instead of where it once was. Shots 2 and 3 fan the elevation a
        # little: three identical shots at a structure that survived the first are three wasted
        # birds, and this is a harness, not a player.
        case "$shot" in
            1) DX=-97; DY=32 ;;
            2) DX=-105; DY=48 ;;
            3) DX=-88;  DY=18 ;;
        esac
        adb shell input swipe "$BX" "$BY" "$((BX+DX))" "$((BY+DY))" 700 >/dev/null 2>&1
        say "    cycle $n shot $shot: bird ${AIM} at ($BX,$BY) -> pull ($((BX+DX)),$((BY+DY)))"
        sleep 8
    done
    settle_frames "$ABLOG" 200 200
    adb exec-out screencap -p > "$OUT/prog_${n}.png" 2>/dev/null
    win_check "$OUT/prog_${n}.png" >/dev/null 2>&1; WC=$?
    [ "$WC" -eq 0 ] && WINS=$((WINS+1))
    adb shell input tap 378 256 >/dev/null 2>&1; sleep 3
    adb shell input tap 378 256 >/dev/null 2>&1; sleep 10
    say "  cycle $n: win=$([ "$WC" -eq 0 ] && echo yes || echo no)  distinct levels so far: $(levels_seen)  h_fatal: $(grep -ac 'h_fatal' "$ABLOG")"
done

say
say "== RESULTS =="
NL=$(levels_seen)
say "  distinct level files opened: $NL"
grep -aoE 'data/levels/[A-Za-z0-9_]+/[A-Za-z0-9_]+' "$ABLOG" 2>/dev/null | sort -u | sed 's/^/    /' | tee -a "$LOG"
# Report the DENOMINATOR, always, straight from the APK. "N distinct levels" invites the reader to
# supply a ceiling, and three times running the ceiling supplied was wrong: 4 read as a limit, 5 was
# written up as "the whole 0_Tutorial episode", and the next run opened a sixth. 0_Tutorial actually
# holds 15 .lua files and the APK carries 20-plus further episodes. A number with no denominator is
# an invitation to guess one.
# apk_levels.py, NOT an inline zipfile snippet. The first version of this used zipfile and printed
# NOTHING in the emulator image, whose python3 is stripped to the point that `import zipfile` fails
# with ModuleNotFoundError: No module named 'shutil'. The run still reported success — an addition
# that silently did nothing. It parses the zip central directory with struct alone now, cross-checked
# against zipfile on the host (4397 entries, identical), and verified to run in the stripped python.
python3 /work/port/validation/apk_levels.py "$APK" "$ABLOG" 2>/dev/null | tee -a "$LOG" \
    || say "  (could not read level counts from the APK — denominator NOT reported)"
say "  wins confirmed from pixels: $WINS of $WANT cycles"
say "  h_fatal: $(h_fatal_report "$ABLOG")"
PID=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')
say "  alive at the end: ${PID:-<no>}"

if [ "$NL" -ge 3 ]; then
    say "  [ OK ] reached $NL distinct levels — beyond the single step PROOF_9 established"
else
    say "  [FAIL] only $NL distinct level file(s): no more than blind input already achieves, so this"
    say "         run does not demonstrate progression beyond what PROOF_9 already shows."
    say "         Check reports/shots/prog_*_pre.png FIRST — if they show sky rather than a"
    say "         slingshot, the camera reset is what failed, not the game."
    FAIL=1
fi
[ -n "$PID" ] || { say "  [FAIL] the process died during progression"; FAIL=1; }
HF=$(grep -ac 'h_fatal' "$ABLOG"); [ "${HF:-0}" -eq 0 ] || { say "  [FAIL] h_fatal during a level transition"; FAIL=1; }
# h_fatal alone is not a health signal — see assert_no_absorbed_faults in lib_metrics.sh (R41).
assert_no_absorbed_faults "${ABLOG}" || FAIL=1

say
# selfhash_verify RETURNS a verdict (0 unchanged / 1 edited mid-run / 2 cannot tell). It was
# called and discarded here, so a run whose script changed underneath it printed
# "*** DISCARD THESE RESULTS ***" and still exited 0 — the one failure that invalidates every
# other line above it.
selfhash_verify; [ $? -eq 0 ] || FAIL=$((FAIL+1))
say "DONE (FAIL=$FAIL)"
adb emu kill >/dev/null 2>&1
exit "$FAIL"
