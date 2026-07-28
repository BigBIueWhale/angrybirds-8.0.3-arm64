# lib_metrics.sh — report emulator metrics without letting "nothing was measured" read as "clean".
#
# WHY THIS EXISTS
# ---------------
# `h_fatal` is the metric this project cites more than any other: it counts `[h_fatal]` lines in the
# shim's log, and 0 means the shim never hit a fatal path. Every script reported it as
#
#     h_fatal:  0   (0 = no crash)
#
# computed with `grep -ac '\[h_fatal\]' "$ABLOG"`. If the app never started, or logcat did not
# attach, or the tag filter matched nothing, that grep also returns 0 — and the line then claims a
# clean run on the strength of an empty file. The same shape has now been found four times in this
# project (a check printing [FAIL] and exiting 0; a layer-4 test passing while the app had been
# killed 11 ms in; three counting checks passing on unreadable inputs; and this).
#
# So the log's own size is checked first, and the metric refuses to report a number it cannot back:
#
#     h_fatal:  0   (0 = no crash; 41207 shim log lines, so this was measured)
#     h_fatal:  NOT MEASURED — the shim log is empty, so 0 here would mean nothing
#
# Usage:
#     source "$(dirname "$0")/lib_metrics.sh"
#     say "  h_fatal:  $(h_fatal_report "$ABLOG")"

# Print an h_fatal count, or an explicit refusal if the log is empty.
h_fatal_report() {
    local log="$1"
    if [ ! -s "$log" ]; then
        printf 'NOT MEASURED — the shim log is empty, so 0 here would mean nothing'
        return 0
    fi
    printf '%s  (0 = no crash; %s shim log lines, so this was measured)' \
        "$(grep -ac '\[h_fatal\]' "$log" 2>/dev/null)" "$(wc -l < "$log")"
}

# Same idea for any other counted marker: a zero is only meaningful if the log exists.
# Uses -E so both plain and extended patterns work through one helper.
marker_report() {
    local log="$1" pat="$2"
    if [ ! -s "$log" ]; then printf 'NOT MEASURED (empty log)'; return 0; fi
    printf '%s' "$(grep -acE "$pat" "$log" 2>/dev/null)"
}
