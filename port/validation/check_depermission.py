#!/usr/bin/env python3
"""check_depermission.py — assert the shipped manifest strips EVERY permission depermission.py claims
to strip, derived from depermission.py itself rather than from a hand-written subset.

    python3 check_depermission.py <shipped-AndroidManifest.xml> <original-AndroidManifest.xml>

WHY THIS EXISTS
---------------
`verify_claims.sh` used to check six permission names, chosen by hand. The manifest actually carries
**ten** that get mangled, so five were ungated — `com.android.vending.INSTALL_REFERRER`, the c2dm
`REGISTRATION` and `SEND` permissions, the Play `BIND_GET_INSTALL_REFERRER_SERVICE`, and the app's own
`C2D_MESSAGE`. A regression that failed to strip any of those would have passed the gate.

That list was not wrong by accident: it matched OPEN_FINDINGS R5's table, which listed the same
subset and also claimed there was nothing at `dangerous` protection level while
`WRITE_EXTERNAL_STORAGE` sat in the manifest. A hand-copied list and a hand-written table drifted
from the artifact together, which is exactly what a derived check prevents.

One of the six was worse than incomplete: `android.permission.ACCESS_WIFI_STATE` is not in Rovio's
manifest at all, so asserting its absence could never fail. A check that cannot fail is not evidence,
and it inflates the count of things that look verified.

WHAT IT CHECKS
--------------
For every (original -> mangled) pair in `depermission.py`'s own `STRIP` map:

  * if the original name is in the ORIGINAL manifest, then in the shipped manifest the original must
    be ABSENT and the mangled form PRESENT — both directions, because "the string is gone" alone is
    equally consistent with reading the wrong file;
  * if the original name is NOT in the original manifest, it is reported as not-applicable rather
    than silently counted as a pass. Vacuous passes are how six checks looked like coverage.

It reads the AXML string pool with the repo's own parser. (An earlier attempt at this decoded the
pool as UTF-16 and found zero permissions — a wrong encoding assumption reads exactly like a clean
manifest, so the parser is shared rather than re-improvised.)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

import struct                                            # noqa: E402
from axml_sdk import parse_string_pool, CHUNK_STRING_POOL  # noqa: E402


def pool(path):
    buf = open(path, "rb").read()
    off = struct.unpack_from("<H", buf, 2)[0]
    while off < len(buf) - 8:
        typ, _hdr, size = struct.unpack_from("<HHI", buf, off)
        if size == 0:
            break
        if typ == CHUNK_STRING_POOL:
            return set(parse_string_pool(buf, off))
        off += size
    return set()


def load_strip():
    """depermission.py's STRIP map is the authority on what is supposed to be stripped.

    Read with `ast`, NOT imported. depermission.py does real work at module level — importing it
    ran its own guard and aborted with "depermission neutralised NOTHING", because it was being
    executed with no manifest to process. A checker that has side effects on the thing it checks is
    worse than no checker; parsing the literal is both safe and sufficient."""
    import ast
    p = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "depermission.py")
    tree = ast.parse(open(p, encoding="utf-8").read(), filename=p)
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id == "STRIP":
                    return ast.literal_eval(node.value)
    raise SystemExit("could not find a STRIP map in %s" % p)


def main(shipped_path, original_path):
    shipped, original = pool(shipped_path), pool(original_path)
    if not shipped or not original:
        print("  [FAIL] a manifest string pool came back empty — nothing was checked", file=sys.stderr)
        return 1
    strip = load_strip()

    checked = na = 0
    bad = []
    for orig, mangled in sorted(strip.items()):
        if orig not in original:
            na += 1
            continue
        checked += 1
        if orig in shipped:
            bad.append("STILL LIVE in the shipped manifest: %s" % orig)
        if mangled not in shipped:
            bad.append("mangled form missing (so the strip did not run on it): %s" % mangled)

    print("  depermission STRIP entries: %d total, %d present in Rovio's manifest and checked, "
          "%d not applicable to this APK" % (len(strip), checked, na))
    if checked == 0:
        print("  [FAIL] none of the STRIP entries were found in the original manifest — wrong file, "
              "so this proved nothing", file=sys.stderr)
        return 1
    for b in bad:
        print("  [FAIL] %s" % b, file=sys.stderr)
    if bad:
        return 1
    print("  [ OK ] all %d stripped permissions are absent in live form AND present mangled" % checked)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check_depermission.py <shipped-manifest> <original-manifest>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
