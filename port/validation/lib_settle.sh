# lib_settle.sh — shared frame-based settle for the emulator playthrough scripts.
#
# WHY THIS EXISTS
# ---------------
# The scripts used to wait a FIXED number of seconds after the last slingshot drag and then
# screenshot. Under SwiftShader the frame rate varies enormously between runs — measured
# anywhere from 1.79 to ~15 fps on the same script and the same host — so a `sleep 16` is
# ~29 frames on a bad run and ~250 on a good one.
#
# That made the tests FLAKY IN THE WORST DIRECTION: on 2026-07-27 09:29 an API-34 run reported
# every log metric green (h_fatal=0, guards firing, process alive) while its screenshot showed a
# level still in progress — the third bird had not landed yet. Nothing in the log said so,
# because no abshim log marker distinguishes a win. That run is why PROOF_8's source could not
# be regenerated.
#
# Waiting on FRAMES instead of wall-clock makes the settle adapt to whatever rate the host
# happens to deliver, and makes the failure LOUD when it cannot be met.
#
# USAGE
#   source "$(dirname "$0")/lib_settle.sh"
#   settle_frames "$ABLOG" 120 300      # advance +120 frames, give up after 300s
#
# Returns 0 if the frame target was reached, 1 if it timed out (and says so on stdout).

settle_frames(){                        # $1=abshim log  $2=frames (default 120)  $3=cap secs (default 300)
    local log="$1" want="${2:-120}" cap="${3:-300}"
    local f0 fn waited=0
    f0=$(grep -aoE 'frame\[[0-9]+\]' "$log" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
    f0=${f0:-0}
    echo "  settle: at frame[$f0], waiting for +$want frames (cap ${cap}s)"
    while [ "$waited" -lt "$cap" ]; do
        sleep 5; waited=$((waited+5))
        fn=$(grep -aoE 'frame\[[0-9]+\]' "$log" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
        fn=${fn:-0}
        if [ "$fn" -ge $((f0+want)) ]; then
            echo "  settle: reached frame[$fn] (+$((fn-f0))) after ~${waited}s"
            return 0
        fi
    done
    fn=$(grep -aoE 'frame\[[0-9]+\]' "$log" 2>/dev/null | tail -1 | grep -oE '[0-9]+'); fn=${fn:-0}
    echo "  settle: WARNING — only reached frame[$fn] (+$((fn-f0)) of +$want) in ${cap}s;"
    echo "  settle: the frame rate is very low, so any screenshot below may be captured EARLY."
    return 1
}
