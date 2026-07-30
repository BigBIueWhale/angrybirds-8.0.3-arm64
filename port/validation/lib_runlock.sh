# lib_runlock.sh — refuse to run while another evidence-producing run is in flight.
#
# WHY THIS EXISTS
# --------------
# 50 scripts in this directory write into the SHARED reports/shots namespace, and not one of them held
# any kind of lock. Two emulator runs launched minutes apart therefore both wrote
# reports/shots/playthrough_abshim.txt, and the numbers I read out of it were a BLEND of two different
# builds -- which is how a conclusion (R61's "no async stop, MASK=0x1F still faults") got recorded from
# evidence that may never have existed. `docker ps` showed ab-emu-sh and ab-emu-fin up at the same
# time; nothing else did.
#
# This is worse than a wrong number, because a blended log looks completely normal: correct format,
# plausible values, monotonic-looking frames. There is no way to tell after the fact.
#
# USAGE — first line of work in any script that writes to reports/shots:
#     source "$(dirname "$0")/lib_runlock.sh"
#     acquire_run_lock "$(basename "$0")"      # exits non-zero if another run holds it
#
# Deliberately NOT a wait: a queued second run would still be a surprise. Refusing loudly makes the
# operator (me) serialise on purpose.
# THE LOCK MUST LIVE ON THE SHARED MOUNT, NOT IN /tmp.
#
# First version defaulted to /tmp/abshim-evidence-run.lock. Every one of these runs happens inside its
# own container, and each container has its OWN /tmp -- so two containers would both acquire happily
# and the guard would not have prevented the very contamination it was written for. Caught by noticing
# the lock reported "pid 1": that is a container init pid, i.e. the lock was container-local.
#
# /work is the repo bind-mount that every one of these scripts already relies on, so a lock there is
# visible to all of them and to the host. Falls back to /tmp only when /work is absent (host-side unit
# tests), where container-vs-container is not the risk.
if [ -d /work ]; then LOCKFILE="${ABSHIM_RUNLOCK:-/work/.abshim-evidence-run.lock}"
else                  LOCKFILE="${ABSHIM_RUNLOCK:-/tmp/abshim-evidence-run.lock}"; fi
acquire_run_lock() {
    local who="${1:-unknown}"
    command -v flock >/dev/null 2>&1 || { echo "  [runlock] flock unavailable — proceeding UNGUARDED ($who)"; return 0; }
    exec 9>"$LOCKFILE" 2>/dev/null || { echo "  [runlock] cannot open $LOCKFILE — proceeding UNGUARDED"; return 0; }
    if ! flock -n 9; then
        echo "  [runlock] REFUSING TO RUN: another evidence run holds $LOCKFILE"
        echo "             ($who). Two runs share reports/shots and would blend their logs;"
        echo "             the resulting numbers would look normal and be meaningless."
        echo "             Wait for it, or check: docker ps | grep ab-emu"
        return 1
    fi
    printf '%s pid=%s\n' "$who" "$$" >&9 2>/dev/null || true
    echo "  [runlock] held by $who (pid $$)"
    return 0
}
