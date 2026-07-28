#!/usr/bin/env python3
"""check_script_paths.py — one implementation of the script_paths.json guard, for all six builds.

    python3 port/check_script_paths.py <manifest.json> <assets-root>

WHY THIS EXISTS
---------------
`data/script_paths.json` is the level-script VFS manifest, and the ONE asset this port adds to the
game. It is not bundled in Rovio's APK (it is normally staged at runtime), and without it
`AAssetManager_open('data/script_paths.json')` fails, the loader maps no script, the engine hits a
JSON ParseError and HANGS at boot. That failure has happened; see `port/ONDEVICE.md`.

It is also **build-generated**, which puts it in a different class from the game's assets: those are
covered by `prepare_inputs.sh`'s sha256 gate on the input APK and cannot drift. This file can. So it
has to be checked where it is produced.

WHAT THE PREVIOUS GUARD MISSED
------------------------------
It was inline in each build script and asserted only that the JSON parses, is non-empty, and that
`items[:50]` are strings. Two problems:

  * `items[:50]` of 2035 is a sample, not a check.
  * Nothing detected INCOMPLETENESS. A half-generated manifest parses, is non-empty and looks like
    paths, while leaving levels unreachable — the same symptom as the missing file it guards against.

So this checks both directions against the staged tree: every listed path must exist, and every
`.lua` under the three trees `gen_script_paths.py` walks (`scripts`, `levels`, `vehicles`) must be
listed. Files outside those trees — `data/blocks/*`, image loadlists — are correctly absent, because
that is the generator's specification, not an oversight.

ONE FILE, SIX CALLERS. It was pasted into all six build scripts, and the moment `build_apk.sh` was
strengthened the other five silently kept the weak version — the exact drift this project has
already consolidated `prepare_inputs.sh`, `lib_settle.sh` and `lib_install.sh` to avoid.
"""
import json
import os
import sys

BASES = ("scripts", "levels", "vehicles")     # exactly what gen_script_paths.py walks


def main(manifest, assets_root):
    try:
        d = json.load(open(manifest))
    except Exception as e:                     # noqa: BLE001 — any parse failure is fatal here
        print("script_paths.json is unparseable: %s" % e, file=sys.stderr)
        return 1

    if not isinstance(d, (list, dict)) or len(d) == 0:
        print("script_paths.json is empty or the wrong container type", file=sys.stderr)
        return 1

    # The real file is a LIST of asset paths, not an object. A first version of this guard asserted
    # isinstance(d, dict) and would have failed EVERY build — caught only by running it against the
    # real generated file rather than the shape assumed from a "{}" bootstrap placeholder that was
    # long superseded. Accept either container.
    items = d if isinstance(d, list) else list(d)
    if not all(isinstance(x, str) for x in items):
        print("script_paths.json contains a non-string entry", file=sys.stderr)
        return 1

    listed = set(items)
    have = set()
    for base in BASES:
        root = os.path.join(assets_root, "data", base)
        for dirpath, _dirnames, filenames in os.walk(root):
            for f in filenames:
                if f.endswith(".lua"):
                    rel = os.path.relpath(os.path.join(dirpath, f), assets_root)
                    have.add(rel.replace(os.sep, "/"))

    if not have:
        # Guards that count must assert their input exists, or "nothing missing" is indistinguishable
        # from "nothing looked at" — the failure this project has hit repeatedly.
        print("no .lua files found under %s/data/{%s} — wrong assets root, so nothing was checked"
              % (assets_root, ",".join(BASES)), file=sys.stderr)
        return 1

    missing = have - listed            # a level the engine cannot reach
    dangling = listed - have           # a path that resolves to nothing
    if missing or dangling:
        print("script_paths.json is INCOMPLETE: %d unlisted, %d dangling"
              % (len(missing), len(dangling)), file=sys.stderr)
        for x in sorted(missing)[:5]:
            print("   unlisted: %s" % x, file=sys.stderr)
        for x in sorted(dangling)[:5]:
            print("   dangling: %s" % x, file=sys.stderr)
        return 1

    print("   script_paths.json: %d entries, complete against %s" % (len(items), "/".join(BASES)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check_script_paths.py <manifest.json> <assets-root>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
