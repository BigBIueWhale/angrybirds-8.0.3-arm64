#!/usr/bin/env python3
"""apk_levels.py — how many level files does the APK actually contain, per episode?

WHY THIS EXISTS
---------------
emu_progression.sh reports "N distinct level files opened". Three times in one session that bare
count was read as a ceiling and written up as one: 4 implied a limit, 5 was recorded as "the whole
0_Tutorial episode", and the very next run opened a sixth. The number was never wrong; it was
published without a denominator, and every guess at the denominator was wrong.

The denominator belongs to the artifact, so it is read from the artifact.

WHY NOT zipfile
---------------
The emulator images ship a stripped python3 where `import zipfile` fails outright:

    File "/usr/lib/python3.10/zipfile.py", line 13, in <module>
    ModuleNotFoundError: No module named 'shutil'

The first version of this used zipfile inside emu_progression.sh, printed nothing at all, and the run
still reported success — an addition that silently did nothing, which is precisely the failure mode
this project keeps having to catch. It was found only because the expected lines were missing from
the output of a deliberate verification run.

So this parses the zip central directory with `struct` alone, the same way win_detect.py decodes PNG
with `zlib` + `struct`. No compression is involved: file NAMES live in the central directory in the
clear, which is all this needs.

    python3 apk_levels.py app.apk                 -> "0_Tutorial 15" per line, one per episode
    python3 apk_levels.py app.apk <shim-log>      -> "opened / total" per episode touched by the log
"""
import sys, struct, re


def zip_names(path):
    """Every stored path in a zip, via the central directory. struct only."""
    d = open(path, 'rb').read()
    # End of Central Directory: signature, then a comment of up to 64 KB. Scan backwards for it.
    eocd = d.rfind(b'PK\x05\x06')
    if eocd < 0:
        raise ValueError("not a zip: no end-of-central-directory record")
    count, size, off = struct.unpack('<HIL', d[eocd + 10:eocd + 20])
    # Zip64 shows up as 0xFFFF/0xFFFFFFFF sentinels. An APK this size will not hit it, but a silent
    # misparse would under-count and produce a WRONG denominator, which is the whole point of this
    # file — so refuse rather than guess.
    if count == 0xFFFF or off == 0xFFFFFFFF:
        raise ValueError("zip64 central directory — not handled, refusing to report a partial count")
    names, p = [], off
    for _ in range(count):
        if d[p:p + 4] != b'PK\x01\x02':
            raise ValueError(f"central directory entry {len(names)} has a bad signature")
        nlen, elen, clen = struct.unpack('<HHH', d[p + 28:p + 34])
        names.append(d[p + 46:p + 46 + nlen].decode('utf-8', 'replace'))
        p += 46 + nlen + elen + clen
    return names


LEVEL = re.compile(r'^assets/data/levels/([A-Za-z0-9_]+)/([A-Za-z0-9_]+)\.lua$')


def episodes(apk):
    total = {}
    for n in zip_names(apk):
        m = LEVEL.match(n)
        if m:
            total.setdefault(m.group(1), set()).add(m.group(2))
    return total


def opened(logpath):
    seen = {}
    with open(logpath, 'rb') as fh:
        for m in re.finditer(rb'data/levels/([A-Za-z0-9_]+)/([A-Za-z0-9_]+)', fh.read()):
            seen.setdefault(m.group(1).decode(), set()).add(m.group(2).decode())
    return seen


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: apk_levels.py <apk> [shim-log]", file=sys.stderr); sys.exit(2)
    total = episodes(sys.argv[1])
    if not total:
        print("apk_levels: no level files found in the APK — refusing to report 0 as a denominator",
              file=sys.stderr)
        sys.exit(1)
    if len(sys.argv) == 2:
        for ep in sorted(total):
            print(f"{ep} {len(total[ep])}")
        sys.exit(0)
    seen = opened(sys.argv[2])
    for ep in sorted(seen):
        print(f"  episode {ep}: {len(seen[ep])} of {len(total.get(ep, ()))} level files opened")
    never = sorted(set(total) - set(seen))
    print(f"  episodes never entered: {len(never)}" + (f" ({', '.join(never[:4])}…)" if never else ""))
