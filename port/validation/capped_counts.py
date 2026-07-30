#!/usr/bin/env python3
"""capped_counts.py — find log counts that are really log CAPS, before anybody cites one as evidence.

WHY
---
Twice in this project a number that came from a `printf` guard was written into the record as a
measurement:

  * `REPRODUCE.md` described galloc's targeted leak as "bounded, ~64 tiny `_Rep`s per run". 64 is
    where `galloc.c` stops printing (`n++<64`). Measured properly the leak is not bounded at all —
    a sustained 7-16 distinct blocks per minute. (R42)
  * `ONDEVICE.md` said the audio mixer "fills ~8 buffers then blocks", from
    `nativeMixData ENABLED` appearing 8x. 8 is `am++<8` in `jni_entry.c`. All three audio logs show
    exactly 8, on two different API levels — which is what a cap looks like, not a buffer limit. (R43)

Both were found by reading code that happened to be nearby. The shim has thirteen distinct rate
limits, so this stops being a matter of noticing:

    2 3 4 8 12 14 16 20 24 40 64 220 400

A count that has reached its cap is a FLOOR, not a total, and must never be quoted as one. This
extracts every (marker, cap) pair from the shim source and reports which counts in which logs have
saturated.

WHAT THIS DELIBERATELY DOES NOT DO: cross-reference caps against the documentation automatically.
That was attempted twice and abandoned, and the reason is worth recording so it is not tried a third
time.

  * Attempt 1 keyed on the bracketed tag — `[audio]` became the keyword "audio" — and flagged six
    documents, all false: "audio" plus a digit 8 somewhere on the same line is not a coincidence
    worth reporting.
  * Attempt 2 required a longer distinctive literal AND a word-boundary match on the cap value. It
    still produced false hits, because `(?<![0-9])8(?![0-9])` matches the 8 in "angrybirds-**8**.0.3"
    and `64` matches "**64**-bit". These documents are full of version strings, sha256 digests and
    byte counts; number-proximity carries no signal in them.

The two real cases (R42's 64, R43's 8) were both found by reading code, and both are now corrected.
Finding a third that way is plausible; finding it by grepping numbers is not. The LOG audit below is
mechanical and reliable — a count that has reached its cap is a floor, full stop — so that is what
this tool does.

    python3 capped_counts.py reports/shots/*abshim*.txt          # audit real logs
    python3 capped_counts.py --list                              # just show the caps found in source
    python3 capped_counts.py --tags <log>...                     # TAG\tCOUNT\tCAP\tSITE, for scripts
    python3 capped_counts.py --markers                           # TAG\tCAP\tSITE\tFULL LITERAL
"""
import sys, os, re, glob

SHIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "shim", "src")

# `if(n++<64) LOG("[WAF] block@..."` and friends. The guard and the LOG are on one line throughout
# this codebase, which is what makes a regex adequate here rather than a parser.
GUARD = re.compile(r"""\+\+\s*<\s*(\d+)\s*\)\s*                 # the cap
                       (?:LOG|LOGE|LOGW|rl)\s*\(                 # the logging call
                       (?:\s*\d+\s*,\s*"[^"]*"\s*,\s*)?          # optional (prio,"tag",) for rl()
                       "([^"]{4,})"                              # the format string
                    """, re.X)


def markers():
    """[(cap, marker, file, line)] — marker is the literal prefix of the format string, i.e. the part
    a grep can actually match, stopping at the first % conversion."""
    out = []
    for path in sorted(glob.glob(os.path.join(SHIM, "*.c"))):
        for i, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
            m = GUARD.search(line)
            if not m:
                continue
            cap, fmt = int(m.group(1)), m.group(2)
            lit = fmt.split("%")[0].strip()
            if len(lit) < 4:            # nothing greppable before the first conversion
                continue
            out.append((cap, lit, os.path.basename(path), i))
    return out


def main(argv):
    listing = "--list" in argv
    logs = [a for a in argv if not a.startswith("--")]
    caps = markers()
    if not caps:
        print("  [FAIL] no capped log sites found in the shim source — the regex has stopped matching,"
              "\n         which would make this check silently vacuous", file=sys.stderr)
        return 1
    # --markers: TAG<TAB>CAP<TAB>SITE<TAB>FULL LITERAL. The literal is last and untruncated so that a
    # consumer can `cut -f4` it regardless of the spaces and quotes it contains.
    #
    # This exists because test_capped.sh first hardcoded marker strings copied from `--list`, which
    # truncates to 70 characters for display. The copies were therefore PREFIXES of the real format
    # strings, matched nothing, and two of the tests passed vacuously — they were negative assertions
    # ("this must not be reported") against a log the tool could not see at all. A test fixture must
    # come from the tool, not from its display output.
    if "--markers" in argv:
        for cap, lit, f, ln in sorted(caps):
            m = re.match(r"(\[[^\]]+\])", lit)
            print(f"{m.group(1) if m else lit[:24]}\t{cap}\t{f}:{ln}\t{lit}")
        return 0

    if listing or not logs:
        print(f"  {len(caps)} capped log site(s) in the shim:")
        for cap, lit, f, ln in sorted(caps):
            print(f"    cap {cap:>4}  {f}:{ln}  {lit[:70]!r}")
        return 0

    # --tags: a STABLE machine interface, deliberately separate from the human report below.
    #
    # lib_metrics.sh used to scrape the human lines with a shell regex for a single-quoted marker. That
    # silently lost `[empty-json-guard]`, whose format string contains `'{}'` — Python's repr() switches
    # to DOUBLE quotes when the string holds an apostrophe, so the marker never matched, and a real
    # floor was dropped from the report. An omitted floor reads as "this count is a real total", which
    # is precisely the error this tool exists to prevent, committed by the tool itself.
    #
    # Emits TAG<TAB>COUNT<TAB>CAP<TAB>SITE, one line per saturated marker, with the bracketed tag
    # extracted in Python where the format string is known exactly. Always exits 0: this is a query,
    # not a gate, and a non-zero exit here would abort any `set -e` caller that merely asked a question.
    if "--tags" in argv:
        for log in logs:
            try:
                text = open(log, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for cap, lit, f, ln in sorted(caps):
                n = text.count(lit)
                if n and n >= cap:
                    m = re.match(r"(\[[^\]]+\])", lit)
                    tag = m.group(1) if m else lit[:24]
                    print(f"{tag}\t{n}\t{cap}\t{f}:{ln}")
        return 0

    saturated = 0
    for log in logs:
        try:
            text = open(log, encoding="utf-8", errors="replace").read()
        except OSError as e:
            print(f"  [FAIL] {log}: {e}")
            saturated += 1
            continue
        for cap, lit, f, ln in sorted(caps):
            n = text.count(lit)
            if n == 0:
                continue
            if n >= cap:
                print(f"  [FLOOR] {os.path.basename(log)}: {lit[:52]!r} = {n}, cap is {cap} "
                      f"({f}:{ln}) — this is a LOWER BOUND, never quote it as a count")
                saturated += 1
            else:
                print(f"  [ ok  ] {os.path.basename(log)}: {lit[:52]!r} = {n} of max {cap} — a real count")
    if saturated:
        print(f"\n  {saturated} saturated counter(s). A saturated counter is not a measurement.")
        return 1
    print("\n  no counter in these logs has reached its cap, so every count above is a real total")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
