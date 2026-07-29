#!/usr/bin/env python3
"""patch_emulator_arm64.py — make emulator 36.6.11's qemu-system-aarch64 able to start a ranchu
arm64 machine on an x86_64 host.

WHY
---
The Android emulator LAUNCHER refuses an arm64 AVD on an x86_64 host, but the engine underneath does
not: it accepts `Target arch = 'arm64'`, assembles a full ranchu machine and starts the QEMU main
loop. It then dies because its own generated argv contains devices that need a PCI bus, and
ranchu-arm64 has none:

    qemu-system-aarch64: PCI bus not available for hda

Three of the four blockers are fixable from OUTSIDE the binary and are handled in
`port/validation/arm64_real_run.sh`, which is where they belong:

    11x virtio_input_multi_touch_pci_N   AVD: hw.screen=touch
    virtio-wifi-pci                      emulator flag: -feature -VirtioWifi
    SIGSEGV in setupSubWindow            flags: -no-skin -qt-hide-window, showDeviceFrame=no

The audio device is the one that cannot. Verified rather than assumed: the AVD's
hw.audioInput/hw.audioOutput=no ARE honoured — the generated argument is literally
`hda:input=off,output=off` — and the device is still added. `-no-audio` and `-audio none` do not
remove it either. The emulator builds that argv in-process, so there is no command line to edit.

WHAT THIS PATCHES, AND WHY IT IS THE SMALLEST POSSIBLE CHANGE
    ONE 8-byte option token: "-soundhw" -> "-pidfile".

    Exactly the same length, so nothing moves and no offsets shift. QEMU's `-pidfile` takes one
    argument and validates nothing about its contents, which is what makes it a safe sink for
    `hda:input=off,output=off`. The option is consumed harmlessly instead of trying to attach an
    Intel HDA codec to a machine with no PCI bus. The side effect is one stray file in the working
    directory named after the argument.

    `-name` was tried first and is WRONG: it parses comma-separated parameters, so it rejected the
    same argument with
        qemu-system-aarch64: -name hda:input=off,output=off: Invalid parameter 'hda:input'
    That is why the target is -pidfile and not something shorter.

WHAT THIS DELIBERATELY DOES *NOT* PATCH
    The `virtio_input_multi_touch_pci_N` literals. They are the registered device TYPE names, and
    this binary ALSO registers MMIO types called `virtio_input_multi_touch_N`; renaming the PCI ones
    would collide with types that already exist. Patching the `..._pci_%d` format string was tried
    and had no effect — the argv is built from the individual type-name literals, not that format —
    which is why the AVD route is used for those instead.

    python3 patch_emulator_arm64.py <qemu-system-aarch64> <output>
"""
import sys, os, hashlib

# The standalone, null-terminated option token. NOT the copies inside the help text, which read
# "-soundhw c1,... enable audio support" — patching one of those would change a usage message and
# nothing else, and the run would fail identically.
TOKEN = b"-soundhw\x00"
REPLACEMENT = b"-pidfile"


def find_token(data):
    """Offsets of the standalone token, i.e. preceded by a NUL so it is its own C string."""
    hits, i = [], data.find(TOKEN)
    while i != -1:
        if i > 0 and data[i - 1] == 0:
            hits.append(i)
        i = data.find(TOKEN, i + 1)
    return hits


def main(src, dst):
    data = bytearray(open(src, "rb").read())
    hits = find_token(data)
    if len(hits) != 1:
        print(f"  [FAIL] expected exactly one standalone '-soundhw' token, found {len(hits)}: "
              f"{[hex(h) for h in hits]}", file=sys.stderr)
        print("         Refusing to guess which one builds the argv — a wrong choice here produces "
              "a binary that fails\n         exactly like the original and looks patched.", file=sys.stderr)
        return 1
    off = hits[0]
    assert len(REPLACEMENT) == len(TOKEN) - 1, "replacement must be the same length as the token"
    data[off:off + len(REPLACEMENT)] = REPLACEMENT
    open(dst, "wb").write(bytes(data))
    os.chmod(dst, 0o755)

    out = open(dst, "rb").read()
    src_b = open(src, "rb").read()
    changed = [i for i in range(len(src_b)) if src_b[i] != out[i]]
    print(f"  patched {os.path.basename(src)} -> {os.path.basename(dst)}")
    print(f"    offset      : {hex(off)}")
    print(f"    token       : '-soundhw' -> '{REPLACEMENT.decode()}'")
    print(f"    bytes changed: {len(changed)} ({[hex(c) for c in changed]})")
    print(f"    sha256 in   : {hashlib.sha256(src_b).hexdigest()}")
    print(f"    sha256 out  : {hashlib.sha256(out).hexdigest()}")
    if find_token(out):
        print("  [FAIL] the token is still present after patching", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
