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

# RESOLUTION. The literal gesture below (80,150)->(600,150) is in the EMULATOR's 640x320 space, which
# is every capture from PROOF_2 to PROOF_21. On the physical A56 the same numbers are a short scratch in
# the top-left corner: they still pan, because any horizontal drag on empty ground pans, but they move
# the view a fraction of a screen per sweep, and four sweeps do not reach the wall.
#
# So the gesture is SCALED to the real screen, with the emulator basis preserved exactly: a 640x320
# device (or a device this cannot query) gets the original literals byte for byte, so no existing PROOF
# changes. Anything else is scaled from that basis.
#
# Geometry is queried once and cached — `wm size` per sweep would be four adb round-trips per reset.
_cam_geom=""
_cam_swipe_xy() {
    local x1=80 y1=150 x2=600 y2=150
    if [ -z "$_cam_geom" ]; then
        _cam_geom=$(adb shell wm size 2>/dev/null | grep -aoE '[0-9]+x[0-9]+' | tail -1)
        [ -z "$_cam_geom" ] && _cam_geom=none
    fi
    case "$_cam_geom" in
        none|640x320) ;;                     # emulator basis, or unknown: literals unchanged
        *)  local w h t
            w=${_cam_geom%x*}; h=${_cam_geom#*x}
            # This game runs LANDSCAPE while `wm size` reports the panel portrait (1080x2340), so the
            # long edge is X. Without this swap the sweep would be scaled by the wrong axis.
            if [ "${w:-0}" -lt "${h:-0}" ]; then t=$w; w=$h; h=$t; fi
            x1=$(( 80 * w / 640 )); x2=$(( 600 * w / 640 ))
            y1=$(( 150 * h / 320 )); y2=$y1 ;;
    esac
    printf '%s %s %s %s' "$x1" "$y1" "$x2" "$y2"
}

# Pan the view back to the level's left edge, where the slingshot is.
#   $1 = sweeps (default 4)   $2 = settle seconds after the last sweep (default 3)
pan_to_slingshot() {
    local sweeps="${1:-4}" settle="${2:-3}" i
    local XY; XY=$(_cam_swipe_xy)
    for i in $(seq 1 "$sweeps"); do
        # HORIZONTAL: finger right -> world right -> view moves LEFT, toward the launch point.
        # Literal emulator basis, for the record and for the verify_claims camera check:
        #     input swipe 80 150 600 150
        adb shell input swipe $XY 400 >/dev/null 2>&1
        # NO VERTICAL SWEEP — tried, measured, reverted.
        #
        # The reasoning for one was sound: the shot gesture drags the finger left AND DOWN (dy +32),
        # so a miss pushes the view right and UP, and a purely horizontal reset cannot undo that.
        # prog_16_s2.png (a PRE-shot frame) really is a screenful of cloud. So a vertical sweep
        # 300,270 -> 300,60 was added.
        #
        # It made things WORSE, measured: 4 levels in 14 cycles, against 5 in 10 and 6 in 18 with the
        # horizontal reset alone. The capture says why — prog_12_s1.png is the EPISODE SELECT screen.
        # A long vertical drag does not pan this game's camera, it navigates out of the level. The
        # harness spent the back half of that run in menus.
        #
        # Kept as a comment rather than deleted: the vertical drift is real and still unhandled, and
        # the next person to notice it will reach for exactly this fix.
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
