/* jni_argbuild.h — the ONE JNI outbound-call argument builder.
 *
 * When the guest engine calls back into Java it invokes
 *     env->Call<T>Method{,V,A}(...)
 * through the 32-bit JNIEnv vtable. Per return type <T> there are THREE adjacent
 * vtable slots, one per calling form:
 *     bare  Call<T>Method (jobject,jmethodID,...)      — C varargs, spilled inline
 *     V     Call<T>MethodV(jobject,jmethodID,va_list)  — a guest va_list pointer
 *     A     Call<T>MethodA(jobject,jmethodID,jvalue*)  — a guest jvalue[] array
 * A correct shim MUST honour all three, because the argument BYTE LAYOUT differs:
 *   - bare/V are C-variadic: the default argument promotions apply, so a `float`
 *     is passed as a `double` (8 bytes) and sub-int types as `int` (4 bytes).
 *     `va_arg(ap,float)` is ill-formed for exactly this reason — reading an 'F'
 *     param as 4 bytes yields a wrong value AND desyncs every later argument.
 *   - A carries natural jvalue cells (8-byte stride, value in the low bytes): a
 *     `float` there occupies 4 bytes and is NOT promoted.
 * These builders convert any of the three forms into a host jvalue[] (see ab_jval)
 * so the device backend can uniformly invoke the real env's ...MethodA form.
 * 'L' cells resolve guest ref tokens -> host object pointers via the handle table.
 *
 * Pure logic over memops + marshal + handle_table => host-testable on x86 with no
 * JVM and no arm64 runtime (test/test_jni_arg.c). The device backend (jni_entry.c)
 * casts ab_jval* -> jvalue*; a _Static_assert there locks the sizeof equality. */
#ifndef ABSHIM_JNI_ARGBUILD_H
#define ABSHIM_JNI_ARGBUILD_H
#include "memops.h"
#include "marshal.h"
#include "handle_table.h"

/* jvalue-layout-compatible output cell: an 8-byte union whose members all sit at
 * offset 0, exactly like the NDK `jvalue`. Declared locally so this TU compiles
 * on the host without <jni.h>. */
typedef union ab_jval {
    uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i;
    int64_t j; float f; double d; void *l;
} ab_jval;

/* Decode the parameters described by a JNI method signature `sig` (e.g. "(IF)V";
 * only the parenthesised parameter list is read) into out[0..returned). At most
 * `maxn` params are decoded. Returns the number decoded.
 *   ab_build_valist  — from a guest va_list at guest address `va` (V / bare-after-
 *                      spill): 'F' read as promoted double then narrowed.
 *   ab_build_jvalarr — from a guest jvalue[] at guest address `ap` (A form):
 *                      natural cells, 8-byte stride, 'F' read as 4-byte float.
 *   ab_build_inline  — from the live AAPCS32 cursor `cur` (bare form, C varargs):
 *                      promoted like a va_list, but pulled via the NCRN/NSAA
 *                      register/stack machine. */
int ab_build_valist (guest_mem *m, handle_table *ht, const char *sig, uint32_t va, ab_jval *out, int maxn);
int ab_build_jvalarr(guest_mem *m, handle_table *ht, const char *sig, uint32_t ap, ab_jval *out, int maxn);
int ab_build_inline (guest_mem *m, handle_table *ht, const char *sig, mcur *cur,  ab_jval *out, int maxn);

#endif /* ABSHIM_JNI_ARGBUILD_H */
