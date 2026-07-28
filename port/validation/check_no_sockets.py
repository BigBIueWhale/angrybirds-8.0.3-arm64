#!/usr/bin/env python3
"""check_no_sockets.py — the shim must import NO network-capable symbol, checked against everything
it imports rather than against a handful of names, and with a positive control so a broken scan
cannot report success.

    python3 check_no_sockets.py <shim.so> <control.so>

WHY THIS EXISTS
---------------
`verify_claims.sh` asserted this by looping over six names: socket, connect, sendto, recvfrom,
getaddrinfo, gethostbyname. A shim that imported `send`, `bind`, `socketpair`, `setsockopt` or
`inet_addr` would have passed. That is the same defect that left five stripped permissions ungated —
a hand-written list standing in for the property it is meant to establish.

The property is "of everything this binary imports, none can reach the network", so it is now checked
that way: enumerate every UND symbol and match the whole set against the network families.

WHY A CONTROL IS PART OF THE CHECK
----------------------------------
An empty result is exactly what a broken scan produces. This was not hypothetical here: the first
version of this scan was written as `grep -EiX '(socket|connect|...)'`, and GNU grep's `-X` takes an
ARGUMENT — it swallowed the pattern as a matcher-type name, so the command matched nothing and
"0 network-capable imports" looked like a clean result. It was a malformed command.

So the control binary must show matches. Rovio's original 32-bit engine imports 18 of them
(bind, connect, getaddrinfo, getpeername, inet_addr, if_nametoindex, …), which is unsurprising for a
game that phoned home — and makes it a perfect control: if the scan reports 0 for the control, the
scan is broken and the shim's 0 means nothing.
"""
import re
import subprocess
import sys

# The families a binary would need to touch a network. Deliberately broad: the cost of a false
# positive is one investigation, the cost of a false negative is shipping a socket capability.
NET = re.compile(r"""^(
    socket|socketpair|connect|bind|listen|accept[0-9]?|shutdown
  | send|sendto|sendmsg|sendmmsg|recv|recvfrom|recvmsg|recvmmsg
  | setsockopt|getsockopt|getsockname|getpeername
  | getaddrinfo|freeaddrinfo|gai_strerror|getnameinfo
  | gethostbyname[0-9a-z_]* | gethostbyaddr[0-9a-z_]*
  | inet_[a-z0-9_]+ | if_[a-z0-9_]+ | res_[a-z0-9_]+ | ns_[a-z0-9_]+
  | android_get[a-z]* | sendfile[0-9]*
)$""", re.X | re.I)


def undefined_symbols(path):
    """Every UND symbol name in the dynamic symbol table.

    Returns None if the table could not be read at all — which must not be confused with "no
    symbols", the distinction this project has had to relearn repeatedly.
    """
    try:
        out = subprocess.run(["readelf", "-sW", "--dyn-syms", path],
                             capture_output=True, text=True, check=False).stdout
    except OSError:
        return None
    if "Symbol table" not in out:
        return None
    syms = set()
    for line in out.splitlines():
        f = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(f) >= 8 and f[6] == "UND" and f[7]:
            syms.add(f[7].split("@")[0])
    return syms


def main(shim, control):
    s = undefined_symbols(shim)
    c = undefined_symbols(control)
    if s is None:
        print("  [FAIL] cannot read the shim's dynamic symbol table — nothing was measured",
              file=sys.stderr)
        return 1
    if c is None:
        print("  [FAIL] cannot read the control's dynamic symbol table — the scan is unvalidated",
              file=sys.stderr)
        return 1

    c_hits = sorted(x for x in c if NET.match(x))
    if not c_hits:
        print("  [FAIL] the control binary shows ZERO network imports, so this scan is not working. "
              "A clean result from it would mean nothing.", file=sys.stderr)
        return 1

    s_hits = sorted(x for x in s if NET.match(x))
    print("  control %s: %d imports, %d network-capable (%s…) — the scan detects them"
          % (control.split("/")[-1], len(c), len(c_hits), ", ".join(c_hits[:4])))
    if s_hits:
        print("  [FAIL] the shim imports %d network-capable symbol(s): %s"
              % (len(s_hits), ", ".join(s_hits)), file=sys.stderr)
        return 1
    print("  [ OK ] shim %s: %d imports, NONE network-capable" % (shim.split("/")[-1], len(s)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check_no_sockets.py <shim.so> <control.so>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
