/* dispatch.h — the ONE boundary trap + libc bridge set (device layer, Audit 01/06).
 * A single UC_HOOK_CODE over the stub arena: when the guest executes a bx-lr stub
 * slot, look up the symbol (loader_stub_name), marshal its args (AAPCS32 via the
 * marshaller), run the bridge (wired to galloc/format/…), and write the return.
 * The stub's `bx lr` then returns to the caller. This is the start of the libc
 * bridge layer; GL/AAsset/JNI dispatch attach to the same mechanism later. */
#ifndef ABSHIM_DISPATCH_H
#define ABSHIM_DISPATCH_H
#include "cpu.h"
#include "loader.h"
#include "jni_passthrough.h"
#include "sched.h"

typedef struct dispatch {
    cpu_t    *cpu;
    loader_t *ld;
    uc_hook   stub_hook;
    int       fatal;                 /* set on abort / __stack_chk_fail */
    char      fatal_name[64];
    uint32_t  errno_slot;            /* guest errno word (single-GEC for now) */
    uint32_t  c_locale;             /* guest "C" string for setlocale */
    int       unimpl_count;
    char      last_unimpl[64];
    char      unimpl_names[96][48];   /* distinct unbridged symbols seen */
    int       unimpl_distinct;
    int       nested_err;             /* nested uc_emu_start failures (S3 probe) */
    int       nested_ran;             /* pthread_once routines run nested */
    uint32_t  tls_key_next;           /* pthread_key_create id counter */
    uint32_t  tls_store[256];         /* key -> value (single-GEC host; per-GEC on device) */
    uint64_t  mono_ns;                /* monotonic clock accumulator */
    jni_state *jni;                   /* set after jni_install; routes AAsset* calls */
    sched     *sch;                   /* set after sched_init; routes pthread_* calls */
    /* per-stub-slot dispatch cache (perf): the FIRST call to a stub resolves which bridge
     * handles it (walking the tables once); every later call skips the strcmp chains. */
    uint8_t     scache_kind[LOADER_MAX_STUBS];
    const void *scache_br[LOADER_MAX_STUBS];
} dispatch_t;

int  dispatch_install(dispatch_t *d, cpu_t *cpu, loader_t *ld);
/* Run the init_array constructors (skipping 0/0xffffffff sentinels).
 * Returns the number that completed cleanly; *total = non-sentinel count. */
int  dispatch_run_init_array(dispatch_t *d, int *total);

#endif /* ABSHIM_DISPATCH_H */
