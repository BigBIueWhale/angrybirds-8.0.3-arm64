#!/usr/bin/env python3
"""find_bird.py — locate the bird sitting on the slingshot, so a drag can start ON it.

WHY THIS EXISTS
---------------
Every driving script here shoots with one hardcoded gesture, `input swipe 207 118 110 150 700`,
and treats it as "the slingshot drag". It is not. It is a drag at a fixed screen position, and it
launches a bird only when the bird happens to be under (207,118). That is true in the tutorial,
which is why PROOF_2..PROOF_9 work, and it is false as soon as the level changes: the camera framing
differs, so the slingshot lands somewhere else.

emu_progression.sh made this visible. With the camera correctly reset, `prog_2_pre.png` shows level 2
framed properly — slingshot at roughly (155,180), a bird loaded on it, two spares behind, pig towers
to the right. The harness dragged from (207,118): empty sky above the slingshot. Cycle after cycle
"failed to win" while the game was working perfectly and simply never being touched.

A drag that misses also PANS THE VIEW (see lib_camera.sh), so the error compounds instead of being
harmless.

WHAT IT DOES
Finds the loaded bird and prints "X Y". The birds are the only strongly saturated red objects on
screen — background is sky/haze, terrain is green and tan, pigs are green, wood is brown, stone is
grey. Of the red blobs, the loaded one is the HIGHEST: spares wait on the ground below.

Deliberately no PIL: the emulator images do not have it, which is the same trap win_detect.py hit
(`python3: command not found` printed where a verdict was expected). It reuses that file's pure
zlib+struct decoder rather than carrying a second copy.

    python3 find_bird.py shot.png          -> "155 180"   (exit 0)
                                           -> "" + exit 1 if no bird is found

A YELLOW MASK WAS TRIED AND REJECTED — measured, not assumed. The tutorial's third level stars
Chuck, who is yellow, so "detect yellow birds too" looks obviously right. Adding
`r>180 and g>150 and b<110` finds the Chuck on the slingshot (272,198, 98 px) and also finds sunlit
foliage in blobs of 2525 and 3314 pixels on that same frame, and 5307 pixels on level 2 — terrain
that outranks the bird under any topmost- or largest-blob rule, on the very frames red already
handles. Red-only is kept because it demonstrably works on both: on the Chuck level it locks onto
the beak at (266,215), which is inside the sprite and close enough to drag, and on red-bird levels
it finds the bird outright. Widening the mask would trade a working heuristic for a broken one.

LIMITS, measured: it also answers confidently on a tutorial instruction card (the printed bird) and
on a win screen. Geometry cannot tell those from a real slingshot. The caller must dismiss dialogs,
let the scene settle, and verify the shot afterwards.

HOW FAR IT IS ACTUALLY VALIDATED: one frame. prog_2_pre.png returns (152,183), which matches the
slingshot position read off the image by eye. That is the whole of the evidence. It is NOT validated
against a tutorial pre-shot frame, because no such capture exists in reports/shots -- the tutorial
images on disk are dialogs, impacts and win screens. Treat a single confirmed case as a single
confirmed case.

Exit 1 rather than a guessed coordinate: a caller that falls back to a constant is back to the bug
this file exists to remove, so "I could not see a bird" has to be distinguishable from an answer.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from win_detect import load          # pure-python PNG decode, shared on purpose


def find_bird(path):
    w, h, ch, px = load(path)
    # The slingshot lives at the level's left edge once the camera is reset, so searching the left
    # half both cuts work and avoids matching red scenery further into the level. The vertical band
    # skips the HUD (score, pause) at the very top and the extreme bottom edge.
    x_hi = int(w * 0.55)
    y_lo, y_hi = int(h * 0.12), int(h * 0.95)

    pts = []
    for y in range(y_lo, y_hi):
        row = y * w * ch
        for x in range(0, x_hi):
            i = row + x * ch
            r, g, b = px[i], px[i + 1], px[i + 2]
            # Saturated red. The 1.6x ratios reject tan terrain and brown wood, which are bright in
            # red but far from dominant in it; the floor of 110 rejects dark shadowed reds.
            if r > 110 and r > g * 1.6 and r > b * 1.6:
                pts.append((x, y))
    if not pts:
        return None

    # A bird is a SOLID BLOB, so group the matching pixels into connected components and ignore the
    # small ones. "Topmost red mass" was the first rule here and it failed on real frames: level 4's
    # capture (prog_4_s2.png) has an animated tutorial hint-hand drawn over the scene, and a handful
    # of reddish pixels along its outline sit higher than the bird. The finder duly answered (288,62)
    # — up in the sky — while the loaded bird sat at about (270,200), and the shot was wasted.
    #
    # Size first, THEN height: among blobs big enough to be a bird, the loaded one is the highest,
    # because the spares wait on the ground below it.
    pset = set(pts)
    seen = set()
    blobs = []
    for p0 in pts:
        if p0 in seen:
            continue
        stack, comp = [p0], []
        while stack:
            q = stack.pop()
            if q in seen:
                continue
            seen.add(q); comp.append(q)
            qx, qy = q
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    n = (qx + dx, qy + dy)
                    if n in pset and n not in seen:
                        stack.append(n)
        blobs.append(comp)

    # 60 px is well under a bird (its saturated core runs to several hundred) and well over the
    # speckle that overlays and damage numerals contribute. Validated against the captures listed
    # below rather than chosen by eye.
    MIN_BLOB = 60
    big = [b for b in blobs if len(b) >= MIN_BLOB]
    if not big:
        return None
    bird = min(big, key=lambda b: min(q[1] for q in b))     # highest qualifying blob
    top_y = min(q[1] for q in bird)
    near = [q for q in bird if abs(q[1] - top_y) <= 22]
    cx = sum(q[0] for q in near) // len(near)
    # Anchor slightly BELOW the topmost row: the top edge of the sprite is its scalp, and a drag
    # must land on the body to be picked up.
    cy = top_y + 8

    # PLAUSIBILITY BAND — and an honest account of how little it buys.
    #
    # This finder answers "where is the reddest thing", which is the bird only when a bird is
    # actually sitting there. It is not a bird detector. Three real captures, measured not imagined:
    #
    #   prog_2_pre.png            (152,183)  correct — a loaded slingshot
    #   interactive_3.png         (317,46)   wrong  — mid-impact debris and damage numerals
    #   modplay_2_afterdialog.png (235,126)  wrong  — a bird ILLUSTRATED inside a tutorial card
    #   prog_1.png                (242,145)  wrong  — a win screen
    #
    # The band below rejects the FIRST of those three wrong answers and NOT the other two: a dialog
    # illustration and a win screen both put red pixels exactly where a slingshot legitimately sits,
    # so no geometric test can separate them. An earlier version of this comment claimed the band
    # "excludes both false positives"; it was written before the cases were run, and the run showed
    # two of them sailing through. Geometry screens out gross nonsense, nothing more.
    #
    # What actually makes this safe is the CALLER: ask only after dialogs are dismissed and the
    # scene has settled, and confirm afterwards that a shot really happened rather than assuming the
    # drag landed. See emu_progression.sh.
    if not (0.10 * w <= cx <= 0.45 * w and 0.28 * h <= cy <= 0.85 * h):
        return ('out-of-band', cx, cy)
    return cx, cy, len(pts), len(near)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: find_bird.py <screenshot.png>", file=sys.stderr); sys.exit(2)
    try:
        r = find_bird(sys.argv[1])
    except Exception as e:
        print(f"find_bird: {e}", file=sys.stderr); sys.exit(2)
    # Two different refusals, reported differently. They were briefly collapsed into one message,
    # and "no bird-coloured pixels found" was printed for a frame FULL of them that simply landed
    # outside the band — a wrong reason is a wrong lead for whoever reads the log next.
    if not r:
        print("find_bird: no bird-coloured pixels in the search region", file=sys.stderr); sys.exit(1)
    if r[0] == 'out-of-band':
        print(f"find_bird: reddest cluster at ({r[1]},{r[2]}) is not where a slingshot sits — "
              f"refusing to call it a bird", file=sys.stderr); sys.exit(1)
    cx, cy, n, nn = r
    if os.environ.get("FIND_BIRD_VERBOSE"):
        print(f"# {n} red px, {nn} in the top cluster", file=sys.stderr)
    print(f"{cx} {cy}")
