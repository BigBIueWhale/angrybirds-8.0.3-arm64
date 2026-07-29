#!/usr/bin/env python3
"""patch_minsdk.py — set minSdkVersion / targetSdkVersion in a binary AndroidManifest.xml.

NOW COMMITTED (it was not). This file produced the evidence in R18's minSdk table and existed only
in an ephemeral /tmp scratchpad, which meant R18's experiment could not be re-run by anyone — the same
defect cont.128 fixed for the whole validation rig, recurring for one tool. Evidence whose instrument
is unreachable is a claim, not a measurement.

DIAGNOSTIC ONLY (R18). On API 36.1 this app exposes no launchable activity, while on API 36 it does;
the original untouched Rovio manifest behaves the same way, so it is not our rewrite. The leading
hypothesis is a platform policy against minSdk=16 / targetSdk=26. This patches those two integers in
place so the hypothesis can be A/B'd instead of argued.

Patches the typed value in the attribute record, which is a fixed 20-byte layout:
    ns(4) name(4) rawValue(4) size(2) res0(1) dataType(1) data(4)
so `data` sits at attr+16 and no string-pool offsets move. Same principle as depermission.py's
same-length rename: change values, never lengths.

    python3 patch_minsdk.py in.xml out.xml <minSdk> <targetSdk>
"""
import sys, struct

STRING_POOL, START_ELEMENT = 0x0001, 0x0102


def parse_string_pool(buf, off):
    _t, hdr, _size = struct.unpack_from("<HHI", buf, off)
    count, _sc, flags, sstart, _ss = struct.unpack_from("<IIIII", buf, off + 8)
    offs = struct.unpack_from("<%dI" % count, buf, off + hdr)
    utf8 = bool(flags & (1 << 8))
    base = off + sstart
    out = []
    for o in offs:
        p = base + o
        if utf8:
            n = buf[p]
            p += 2 if n & 0x80 else 1          # skip char-count varint
            n = buf[p]
            if n & 0x80:
                n = ((n & 0x7F) << 8) | buf[p + 1]; p += 2
            else:
                p += 1
            out.append(buf[p:p + n].decode("utf-8", "replace"))
        else:
            n = struct.unpack_from("<H", buf, p)[0]; p += 2
            out.append(buf[p:p + n * 2].decode("utf-16-le", "replace"))
    return out


def main(src, dst, minsdk, targetsdk):
    buf = bytearray(open(src, "rb").read())
    _t, hdr, _sz = struct.unpack_from("<HHI", buf, 0)
    strings, off, changed = None, hdr, []
    while off < len(buf):
        typ, hdr_sz, size = struct.unpack_from("<HHI", buf, off)
        if size == 0:
            break
        if typ == STRING_POOL and strings is None:
            strings = parse_string_pool(buf, off)
        elif typ == START_ELEMENT and strings:
            name_ix = struct.unpack_from("<I", buf, off + hdr_sz + 4)[0]
            elem = strings[name_ix] if name_ix < len(strings) else ""
            if elem == "uses-sdk":
                a_start, a_size, a_count = struct.unpack_from("<HHH", buf, off + hdr_sz + 8)
                for i in range(a_count):
                    a = off + hdr_sz + a_start + i * a_size
                    an = struct.unpack_from("<I", buf, a + 4)[0]
                    attr = strings[an] if an < len(strings) else ""
                    old = struct.unpack_from("<I", buf, a + 16)[0]
                    if attr == "minSdkVersion":
                        struct.pack_into("<I", buf, a + 16, minsdk); changed.append(f"minSdkVersion {old} -> {minsdk}")
                    elif attr == "targetSdkVersion":
                        struct.pack_into("<I", buf, a + 16, targetsdk); changed.append(f"targetSdkVersion {old} -> {targetsdk}")
        off += size
    if not changed:
        print("patch_minsdk: found no uses-sdk attributes to patch — refusing to write an "
              "unchanged file that would look patched", file=sys.stderr)
        return 1
    open(dst, "wb").write(bytes(buf))
    for c in changed:
        print(f"  {c}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr); sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])))
