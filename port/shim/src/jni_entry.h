/* jni_entry.h — the 72 arm64 Java_* entry thunks + shim_call (Audit 08, device).
 *
 * Each generated thunk (jni_thunks.gen.c) has the exact JNI C signature so the
 * arm64 compiler places its args per AAPCS64; it packs them into a jvalue[] and
 * calls shim_call(), which — driven by the DEX `shorty` (return + param type
 * classes) — marshals each into the guest soft-float ABI (int/Z/B/C/S -> word;
 * J/D -> 8-aligned reg-pair; F -> word bit-pattern; L -> a handle token via the
 * handle table), runs the guest Java_* under emulation on the calling carrier
 * thread, and converts the return. Device-only (needs the NDK jni.h + the real
 * per-thread JNIEnv). */
#ifndef ABSHIM_JNI_ENTRY_H
#define ABSHIM_JNI_ENTRY_H
#include <jni.h>

/* Marshal `args` (packed per the JNI C signature) into the guest per `shorty`,
 * run the guest function exported as `name` in the engine, return the result. */
jvalue shim_call(JNIEnv *env, jobject thiz, const char *name, const char *shorty, jvalue *args, int nargs);

typedef struct { const char *name; const char *shorty; } shim_thunk_ent;
extern const shim_thunk_ent SHIM_THUNKS[];   /* NULL-terminated */

#endif /* ABSHIM_JNI_ENTRY_H */
