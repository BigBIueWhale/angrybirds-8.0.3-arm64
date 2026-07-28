#!/usr/bin/env python3
"""Strip network / tracking permissions from a binary AndroidManifest.xml (AXML) by
renaming the permission strings IN PLACE to a same-length invalid name, so Android
silently declines to grant them. Bulletproof 'remove all phone-homes': with no INTERNET
permission the process cannot open any socket -> every ad/analytics/cloud call fails at
the OS level. Same byte length => no AXML string-pool offset fixups needed.

Usage: depermission.py <in_AndroidManifest.xml> <out_AndroidManifest.xml>
"""
import sys
# old -> new (must be identical length); flip the first char of the leaf name to kill it
STRIP = {
    "android.permission.INTERNET":               "android.permission.XNTERNET",
    "android.permission.ACCESS_NETWORK_STATE":   "android.permission.XCCESS_NETWORK_STATE",
    "android.permission.ACCESS_WIFI_STATE":      "android.permission.XCCESS_WIFI_STATE",
    "android.permission.CHANGE_WIFI_STATE":      "android.permission.XHANGE_WIFI_STATE",
    "android.permission.READ_PHONE_STATE":       "android.permission.XEAD_PHONE_STATE",
    "android.permission.ACCESS_COARSE_LOCATION": "android.permission.XCCESS_COARSE_LOCATION",
    "android.permission.ACCESS_FINE_LOCATION":   "android.permission.XCCESS_FINE_LOCATION",
    "com.android.vending.CHECK_LICENSE":         "com.android.vending.XHECK_LICENSE",
    "com.android.vending.BILLING":               "com.android.vending.XILLING",
    "android.permission.GET_ACCOUNTS":           "android.permission.XET_ACCOUNTS",
    # push / cloud-messaging (GCM/C2DM) — dead without INTERNET, but strip the whole family
    "com.google.android.c2dm.permission.RECEIVE":     "com.google.android.c2dm.permission.XECEIVE",
    "com.google.android.c2dm.permission.SEND":        "com.google.android.c2dm.permission.XEND",
    "com.google.android.c2dm.permission.REGISTRATION":"com.google.android.c2dm.permission.XEGISTRATION",
    "com.google.android.c2dm.intent.RECEIVE":         "com.google.android.c2dm.intent.XECEIVE",
    "com.rovio.angrybirds.permission.C2D_MESSAGE":    "com.rovio.angrybirds.permission.X2D_MESSAGE",
    # Play install-referrer attribution (a tracking phone-home) — service + broadcast action
    "com.google.android.finsky.permission.BIND_GET_INSTALL_REFERRER_SERVICE":
        "com.google.android.finsky.permission.XIND_GET_INSTALL_REFERRER_SERVICE",
    "com.android.vending.INSTALL_REFERRER":      "com.android.vending.XNSTALL_REFERRER",
}
data = open(sys.argv[1], "rb").read()
total = 0
killed = {}
for old, new in STRIP.items():
    assert len(old) == len(new), old
    for enc in ("utf-8", "utf-16-le"):            # AXML string pool is UTF-8 or UTF-16LE
        ob, nb = old.encode(enc), new.encode(enc)
        c = data.count(ob)
        if c:
            data = data.replace(ob, nb)
            killed[old] = killed.get(old, 0) + c
            print(f"  killed {old.split('.')[-1]:26} ({enc}, x{c})")
            total += c
# FAIL LOUDLY IF NOTHING WAS NEUTRALISED. This used to write the file and report
# "done: 0 permission reference(s) neutralised" as though that were a result. If the input APK's
# manifest ever changed shape — different encoding, a repacked string pool, a wrong path argument —
# the build would sail on and emit a NETWORK-CAPABLE APK, with the whole de-phone-home guarantee
# resting on someone noticing a zero. verify_claims.sh does catch it downstream (it re-reads the
# shipped manifest for live permissions), but a build that silently produces the opposite of what it
# claims should not get as far as the gate.
#
# MUST_KILL is the subset the guarantee actually depends on: INTERNET is what makes every socket
# fail at the OS level. The rest of STRIP is defence in depth and may legitimately be absent from a
# given APK, so only this one is required — asserting on all of them would break on a manifest that
# simply never declared, say, CHANGE_WIFI_STATE.
MUST_KILL = "android.permission.INTERNET"
if total == 0:
    sys.exit("FATAL: depermission neutralised NOTHING — the manifest is not the expected AXML, "
             "or the wrong file was passed. Refusing to emit a network-capable APK.")
if not killed.get(MUST_KILL):
    sys.exit(f"FATAL: {MUST_KILL} was not found in the manifest, so it was not neutralised. "
             "Either the input changed or the string pool encoding is unhandled; refusing to "
             "continue, because no-INTERNET is the layer every other de-phone-home layer rests on.")
open(sys.argv[2], "wb").write(data)
print(f"done: {total} permission reference(s) neutralised ({len(killed)} distinct)")
