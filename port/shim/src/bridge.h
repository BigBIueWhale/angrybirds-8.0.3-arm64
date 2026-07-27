/* bridge.h — GL + asset bridge entry points (Audit 07). Both dlsym the real
 * arm64 libGLESv2/libandroid at runtime (host: dlsym -> NULL -> no-op). */
#ifndef ABSHIM_BRIDGE_H
#define ABSHIM_BRIDGE_H
#include "cpu.h"
#include "marshal.h"
#include "jni_passthrough.h"

/* Handle a gl* call (pull args, forward, set r0). Returns 1 if it handled it. */
int gl_try(cpu_t *c, const char *name, mcur *cur);

/* Handle an AAsset or AAssetManager call (needs the JNI state for env + token
 * resolution). Returns 1 if handled. */
int asset_try(cpu_t *c, jni_state *J, const char *name, mcur *cur);

/* Handle a runtime libc/libm call: math (host passthrough), string/conv, ctype,
 * wide-char, rand48, sysinfo, and the NETWORK/dl HARD-FAIL facade (never a real
 * socket; UNIMPL->0 would wrongly read as success). Returns 1 if handled. */
int libc_try(cpu_t *c, const char *name, mcur *cur);

/* Handle a stdio FILE* / raw fd / mmap / filesystem call (real host-backed; the
 * shim runs in the app process so guest paths are accessible). Returns 1 if handled. */
int file_try(cpu_t *c, const char *name, mcur *cur);

#endif /* ABSHIM_BRIDGE_H */
