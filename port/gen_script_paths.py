#!/usr/bin/env python3
"""Generate data/script_paths.json — the level-script VFS manifest the Fusion engine's scene loader
reads (AAssetManager_open('data/script_paths.json'), imported into a Lua table, looked up by
findScriptPath(name)). It is NOT bundled in the APK (normally runtime-staged), so we synthesise it
from the APK's own scripts. The engine reads it as PLAINTEXT JSON.

Schema is a flat string->string map. Because the exact key convention is not yet pinned, we emit
several key variants per script (all pointing at the real relative path), so a lookup by any of
{full "data/scripts/.../X.lua" path, basename "X", "X.lua", subpath "sub/X"} resolves.

Usage: gen_script_paths.py <assets_root>   # prints JSON to stdout
"""
import os, json, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
scripts_dir = os.path.join(root, "data", "scripts")

# --- VARIANT: JSON ARRAY of all .lua paths ------------------------------------------------------
# Hypothesis (UPDATE 96): the consumer calls root.asArray() -> both `{}` and an object throw
# util::JSON::BadType; a JSON array does not. Emit an array of every bundled .lua relative path
# (scripts + levels + vehicles). Toggle with SP_SHAPE=array|object (default array now).
if os.environ.get("SP_SHAPE", "array") == "array":
    data_dir = os.path.join(root, "data")
    arr = []
    for base in ("scripts", "levels", "vehicles"):
        d = os.path.join(data_dir, base)
        if not os.path.isdir(d):
            continue
        for dp, _dn, fn in os.walk(d):
            for f in fn:
                if f.endswith(".lua"):
                    arr.append(os.path.relpath(os.path.join(dp, f), root).replace("\\", "/"))
    arr.sort()
    print(json.dumps(arr, separators=(",", ":")))
    sys.exit(0)
# --- end array variant --------------------------------------------------------------------------

# The Fusion scene loader reads base-path keys from this table (engine .rodata cluster:
# fontPath/audioPath/localizationPath/levelPath/scriptPath/commonScriptPath/imagePath/...).
# findScriptPath(name) builds a full path from these. Empty `{}` -> lookup returns nil -> JSON BadType.
m = {
    "scriptPath": "data/scripts/",
    "commonScriptPath": "data/scripts/",
    "levelPath": "data/levels/",
    "fontPath": "data/fonts/",
    "audioPath": "data/audio/",
    "imagePath": "data/images/",
    "localizationPath": "data/localization/",
}

# Plus an explicit name->path map for every bundled script, under several key variants, so a lookup
# by full path / basename / basename.lua / subpath all resolve regardless of the exact convention.
if os.path.isdir(scripts_dir):
    for dp, _dn, fn in os.walk(scripts_dir):
        for f in fn:
            if not f.endswith(".lua"):
                continue
            full = os.path.relpath(os.path.join(dp, f), root).replace("\\", "/")   # data/scripts/.../X.lua
            base = f[:-4]                                                          # X
            sub = os.path.relpath(os.path.join(dp, f), scripts_dir).replace("\\", "/")[:-4]  # sub/X
            for k in (full, base, f, sub):
                m.setdefault(k, full)
print(json.dumps(m, separators=(",", ":")))
