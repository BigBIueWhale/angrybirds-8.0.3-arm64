#!/usr/bin/env python3
"""check_killswitches.py — de-phone-home layer 4: the SDK auto-init flags must be present AND false.

    python3 check_killswitches.py <shipped-AndroidManifest.xml>

WHY THIS EXISTS
---------------
`verify_claims.sh` checked layer 4 with `strings | grep -q <flag-name>`: it confirmed the flag NAME
appears in the manifest. The flag's value is an AXML integer, not a pool string, so `grep` cannot see
it — meaning an injector bug that wrote `true` would have passed this gate while leaving FCM
auto-init switched on. The check confirmed the presence of a switch, not its position.

The layer it guards is the one the missing INTERNET permission cannot cover: FCM registration is
performed by Google Play Services *for* the app, so it survives layer 1. That makes "is it actually
false" the whole claim.

The flag list is read from `manifest_firebase_off.py`'s own `FLAGS`, so adding a kill-switch there
extends this check automatically. With `ast`, not by importing — the sibling checker learned that
importing a build script executes it.

Refuses rather than passes when the manifest yields no `meta-data` elements at all, since "none of
the flags are wrong" would otherwise be indistinguishable from "nothing was parsed".
"""
import ast
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from axml_sdk import parse_string_pool, CHUNK_STRING_POOL, CHUNK_START_TAG  # noqa: E402

TYPE_STRING = 0x03
TYPE_INT_BOOLEAN = 0x12


def metadata(path):
    """Every <meta-data name=… value=…> in the manifest, as {name: value}."""
    buf = open(path, "rb").read()
    strings, out = [], {}
    off = struct.unpack_from("<H", buf, 2)[0]
    while off < len(buf) - 8:
        typ, _hdr, size = struct.unpack_from("<HHI", buf, off)
        if size == 0:
            break
        if typ == CHUNK_STRING_POOL and not strings:
            strings = parse_string_pool(buf, off)
        elif typ == CHUNK_START_TAG and strings:
            _ns, name, a_start, a_size, a_count = struct.unpack_from("<IIHHH", buf, off + 16)
            if name < len(strings) and strings[name] == "meta-data":
                base, attrs = off + 16 + a_start, {}
                for i in range(a_count):
                    p = base + i * a_size
                    _ans, aname, _raw = struct.unpack_from("<III", buf, p)
                    _sz, _res0, dtype = struct.unpack_from("<HBB", buf, p + 12)
                    data = struct.unpack_from("<I", buf, p + 16)[0]
                    key = strings[aname] if aname < len(strings) else "?"
                    if dtype == TYPE_STRING and data < len(strings):
                        attrs[key] = strings[data]
                    elif dtype == TYPE_INT_BOOLEAN:
                        attrs[key] = bool(data)
                    else:
                        attrs[key] = data
                if "name" in attrs:
                    out[attrs["name"]] = attrs.get("value")
        off += size
    return out


def load_flags():
    p = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "manifest_firebase_off.py")
    tree = ast.parse(open(p, encoding="utf-8").read(), filename=p)
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id == "FLAGS":
                    return ast.literal_eval(node.value)
    raise SystemExit("no FLAGS list in %s" % p)


def main(manifest):
    md = metadata(manifest)
    if not md:
        print("  [FAIL] no <meta-data> parsed from the manifest — nothing was checked", file=sys.stderr)
        return 1
    flags = load_flags()
    bad = []
    for name, want_int in flags:
        if name not in md:
            bad.append("kill-switch MISSING: %s" % name)
            continue
        got = md[name]
        want = bool(want_int)
        if bool(got) is not want:
            bad.append("kill-switch %s is %r, expected %r — the switch is present but in the wrong "
                       "position" % (name, got, want))
    print("  layer-4 kill-switches: %d declared in manifest_firebase_off.py, %d <meta-data> in the "
          "manifest" % (len(flags), len(md)))
    for b in bad:
        print("  [FAIL] %s" % b, file=sys.stderr)
    if bad:
        return 1
    print("  [ OK ] all %d kill-switches present AND false: %s"
          % (len(flags), ", ".join(n for n, _ in flags)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_killswitches.py <shipped-AndroidManifest.xml>")
    sys.exit(main(sys.argv[1]))
