# lib_wincheck.sh — score the level-end screen, and NEVER confuse "cannot check" with "not a win".
#
# WHY THIS EXISTS
# ---------------
# The first version of this check ran `python3 win_detect.py` inline in the playthrough scripts. The
# emulator images have no python3, so it printed
#
#     win check:  NOT a win screen (see reasons) -> modplay_3_end.png
#                 python3: command not found
#
# on a run that HAD won — scored [ WIN ] gold=0.0572 dark=0.5241 lum=57.9 the moment the same image
# was fed to the same detector on a host that has an interpreter. A missing tool was reported as a
# statement about the game. That is the defect this project keeps finding, in the code written to
# hunt it, and in the cry-wolf direction: a false failure teaches people to ignore the check.
#
# So there are THREE outcomes here, never two:
#   WIN CONFIRMED   the detector ran and said yes
#   NOT a win       the detector ran and said no, with the failing criterion
#   COULD NOT CHECK no interpreter — says so, and prints the exact command to score it elsewhere
#
# The screenshot is always written, so a deferred check is a deferral, not a loss of evidence.
# A library must not assume its caller defines say(). Two scripts (emu_playthrough.sh,
# emu_playthrough_release.sh) log with inline `echo ... | tee -a "$LOG"` and define no say() at all,
# so wiring win_check() into them produced "say: command not found" and the check's entire output —
# including the reason on failure — vanished while the function still returned a status nobody saw.
# A check whose verdict is invisible is worse than no check: it looks wired up.
_wc_say() {
    # NOT `echo "$@" ${LOG:+| tee -a "$LOG"}` — a `|` produced by a parameter expansion is a literal
    # word, not a pipe operator, so that printed the text followed by "| tee -a /path". Word-splitting
    # cannot create shell syntax; the branch has to be written out.
    if declare -F say >/dev/null 2>&1; then say "$@"
    elif [ -n "$LOG" ]; then echo "$@" | tee -a "$LOG"
    else echo "$@"; fi
}

win_check() {                     # $1 = screenshot path
    local shot="$1"
    local det="$(dirname "${BASH_SOURCE[0]}")/win_detect.py"
    if ! command -v python3 >/dev/null 2>&1; then
        _wc_say "  win check:  COULD NOT CHECK — no python3 in this image (this is NOT a verdict on the game)"
        _wc_say "              score it with: python3 port/validation/win_detect.py ${shot#/work/}"
        return 2
    fi
    if [ ! -s "$shot" ]; then
        _wc_say "  win check:  COULD NOT CHECK — $shot is missing or empty"
        return 2
    fi
    local out rc
    out=$(python3 "$det" "$shot" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then _wc_say "  win check:  WIN CONFIRMED from pixels"
    else                    _wc_say "  win check:  NOT a win screen — reasons below"; fi
    while IFS= read -r _wl; do _wc_say "              $_wl"; done <<< "$out"
    return "$rc"
}
