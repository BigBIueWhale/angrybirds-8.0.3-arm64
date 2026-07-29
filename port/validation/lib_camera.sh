# lib_camera.sh — put the view back on the slingshot before trying to shoot.
#
# WHY THIS EXISTS
# ---------------
# Every driving script in this project shoots with the same fixed gesture:
#
#     adb shell input swipe 207 118 110 150 700
#
# which works, and is how PROOF_2..PROOF_9 were captured. It works because at level start the bird
# is sitting under (207,118). It is not a slingshot command — it is a drag at a screen location, and
# in this game a drag that does NOT start on the bird PANS THE CAMERA instead.
#
# So the gesture is self-defeating when repeated. emu_progression.sh drove five win->NEXT cycles and
# won exactly once; the natural reading was "fixed coordinates cannot aim, so they cannot clear
# arbitrary levels", and that explanation was written into the script before anyone looked at the
# captures. The captures said something else entirely: prog_5.png is a screenful of empty sky with
# SCORE 0 and a live pause button. Cycle 1's drag hit the bird; it won; level 2 loaded with the
# slingshot somewhere else; from then on every drag missed, panned the view a little further right,
# and the harness spent four cycles swiping at scenery. Nothing was wrong with the game.
#
# The bug is in the harness, and it is invisible from logs: frames advance, h_fatal stays 0, the
# process is healthy, the level file is open. Only a screenshot shows that the camera is nowhere
# near the level. That is why pan_to_slingshot() is paired with a pre-shot capture in the callers.
#
# HOW THE RESET WORKS
# A rightward finger swipe drags the world right, which moves the VIEW left, toward the launch point
# at the level's left edge. The camera clamps there, so over-panning costs nothing and there is no
# need to know how far it drifted — four sweeps put any reachable view back at the wall. Deliberately
# not a tap: a tap during play can launch the bird or hit UI.
#
# USAGE
#     source "$(dirname "$0")/lib_camera.sh"
#     pan_to_slingshot          # then shoot
#
# It lives here rather than being pasted into each caller because this project has already learned
# that lesson twice (see lib_install.sh's header: "same fix twice is a fix that will drift"). It was
# about to be pasted a third time.

# Pan the view back to the level's left edge, where the slingshot is.
#   $1 = sweeps (default 4)   $2 = settle seconds after the last sweep (default 3)
pan_to_slingshot() {
    local sweeps="${1:-4}" settle="${2:-3}" i
    for i in $(seq 1 "$sweeps"); do
        adb shell input swipe 80 150 600 150 400 >/dev/null 2>&1
        sleep 2
    done
    sleep "$settle"
}

# Pan back, then record what the harness is about to shoot at.
# The capture is the whole point: a failed cycle is ambiguous without it, because "the drag missed
# the bird" and "the view was pointed at empty sky" produce identical logs. With the image the two
# are one glance apart.
#   $1 = png path to write
pan_and_capture() {
    pan_to_slingshot
    [ -n "$1" ] && adb exec-out screencap -p > "$1" 2>/dev/null
}
