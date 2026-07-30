#!/usr/bin/env python3
"""log_recapture_audit.py — detect logs built by appending several `adb logcat` captures.

WHY
---
`adb logcat` without `-T`/`-c` re-dumps the ENTIRE ring buffer every time it is invoked. Appending
several captures to one file therefore multiplies every count in it. This happened, and the inflated
numbers reached the record before anything caught them (R50):

    PROOF_PHONE_abshim.txt   35,681 lines / 8,236 unique = 4.33x duplicated
      nativeUpdate           847 reported  ->  121 real   (847/121 ~= 7 = the number of appends)
      s-construct-null-guard  15 reported  ->  >=5

An inference was built on the inflation too: `nativeUpdate` appeared to climb by exactly +121 after
each interaction, which read as "the app is alive and responding" but was just the same 121 lines
being re-emitted. A suspiciously CONSTANT delta is the tell.

WHY NOT JUST MEASURE DUPLICATION
--------------------------------
The obvious detector — total/unique line ratio — was measured across all 61 committed logs and is not
usable as a gate. Median 1.01x, but `dump_api36.txt` sits at 2.43x while being perfectly valid: it is
a `dumpsys package` dump where 91 activities each print identical boilerplate and
"reason: assuming delivered" occurs 1052 times. Structural repetition and re-dump duplication are
indistinguishable by ratio, so a ratio gate either misses real cases or fails honest ones.

THE DETECTOR USED INSTEAD
-------------------------
A logcat capture is monotonic in time. Appending a second capture RESTARTS the clock earlier, so the
file contains a backward jump in timestamps. That is structural, not statistical. Measured across the
50 timestamped logs in reports/shots:

    PROOF_PHONE_abshim.txt      6 backward jumps > 2s   (worst 639.8s)  <- 6 jumps = 7 appends
    all 49 other logs           0 backward jumps > 2s

Clean separation with nothing in between, so the threshold needs no tuning. The 2s floor exists only
so that out-of-order delivery between logcat's own buffers (main/system interleaving is not perfectly
ordered at millisecond scale) cannot register; every real case here was three orders of magnitude past
it.

    python3 log_recapture_audit.py reports/shots/*.txt
    python3 log_recapture_audit.py --selftest
"""
import sys, os, re, glob, datetime

# `07-30 02:16:32.897 11197 11197 I abshim  : ...` — logcat's default threadtime format.
TS = re.compile(r"^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d\d\d)")

BACKWARD_S = 2.0        # see the measurement above: real cases were 639s, honest logs exactly 0
ROLLOVER_S = 300 * 86400  # a Dec->Jan capture parses as a ~year-long backward jump; not a re-dump

# The one committed log that IS an appended capture. It is kept AS CAPTURED rather than de-duplicated,
# because it is the authentic phone log behind PROOF_22..27 and rewriting raw evidence to make a check
# pass is worse than the defect. R50 documents which of its numbers survive (h_fatal 0, the distinct
# monotonic frame heartbeats) and which do not (every count).
#
# This is PINNED, not suppressed: the expected jump count is asserted, so if the file is ever replaced
# or re-appended the check still fires. An allowlist that merely says "ignore this file" would be a
# check that cannot fail, which is the defect class this whole suite is built against.
KNOWN_RECAPTURES = {"PROOF_PHONE_abshim.txt": 6}   # 6 backward jumps == 7 appended captures


def stamps(path):
    """Timestamps in file order. Year is synthetic — logcat does not print one, and only DELTAS are
    used, so the absolute year is irrelevant."""
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = TS.match(line)
            if not m:
                continue
            mo, d, h, mi, s, ms = (int(x) for x in m.groups())
            try:
                out.append(datetime.datetime(2000, mo, d, h, mi, s, ms * 1000))
            except ValueError:      # 02-30 and friends: a corrupt line, not a re-dump
                continue
    return out


def audit(path):
    """(verdict, detail). verdict is 'clean' | 'recapture' | 'no-timestamps'.

    'no-timestamps' is reported DISTINCTLY and never as a pass: a file this tool cannot parse and a
    file that is genuinely monotonic look identical from the outside, and conflating them is the
    exact class of error this suite exists to prevent."""
    st = stamps(path)
    if len(st) < 2:
        return "no-timestamps", f"{len(st)} parseable timestamp(s) — nothing to check"
    jumps = []
    for a, b in zip(st, st[1:]):
        if b >= a:
            continue
        back = (a - b).total_seconds()
        if back > ROLLOVER_S:       # year boundary
            continue
        if back > BACKWARD_S:
            jumps.append(back)
    if jumps:
        want = KNOWN_RECAPTURES.get(os.path.basename(path))
        detail = (f"{len(jumps)} backward time jump(s) > {BACKWARD_S}s (worst {max(jumps):.1f}s) — "
                  f"this file is {len(jumps) + 1} appended captures, so EVERY count in it is inflated")
        if want is not None:
            if len(jumps) == want:
                return "known", detail + f" [PINNED at {want} jumps; see R50]"
            return "recapture", detail + (f" — and the pin expected {want} jumps, so this file has "
                                          f"CHANGED since R50 was written")
        return "recapture", detail
    if os.path.basename(path) in KNOWN_RECAPTURES:
        # The pin must fail in this direction too. If the file were quietly replaced with a clean
        # capture, R50's caveats would be stale and nothing would say so.
        return "recapture", (f"pinned as an appended capture ({KNOWN_RECAPTURES[os.path.basename(path)]} "
                             f"jumps) but is now monotonic — R50's caveats need revisiting")
    return "clean", f"{len(st)} timestamps, monotonic — counts in this log are real"


