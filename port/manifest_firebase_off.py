#!/usr/bin/env python3
# De-phone-home (SDK-collection): inject boolean <meta-data> flags into <application> that disable
# auto-collecting analytics SDKs — flags each SDK's own library READS (confirmed present in
# classes.dex), so they take effect without code patching:
#   * firebase_messaging_auto_init_enabled=false  — stops Firebase Cloud Messaging from auto-
#     registering a device token via Google Play Services (the one phone-home the removed INTERNET
#     permission can't stop, since GMS does that network on the app's behalf).
#   * com.facebook.sdk.AutoLogAppEventsEnabled=false — stops the Facebook SDK from auto-logging app
#     events locally (it queues them in files/AppEventsLogger.persistedevents to upload later; no
#     INTERNET already blocks the upload, this stops the collection too).
# Each SDK still initialises normally (no config change) so nothing else is affected. NB the native
# Flurry / Rovio-BI analytics live in libengine32 (not manifest-gated) and still collect locally —
# but, like these, cannot leave the device (no INTERNET => every socket is kernel-denied).
#
# HOW (surgical, deterministic, idempotent): clone an existing boolean <meta-data> START/END chunk
# pair as a template, retarget android:name -> a newly-appended pool string and android:value ->
# boolean false, and splice one such element per still-absent flag as the first children of
# <application>. A no-op for any flag already present.
import sys, struct

# (meta-data name, android:value boolean as int 0/1). All false here.
FLAGS = [
    ("firebase_messaging_auto_init_enabled", 0),
    ("com.facebook.sdk.AutoLogAppEventsEnabled", 0),
]

def transform(ax: bytes) -> bytes:
    def u16(b,o): return struct.unpack('<H', b[o:o+2])[0]
    def u32(b,o): return struct.unpack('<I', b[o:o+4])[0]
    assert u32(ax,0) == 0x00080003, "not binary AXML"

    # ---- locate chunks: string pool @8, then resource map, then the XML node stream ----
    sp = 8
    assert u16(ax,sp) == 0x0001, "string pool not at offset 8"
    pool_size = u32(ax, sp+4)
    scount    = u32(ax, sp+8)
    stycount  = u32(ax, sp+12)
    flags     = u32(ax, sp+16)
    stroff    = u32(ax, sp+20)
    styoff    = u32(ax, sp+24)
    utf8      = bool(flags & 0x100)
    assert stycount == 0 and styoff == 0, "styled string pool unsupported"
    offs = [u32(ax, sp+28+4*i) for i in range(scount)]
    data_base = sp + stroff
    data_region = ax[data_base : sp + pool_size]          # offset array's data (incl. trailing pad)

    def dec(i):
        p = data_base + offs[i]
        n = ax[p]; p += 2 if (n & 0x80) else 1
        m = ax[p]
        if m & 0x80: bl = ((m & 0x7f) << 8) | ax[p+1]; p += 2
        else: bl = m; p += 1
        return ax[p:p+bl].decode('utf-8','replace')
    names = {dec(i): i for i in range(scount)}

    new_flags = [(nm, val) for (nm, val) in FLAGS if nm not in names]
    if not new_flags:                                      # idempotent: everything already present
        return ax
    for req in ('meta-data','name','value','application'):
        assert req in names, f"expected pooled string {req!r} absent"
    IDX_METADATA = names['meta-data']

    # ---- append the new flag-name strings to the pool (UTF-8 AXML form: u16len,u8len,bytes,NUL) ----
    assert utf8, "manifest pool is UTF-16; encoder expects UTF-8"
    def enc_len(v):
        return bytes([v]) if v < 0x80 else bytes([0x80 | (v >> 8), v & 0xFF])
    flag_idx = {}
    new_offs = []
    new_strs = b''
    cur_off = len(data_region)
    for i, (nm, _val) in enumerate(new_flags):
        flag_idx[nm] = scount + i
        b = nm.encode('utf-8')
        sb = enc_len(len(nm.encode('utf-16-le'))//2) + enc_len(len(b)) + b + b'\x00'
        new_offs.append(cur_off)
        new_strs += sb
        cur_off += len(sb)
    new_data_region = data_region + new_strs

    # ---- rebuild the string-pool chunk (+N offset entries, +N strings) ----
    n = len(new_flags)
    new_scount = scount + n
    new_stroff = stroff + 4*n                               # offset array grew by N u32
    new_off_array = b''.join(struct.pack('<I', o) for o in offs) \
                  + b''.join(struct.pack('<I', o) for o in new_offs)
    body = new_off_array + new_data_region
    unpadded = 28 + len(body)
    pad = (-unpadded) % 4
    new_pool_size = unpadded + pad
    pool = bytearray()
    pool += struct.pack('<HH', 0x0001, 28)
    pool += struct.pack('<I', new_pool_size)
    pool += struct.pack('<I', new_scount)
    pool += struct.pack('<I', 0)                            # styleCount
    pool += struct.pack('<I', flags)
    pool += struct.pack('<I', new_stroff)
    pool += struct.pack('<I', 0)                            # stylesStart
    pool += body + b'\x00'*pad
    assert len(pool) == new_pool_size

    # ---- everything after the string pool: resource map + XML node stream ----
    rest = ax[sp + pool_size:]

    # clone an existing boolean <meta-data> START/END pair as a template (airview.enable = bool)
    def find_template(buf):
        p = 0
        while p + 8 <= len(buf):
            ct = u16(buf,p); cs = u32(buf,p+4)
            if ct == 0x0102 and u32(buf,p+20) == IDX_METADATA:
                ab = p + 16 + u16(buf,p+24)
                nmval = u32(buf, ab+8)
                if nmval < scount and 'airview' in (dec(nmval) or ''):
                    end = p + cs; ecs = u32(buf, end+4)
                    return buf[p:end], buf[end:end+ecs]
            if cs <= 0: break
            p += cs
        raise RuntimeError("meta-data template not found")
    tmpl_start, tmpl_end = find_template(rest)
    assert len(tmpl_start) == 76 and len(tmpl_end) == 24, (len(tmpl_start), len(tmpl_end))

    new_elements = bytearray()
    for (nm, val) in new_flags:
        idx = flag_idx[nm]
        ns = bytearray(tmpl_start)
        struct.pack_into('<I', ns, 44, idx)                # attr0(android:name) rawValue -> flag str
        struct.pack_into('<I', ns, 52, idx)                # attr0 Res_value.data      -> flag str
        struct.pack_into('<I', ns, 72, val)                # attr1(android:value) bool -> val (0=false)
        new_elements += ns + tmpl_end

    # ---- splice the new elements right after the <application> START chunk ----
    out_rest = bytearray(); spliced = False; p = 0
    IDX_APP = names['application']
    while p + 8 <= len(rest):
        ct = u16(rest,p); cs = u32(rest,p+4)
        out_rest += rest[p:p+cs]
        if not spliced and ct == 0x0102 and u32(rest,p+20) == IDX_APP:
            out_rest += new_elements
            spliced = True
        if cs <= 0: break
        p += cs
    assert spliced, "<application> START chunk not found"

    out = bytearray()
    out += ax[0:8]
    out += pool
    out += out_rest
    struct.pack_into('<I', out, 4, len(out))               # fix total file size
    return bytes(out)

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src,'rb').read()
    out = transform(data)
    open(dst,'wb').write(out)
    if out is data or len(out) == len(data):
        print("manifest_firebase_off: no change (all analytics-disable flags already present)")
    else:
        print(f"manifest_firebase_off: injected analytics-disable flags: {len(data)} -> {len(out)} bytes")
