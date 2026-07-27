#!/usr/bin/env python3
"""axml_sdk.py — read minSdkVersion / targetSdkVersion straight out of a binary AndroidManifest.xml.

WHY THIS EXISTS
---------------
verify_claims.sh checks that the artifact is minSdk 16 / targetSdk 26 — the values that let the APK
install on the A56 (targetSdk 26 is what makes modern Android show the one-time "built for an older
version" dialog and then install, rather than refuse). That check used androguard in the ab-analyze
image, which meant it could only run from a host with docker. But the script's documented invocation
is INSIDE the ab-port container (REPRODUCE.md step 3), where there is no docker socket — so in the
one mode that is actually documented, the check silently skipped every time.

Everything else in verify_claims.sh is self-contained. This makes the last one self-contained too,
using only the stdlib. AXML is a small, stable, well-specified format; parsing the two integers we
need is far less machinery than requiring a second container image.

Usage:  python3 axml_sdk.py <AndroidManifest.xml>     (binary AXML, as unzipped from the APK)
Prints: "RESULT <minSdk> <targetSdk>"  — either value is -1 if the attribute is absent.
Exits non-zero if the file is not parseable AXML, so a malformed manifest fails loudly rather than
reporting a plausible-looking -1.
"""
import struct
import sys

RES_XML_TYPE = 0x0003
CHUNK_STRING_POOL = 0x0001
CHUNK_RESOURCE_MAP = 0x0180
CHUNK_START_TAG = 0x0102

ATTR_MIN_SDK = 0x0101020C
ATTR_TARGET_SDK = 0x01010270
TYPE_INT_DEC = 0x10


def parse_string_pool(buf, off):
    """Return the pool's string list. Only used to locate <uses-sdk>."""
    _typ, hdr_sz, _size = struct.unpack_from("<HHI", buf, off)
    count, _style_cnt, flags, str_start, _style_start = struct.unpack_from("<IIIII", buf, off + 8)
    utf8 = bool(flags & (1 << 8))
    offs = struct.unpack_from("<%dI" % count, buf, off + hdr_sz)
    base = off + str_start
    out = []
    for o in offs:
        p = base + o
        if utf8:
            # two varint lengths (utf16 len, then byte len); high bit means 2-byte varint
            n = buf[p]; p += 2 if n & 0x80 else 1
            n = buf[p]
            if n & 0x80:
                n = ((n & 0x7F) << 8) | buf[p + 1]; p += 2
            else:
                p += 1
            out.append(buf[p:p + n].decode("utf-8", "replace"))
        else:
            n = struct.unpack_from("<H", buf, p)[0]; p += 2
            if n & 0x8000:  # long form
                n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", buf, p)[0]; p += 2
            out.append(buf[p:p + n * 2].decode("utf-16-le", "replace"))
    return out


def main(path):
    with open(path, "rb") as f:
        buf = f.read()
    if len(buf) < 8 or struct.unpack_from("<H", buf, 0)[0] != RES_XML_TYPE:
        sys.exit("not a binary AndroidManifest.xml (bad magic): %s" % path)

    strings, resmap = [], []
    off = struct.unpack_from("<H", buf, 2)[0]          # skip the file header
    minsdk = targetsdk = -1

    while off + 8 <= len(buf):
        typ, hdr_sz, size = struct.unpack_from("<HHI", buf, off)
        if size < 8 or off + size > len(buf):
            break
        if typ == CHUNK_STRING_POOL:
            strings = parse_string_pool(buf, off)
        elif typ == CHUNK_RESOURCE_MAP:
            n = (size - hdr_sz) // 4
            resmap = list(struct.unpack_from("<%dI" % n, buf, off + hdr_sz))
        elif typ == CHUNK_START_TAG:
            name_ix = struct.unpack_from("<I", buf, off + hdr_sz + 4)[0]
            tag = strings[name_ix] if name_ix < len(strings) else ""
            if tag == "uses-sdk":
                a_start, a_size, a_count = struct.unpack_from("<HHH", buf, off + hdr_sz + 8)
                for i in range(a_count):
                    a = off + hdr_sz + a_start + i * a_size
                    _ns, an = struct.unpack_from("<II", buf, a)
                    dtype, data = struct.unpack_from("<BI", buf, a + 15)
                    # attribute identity comes from the RESOURCE MAP (name index -> resource id),
                    # not from the attribute's string, which is often empty in a stripped manifest
                    rid = resmap[an] if an < len(resmap) else 0
                    if dtype == TYPE_INT_DEC:
                        if rid == ATTR_MIN_SDK:
                            minsdk = data
                        elif rid == ATTR_TARGET_SDK:
                            targetsdk = data
        off += size

    if not strings:
        sys.exit("no string pool found — manifest is not parseable AXML")
    print("RESULT %d %d" % (minsdk, targetsdk))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: axml_sdk.py <AndroidManifest.xml>")
    main(sys.argv[1])
