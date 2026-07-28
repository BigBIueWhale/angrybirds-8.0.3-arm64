#!/usr/bin/env python3
"""gl_caps.py — what does the driver actually TELL the engine, and does that differ between the rig
and the phone?

WHY THIS EXISTS
---------------
R9 lists the GPU as one of only two surfaces no emulator here stands in for. R10 screened the shader
SOURCE. This screens the other half of the same surface: the driver's self-description, because the
engine branches on it.

`libAngryBirdsClassic.so` imports glGetString and glCompressedTexImage2D, and contains exactly three
GL extension name strings:

    GL_OES_compressed_ETC1_RGB8_texture
    GL_OES_texture_npot
    GL_OES_vertex_buffer_object

That is the signature of a capability query: read GL_EXTENSIONS, look for these, pick a texture and
geometry path. If SwiftShader advertises a different set than the A56's Mali/Xclipse, then the phone
takes a branch that nothing in this project has ever executed — and the symptom would be missing or
corrupt textures, which is indistinguishable at a glance from a shader problem.

WHY NOT JUST GREP THE DRIVER BINARY
-----------------------------------
Because a string present in libGLESv2.so need not appear in what glGetString(GL_EXTENSIONS) actually
returns: it may be an internal table, a name gated by context version, or dead. The binary tells you
what the driver COULD say; only the returned string tells you what it DID say to this engine, in
this context. The shim's bridge is the one place that value exists, so the capture is the evidence
and this script is the reader.

WHAT IT REFUSES TO DO
---------------------
Report a capability screen when the engine never asked. A capture with no GL_EXTENSIONS query yields
"none of the three extensions present", which reads as a dramatic finding and actually means no data
— the same "zero of everything" failure shader_screen.py was built to refuse. Absence of the query
is therefore a hard FAIL, not a result.

    python3 gl_caps.py <capture>_abshim.txt [engine.so] [save-baseline-path]

Given a third argument it writes the sorted extension list there. That file is the point of the
exercise: it is what a device capture gets DIFFED against. It is written by this script rather than
by hand so it is regenerable by re-running the harness — a baseline assembled once by an ad-hoc
command is the same untrustworthy artifact as an ad-hoc capture.
"""
import sys, re, os, collections

NAMES = {0x1F00: "VENDOR", 0x1F01: "RENDERER", 0x1F02: "VERSION", 0x1F03: "EXTENSIONS"}
DEFAULT_ENGINE = "/work/work803/libv7/libAngryBirdsClassic.so"


def load(path):
    """Return ({name: text}, [incomplete]) for entries whose chunks reassemble to the logged length."""
    chunks, lens = collections.defaultdict(dict), {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"\[gl-str\] name=0x([0-9a-fA-F]+)\([^)]*\) c=(\d+) len=(\d+) :(.*)$", line.rstrip("\n"))
        if not m:
            continue
        key = int(m.group(1), 16)
        chunks[key][int(m.group(2))] = m.group(4)
        lens[key] = int(m.group(3))
    complete, partial = {}, []
    for key, cs in chunks.items():
        want = lens[key]
        expect = max(1, (want + 479) // 480)          # the bridge emits 480-char chunks
        text = "".join(cs[i] for i in sorted(cs))
        if len(cs) == expect and len(text) == want:
            complete[key] = text
        else:
            partial.append((key, len(cs), expect, len(text), want))
    return complete, partial


def engine_extensions(path):
    """The GL extension names the engine binary itself references. Derived, not hardcoded: if the
    engine is ever rebuilt or a different build is screened, a stale hardcoded list would quietly
    screen the wrong thing."""
    try:
        blob = open(path, "rb").read()
    except OSError:
        return None
    return sorted(set(m.decode("ascii") for m in re.findall(
        rb"GL_(?:OES|EXT|IMG|AMD|ARM|NV|QCOM|KHR)_[A-Za-z0-9_]+", blob)))


def save_baseline(path, ext, nbytes, cap):
    """Write the sorted extension list for a later device diff. Sorted, one per line, with the
    provenance in a comment header: an unsorted or unattributed baseline makes a `diff` against a
    device capture unreadable, which defeats the only purpose it has."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# GL_EXTENSIONS as returned to the engine, one per line, sorted.\n"
                 "# This is the BASELINE to diff a device capture against: the engine branches on\n"
                 "# this string (OPEN_FINDINGS R11), so a difference is a different code path.\n"
                 f"# source capture: {os.path.basename(cap)}\n"
                 f"# {len(ext)} extensions, {nbytes} bytes.\n")
        fh.write("\n".join(ext) + "\n")
    print(f"  baseline written: {path} ({len(ext)} extensions)")


def main(cap, engine_path, baseline_path=None):
    complete, partial = load(cap)
    print(f"  capture: {cap}")
    if partial:
        print(f"  entries INCOMPLETE (excluded): {len(partial)}")
        for key, got, exp, glen, wlen in partial:
            print(f"      name=0x{key:04x}: chunks {got}/{exp}, bytes {glen}/{wlen}")

    if 0x1F03 not in complete:
        print("  [FAIL] the engine never completed a glGetString(GL_EXTENSIONS) query in this capture.")
        print("         Screening now would report 'no extensions found', which is not a finding —")
        print("         it is an absent measurement. Re-capture; do not quote this run.")
        return 1

    for key in (0x1F00, 0x1F01, 0x1F02):
        if key in complete:
            print(f"  {NAMES[key]:<10} {complete[key]}")

    advertised = set(complete[0x1F03].split())
    print(f"  EXTENSIONS {len(advertised)} advertised ({len(complete[0x1F03])} bytes)")
    if baseline_path:
        save_baseline(baseline_path, sorted(advertised), len(complete[0x1F03]), cap)

    wanted = engine_extensions(engine_path)
    if wanted is None:
        print(f"  [FAIL] cannot read the engine at {engine_path} — the list of extensions the engine")
        print("         actually looks for has to be derived from the binary being screened, not")
        print("         assumed. Pass the correct path as argument 2.")
        return 1
    if not wanted:
        print(f"  [FAIL] no GL extension names found in {engine_path} — wrong file, or the read failed.")
        return 1

    print(f"\n  === the {len(wanted)} extension name(s) the ENGINE references, vs what this driver advertises ===")
    missing = []
    for e in wanted:
        hit = e in advertised
        print(f"    {'ADVERTISED' if hit else 'not present'}   {e}")
        if not hit:
            missing.append(e)

    print()
    if missing:
        print(f"  {len(missing)} extension(s) the engine looks for are NOT advertised here.")
        print("  That is not automatically a defect — GL_OES_vertex_buffer_object is a GLES1-era name")
        print("  whose functionality is CORE in GLES2, so no GLES2 driver advertises it and both the")
        print("  rig and the phone see it absent. What matters for portability is whether the rig and")
        print("  the device would answer DIFFERENTLY, since only then does the phone take an untested")
        print("  branch. Record the set above and compare it with a device capture (ONDEVICE.md).")
    else:
        print("  Every extension the engine looks for is advertised here.")
    print("  NOTE: this describes THIS driver. It bounds the risk rather than eliminating it — only a")
    print("  capture from the A56 shows what Mali/Xclipse answers.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3, 4):
        print("usage: gl_caps.py <capture>_abshim.txt [engine.so] [save-baseline-path]")
        sys.exit(2)
    sys.exit(main(sys.argv[1],
                  sys.argv[2] if len(sys.argv) >= 3 else DEFAULT_ENGINE,
                  sys.argv[3] if len(sys.argv) == 4 else None))
