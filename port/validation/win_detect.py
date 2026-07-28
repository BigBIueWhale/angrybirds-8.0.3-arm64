#!/usr/bin/env python3
"""win_detect.py — decide whether a screenshot shows the LEVEL CLEARED screen, so the play test
stops being the one step that "must be looked at, not scored".

WHY THIS EXISTS
---------------
Every playthrough script ends with a line like

    win check:  SCREENSHOT ONLY -> modplay_3_end.png (a win shows 'LEVEL CLEARED' + stars + score)

because NOTHING IN THE LOG DISTINGUISHES A WIN. That is not an oversight, it was measured: a winning
run and a non-winning run of the same script produce identical marker counts (levelCompleteStars=8,
once-complete=6 in both) — those are asset PRELOADS, which is exactly what the scripts warn. So the
only evidence of a win was a human looking at a PNG, and validate_all.sh has to exclude the play
tests for that reason.

WHAT IT KEYS ON
---------------
The level-end screen is a modal that dims the whole scene and puts three gold stars over it.
Measured across 9 labelled win screens and 6 non-win screens (640x320 captures):

                        gold-star frac    dark frac      mean luminance
    win screens         0.0571 – 0.0573   0.523 – 0.526  57.8 – 58.1
    gameplay / menus    0.000  – 0.0795   0.0005– 0.045  187  – 214

`dark` separates by more than a factor of ten and luminance by more than three, so the thresholds sit
in a very wide gap rather than being tuned to the samples. Gold alone does NOT separate — a launched
bird scores 0.0795, higher than any win — which is why it is a supporting criterion, never the test.

THE FAILURE THIS IS BUILT TO AVOID
----------------------------------
A crashed or blank app renders a BLACK screen: dark≈1.0 and luminance≈0, i.e. it passes both dimming
criteria with room to spare. Calling that a win would turn the worst outcome into a green tick — the
precise "absence of evidence read as evidence" shape this project keeps finding. Hence the gold-star
floor: a win has stars, a black screen has none. All three criteria must hold.

It also reports a REASON on refusal, so a failed check says which criterion missed rather than just
"not a win".

    python3 win_detect.py <shot.png> [...]     # exit 0 only if EVERY image is a win screen
"""
import sys, zlib, struct

def load(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError("not a PNG")
    pos, idat = 8, b''
    w = h = bd = ct = None
    while pos < len(d):
        ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
        data = d[pos:pos+ln]; pos += ln + 4
        if typ == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', data)
            if bd != 8 or il != 0 or ct not in (2, 6):
                raise ValueError(f"unsupported PNG (bitdepth={bd} colour={ct} interlace={il})")
        elif typ == b'IDAT':
            idat += data
        elif typ == b'IEND':
            break
    raw = zlib.decompress(idat)
    ch = 3 if ct == 2 else 4
    stride = w * ch
    out = bytearray(w * h * ch)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(ch, stride): line[i] = (line[i] + line[i-ch]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]; c = prev[i-ch] if i >= ch else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, ch, bytes(out)

# Thresholds sit in the middle of the measured gap, not at its edge.
GOLD_MIN = 0.010   # wins 0.057 with THREE stars; a black screen scores exactly 0.000.
                   # Deliberately far below the observed win value rather than just under it: every
                   # labelled win here is a 3-star clear, and a 1-star clear would show roughly a
                   # third of the gold. Tuning this to 0.030 (snug under 0.057) would have rejected
                   # a genuine 1-star win. It can afford to be loose because it is not what
                   # identifies a win — dark/luminance do that — its ONLY job is to keep a black
                   # crashed screen from satisfying the two dimming criteria. The brightest non-win
                   # sample (0.0795) and the menu (0.0158) both clear this floor and are still
                   # rejected, because all three criteria must hold.
DARK_MIN = 0.350   # wins 0.523; gameplay <= 0.045
LUM_MAX  = 110.0   # wins 58;    gameplay >= 187

def features(path):
    w, h, ch, px = load(path)
    x0, x1 = int(w*0.25), int(w*0.75)
    y0, y1 = int(h*0.15), int(h*0.65)
    gold = dark = tot = 0
    lum = 0
    for y in range(y0, y1):
        row = y*w*ch
        for x in range(x0, x1):
            i = row + x*ch
            r, g, b = px[i], px[i+1], px[i+2]
            tot += 1
            L = (r*299 + g*587 + b*114)//1000
            lum += L
            if r > 200 and g > 150 and b < 110 and (r-b) > 110: gold += 1
            if L < 40: dark += 1
    if tot == 0:
        raise ValueError("empty sample region")
    return gold/tot, dark/tot, lum/tot

def verdict(path):
    g, d, l = features(path)
    why = []
    if g < GOLD_MIN: why.append(f"no star cluster (gold {g:.4f} < {GOLD_MIN}) — a BLACK/crashed screen looks like this")
    if d < DARK_MIN: why.append(f"scene not dimmed (dark {d:.4f} < {DARK_MIN})")
    if l > LUM_MAX:  why.append(f"too bright for a modal (lum {l:.1f} > {LUM_MAX})")
    return (not why), g, d, l, why

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: win_detect.py <shot.png> [...]"); sys.exit(2)
    rc = 0
    for p in sys.argv[1:]:
        try:
            ok, g, d, l, why = verdict(p)
        except Exception as e:
            print(f"  [FAIL] {p}: cannot read ({e})"); rc = 1; continue
        if ok:
            print(f"  [ WIN ] {p}  gold={g:.4f} dark={d:.4f} lum={l:.1f}")
        else:
            print(f"  [ NOT ] {p}  gold={g:.4f} dark={d:.4f} lum={l:.1f}")
            for r in why: print(f"          {r}")
            rc = 1
    sys.exit(rc)
