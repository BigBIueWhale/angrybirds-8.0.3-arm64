# lib_selfhash.sh — make "this script was edited while it ran" a LOUD failure instead of an invisible one.
#
# WHY THIS EXISTS
# ---------------
# On 2026-07-28 an Android 16 playthrough logged its wait phase twice:
#
#     == wait for game render (frame[601]+) ==
#       card at ~180s frame[601]
#     == wait for game render (frame[601]+) ==
#       card at ~5s frame[601]
#
# from a script containing exactly ONE such block. Nothing was wrong with the script. The script had
# been EDITED WHILE A CONTAINER WAS EXECUTING IT: bash reads a script incrementally BY BYTE OFFSET,
# so inserting lines shifts those offsets under the running interpreter and it re-enters or skips a
# region. The run completed, printed a plausible RESULTS block, and reported a win.
#
# That is the dangerous part. The output looked normal. Only a doubled log line — noticed by chance —
# revealed that the program which produced those results matched no file on disk. Had the edit
# landed a few lines elsewhere, the run could have skipped the slingshot drags entirely and still
# printed a tidy summary.
#
# The rule (port/validation/README.md) is that a script is frozen while a container runs it. Rules
# get broken; this makes the breach detectable afterwards rather than depending on someone spotting
# a repeated line.
#
# USAGE — first line of work, and again before the results block:
#     source "$(dirname "$0")/lib_selfhash.sh"
#     selfhash_begin                 # records the hash and prints it
#     ...
#     selfhash_verify || say "  (results above are from a script that changed mid-run — discard them)"
#
# It reports rather than aborting: by the time a change is detected the run has already happened,
# and killing it would throw away logs that are useful for diagnosis. What matters is that the
# evidence is LABELLED untrustworthy, so it cannot be quoted later as if it were clean.

selfhash_begin() {
    _SELFHASH_FILE="${BASH_SOURCE[1]:-$0}"
    _SELFHASH_AT_START="$(sha256sum "$_SELFHASH_FILE" 2>/dev/null | cut -d' ' -f1)"
    if [ -z "$_SELFHASH_AT_START" ]; then
        say "  script identity: UNKNOWN (could not hash $_SELFHASH_FILE) — a mid-run edit would go undetected"
        return 2
    fi
    say "  script identity: ${_SELFHASH_AT_START:0:16}… ($(basename "$_SELFHASH_FILE"))"
    return 0
}

# Returns 0 if unchanged, 1 if it changed mid-run, 2 if it could not be checked.
selfhash_verify() {
    [ -n "$_SELFHASH_AT_START" ] || { say "  script identity: not recorded at start — cannot verify"; return 2; }
    local now; now="$(sha256sum "$_SELFHASH_FILE" 2>/dev/null | cut -d' ' -f1)"
    if [ -z "$now" ]; then
        say "  script identity: CANNOT RE-CHECK ($_SELFHASH_FILE unreadable now)"; return 2
    fi
    if [ "$now" = "$_SELFHASH_AT_START" ]; then
        say "  script identity: unchanged during the run (${now:0:16}…) — results are from this file"
        return 0
    fi
    say "  *** SCRIPT CHANGED WHILE RUNNING — DISCARD THESE RESULTS ***"
    say "      started as ${_SELFHASH_AT_START:0:16}…, is now ${now:0:16}…"
    say "      bash reads scripts by byte offset, so an edit mid-run can make it re-enter or skip"
    say "      whole sections. The output above may look normal and mean nothing. Re-run."
    return 1
}
