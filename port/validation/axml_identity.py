#!/usr/bin/env python3
"""axml_identity.py — read package name + versionName from a binary AndroidManifest.xml.

Why not `strings`: a first attempt grabbed the first x.y.z-looking string in the pool and got
"25.3.1" — a bundled library's version, not the app's. It compared equal between the original and
the converted APK, so the check PASSED while measuring the wrong thing. The pool has no ordering
guarantee; identity has to come from the <manifest> element's attributes, resolved through the
resource map exactly as axml_sdk.py resolves minSdk/targetSdk.
"""
import struct, sys
CHUNK_STRING_POOL, CHUNK_RES_MAP, CHUNK_START_TAG = 0x0001, 0x0180, 0x0102
ATTR_VERSION_NAME = 0x0101021C
TYPE_STRING = 0x03

def parse_string_pool(buf, off):
    _t, hdr, size = struct.unpack_from("<HHI", buf, off)
    count, _sc, flags, sstart, _ss = struct.unpack_from("<IIIII", buf, off + 8)
    utf8 = (flags & (1 << 8)) != 0
    offs = struct.unpack_from("<%dI" % count, buf, off + hdr)
    base = off + sstart
    out = []
    for o in offs:
        p = base + o
        if utf8:
            n = buf[p]; p += 1
            if n & 0x80: n = ((n & 0x7F) << 8) | buf[p]; p += 1
            m = buf[p]; p += 1
            if m & 0x80: m = ((m & 0x7F) << 8) | buf[p]; p += 1
            out.append(buf[p:p+m].decode("utf-8", "replace"))
        else:
            n = struct.unpack_from("<H", buf, p)[0]; p += 2
            out.append(buf[p:p+n*2].decode("utf-16-le", "replace"))
    return out, size

def main(path):
    buf = open(path, "rb").read()
    off, strings, resmap = 8, [], []
    pkg = ver = None
    while off + 8 <= len(buf):
        typ, hdr_sz, size = struct.unpack_from("<HHI", buf, off)
        if size <= 0: break
        if typ == CHUNK_STRING_POOL and not strings:
            strings, _ = parse_string_pool(buf, off)
        elif typ == CHUNK_RES_MAP:
            n = (size - hdr_sz) // 4
            resmap = list(struct.unpack_from("<%dI" % n, buf, off + hdr_sz))
        elif typ == CHUNK_START_TAG:
            name_ix = struct.unpack_from("<I", buf, off + hdr_sz + 4)[0]
            tag = strings[name_ix] if name_ix < len(strings) else ""
            if tag == "manifest":
                a_start, a_size, a_count = struct.unpack_from("<HHH", buf, off + hdr_sz + 8)
                for i in range(a_count):
                    a = off + hdr_sz + a_start + i * a_size
                    _ns, an = struct.unpack_from("<II", buf, a)
                    raw = struct.unpack_from("<I", buf, a + 8)[0]
                    dtype, data = struct.unpack_from("<BI", buf, a + 15)
                    aname = strings[an] if an < len(strings) else ""
                    rid = resmap[an] if an < len(resmap) else 0
                    if aname == "package" and raw < len(strings):
                        pkg = strings[raw]
                    elif rid == ATTR_VERSION_NAME and dtype == TYPE_STRING and raw < len(strings):
                        ver = strings[raw]
        off += size
    if not strings: sys.exit("not parseable AXML")
    if not pkg or not ver: sys.exit(f"could not resolve identity (pkg={pkg} ver={ver})")
    print(f"{pkg} {ver}")

main(sys.argv[1])
