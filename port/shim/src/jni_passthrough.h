/* jni_passthrough.h — guest JNIEnv/JavaVM sentinels + slot dispatch (Audit 08).
 *
 * The guest is handed a JNIEnv* / JavaVM* SENTINEL: a guest pointer to a guest
 * vtable of trampoline slots in the RG_JNI arena. When the engine does
 * `(*env)->FindClass(env,...)` it traps into a slot; a UC_HOOK_CODE over RG_JNI
 * dispatches by slot index, marshals args, runs the passthrough, and returns via
 * the handle table (a real ref/ID crosses only as a 32-bit token, Audit 08 J1).
 *
 * Two backends behind the same mechanism:
 *   - HOST-FAKE (this file): no JVM — return plausible tokens so nativeInit
 *     progresses to the JVM-dependency boundary (validates the plumbing on x86);
 *   - DEVICE-REAL (added for the APK): forward each slot to the real per-thread
 *     JNIEnv on the calling carrier thread. The marshalling/handle-table code is
 *     identical; only the leaf action differs. */
#ifndef ABSHIM_JNI_PASSTHROUGH_H
#define ABSHIM_JNI_PASSTHROUGH_H
#include "cpu.h"
#include "loader.h"
#include "handle_table.h"

typedef struct jni_state {
    cpu_t        *cpu;
    handle_table *ht;
    uc_hook       hook;
    uint32_t      env;               /* guest JNIEnv*  sentinel */
    uint32_t      vm;                /* guest JavaVM*  sentinel */
    uint32_t      scratch;           /* guest scratch bump (GUESTDATA) */
    uint32_t      scratch_end;
    uint32_t      call_count[260];   /* env-slot call histogram */
    int           slots_used[260];
    uint32_t      fake_next;         /* synthetic real-pointer counter */
    char          sigs[1024][80]; int nsig;   /* captured GetMethodID signatures */
    /* host-fake string values: NewStringUTF/jni_fake_intern_string store the bytes so
       GetStringUTFChars round-trips them (lets a test pass a real config string, e.g. the
       filesDir path nativeConfig needs). Device-real backend ignores this. */
    struct { uint32_t tok; char *val; } fstr[128]; int nfstr;
    int           host_fake;         /* 1 = no JVM (host test) */
    /* pluggable leaf dispatch: same sentinel/vtable/hook mechanism, different leaf.
       jni_install sets the host-fake defaults; jni_entry (device) overrides. */
    void        (*disp_env)(struct jni_state *J, uint32_t slot);
    void        (*disp_vm )(struct jni_state *J, uint32_t slot);
    void         *real_env;          /* current carrier thread's real JNIEnv* (device) */
    void         *real_vm;           /* real JavaVM* (device) */
} jni_state;

/* Build the sentinels + vtables + install the RG_JNI hook. host_fake!=0 for the
 * x86 host test. Returns 0 on success. */
int  jni_install(jni_state *J, cpu_t *cpu, int host_fake);
void jni_free(jni_state *J);

/* HOST-FAKE only: intern a C string as a fake jstring token whose GetStringUTFChars returns
   `val`. Lets a test hand the engine a real string (e.g. the filesDir path). Returns a token. */
uint32_t jni_fake_intern_string(jni_state *J, const char *val);

#endif /* ABSHIM_JNI_PASSTHROUGH_H */
