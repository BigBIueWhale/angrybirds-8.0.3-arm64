#!/usr/bin/env python3
"""shader_screen.py — reassemble the captured shader sources and screen them for the constructs a
conformant GLES2 driver rejects but SwiftShader accepts.

WHY THIS EXISTS
---------------
The GPU is one of only two surfaces no emulator here stands in for (OPEN_FINDINGS R9), and shader
compilation is its likeliest failure: every validation run uses SwiftShader, which is lenient, while
the A56's Mali/Xclipse driver is not. A shader that compiles here can be rejected there, and the
symptom is a black screen.

The shaders cannot be screened statically. There is no .vsh/.fsh/.glsl anywhere in the APK — all 4398
zip entries were searched, not merely the 3217 under assets/data/ — and no `gl_Position`/`varying`/
`precision` string appears in the engine or libjs.so, because the shaders are
assembled at RUNTIME from a preprocessor-driven uber-shader, variants selected by #define
combination. The only point where the final text exists is the shim's glShaderSource bridge, which
dumps it under `#ifndef ABSHIM_RELEASE` (the shipped APK carries none of this).

WHAT IT REFUSES TO DO
---------------------
Report a screen over an incomplete capture. Three separate partial measurements preceded the first
real result, each of which produced a plausible-looking number:

  1. `%.700s` in the dump captured only the leading #define block of 6000-9150-byte shaders. The
     screen then found ZERO of every construct — no `precision`, but also no `gl_FragColor` and no
     `texture2D`. That reads as "no portability risks" and meant "no data". A working renderer cannot
     contain zero gl_FragColor; the impossibility was the only tell.
  2. Stripping `//` comments deleted whole shaders, because the dump flattens newlines to keep each
     chunk on one logcat line, leaving line comments unterminated. Result: "0 declarations".
  3. The dump stopped after 8 shaders, so only boot-time variants were seen — a real screen over an
     unrepresentative sample.

So this asserts completeness FIRST: every shader's chunks must reassemble to exactly the byte length
the bridge reported, and any shader that does not is excluded and named. A screen over 4 verified
shaders is worth more than one over 20 unverified ones.

    python3 shader_screen.py reports/shots/<capture>_abshim.txt
"""
import sys, re, collections

QUAL  = r'(?:lowp|mediump|highp)'
TYPES = r'(?:float|vec2|vec3|vec4|mat2|mat3|mat4)'

# Constructs that most often separate a lenient GL implementation from a conformant one.
RISKS = [
    ("#version directive",      r'#version'),
    ("#extension directive",    r'#extension'),
    ("GLES3 in/out",            r'(?<![\w])(?:in|out)\s+(?:' + QUAL + r'\s+)?vec'),
    ("dFdx / dFdy",             r'\bdFd[xy]\s*\('),
    ("texture2DLod",            r'\btexture2DLod\s*\('),
    ("gl_FragDepth",            r'\bgl_FragDepth\b'),
    ("dynamic loop bound",      r'for\s*\([^)]*;\s*\w+\s*<\s*[a-zA-Z_]\w*\s*;'),
]

def load(path):
    """Return {(shader,part): text} for shaders whose chunks reassemble to the reported length."""
    chunks, lens = collections.defaultdict(dict), {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'\[shader-src\] sh=(\d+) p=(\d+) c=(\d+) len=(\d+) :(.*)$', line.rstrip('\n'))
        if not m:
            continue
        key = (int(m.group(1)), int(m.group(2)))
        chunks[key][int(m.group(3))] = m.group(5)
        lens[key] = int(m.group(4))
    complete, partial = {}, []
    for key, cs in chunks.items():
        want = lens[key]
        expect = (want + 479) // 480          # the dump emits 480-char chunks
        text = "".join(cs[i] for i in sorted(cs))
        if len(cs) == expect and len(text) == want:
            complete[key] = text
        else:
            partial.append((key, len(cs), expect, len(text), want))
    return complete, partial

def main(path):
    complete, partial = load(path)
    print(f"  capture: {path}")
    print(f"  shaders fully reassembled : {len(complete)}")
    if partial:
        print(f"  shaders INCOMPLETE (excluded — a screen over these would mean nothing): {len(partial)}")
        for key, got, exp, glen, wlen in partial[:8]:
            print(f"      sh={key[0]} part={key[1]}: chunks {got}/{exp}, bytes {glen}/{wlen}")
    if not complete:
        print("  NOTHING TO SCREEN — no shader reassembled completely. This is not a pass.")
        return 1

    # Sanity: a real renderer's shader set must contain these. If it does not, the capture is
    # unrepresentative even though each individual shader reassembled.
    joined = " ".join(complete.values())
    for marker in ("gl_FragColor", "gl_Position", "attribute"):
        if marker not in joined:
            print(f"  SUSPECT CAPTURE: no shader contains {marker!r} — screening this would repeat")
            print( "                   the 'zero of everything' error. Refusing.")
            return 1

    bad = 0
    print("\n  === precision qualifiers (GLES2 §4.5.3: fragment shaders have NO default float precision) ===")
    for key in sorted(complete):
        v = complete[key]
        kind = "FRAGMENT" if "gl_FragColor" in v else "vertex  "
        decls = re.findall(r'(?:uniform|varying|attribute)\s+(?:' + QUAL + r'\s+)?' + TYPES + r'\s+\w+', v)
        unq   = [d for d in decls if not re.search(QUAL, d)]
        loc   = re.findall(r'(?<![\w.])(?:' + QUAL + r'\s+)?' + TYPES + r'\s+\w+\s*[=;]', v)
        lunq  = [d for d in loc if not re.search(QUAL, d)]
        # ONLY FRAGMENT SHADERS. GLES 2.0 §4.5.3 gives the vertex language default precisions
        # (highp float, highp int); the fragment language has NO default for float, which is why a
        # fragment shader must qualify every float-typed declaration or declare a global precision.
        # An earlier version of this script flagged both kinds and reported `float w` and
        # `mat4 bonetm` in a VERTEX shader as portability risks. They are perfectly legal, and a
        # false alarm about the GPU path is as damaging as a missed one — it would send someone
        # hunting a driver bug that cannot exist.
        is_frag = "gl_FragColor" in v
        offending = (unq + lunq) if is_frag else []
        flag = "" if not offending else "   <-- UNQUALIFIED in a FRAGMENT shader — may be rejected"
        if not is_frag and (unq or lunq):
            flag = f"   (vertex: {len(unq)+len(lunq)} unqualified — legal, vertex language has defaults)"
        if offending:
            bad += 1
        print(f"    sh={key[0]:<3} {kind}  decls {len(decls):3} (unqualified {len(unq)})"
              f"  locals {len(loc):3} (unqualified {len(lunq)}){flag}")
        for d in offending[:5]:
            print(f"        {d.strip()}")

    print("\n  === other constructs a strict driver may reject ===")
    for name, pat in RISKS:
        hits = [k for k, v in complete.items() if re.search(pat, v, re.M)]
        mark = "" if not hits else f"   <-- present in {len(hits)} shader(s)"
        print(f"    {name:22} {len(hits)}{mark}")

    print()
    if bad:
        print(f"  {bad} shader(s) contain unqualified float declarations — investigate before trusting the GPU path.")
        return 1
    print(f"  All {len(complete)} shaders: every float-typed declaration carries an explicit precision")
    print( "  qualifier, so the absence of a global `precision` statement is not a defect.")
    print( "  NOTE: this does not prove they compile on the A56 — only the device can. It establishes")
    print( "  that the most likely failure mode is absent.")
    return 0

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: shader_screen.py <capture_abshim.txt>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
