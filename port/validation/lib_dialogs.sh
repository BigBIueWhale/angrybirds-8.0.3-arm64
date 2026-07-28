# lib_dialogs.sh — dismiss the system dialogs that sit on top of the game, in the right order.
#
# WHY THIS EXISTS
# ---------------
# Android puts up to two modal system dialogs in front of this app, and they differ by OS version:
#
#   API 25 / 34 : "This app was built for an older version of Android"        [OK]
#   API 36      : the above, PLUS Android 16's immersive-mode notice
#                 "Viewing full screen - to exit, swipe down from the top"    [Got it]
#
# On Android 16 the fullscreen notice is drawn ON TOP, and while it is up it swallows every touch.
# A script that dismisses only the older-Android dialog sees frames climb, h_fatal=0, and no
# progress whatsoever - which looks exactly like a shim bug and is not one. That cost two wrong
# diagnoses before being pinned down: emu_modern_playthrough.sh first, then emu_modern_progress.sh
# hit the identical wall on API 36.
#
# Hence one implementation rather than a copy per script. The same reasoning as prepare_inputs.sh
# and lib_settle.sh: a fix pasted into several scripts is a fix that will drift in several scripts.
#
# Usage, after the app is launched and has had a moment to draw:
#     source "$(dirname "$0")/lib_dialogs.sh"
#     dismiss_system_dialogs "$OUT/${PFX}_1b_afterdialogs.png"     # screenshot arg optional
#
# Taps are top-down and repeated. Extra taps are harmless on OS versions where a given dialog does
# not exist - they land on the game's background - and that is much cheaper than trying to detect
# which OS put what on screen.

# Find a button by its LABEL and tap its centre, whatever the screen size.
#
# The fixed taps below are correct for the default AVD and meaningless anywhere else. That stopped
# being hypothetical with emu_a56_screen.sh, which runs at the A56's real 1080x2340: the API 36
# fullscreen notice appeared, (510,190) missed its "Got it" entirely, the dialog kept swallowing
# every touch, and the run produced a perfectly healthy-looking log — frames climbing, h_fatal=0 —
# with the game stuck on its first card. Exactly the wrong diagnosis this file was written to
# prevent, reappearing at a resolution it did not anticipate.
#
# Best-effort and non-fatal: if uiautomator is unavailable or the dialog is not on screen, this
# returns non-zero and the fixed taps still run, so every existing caller behaves as before.
_tap_button_labelled() {              # $1 = exact label text, e.g. "Got it"
    local xml b nums x y
    adb shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 || return 1
    xml=$(adb shell cat /sdcard/ui.xml 2>/dev/null | tr -d '\r')
    [ -n "$xml" ] || return 1
    # One node per line, then the bounds of the node carrying that text: bounds="[x1,y1][x2,y2]"
    b=$(printf '%s' "$xml" | tr '<' '\n' | grep -F "text=\"$1\"" \
        | grep -oE 'bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"' | head -1)
    [ -n "$b" ] || return 1
    nums=$(printf '%s' "$b" | grep -oE '[0-9]+' | tr '\n' ' ')
    set -- $nums
    [ $# -eq 4 ] || return 1
    x=$(( ($1 + $3) / 2 )); y=$(( ($2 + $4) / 2 ))
    adb shell input tap "$x" "$y" >/dev/null 2>&1
    return 0
}

dismiss_system_dialogs() {
    local shot="$1"
    local pass lbl
    # Resolution-independent pass first. Labels are tried in the order they stack, topmost first.
    for lbl in "Got it" "GOT IT" "Got It" "OK" "Ok"; do
        _tap_button_labelled "$lbl" && sleep 1
    done
    for pass in 1 2; do
        adb shell input tap 510 190 >/dev/null 2>&1; sleep 1   # "Got it" — API 36 fullscreen notice (topmost)
        adb shell input tap 468 236 >/dev/null 2>&1; sleep 1   # "OK"     — older-Android notice, API 36 layout
        adb shell input tap 490 237 >/dev/null 2>&1; sleep 1   # "OK"     — older-Android notice, API 25/34 layout
    done
    sleep 2
    [ -n "$shot" ] && adb exec-out screencap -p > "$shot" 2>/dev/null
    return 0
}