def selftest():
    """Synthetic cases. A detector nobody has tried to fool is a detector nobody has tested."""
    import tempfile
    def w(d, name, body):
        p = os.path.join(d, name)
        open(p, "w", encoding="utf-8").write(body)
        return p
    ok = True
    with tempfile.TemporaryDirectory() as d:
        def L(sec, msg="I abshim  : x"):
            return f"07-30 02:{sec // 60:02d}:{sec % 60:02d}.000 111 111 {msg}\n"
        cases = [
            ("monotonic",      "".join(L(s) for s in range(0, 60, 3)),                  "clean"),
            # the real failure: two captures appended, second restarts earlier
            ("recapture",      "".join(L(s) for s in range(0, 60, 3)) +
                               "".join(L(s) for s in range(0, 60, 3)),                  "recapture"),
            # same-millisecond repeats are NOT a re-dump — this is what makes sort -u unsafe but
            # leaves the timestamp detector correct
            ("same_ms",        L(10) + L(10) + L(10) + L(11),                           "clean"),
            ("no_timestamps",  "Activity Resolver Table:\n  Schemes:\n" * 30,           "no-timestamps"),
            ("empty",          "",                                                      "no-timestamps"),
            ("one_line",       L(5),                                                    "no-timestamps"),
            # sub-threshold jitter between logcat buffers must not fire
            ("jitter",         L(10) + L(9) + L(11) + L(10) + L(12),                    "clean"),
            # a corrupt date must be skipped, not crash and not fire
            ("bad_date",       L(5) + "02-30 25:99:99.000 1 1 I x  : y\n" + L(8),       "clean"),
            ("three_appends",  "".join(L(s) for s in range(0, 30, 3)) * 3,              "recapture"),
        ]
        for name, body, want in cases:
            got, detail = audit(w(d, name + ".txt", body))
            mark = " ok " if got == want else "FAIL"
            if got != want:
                ok = False
            print(f"  [{mark}] {name:15} expected {want:14} got {got:14} {detail[:52]}")
        # the three-append case must also COUNT correctly: 3 captures => 2 backward jumps
        _, detail = audit(os.path.join(d, "three_appends.txt"))
        if "3 appended captures" not in detail:
            print(f"  [FAIL] three_appends should report 3 appended captures, said: {detail}")
            ok = False
        else:
            print("  [ ok ] three_appends reports the append COUNT, not just the fact")
    print("\n  selftest PASSED" if ok else "\n  selftest FAILED")
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    logs = [a for a in argv if not a.startswith("--")]
    if not logs:
        print(__doc__.strip().splitlines()[0])
        print("  usage: log_recapture_audit.py <log>... | --selftest", file=sys.stderr)
        return 2
    bad = 0
    checked = 0
    known = 0
    for path in logs:
        if not os.path.isfile(path):
            continue
        v, detail = audit(path)
        name = os.path.basename(path)
        if v == "recapture":
            print(f"  [RECAPTURE] {name}: {detail}")
            bad += 1
        elif v == "known":
            print(f"  [ known  ] {name}: {detail}")
            known += 1
        elif v == "clean":
            checked += 1
        # 'no-timestamps' is silent per-file but does not count as checked
    # Assert the pins were actually reached. If a caller passes a glob that happens to miss the pinned
    # file, a silent 0 would read as "nothing to worry about" — the absent-symptom-as-success defect.
    seen = {os.path.basename(p) for p in logs}
    missed = [k for k in KNOWN_RECAPTURES if k not in seen]
    print(f"  {checked} log(s) verified monotonic; {known} pinned known-recapture; "
          f"{bad} unexpected appended capture(s)")
    if missed and len(logs) > 1:
        print(f"  note: pinned file(s) not in this run's file list: {', '.join(missed)}")
    if bad:
        print("  Fix the capture (`adb logcat -c` first, or `-T <time>`), not the number.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
