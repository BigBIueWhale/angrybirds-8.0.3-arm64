#!/usr/bin/env python3
"""png_sane.py — is this screenshot a picture of something, or a picture of nothing?

WHY
---
Capture scripts here write a screenshot and then say DONE. `adb exec-out screencap -p > file`
succeeds as a shell redirect whether or not screencap produced anything: if the device is wedged,
the app has died, or the surface is black, the file still appears, still has a PNG header, and still
looks like evidence. One such file has already been produced in this tree — a capture written from a
failed run that turned out to hold 72 bytes of ASCII — and it was named as a proof.

This answers only the mechanical question, and deliberately not the interesting one:

    CAN answer   "this file is a decodable PNG of the expected size, and it is not a blank,
                  solid, or near-solid frame"
    CANNOT answer "this shows the bird mid-flight" / "this is level 2" / "the timing was right"

The second kind still needs a person to look, which is why the capture scripts say so. The point is
that a run which produced a black rectangle should not reach the stage where a person is asked to
squint at it — it should have already failed.

THRESHOLDS ARE MEASURED, NOT GUESSED (reproduce the table with --survey)
    Across all 27 real captures in reports/shots, at 5-bit-per-channel quantisation:

        dominant-colour share   0.072 (mid-flight) .. 0.283 (the dimmed win screens)
        distinct colours          570 (a menu)     .. 1947 (the 2340x1080 A56 shot)
        a solid frame             1.000 and exactly 1 colour

    The first draft of this file asserted "well under 0.5" and "thousands of colours" BEFORE
    measuring; both were wrong (0.283 and 570). Numbers here are what the survey printed.

    python3 png_sane.py shot.png [more.png ...]     # exit 0 = all sane
    python3 png_sane.py --survey reports/shots/*.png   # print the measurements, judge nothing
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from win_detect import load          # pure-python PNG decode, shared on purpose

MAX_UNIFORM  = 0.650   # midway between the worst real capture (0.283) and a solid frame (1.000),
                       # so it rejects a blank screen without rejecting a legitimately dim one.
MIN_DISTINCT = 64      # NOT at the midpoint (285) on purpose: uniformity above is what discriminates,
                       # and this only has to separate "a rendered scene" from "a flat rectangle",
                       # which scores 1-20. Left loose so an unusually sparse but real frame — a
                       # loading screen, a mostly-empty menu — cannot fail a run for being plain.


def measure(path):
    """(w, h, dominant-colour share, distinct coarse colours). Colours are quantised to 5 bits per
    channel so that JPEG-ish gradient noise in a flat sky does not read as thousands of colours."""
    w, h, ch, px = load(path)
    hist = {}
    # Sample every 2nd row/column: 4x faster, and uniformity is a global property — a frame that is
    # blank on the sampled half and busy on the other does not exist in this rig.
    for y in range(0, h, 2):
        base = y * w * ch
        for x in range(0, w, 2):
            i = base + x * ch
            k = (px[i] >> 3, px[i + 1] >> 3, px[i + 2] >> 3)
            hist[k] = hist.get(k, 0) + 1
    n = sum(hist.values())
    return w, h, (max(hist.values()) / n if n else 1.0), len(hist)


def main(argv):
    survey = "--survey" in argv
    expect = None
    args = []
    it = iter(argv)
    for a in it:
        if a == "--survey":
            continue
        if a == "--expect":
            # `--expect` as the final argument used to raise StopIteration and print a traceback.
            # It exited 1, so it failed closed rather than passing something unchecked — but a
            # traceback tells the reader nothing about what to do.
            expect = next(it, None)
            if expect is None:
                print("  [FAIL] --expect needs a value, e.g. --expect 640x320", file=sys.stderr)
                return 2
            continue
        args.append(a)
    if not args:
        print(__doc__, file=sys.stderr)
        return 2

    bad = 0
    if survey:
        print(f"{'file':52s} {'size':>10s} {'uniform':>8s} {'colours':>8s}")
    for p in args:
        try:
            w, h, uni, dis = measure(p)
        except Exception as e:                       # unreadable IS the failure, not an excuse
            print(f"  [FAIL] {os.path.basename(p)}: cannot decode as a PNG ({e})")
            bad += 1
            continue
        if survey:
            print(f"{os.path.basename(p):52s} {w}x{h:<5d} {uni:8.3f} {dis:8d}")
            continue
        why = []
        if uni > MAX_UNIFORM:
            why.append(f"{uni*100:.1f}% of it is a single colour — this is a blank/solid frame")
        if dis < MIN_DISTINCT:
            why.append(f"only {dis} distinct colours — nothing was rendered")
        if expect and f"{w}x{h}" != expect:
            why.append(f"size {w}x{h}, expected {expect}")
        if why:
            print(f"  [FAIL] {os.path.basename(p)}: " + "; ".join(why))
            bad += 1
        else:
            print(f"  [ OK ] {os.path.basename(p)}: {w}x{h}, {dis} colours, "
                  f"dominant {uni*100:.1f}%")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
