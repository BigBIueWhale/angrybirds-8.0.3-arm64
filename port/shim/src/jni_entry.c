/* jni_entry.c — the shipping arm64 entry: JNI_OnLoad + shim_call + the
 * device-real JNI backend (Audit 08). DEVICE-ONLY (needs the NDK jni.h + a real
 * JVM). The 72 generated thunks (jni_thunks.gen.c) call shim_call; JNI_OnLoad
 * loads the 32-bit engine and wires cpu/loader/dispatch/jni. The env/VM slot
 * handlers forward to the real per-thread env — the marshalling + handle-table
 * code is identical to the host-fake backend; only the leaf action differs. */
#include "jni_entry.h"
#include "cpu.h"
#include "loader.h"
#include "dispatch.h"
#include "jni_passthrough.h"
#include "marshal.h"
#include "elf32.h"
#include "handle_table.h"
#include "regions.h"
#include "jni_argbuild.h"
#include <unicorn/unicorn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <android/log.h>

#define LOG(...)  __android_log_print(4,"abshim",__VA_ARGS__)
/* Upper bound on a single guest-driven JNI array/string transfer. Every legitimate call in this
 * game is orders of magnitude below this; the cap exists so a wild guest length cannot become a
 * host allocation size or a copy length. */
#define ABSHIM_JNI_ARR_MAX (64ull<<20)
#define LOGE(...) __android_log_print(6,"abshim",__VA_ARGS__)
/* --- hang watchdog state (defined here so env_dispatch_real/shim_call below can record ops) --- */
volatile unsigned long g_op_seq = 0;              /* bumped by every bridge + JNI dispatch (dispatch.c externs it) */
volatile const char *g_op_bridge = "(init)";
volatile int g_op_slot = -1;
volatile const char *g_cur_native = "(none)";
extern volatile unsigned long g_sched_tick;       /* sched.c: bumped once per run_loop quantum */
void abshim_mark_op(const char *bridge, int slot){ g_op_bridge=bridge; g_op_slot=slot; g_op_seq++; }
/* GL activity counters (bridge_gl.c): how many draws/clears the engine has issued — surfaced
 * periodically from the render path so on-device logcat answers "is it actually drawing?". */
extern unsigned long g_gl_draws, g_gl_clears, g_gl_useprog;

/* the Call-family backend builds args into ab_jval[] and casts to jvalue* for the
 * real env; lock the layout equivalence that cast relies on (both 8-byte unions,
 * every member at offset 0). */
_Static_assert(sizeof(ab_jval)==sizeof(jvalue), "ab_jval must mirror jvalue size");

/* ---- global shim state (one guest CPU under a coarse lock = the BEL) ---- */
static struct {
    cpu_t     cpu;
    loader_t  ld;
    dispatch_t disp;
    sched     sch;                       /* GEL green-thread scheduler (real pthreads) */
    jni_state jni;
    uint8_t  *engine; size_t engine_len;
    int       ready;
    pthread_mutex_t bel;                 /* one uc, touched only under this lock (M14) */
    /* guest-symbol address cache for the 72 thunks */
    struct { const char*name; uint32_t addr; } scache[128]; int nsc;
} G;

static JNIEnv *renv(void){ return (JNIEnv*)G.jni.real_env; }
static handle_table *HT(void){ return G.jni.ht; }

/* read a guest C string into a host buffer */
static void rd_str(uint32_t p, char*o, int max){ int i=0; if(!p){o[0]=0;return;} for(;i<max-1;i++){ uint8_t ch=0; if(uc_mem_read(G.cpu.uc,p+i,&ch,1)!=UC_ERR_OK||!ch) break; o[i]=(char)ch; } o[i]=0; }
static void set_r0(uint32_t v){ uc_reg_write(G.cpu.uc,UC_ARM_REG_R0,&v); }
static void set_r1(uint32_t v){ uc_reg_write(G.cpu.uc,UC_ARM_REG_R1,&v); }
static uint32_t tok(jobject o, int kind){ return o? ht_new_ref(HT(),kind,(void*)o) : HT_NULL; }
static jobject real_of(uint32_t t){ return (jobject)ht_resolve(HT(),t); }
/* Free a guest heap buffer handed back at a Release* call. Range-guarded so a foreign/
 * bogus pointer (per the JNI contract this can't happen, but be defensive like f_fclose)
 * is ignored rather than corrupting the allocator. Without this, every Get<String/Array>
 * that returns a fresh galloc'd guest copy would leak the guest heap. */
static void gfree_heap(cpu_t*c, uint32_t p){ if(p>=RG_HEAP && p<RG_HEAP+RG_HEAP_SZ) galloc_free(c->heap,p); }

/* The guest va_list / jvalue[] / inline arg walkers live in jni_argbuild.c (shared
 * host-testable module, test/test_jni_arg.c) — including the C-varargs float->double
 * promotion that a naive 4-byte 'F' read would get wrong. */

/* look up the captured signature for a jmethodID token */
static const char* sig_of(uint32_t midtok){ int i=(int)(intptr_t)ht_id_desc(HT(),midtok); return (i>=0 && i<G.jni.nsig)? G.jni.sigs[i] : "()V"; }

/* DE-PHONE-HOME + UNBLOCK. These static SDK methods BIND to Google Play Services / Play Store and BLOCK; under
 * the single-run_loop green model a blocking ART Call freezes every green thread (incl. render) = the gate
 * before draws>0. We capture each by NAME at GetStaticMethodID (methodIDs are stable/comparable, unlike the
 * per-call jclass local refs) and short-circuit its Call to a privacy-safe default WITHOUT the ART call — which
 * both unblocks the render thread AND removes the phone-home. ret: 'Z'/'I'/... -> 0; 'L' -> empty string. To
 * neutralize another discovered blocker, add one row. */
static struct { const char *name; char ret; jmethodID mid; } g_neut_meth[] = {
    { "advertisingTrackingEnabled", 'Z', 0 },  /* Utils: GMS AdvertisingIdClient.getAdvertisingIdInfo -> BLOCKS */
    { "advertisingId",              'L', 0 },  /* Utils: same GMS ad-id bind, returns String -> BLOCKS */
    { "queryNewPlayReferrer",       'Z', 0 },  /* Utils: InstallReferrerClient Play Store bind */
};
#define N_NEUT_METH ((int)(sizeof(g_neut_meth)/sizeof(g_neut_meth[0])))
static int cap_sig(const char*s){ if(G.jni.nsig<1024){ snprintf(G.jni.sigs[G.jni.nsig],80,"%s",s); return G.jni.nsig++; } return -1; }

/* ---- primitive-array helpers. type index: 0 Bool,1 Byte,2 Char,3 Short,4 Int,
 * 5 Long,6 Float,7 Double (the New/Get/Release/Region JNIEnv families are regular
 * in this order; object arrays are handled separately at slots 172/173/174). ---- */
static int jarr_esz(int ty){ static const int s[8]={1,1,2,2,4,8,4,8}; return (ty>=0&&ty<8)?s[ty]:1; }
static void* jarr_get_elems(JNIEnv*e,int ty,jarray a){ switch(ty){
    case 0: return (*e)->GetBooleanArrayElements(e,(jbooleanArray)a,0);
    case 1: return (*e)->GetByteArrayElements(e,(jbyteArray)a,0);
    case 2: return (*e)->GetCharArrayElements(e,(jcharArray)a,0);
    case 3: return (*e)->GetShortArrayElements(e,(jshortArray)a,0);
    case 4: return (*e)->GetIntArrayElements(e,(jintArray)a,0);
    case 5: return (*e)->GetLongArrayElements(e,(jlongArray)a,0);
    case 6: return (*e)->GetFloatArrayElements(e,(jfloatArray)a,0);
    default:return (*e)->GetDoubleArrayElements(e,(jdoubleArray)a,0); } }
static void jarr_rel_elems(JNIEnv*e,int ty,jarray a,void*el,jint mode){ switch(ty){
    case 0: (*e)->ReleaseBooleanArrayElements(e,(jbooleanArray)a,(jboolean*)el,mode); break;
    case 1: (*e)->ReleaseByteArrayElements(e,(jbyteArray)a,(jbyte*)el,mode); break;
    case 2: (*e)->ReleaseCharArrayElements(e,(jcharArray)a,(jchar*)el,mode); break;
    case 3: (*e)->ReleaseShortArrayElements(e,(jshortArray)a,(jshort*)el,mode); break;
    case 4: (*e)->ReleaseIntArrayElements(e,(jintArray)a,(jint*)el,mode); break;
    case 5: (*e)->ReleaseLongArrayElements(e,(jlongArray)a,(jlong*)el,mode); break;
    case 6: (*e)->ReleaseFloatArrayElements(e,(jfloatArray)a,(jfloat*)el,mode); break;
    default:(*e)->ReleaseDoubleArrayElements(e,(jdoubleArray)a,(jdouble*)el,mode); break; } }
static void jarr_get_region(JNIEnv*e,int ty,jarray a,jsize st,jsize ln,void*b){ switch(ty){
    case 0: (*e)->GetBooleanArrayRegion(e,(jbooleanArray)a,st,ln,(jboolean*)b); break;
    case 1: (*e)->GetByteArrayRegion(e,(jbyteArray)a,st,ln,(jbyte*)b); break;
    case 2: (*e)->GetCharArrayRegion(e,(jcharArray)a,st,ln,(jchar*)b); break;
    case 3: (*e)->GetShortArrayRegion(e,(jshortArray)a,st,ln,(jshort*)b); break;
    case 4: (*e)->GetIntArrayRegion(e,(jintArray)a,st,ln,(jint*)b); break;
    case 5: (*e)->GetLongArrayRegion(e,(jlongArray)a,st,ln,(jlong*)b); break;
    case 6: (*e)->GetFloatArrayRegion(e,(jfloatArray)a,st,ln,(jfloat*)b); break;
    default:(*e)->GetDoubleArrayRegion(e,(jdoubleArray)a,st,ln,(jdouble*)b); break; } }
static void jarr_set_region(JNIEnv*e,int ty,jarray a,jsize st,jsize ln,const void*b){ switch(ty){
    case 0: (*e)->SetBooleanArrayRegion(e,(jbooleanArray)a,st,ln,(const jboolean*)b); break;
    case 1: (*e)->SetByteArrayRegion(e,(jbyteArray)a,st,ln,(const jbyte*)b); break;
    case 2: (*e)->SetCharArrayRegion(e,(jcharArray)a,st,ln,(const jchar*)b); break;
    case 3: (*e)->SetShortArrayRegion(e,(jshortArray)a,st,ln,(const jshort*)b); break;
    case 4: (*e)->SetIntArrayRegion(e,(jintArray)a,st,ln,(const jint*)b); break;
    case 5: (*e)->SetLongArrayRegion(e,(jlongArray)a,st,ln,(const jlong*)b); break;
    case 6: (*e)->SetFloatArrayRegion(e,(jfloatArray)a,st,ln,(const jfloat*)b); break;
    default:(*e)->SetDoubleArrayRegion(e,(jdoubleArray)a,st,ln,(const jdouble*)b); break; } }
static jarray jarr_new(JNIEnv*e,int ty,jsize n){ switch(ty){
    case 0: return (*e)->NewBooleanArray(e,n); case 1: return (*e)->NewByteArray(e,n);
    case 2: return (*e)->NewCharArray(e,n);    case 3: return (*e)->NewShortArray(e,n);
    case 4: return (*e)->NewIntArray(e,n);     case 5: return (*e)->NewLongArray(e,n);
    case 6: return (*e)->NewFloatArray(e,n);   default:return (*e)->NewDoubleArray(e,n); } }

/* --- S2 blocking-JNI stop/restart: per-gthread stash of a Call*Method + its raw result.
 * env_dispatch_real fills g_bcall[gthread-index] + redirects the guest to RG_RET; run_loop
 * (sched.c) then calls jni_block_do_cb with the GEL RELEASED (the real, possibly-blocking ART
 * call) and jni_block_finish_cb with the GEL HELD (tokenise + write r0/r1). This lets the render
 * green thread run while the audio green thread waits in ART -> the cross-thread deadlock cannot
 * form (matches real-device concurrency, where native calls never share a global lock). --- */
static struct jni_bcall {
    jobject   ro; jmethodID rm; jvalue av[16];
    int       type, is_static;
    jobject   r_obj; uint64_t r_u; float r_f; double r_d;   /* raw ART result, by return kind */
} g_bcall[SCHED_MAX_THREADS];

#ifdef ABSHIM_SLOWCALL
/* Own clock helper, OUTSIDE the profiling-only conditional below. ABSHIM_SLOWCALL is a RELEASE build
 * plus a flag, so it cannot borrow jniblk_now_ns() from that block -- the third time in this session a
 * addition landed inside `#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)` and vanished from the
 * shipping configuration. Check the enclosing conditional before adding anything to this file. */
static uint64_t slowcall_now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
/* Time spent in the BLOCKING half of a JNI call. This is separate from jni_passthrough.c's timer
 * and the split is wrong without it: for a Call*Method, env_dispatch_real only STASHES the call and
 * redirects the guest to RG_RET, so uc_emu_start has already returned by the time the real ART call
 * runs here in run_loop — outside jni_hook_cb, and with the GEL RELEASED.
 *
 * Left unmeasured, this time falls into the residual and gets reported as "emulator". It is the
 * opposite: the guest is not executing at all, the carrier thread is parked inside ART. Attributing
 * a blocking wait to Unicorn would overstate emulation exactly where the conclusion ("CPU-bound on
 * emulation") is most load-bearing, so it is counted as JNI. Same __thread reasoning as elsewhere;
 * this runs on the carrier thread whose shim_call is being measured. */
static __thread uint64_t g_jniblk_ns = 0, g_jniblk_n = 0;
uint64_t jni_block_ns(void){ return g_jniblk_ns; }
uint64_t jni_block_calls(void){ return g_jniblk_n; }
static uint64_t jniblk_now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static void jni_block_do_cb_inner(gthread *gt);
static void jni_block_do_cb(gthread *gt){
    uint64_t _b0 = jniblk_now_ns();
    jni_block_do_cb_inner(gt);
    g_jniblk_ns += jniblk_now_ns() - _b0; g_jniblk_n++;
}
static void jni_block_do_cb_inner(gthread *gt){
#else
static void jni_block_do_cb(gthread *gt){          /* GEL RELEASED: the blocking ART Call*MethodA */
#endif
    int k=(int)(gt - G.sch.all); if(k<0||k>=SCHED_MAX_THREADS) return;
    struct jni_bcall *b=&g_bcall[k]; JNIEnv*e=renv();
    { static int n=0; if(n++<24) LOG("[S2] do_call ENTER gt=%d type=%d static=%d (GEL released)", k, b->type, b->is_static); }
    jobject ro=b->ro; jmethodID rm=b->rm; jvalue*jv=b->av; int st=b->is_static;
#ifdef ABSHIM_AUDIO
    /* cont.119: also drop the coarse BEL (not just the GEL, which sched.c already released) so a
     * DIFFERENT Java thread — the render/GLThread in shim_call — can enter, run its gt, and release
     * the monitor this ART call blocks on -> breaks the audio-mixer cross-thread deadlock (cont.118).
     * SAFE: jni_block_do_cb is only reached when the run_loop drives (G.sch.drive_active), which is
     * always under shim_call's G.bel; `e` (this thread's env) was captured above. */
    pthread_mutex_unlock(&G.bel);
#endif
    switch(b->type){
    case 0: b->r_obj = st?(*e)->CallStaticObjectMethodA(e,ro,rm,jv):(*e)->CallObjectMethodA(e,ro,rm,jv); break;
    case 1: b->r_u  = st?(*e)->CallStaticBooleanMethodA(e,ro,rm,jv):(*e)->CallBooleanMethodA(e,ro,rm,jv); break;
    case 2: b->r_u  = (uint8_t)(st?(*e)->CallStaticByteMethodA(e,ro,rm,jv):(*e)->CallByteMethodA(e,ro,rm,jv)); break;
    case 3: b->r_u  = (uint16_t)(st?(*e)->CallStaticCharMethodA(e,ro,rm,jv):(*e)->CallCharMethodA(e,ro,rm,jv)); break;
    case 4: b->r_u  = (uint32_t)(int32_t)(int16_t)(st?(*e)->CallStaticShortMethodA(e,ro,rm,jv):(*e)->CallShortMethodA(e,ro,rm,jv)); break;
    case 5: b->r_u  = (uint32_t)(st?(*e)->CallStaticIntMethodA(e,ro,rm,jv):(*e)->CallIntMethodA(e,ro,rm,jv)); break;
    case 6: b->r_u  = (uint64_t)(st?(*e)->CallStaticLongMethodA(e,ro,rm,jv):(*e)->CallLongMethodA(e,ro,rm,jv)); break;
    case 7: b->r_f  = st?(*e)->CallStaticFloatMethodA(e,ro,rm,jv):(*e)->CallFloatMethodA(e,ro,rm,jv); break;
    case 8: b->r_d  = st?(*e)->CallStaticDoubleMethodA(e,ro,rm,jv):(*e)->CallDoubleMethodA(e,ro,rm,jv); break;
    default: if(st)(*e)->CallStaticVoidMethodA(e,ro,rm,jv); else (*e)->CallVoidMethodA(e,ro,rm,jv); break;
    }
#ifdef ABSHIM_AUDIO
    pthread_mutex_lock(&G.bel);      /* re-acquire the BEL (render released it when its shim_call returned) */
    G.jni.real_env = e;              /* render changed the global real_env while we were parked; restore ours */
#endif
    { static int n=0; if(n++<24) LOG("[S2] do_call RET   gt=%d type=%d (ART call returned)", k, b->type); }
}
static void jni_block_finish_cb(gthread *gt){      /* GEL HELD: tokenise + inject r0/r1 */
    int k=(int)(gt - G.sch.all); if(k<0||k>=SCHED_MAX_THREADS) return;
    struct jni_bcall *b=&g_bcall[k];
    switch(b->type){
    case 0: set_r0(tok(b->r_obj, HK_LOCAL)); break;
    case 1: case 2: case 3: case 4: case 5: set_r0((uint32_t)b->r_u); break;
    case 6: set_r0((uint32_t)b->r_u); set_r1((uint32_t)(b->r_u>>32)); break;
    case 7: { uint32_t bb; memcpy(&bb,&b->r_f,4); set_r0(bb); } break;
    case 8: { uint64_t bb; memcpy(&bb,&b->r_d,8); set_r0((uint32_t)bb); set_r1((uint32_t)(bb>>32)); } break;
    default: break;                                /* void */
    }
}

/* ---- device-real env slot dispatch ---- */
static void env_dispatch_real(jni_state*J, uint32_t slot){
    abshim_mark_op(NULL,(int)slot);                             /* DIAG watchdog: last JNI slot (catches a blocking Call*Method) */
    cpu_t*c=J->cpu; JNIEnv*e=renv(); mcur cur; marshal_cur_init(&c->mem,&cur);
    (void)marshal_pull_word(&c->mem,&cur);                       /* env */
    #define PW marshal_pull_word(&c->mem,&cur)
    /* Call<T>Method{,V,A} (34..63) and CallStatic<T>Method{,V,A} (114..143): per
     * return type <T>, three adjacent vtable slots — bare(...)/V(va_list)/A(jvalue*).
     * type = return kind (0 Object .. 9 Void); form = 0 bare / 1 V / 2 A. All three
     * converge on the real env's ...MethodA once the args are in a jvalue[]. The
     * old [35,62]/[115,142] range dropped ...MethodA (63/143) and mis-read the A
     * form as a va_list; the three forms also differ on float promotion. */
    if ((slot>=34 && slot<=63) || (slot>=114 && slot<=143)){
        int is_static = slot>=114; int base=is_static?114:34;
        int rel=(int)slot-base; int type=rel/3, form=rel%3;
        uint32_t o=PW, mid=PW;
        jobject ro=real_of(o); jmethodID rm=(jmethodID)real_of(mid); const char*sig=sig_of(mid);
        /* DE-PHONE-HOME + UNBLOCK: blocking SDK static methods (Play Services / Play Store binds) freeze the
         * run_loop; return a privacy-safe default WITHOUT the ART call so the render thread is never starved. */
        if(is_static && rm) for(int i=0;i<N_NEUT_METH;i++) if(rm==g_neut_meth[i].mid){
            if(g_neut_meth[i].ret=='L') set_r0(tok((*e)->NewStringUTF(e,""),HK_LOCAL)); else set_r0(0);
            { static int n=0; if(n++<8) LOG("[de-phonehome] %s -> default (skip blocking SDK call, unblock render)",g_neut_meth[i].name); }
            return;
        }
        ab_jval av[16]; memset(av,0,sizeof av); jvalue*jv=(jvalue*)av;
        if(form==1){ uint32_t va=PW; ab_build_valist(&c->mem,HT(),sig,va,av,16); }        /* ...MethodV (va_list) */
        else if(form==2){ uint32_t ap=PW; ab_build_jvalarr(&c->mem,HT(),sig,ap,av,16); }  /* ...MethodA (jvalue[]) */
        else { ab_build_inline(&c->mem,HT(),sig,&cur,av,16); }                            /* bare ...Method (inline varargs) */
        /* S2 stop/restart: when run_loop is the active driver, DON'T do the ART call inline (it may
         * block waiting on a Java monitor held by another green thread that needs the GEL -> deadlock).
         * Instead stash the resolved call, redirect the guest to RG_RET (its JNI stub's own bx lr), and
         * let run_loop do the call with the GEL released. Outside run_loop (guest JNI_OnLoad via cpu_call)
         * fall through to the inline call. */
        if(G.sch.drive_active && G.sch.cur && G.sch.jni_do_call){
            gthread *gt=G.sch.cur; int k=(int)(gt - G.sch.all);
            if(k>=0 && k<SCHED_MAX_THREADS){
                struct jni_bcall *b=&g_bcall[k];
                { static int n=0; if(n++<40) LOG("[S2] stash slot=%u type=%d static=%d sig=%s gt=%d nthreads=%d runq=%s", slot, type, is_static, sig?sig:"?", k, G.sch.nthreads, G.sch.runq_head?"Y":"n"); }
                b->ro=ro; b->rm=rm; memcpy(b->av, av, sizeof b->av); b->type=type; b->is_static=is_static;
                uint32_t lr=0; uc_reg_read(c->uc,UC_ARM_REG_LR,&lr); gt->jni_ret_pc=lr;
                uint32_t rr=RG_RET; uc_reg_write(c->uc,UC_ARM_REG_LR,&rr);   /* stub bx lr -> RG_RET */
                gt->pending_jni=1;
                return;
            }
        }
        switch(type){
        case 0: set_r0(tok(is_static?(*e)->CallStaticObjectMethodA(e,ro,rm,jv):(*e)->CallObjectMethodA(e,ro,rm,jv), HK_LOCAL)); break;
        case 1: set_r0(is_static?(*e)->CallStaticBooleanMethodA(e,ro,rm,jv):(*e)->CallBooleanMethodA(e,ro,rm,jv)); break;
        case 2: set_r0((uint8_t)(is_static?(*e)->CallStaticByteMethodA(e,ro,rm,jv):(*e)->CallByteMethodA(e,ro,rm,jv))); break;
        case 3: set_r0((uint16_t)(is_static?(*e)->CallStaticCharMethodA(e,ro,rm,jv):(*e)->CallCharMethodA(e,ro,rm,jv))); break;
        case 4: set_r0((int16_t)(is_static?(*e)->CallStaticShortMethodA(e,ro,rm,jv):(*e)->CallShortMethodA(e,ro,rm,jv))); break;
        case 5: set_r0((uint32_t)(is_static?(*e)->CallStaticIntMethodA(e,ro,rm,jv):(*e)->CallIntMethodA(e,ro,rm,jv))); break;
        case 6: { uint64_t v=(uint64_t)(is_static?(*e)->CallStaticLongMethodA(e,ro,rm,jv):(*e)->CallLongMethodA(e,ro,rm,jv)); set_r0((uint32_t)v); set_r1((uint32_t)(v>>32)); } break;
        case 7: { float f=is_static?(*e)->CallStaticFloatMethodA(e,ro,rm,jv):(*e)->CallFloatMethodA(e,ro,rm,jv); uint32_t b; memcpy(&b,&f,4); set_r0(b); } break;
        case 8: { double d=is_static?(*e)->CallStaticDoubleMethodA(e,ro,rm,jv):(*e)->CallDoubleMethodA(e,ro,rm,jv); uint64_t b; memcpy(&b,&d,8); set_r0((uint32_t)b); set_r1((uint32_t)(b>>32)); } break;
        default: if(is_static)(*e)->CallStaticVoidMethodA(e,ro,rm,jv); else (*e)->CallVoidMethodA(e,ro,rm,jv); break;
        }
        return;
    }
    /* ===== extended JNIEnv surface (evidence-driven: the engine FindClass's
     * Build$VERSION / HashMap / ArrayList / PackageInfo / Signature, so it reads
     * fields, constructs objects, and moves array data). Families are regular; every
     * slot index verified against the NDK JNINativeInterface. ===== */
    #define PWD marshal_pull_dword(&c->mem,&cur)
    static uint32_t lframe[64]; static int lframe_n;   /* PushLocalFrame mark stack (single guest, under BEL) */
    /* Get<T>Field (95..103) / GetStatic<T>Field (145..153). ty: 0 Obj,1 Z,2 B,3 C,4 S,5 I,6 J,7 F,8 D */
    if ((slot>=95 && slot<=103) || (slot>=145 && slot<=153)){
        int is_static=slot>=145; int ty=(int)slot-(is_static?145:95);
        uint32_t ob=PW, fid=PW; void*rob=real_of(ob); jfieldID rf=(jfieldID)real_of(fid);
        jobject o=(jobject)rob; jclass k=(jclass)rob;
        switch(ty){
        case 0: set_r0(tok(is_static?(*e)->GetStaticObjectField(e,k,rf):(*e)->GetObjectField(e,o,rf),HK_LOCAL)); break;
        case 1: set_r0(is_static?(*e)->GetStaticBooleanField(e,k,rf):(*e)->GetBooleanField(e,o,rf)); break;
        case 2: set_r0((uint32_t)(int32_t)(int8_t)(is_static?(*e)->GetStaticByteField(e,k,rf):(*e)->GetByteField(e,o,rf))); break;
        case 3: set_r0((uint16_t)(is_static?(*e)->GetStaticCharField(e,k,rf):(*e)->GetCharField(e,o,rf))); break;
        case 4: set_r0((uint32_t)(int32_t)(int16_t)(is_static?(*e)->GetStaticShortField(e,k,rf):(*e)->GetShortField(e,o,rf))); break;
        case 5: set_r0((uint32_t)(is_static?(*e)->GetStaticIntField(e,k,rf):(*e)->GetIntField(e,o,rf))); break;
        case 6: { uint64_t v=(uint64_t)(is_static?(*e)->GetStaticLongField(e,k,rf):(*e)->GetLongField(e,o,rf)); set_r0((uint32_t)v); set_r1((uint32_t)(v>>32)); } break;
        case 7: { float f=is_static?(*e)->GetStaticFloatField(e,k,rf):(*e)->GetFloatField(e,o,rf); uint32_t b; memcpy(&b,&f,4); set_r0(b); } break;
        default:{ double d=is_static?(*e)->GetStaticDoubleField(e,k,rf):(*e)->GetDoubleField(e,o,rf); uint64_t b; memcpy(&b,&d,8); set_r0((uint32_t)b); set_r1((uint32_t)(b>>32)); } break;
        }
        return;
    }
    /* Set<T>Field (104..112) / SetStatic<T>Field (154..162) */
    if ((slot>=104 && slot<=112) || (slot>=154 && slot<=162)){
        int is_static=slot>=154; int ty=(int)slot-(is_static?154:104);
        uint32_t ob=PW, fid=PW; void*rob=real_of(ob); jfieldID rf=(jfieldID)real_of(fid);
        jobject o=(jobject)rob; jclass k=(jclass)rob;
        switch(ty){
        case 0: { jobject v=real_of(PW); if(is_static)(*e)->SetStaticObjectField(e,k,rf,v); else (*e)->SetObjectField(e,o,rf,v); } break;
        case 1: { uint32_t v=PW; if(is_static)(*e)->SetStaticBooleanField(e,k,rf,(jboolean)(v&1)); else (*e)->SetBooleanField(e,o,rf,(jboolean)(v&1)); } break;
        case 2: { uint32_t v=PW; if(is_static)(*e)->SetStaticByteField(e,k,rf,(jbyte)v); else (*e)->SetByteField(e,o,rf,(jbyte)v); } break;
        case 3: { uint32_t v=PW; if(is_static)(*e)->SetStaticCharField(e,k,rf,(jchar)v); else (*e)->SetCharField(e,o,rf,(jchar)v); } break;
        case 4: { uint32_t v=PW; if(is_static)(*e)->SetStaticShortField(e,k,rf,(jshort)v); else (*e)->SetShortField(e,o,rf,(jshort)v); } break;
        case 5: { uint32_t v=PW; if(is_static)(*e)->SetStaticIntField(e,k,rf,(jint)v); else (*e)->SetIntField(e,o,rf,(jint)v); } break;
        case 6: { uint64_t v=PWD; if(is_static)(*e)->SetStaticLongField(e,k,rf,(jlong)v); else (*e)->SetLongField(e,o,rf,(jlong)v); } break;
        case 7: { uint32_t v=PW; float f; memcpy(&f,&v,4); if(is_static)(*e)->SetStaticFloatField(e,k,rf,f); else (*e)->SetFloatField(e,o,rf,f); } break;
        default:{ uint64_t v=PWD; double d; memcpy(&d,&v,8); if(is_static)(*e)->SetStaticDoubleField(e,k,rf,d); else (*e)->SetDoubleField(e,o,rf,d); } break;
        }
        return;
    }
    /* NewObject(28,bare)/NewObjectV(29)/NewObjectA(30): same three arg-forms as the
     * Call family; converge on NewObjectA with the <init> methodID's captured sig. */
    if (slot>=28 && slot<=30){
        int form=(int)slot-28;
        uint32_t cl=PW, mid=PW; jclass rc=(jclass)real_of(cl); jmethodID rm=(jmethodID)real_of(mid); const char*sig=sig_of(mid);
        ab_jval av[16]; memset(av,0,sizeof av); jvalue*jv=(jvalue*)av;
        if(form==1){ uint32_t va=PW; ab_build_valist(&c->mem,HT(),sig,va,av,16); }
        else if(form==2){ uint32_t ap=PW; ab_build_jvalarr(&c->mem,HT(),sig,ap,av,16); }
        else { ab_build_inline(&c->mem,HT(),sig,&cur,av,16); }
        set_r0(tok((*e)->NewObjectA(e,rc,rm,jv),HK_LOCAL));
        return;
    }
    /* New<T>Array (175..182) */
    if (slot>=175 && slot<=182){ int ty=(int)slot-175; uint32_t len=PW;
        set_r0(tok(jarr_new(e,ty,(jsize)len),HK_LOCAL)); return; }
    /* Get<T>ArrayElements (183..190): copy Java array -> fresh guest buffer; the guest
     * mutates it and commits back at Release<T>ArrayElements (191..198). */
    if (slot>=183 && slot<=190){ int ty=(int)slot-183; uint32_t ar=PW; (void)PW;
        jarray ja=(jarray)real_of(ar); jsize len=(*e)->GetArrayLength(e,ja); uint32_t bytes=(uint32_t)len*jarr_esz(ty);
        uint32_t g=galloc_malloc(c->heap, bytes?bytes:1);
        if(g){ void*el=jarr_get_elems(e,ty,ja); if(el){ uc_mem_write(c->uc,g,el,bytes); jarr_rel_elems(e,ty,ja,el,JNI_ABORT); } }
        set_r0(g); return; }
    if (slot>=191 && slot<=198){ int ty=(int)slot-191; uint32_t ar=PW,g=PW,mode=PW;
        jarray ja=(jarray)real_of(ar); jsize len=(*e)->GetArrayLength(e,ja); uint32_t bytes=(uint32_t)len*jarr_esz(ty);
        if(mode!=2 /*JNI_ABORT*/){ void*el=jarr_get_elems(e,ty,ja); if(el){ uc_mem_read(c->uc,g,el,bytes); jarr_rel_elems(e,ty,ja,el,0); } }
        if(mode!=1 /*JNI_COMMIT keeps the buffer*/) gfree_heap(c,g);   /* free the Get<T>ArrayElements copy */
        return; }
    /* Get<T>ArrayRegion (199..206) / Set<T>ArrayRegion (207..214) via a host temp buffer */
    if (slot>=199 && slot<=214){ int isset=slot>=207; int ty=(int)slot-(isset?207:199);
        uint32_t ar=PW,start=PW,len=PW,buf=PW; jarray ja=(jarray)real_of(ar);
        /* `len` here comes from the GUEST (unlike the Get<T>ArrayElements path, whose length comes
         * from GetArrayLength). len*esz was computed in 32 bits and could WRAP: with len=0x40000001
         * and esz=4 the product is 4, malloc(4) succeeds, and jarr_get_region is then asked to move
         * 0x40000001 elements through that 4-byte buffer. Compute in 64 bits and reject anything
         * far larger than any real call in this game makes. */
        uint64_t bytes64=(uint64_t)len*(uint64_t)jarr_esz(ty);
        if(bytes64>ABSHIM_JNI_ARR_MAX) return;
        uint32_t bytes=(uint32_t)bytes64; void*tmp=bytes?malloc(bytes):NULL;
        if(tmp){ if(isset){ uc_mem_read(c->uc,buf,tmp,bytes); jarr_set_region(e,ty,ja,(jsize)start,(jsize)len,tmp); }
                 else { jarr_get_region(e,ty,ja,(jsize)start,(jsize)len,tmp); uc_mem_write(c->uc,buf,tmp,bytes); } free(tmp); }
        return; }
    char s1[256], s2[512];
    switch(slot){
    case 4:  set_r0((*e)->GetVersion(e)); break;
    case 6:  { uint32_t n=PW; rd_str(n,s1,sizeof s1);
               { static int fc=0; if(fc++<400) LOG("[FindClass] '%s'", s1); }   /* DIAG: pin the last subsystem before the post-scene analytics block */
               /* DE-PHONE-HOME: the post-scene block is the analytics/ads/advertising-ID/geo init
                * (Flurry, RCS ads, AdvertisingIdClient, android/location) — all phone-homes the user
                * wants removed; the game plays fully offline without them and the engine try/catches
                * the resulting class-not-found (it already handles InvocationTargetException on these).
                * Return a NULL class so the engine skips each subsystem — removes the phone-homes AND
                * the hang. Carefully NOT java/util/Locale (i18n) nor rcs/core (core util) nor Build. */
               /* NOTE (do NOT neutralize at FindClass): both the broad {flurry,rcs/ads,location} skip
                * AND the narrow {location}-only skip make FindClass return null, which the engine's
                * wrapper turns into an uncaught java::ClassNotFound that BREAKS THE SCENE CTOR (which
                * itself FindClass'es android/location + reads the rcs/ads config) -> no [g+0x80] STORE
                * -> draws=0. The phone-homes are ALREADY de-facto dead via the stripped INTERNET perm
                * (they fail cleanly and the ctor tolerates that -> the scene DID set with them present).
                * The one thing that HANGS is a location REQUEST in nativeResume (AFTER the ctor); that
                * needs a request-level neut (getSystemService(LOCATION)->null, or the request method),
                * NOT a class-level one. So FindClass here is pure passthrough (with the diag log above). */
               set_r0(tok((*e)->FindClass(e,s1),HK_LOCAL)); } break;
    case 13: { uint32_t o=PW; set_r0((*e)->Throw(e,(jthrowable)real_of(o))); } break;
    case 14: { uint32_t cl=PW,m=PW; rd_str(m,s1,sizeof s1); set_r0((*e)->ThrowNew(e,(jclass)real_of(cl),s1)); } break;
    case 15: set_r0(tok((*e)->ExceptionOccurred(e),HK_LOCAL)); break;
    case 16: (*e)->ExceptionDescribe(e); break;
    case 17: (*e)->ExceptionClear(e); break;
    case 18: { uint32_t m=PW; rd_str(m,s1,sizeof s1);   /* JNIEnv->FatalError: the engine reports an UNCAUGHT exception — offline this is a failed https phone-home (RCS/configmaster/League/FB) whose "invalid scheme" io::IOException has no handler. Do NOT forward to the real FatalError (-> art::Runtime::Abort -> SIGABRT); log it and stop THIS native call gracefully (fatal). Line 357 resets fatal before the next call, so the app survives and the lifecycle (Resume/Resize/Update/Render) continues offline. */
        LOG("[fatal-suppressed] guest JNIEnv->FatalError: '%s' — continuing offline (not aborting)", s1);
        G.disp.fatal=1; uc_emu_stop(G.cpu.uc); } break;
    case 21: { uint32_t o=PW; set_r0(tok((*e)->NewGlobalRef(e,real_of(o)),HK_GLOBAL)); } break;
    case 22: { uint32_t o=PW; (*e)->DeleteGlobalRef(e,real_of(o)); ht_delete_ref(HT(),o); } break;
    case 23: { uint32_t o=PW; (*e)->DeleteLocalRef(e,real_of(o)); ht_delete_ref(HT(),o); } break;
    case 24: { uint32_t a=PW,b=PW; set_r0((*e)->IsSameObject(e,real_of(a),real_of(b))); } break;
    case 25: { uint32_t o=PW; set_r0(tok((*e)->NewLocalRef(e,real_of(o)),HK_LOCAL)); } break;
    case 31: { uint32_t o=PW; set_r0(tok((*e)->GetObjectClass(e,real_of(o)),HK_LOCAL)); } break;
    case 32: { uint32_t o=PW,cl=PW; set_r0((*e)->IsInstanceOf(e,real_of(o),(jclass)real_of(cl))); } break;
    case 33: { uint32_t cl=PW,nm=PW,sg=PW; rd_str(nm,s1,sizeof s1); rd_str(sg,s2,sizeof s2);
               jmethodID m=(*e)->GetMethodID(e,(jclass)real_of(cl),s1,s2); set_r0(m?ht_intern_id(HT(),(void*)m,(void*)(intptr_t)cap_sig(s2)):HT_NULL); } break;
    case 113:{ uint32_t cl=PW,nm=PW,sg=PW; rd_str(nm,s1,sizeof s1); rd_str(sg,s2,sizeof s2);
               jmethodID m=(*e)->GetStaticMethodID(e,(jclass)real_of(cl),s1,s2);
               if(m) for(int i=0;i<N_NEUT_METH;i++) if(!strcmp(s1,g_neut_meth[i].name)){ g_neut_meth[i].mid=m; LOG("[de-phonehome] captured blocking SDK method %s -> will neutralize",s1); break; }
#ifndef ABSHIM_RELEASE
               /* DIAG (dedup'd): trail of every distinct static method resolved -> pinpoints the next blocking-Call class/method if one freezes */
               { static char sseen[256][40]; static int nss; int sv=0; for(int i=0;i<nss;i++) if(!strcmp(sseen[i],s1)){sv=1;break;} if(!sv&&nss<256){ strncpy(sseen[nss],s1,39); sseen[nss][39]=0; nss++; LOG("[gsmid] %s %s", s1, s2); } }
#endif
               set_r0(m?ht_intern_id(HT(),(void*)m,(void*)(intptr_t)cap_sig(s2)):HT_NULL); } break;
    case 94: { uint32_t cl=PW,nm=PW,sg=PW; rd_str(nm,s1,sizeof s1); rd_str(sg,s2,sizeof s2);
               jfieldID f=(*e)->GetFieldID(e,(jclass)real_of(cl),s1,s2); set_r0(f?ht_intern_id(HT(),(void*)f,NULL):HT_NULL); } break;
    case 144:{ uint32_t cl=PW,nm=PW,sg=PW; rd_str(nm,s1,sizeof s1); rd_str(sg,s2,sizeof s2);
               jfieldID f=(*e)->GetStaticFieldID(e,(jclass)real_of(cl),s1,s2); set_r0(f?ht_intern_id(HT(),(void*)f,NULL):HT_NULL); } break;
    case 164:{ uint32_t st=PW; set_r0((*e)->GetStringLength(e,(jstring)real_of(st))); } break;
    case 167:{ uint32_t st=PW; rd_str(st,s2,sizeof s2); set_r0(tok((*e)->NewStringUTF(e,s2),HK_LOCAL)); } break;
    case 168:{ uint32_t st=PW; set_r0((*e)->GetStringUTFLength(e,(jstring)real_of(st))); } break;
    case 169:{ uint32_t st=PW; (void)PW; const char*u=(*e)->GetStringUTFChars(e,(jstring)real_of(st),0);
               uint32_t len=u?(uint32_t)strlen(u):0; uint32_t g=galloc_malloc(c->heap,len+1); if(g){ if(u)uc_mem_write(c->uc,g,u,len); uint8_t z=0; uc_mem_write(c->uc,g+len,&z,1);} if(u)(*e)->ReleaseStringUTFChars(e,(jstring)real_of(st),u); set_r0(g); } break;
    case 170: { (void)PW; uint32_t ch=PW; gfree_heap(c,ch); } break;   /* ReleaseStringUTFChars: free the GetStringUTFChars copy */
    case 171:{ uint32_t ar=PW; set_r0((*e)->GetArrayLength(e,(jarray)real_of(ar))); } break;
    case 172:{ uint32_t n=PW,cl=PW,in=PW; set_r0(tok((*e)->NewObjectArray(e,(jsize)n,(jclass)real_of(cl),real_of(in)),HK_LOCAL)); } break;
    case 173:{ uint32_t ar=PW,ix=PW; set_r0(tok((*e)->GetObjectArrayElement(e,(jobjectArray)real_of(ar),(jsize)ix),HK_LOCAL)); } break;
    case 174:{ uint32_t ar=PW,ix=PW,v=PW; (*e)->SetObjectArrayElement(e,(jobjectArray)real_of(ar),(jsize)ix,real_of(v)); } break;
    /* object/class helpers, ref lifetime, monitors, UTF-16 strings.
     * (All primitive-array element/region ops incl. byte arrays are handled by the
     * generalized range branches above this switch.) */
    case 27: { uint32_t cl=PW; set_r0(tok((*e)->AllocObject(e,(jclass)real_of(cl)),HK_LOCAL)); } break;
    case 10: { uint32_t cl=PW; set_r0(tok((*e)->GetSuperclass(e,(jclass)real_of(cl)),HK_LOCAL)); } break;
    case 11: { uint32_t a=PW,b=PW; set_r0((*e)->IsAssignableFrom(e,(jclass)real_of(a),(jclass)real_of(b))); } break;
    case 19: { uint32_t cap=PW; jint r=(*e)->PushLocalFrame(e,(jint)cap); if(r==0 && lframe_n<64) lframe[lframe_n++]=ht_frame_mark(HT()); set_r0((uint32_t)r); } break;
    case 20: { uint32_t res=PW; jobject rr=(*e)->PopLocalFrame(e,real_of(res)); if(lframe_n>0) ht_frame_pop(HT(),lframe[--lframe_n]); set_r0(tok(rr,HK_LOCAL)); } break;
    case 26: { uint32_t cap=PW; set_r0((uint32_t)(*e)->EnsureLocalCapacity(e,(jint)cap)); } break;
    case 217:{ uint32_t o=PW; set_r0((uint32_t)(*e)->MonitorEnter(e,real_of(o))); } break;
    case 218:{ uint32_t o=PW; set_r0((uint32_t)(*e)->MonitorExit(e,real_of(o))); } break;
    case 226:{ uint32_t o=PW; set_r0(tok((*e)->NewWeakGlobalRef(e,real_of(o)),HK_WEAK)); } break;
    case 227:{ uint32_t o=PW; (*e)->DeleteWeakGlobalRef(e,(jweak)real_of(o)); ht_delete_ref(HT(),o); } break;
    case 232:{ uint32_t o=PW; set_r0((uint32_t)(*e)->GetObjectRefType(e,real_of(o))); } break;
    /* NewString: len*2 was computed in 32 bits and could wrap (len=0x80000000 -> 0); jsize is also
     * signed, so a huge len reaches the JVM negative. Bound it like the array regions. */
    case 163:{ uint32_t p=PW,len=PW; if((uint64_t)len*2ull>ABSHIM_JNI_ARR_MAX){ set_r0(tok(NULL,HK_LOCAL)); break; }
               jchar*t=(jchar*)malloc(((size_t)(len?len:1))*2); if(t) uc_mem_read(c->uc,p,t,(size_t)len*2);
               jstring s=t?(*e)->NewString(e,t,(jsize)len):(*e)->NewString(e,NULL,0); free(t); set_r0(tok(s,HK_LOCAL)); } break;
    case 165:{ uint32_t st=PW; (void)PW; jstring js=(jstring)real_of(st); jsize len=(*e)->GetStringLength(e,js);
               const jchar*u=(*e)->GetStringChars(e,js,0); uint32_t g=galloc_malloc(c->heap,(uint32_t)(len+1)*2);
               if(g){ if(u)uc_mem_write(c->uc,g,u,(uint32_t)len*2); uint16_t z=0; uc_mem_write(c->uc,g+(uint32_t)len*2,&z,2);} if(u)(*e)->ReleaseStringChars(e,js,u); set_r0(g); } break;
    case 166:{ (void)PW; uint32_t ch=PW; gfree_heap(c,ch); } break;   /* ReleaseStringChars: free the GetStringChars copy */
    case 219:{ uint32_t pvm=PW; if(pvm) gm_wr32(&c->mem,pvm,J->vm); set_r0(0); } break;
    case 221:{ uint32_t st=PW,start=PW,len=PW,buf=PW; char tmp[1024]; (*e)->GetStringUTFRegion(e,(jstring)real_of(st),(jsize)start,(jsize)len,tmp);
               uint32_t bl=(uint32_t)strlen(tmp); if(buf){ uc_mem_write(c->uc,buf,tmp,bl); uint8_t z=0; uc_mem_write(c->uc,buf+bl,&z,1);} } break;
    case 228: set_r0((*e)->ExceptionCheck(e)); break;
    /* Any JNIEnv method not explicitly bridged above limps with 0/NULL — but say
     * so LOUDLY (once per slot) so the on-device logcat names exactly which method
     * the engine needs, instead of a silent wrong value. (Defensive-correct: the
     * unexpected is visible, mirroring the libc UNIMPL + coverage gate.) */
    default: { static uint8_t seen[260]; if(slot<260 && !seen[slot]){ seen[slot]=1;
                 LOGE("JNIEnv slot %u UNHANDLED -> 0 (bridge it in env_dispatch_real if the engine calls it)", slot); }
               set_r0(0); } break;
    }
    #undef PW
    #undef PWD
}
static void vm_dispatch_real(jni_state*J, uint32_t slot){
    cpu_t*c=J->cpu; mcur cur; marshal_cur_init(&c->mem,&cur); (void)marshal_pull_word(&c->mem,&cur);
    switch(slot){
    case 4: case 6: case 7: { uint32_t penv=marshal_pull_word(&c->mem,&cur); if(penv) gm_wr32(&c->mem,penv,J->env); set_r0(0); } break;
    case 3: case 5: set_r0(0); break;   /* DestroyJavaVM / DetachCurrentThread -> JNI_OK (intentional) */
    default: { static uint8_t seen[16]; uint32_t k=slot<16?slot:15; if(!seen[k]){ seen[k]=1; LOGE("JavaVM slot %u UNHANDLED -> 0", slot); } set_r0(0); } break;
    }
}

static uint32_t resolve_guest(const char*name){
    for(int i=0;i<G.nsc;i++) if(!strcmp(G.scache[i].name,name)) return G.scache[i].addr;
    uint32_t si; if(elf32_find_symbol(&G.ld.img,name,&si)) return 0;
    elf32_symres r=elf32_classify(&G.ld.img,si);
    uint32_t a=(RG_ENGINE+r.value)|(r.thumb?1u:0u);
    if(G.nsc<128){ G.scache[G.nsc].name=name; G.scache[G.nsc].addr=a; G.nsc++; }
    return a;
}

#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
/* Time spent INSIDE the shim per Java->native entry, and by subtraction the time spent OUTSIDE it.
 * shim_call is the only door from ART into this port, so the gap between one return and the next
 * entry is precisely the Java side: GLSurfaceView's eglSwapBuffers, SwiftShader's rasterisation and
 * any vsync wait - none of which the shim can see from within. Timing the bridge (gl_try) showed
 * only that our marshalling is negligible; this is what actually splits "the port's cost" from
 * "everything else in the frame". Wrapper rather than inline timers because shim_call has many
 * return paths and instrumenting each one would eventually miss one. */
/* THREAD-LOCAL. shim_call is entered from several ART threads (render, audio, lifecycle), so
 * process-wide accumulators overlap: a first version summed them and reported IN 93% + OUT 65% =
 * 158% of wall time, which is impossible and is exactly how you notice the model is wrong. Each
 * thread now accounts separately, and the perf line reports the thread that emits it - the render
 * thread - so IN + OUT is a partition of that thread's wall time and must total ~100%. */
static __thread uint64_t g_in_ns = 0, g_out_ns = 0, g_calls = 0, g_last_exit = 0;
uint64_t shim_in_ns(void){ return g_in_ns; }
uint64_t shim_out_ns(void){ return g_out_ns; }
uint64_t shim_entries(void){ return g_calls; }
static uint64_t sc_now(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static jvalue shim_call_inner(JNIEnv *env, jobject thiz, const char *name, const char *shorty, jvalue *args, int nargs);
jvalue shim_call(JNIEnv *env, jobject thiz, const char *name, const char *shorty, jvalue *args, int nargs){
    uint64_t t0 = sc_now();
    if (g_last_exit) g_out_ns += t0 - g_last_exit;
    jvalue r = shim_call_inner(env, thiz, name, shorty, args, nargs);
    uint64_t t1 = sc_now();
    g_in_ns += t1 - t0; g_last_exit = t1; g_calls++;
    return r;
}
static jvalue shim_call_inner(JNIEnv *env, jobject thiz, const char *name, const char *shorty, jvalue *args, int nargs){
#else
jvalue shim_call(JNIEnv *env, jobject thiz, const char *name, const char *shorty, jvalue *args, int nargs){
#endif
    jvalue ret; memset(&ret,0,sizeof ret);
    if(!G.ready) return ret;
    g_cur_native = name;                                        /* DIAG watchdog: which native entry is executing */
    /* AUDIO ISOLATION (interim — clearly NOT the final state): AudioOutput.nativeMixData runs on ART's
     * audio thread and makes a BLOCKING JNI CallBooleanMethodV (slot 38) into ART while this shim_call
     * holds the coarse BEL + the scheduler GEL -> the render/update thread (which needs the BEL) is
     * starved -> the game freezes with the scene built but 0 draws (the cont.24-flagged audio/BEL
     * cross-thread hazard, now confirmed via the [WATCHDOG] on the x86-shim-in-real-ART rig). Until the
     * proper fix lands (release the BEL/GEL around blocking JNI callouts per Audit-02/S2, OR fix the
     * upstream GameLua _Rep corruption that may hand nativeMixData a bad methodID), NO-OP the native
     * mix so the render path is unblocked and draws>0 becomes reachable. Silent audio; gate-cleaned
     * with the diag hooks before the deliverable. */
    /* AUDIO: default silent (no-op) — the correct shipping choice. -DABSHIM_AUDIO is an EXPERIMENTAL
     * build (cont.119): it runs the mixer + enables the BEL-release in jni_block_do_cb, which DOES
     * break the cont.118 cross-thread deadlock (0 WATCHDOG freezes, mixer loop runs, blocking calls
      * return).
      *
      * STATUS CORRECTED 2026-07-28. This comment used to end "but then CRASHES with a __stack_chk_fail
      * in nativeResume ... deferred". THAT CRASH NO LONGER OCCURS — the note described a state the
      * code had already moved past. Measured across all three captured audio runs (audioplay,
      * audiomod, audio): `nativeMixData ENABLED` appears 8x in each, so the mixer really is running,
      * with stack_chk_fail = 0 and h_fatal = 0 in every one. The variant plays and wins with audio
      * active (PROOF_10, PROOF_17).
      *
      * A stale "this crashes, work deferred" note is worse than none: it tells the next reader that a
      * SHIPPED variant is broken while evidence in the same repo shows it working, and it contradicts
      * RELEASE_NOTES.md, which offers the audio APK as crash-free.
      *
      * WHAT REMAINS OPEN is narrower and device-only: whether playback is CONTINUOUS on real hardware.
      * The emulator's audio backend cannot init headless, so the buffer never drains there — silence or
      * stutter on the host proves nothing either way. See OPEN_FINDINGS R9.
      *
      * The default (silent) build remains the shipping choice, byte-identical to the deliverable. */
    if(name && strstr(name,"nativeMixData")){
#ifndef ABSHIM_AUDIO
        static int am=0; if(am++<3) LOG("[audio-isolate] nativeMixData no-op (unblock render thread; BEL/audio fix pending)"); return ret;
#else
        { static int am=0; if(am++<8) LOG("[audio] nativeMixData ENABLED (WIP BEL-release; concurrency crash pending)"); }
#endif
    }
#ifdef ABSHIM_SLOWCALL
    /* ---- SLOWCALL: separate "this call was long" from "this call waited for the lock" ----
     * R63 established that Android delivers a touch in ~1 ms and the whole multi-second latency is
     * inside the shim, with input blocking on the BEL held by the render call. What it could NOT
     * establish is WHY the render call is long: a far-off idle sleep (my first story, which its own
     * dump contradicted) or simply a genuinely long nativeUpdate doing asset/level work.
     *
     * Those two are trivially separable and nothing measured them: time the BEL WAIT and the call
     * BODY independently, and print any call whose total crosses a threshold. A spike then reads as
     * either belwait>>body (someone else held the lock: look at who) or body>>belwait (this call
     * itself is slow: look at what it does).
     *
     * Diagnostic variant only (ABSHIM_SLOWCALL=1 bash port/build_apk.sh), so the shipping artifact
     * stays byte-identical - same discipline as ABSHIM_GPUCAP. Verified: the default build's sha256 is
     * unchanged with these blocks present.
     *
     * LIMITATION, worth knowing before reading a silent log: this reports at call EXIT, so a call that
     * has not returned yet prints nothing. On the A56 the whole ~10-minute cold first launch produced
     * ZERO lines for exactly that reason - the boot call was still running. It is the right shape for
     * catching input spikes (those end) and the wrong shape for diagnosing a hang; a hang needs
     * entry-side logging plus a watchdog, which the sched-dump already provides. */
    uint64_t _sc_want = slowcall_now_ns();
#endif
    pthread_mutex_lock(&G.bel);                                  /* the BEL: serialise Java entries */
#ifdef ABSHIM_SLOWCALL
    uint64_t _sc_got = slowcall_now_ns();
#endif
    G.jni.real_env = env;
    gthread *gt = sched_host_gthread(&G.sch);                    /* per-Java-thread guest context */
    if(!gt){ pthread_mutex_unlock(&G.bel); return ret; }
    uint32_t gaddr = resolve_guest(name);
    /* log the FIRST time each distinct native is called -> the on-device logcat shows the
     * boot/call sequence (nativeInit, nativeResize, nativeUpdate, ...) without flooding */
    { static char seen[128][48]; static int nseen; static int capped=0; int sv=0;
      for(int i=0;i<nseen;i++) if(!strcmp(seen[i],name)){ sv=1; break; }
      if(!sv){ if(nseen<128){ strncpy(seen[nseen],name,47); seen[nseen][47]=0; nseen++;
                 LOG("call[%d] %s (%s) @0x%x", nseen, name, shorty, gaddr); }   /* log ONLY on store */
               else if(!capped){ capped=1; LOG("call-log: >128 distinct natives; further first-call logs suppressed"); } } }  /* INFO, not LOGE: this is a diagnostic notice, and ONDEVICE.md tells people to triage by looking for errors */
    uint32_t thiz_tok = tok(thiz, HK_LOCAL);
    uint32_t frame = ht_frame_mark(HT());
    /* pass 1: stack bytes for spilled args (env=r0, thiz=r1 already) */
    int ncrn=2; uint32_t sbytes=0; const char*sp=shorty+1;
    for(int i=0;i<nargs;i++){ char t=sp[i]; int is64=(t=='J'||t=='D');
        if(is64){ if(ncrn&1)ncrn++; if(ncrn<=2)ncrn+=2; else { sbytes=(sbytes+7)&~7u; sbytes+=8; ncrn=4; } }
        else { if(ncrn<4)ncrn++; else sbytes+=4; } }
    uint32_t stacktop=gt->stack_hi;                             /* this thread's own stack slice */
    uint32_t gsp=(stacktop-sbytes)&~7u;
    /* pass 2: place */
    uc_reg_write(G.cpu.uc,UC_ARM_REG_SP,&gsp);
    mcur pc={.ncrn=0,.nsaa=gsp};
    marshal_place_word(&G.cpu.mem,&pc,G.jni.env);
    marshal_place_word(&G.cpu.mem,&pc,thiz_tok);
    for(int i=0;i<nargs;i++){ char t=shorty[1+i];
        if(t=='J'){ marshal_place_dword(&G.cpu.mem,&pc,(uint64_t)args[i].j); }
        else if(t=='D'){ uint64_t b; memcpy(&b,&args[i].d,8); marshal_place_dword(&G.cpu.mem,&pc,b); }
        else if(t=='F'){ uint32_t b; memcpy(&b,&args[i].f,4); marshal_place_word(&G.cpu.mem,&pc,b); }
        else if(t=='L'){ marshal_place_word(&G.cpu.mem,&pc,tok(args[i].l,HK_LOCAL)); }
        else { marshal_place_word(&G.cpu.mem,&pc,(uint32_t)args[i].i); } }
    uint32_t lr=RG_RET; uc_reg_write(G.cpu.uc,UC_ARM_REG_LR,&lr);
    G.disp.fatal=0;
    uc_err er=sched_enter(&G.sch, gt, gaddr, 0);    /* run under the GEL scheduler; green threads */
    if(er!=UC_ERR_OK || G.disp.fatal){ uint32_t pc2=0; uc_reg_read(G.cpu.uc,UC_ARM_REG_PC,&pc2);
        LOGE("shim_call %s emu=%s fatal=%d pc=0x%x", name, uc_strerror(er), G.disp.fatal, pc2);
        /* Do NOT (*env)->FatalError here — that aborts the app. A guest fatal (uncaught exception from a
         * failed offline phone-home, or abort()) just fails THIS native call; fatal is reset at line 357
         * before the next call, so the app survives and the lifecycle continues offline. */
        if(G.disp.fatal){ LOGE("shim_call %s: guest fatal SUPPRESSED (offline; not aborting)", name); } }
    /* on-device draw-activity signal: every 300th FRAME native, report cumulative GL draws.
     * The frame driver is nativeUpdate in single-thread mode (nativeUpdate does update+render)
     * and nativeRender in multi-thread mode — fire on either so this works in both. draws
     * climbing = geometry submitted; stuck at 0 = booted but drawing nothing. */
    { size_t ln=strlen(name);
      int isframe = ln>=12 && (!strcmp(name+ln-12,"nativeRender") || !strcmp(name+ln-12,"nativeUpdate"));
      if(isframe){ static unsigned rf; static unsigned long d0;
#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
        /* Emulation share of wall time, sampled with the frame log. Separates the port's own cost
         * from the rasteriser's, which is what SwiftShader-based numbers cannot do. */
        { extern uint64_t gl_bridge_ns(void); extern uint64_t gl_bridge_calls(void);
          extern uint64_t stub_bridge_ns(void); extern uint64_t stub_bridge_calls(void);
          extern uint64_t jni_pass_ns(void); extern uint64_t jni_block_ns(void);
          static uint64_t w0=0, g0=0, c0=0, i0=0, o0=0, e0=0, s0=0, j0=0, sc0=0, b0=0; static int prim=0;
          extern uint64_t shim_in_ns(void); extern uint64_t shim_out_ns(void); extern uint64_t shim_entries(void);
          struct timespec _ts; clock_gettime(CLOCK_MONOTONIC,&_ts);
          uint64_t wn=(uint64_t)_ts.tv_sec*1000000000ull+(uint64_t)_ts.tv_nsec;
          uint64_t gn=gl_bridge_ns(), cn=gl_bridge_calls();
          if(!prim){ prim=1; w0=wn; g0=gn; c0=cn; i0=shim_in_ns(); o0=shim_out_ns(); e0=shim_entries();
                     s0=stub_bridge_ns(); j0=jni_pass_ns(); sc0=stub_bridge_calls(); b0=jni_block_ns(); }
          else if((rf % 300u)==0u && wn>w0){
              uint64_t dw=wn-w0, dg=gn-g0;
              uint64_t di = shim_in_ns()-i0, dou = shim_out_ns()-o0;
              /* Split the in-shim time. ds = ALL native bridges (GL + asset + libc + file + bent),
               * dj = guest->JVM JNI passthrough (a different hook, so not inside ds). What is left
               * is the emulator proper: Unicorn translating/running ARM32, plus hook dispatch and
               * the scheduler. That residual is the number that decides whether this port is
               * CPU-bound in a way we could optimise, or bound by the emulator itself. */
              /* JNI has TWO halves and only counting the first would misattribute the second to
               * the emulator: dispatch (inside the RG_JNI hook) and the blocking ART call (in
               * run_loop, GEL released). See jni_block_do_cb. */
              uint64_t ds = stub_bridge_ns()-s0;
              uint64_t djd = jni_pass_ns()-j0, djb = jni_block_ns()-b0, dj = djd + djb;
              uint64_t acct = ds + dj;
              uint64_t demu = (di > acct) ? (di - acct) : 0;
              /* Invariants. Two prior measurements in this work were wrong in ways no test caught —
               * only impossible arithmetic exposed them — so the impossible cases are named in the
               * output instead of being left for a reader to notice: GL is dispatched from stub_cb
               * so it must be a subset of ds, and ds+dj cannot exceed the in-shim time that
               * contains them. Either violation means the accounting is broken, not that the port
               * is slow, and BAD makes that unmissable. */
              const char *bad = (dg > ds || acct > di) ? "  [BAD: accounting violated]" : "";
              LOG("[perf] frames=%u wall=%llums | IN-shim=%llums (%llu%%) of which GLbridge=%llums | OUT-shim(Java swap/vsync)=%llums (%llu%%) | entries=%llu",
                  rf, (unsigned long long)(dw/1000000ull),
                  (unsigned long long)(di/1000000ull), (unsigned long long)(di*100ull/dw),
                  (unsigned long long)(dg/1000000ull),
                  (unsigned long long)(dou/1000000ull), (unsigned long long)(dou*100ull/dw),
                  (unsigned long long)(shim_entries()-e0));
              LOG("[perf-split] IN-shim=%llums = emulator=%llums (%llu%% of frame) + bridges=%llums (%llu%%, %llu calls, GL %llums) + JNI=%llums (%llu%%: dispatch %llums + ART-blocking %llums)%s",
                  (unsigned long long)(di/1000000ull),
                  (unsigned long long)(demu/1000000ull), (unsigned long long)(demu*100ull/dw),
                  (unsigned long long)(ds/1000000ull), (unsigned long long)(ds*100ull/dw),
                  (unsigned long long)(stub_bridge_calls()-sc0),
                  (unsigned long long)(dg/1000000ull),
                  (unsigned long long)(dj/1000000ull), (unsigned long long)(dj*100ull/dw),
                  (unsigned long long)(djd/1000000ull), (unsigned long long)(djb/1000000ull), bad);
              i0=shim_in_ns(); o0=shim_out_ns(); e0=shim_entries();
              w0=wn; g0=gn; c0=cn; s0=stub_bridge_ns(); j0=jni_pass_ns(); sc0=stub_bridge_calls(); b0=jni_block_ns();
          } }
#endif
        if((++rf % 300u)==1u){ LOG("frame[%u] GL draws=%lu (+%lu since last) clears=%lu useProgram=%lu",
            rf, g_gl_draws, g_gl_draws-d0, g_gl_clears, g_gl_useprog); d0=g_gl_draws; } } }
    /* convert return per shorty[0] */
    uint32_t r0=0,r1=0; uc_reg_read(G.cpu.uc,UC_ARM_REG_R0,&r0); uc_reg_read(G.cpu.uc,UC_ARM_REG_R1,&r1);
    switch(shorty[0]){
    case 'V': break;
    case 'Z': ret.z=(jboolean)(r0&1); break;  case 'B': ret.b=(jbyte)r0; break;
    case 'C': ret.c=(jchar)r0; break;         case 'S': ret.s=(jshort)r0; break;
    case 'I': ret.i=(jint)r0; break;
    case 'J': ret.j=(jlong)(((uint64_t)r1<<32)|r0); break;
    case 'F': memcpy(&ret.f,&r0,4); break;
    case 'D': { uint64_t b=((uint64_t)r1<<32)|r0; memcpy(&ret.d,&b,8); } break;
    case 'L': ret.l=real_of(r0); break;
    }
    ht_frame_pop(HT(), frame);                                  /* free this activation's local tokens */
#ifdef ABSHIM_SLOWCALL
    {   uint64_t _sc_end = slowcall_now_ns();
        double bel_ms  = (double)(_sc_got  - _sc_want) / 1e6;
        double body_ms = (double)(_sc_end  - _sc_got ) / 1e6;
        /* 120 ms: well above a healthy frame at the measured ~20 fps, well below the spikes. */
        if (bel_ms + body_ms > 120.0)
            LOG("[slowcall] %s total=%.0fms belwait=%.0fms body=%.0fms  (%s)",
                name, bel_ms + body_ms, bel_ms, body_ms,
                bel_ms > body_ms ? "WAITED for the BEL - another call held it"
                                 : "THIS call was slow - not lock contention");
    }
#endif
    pthread_mutex_unlock(&G.bel);
    return ret;
}

/* ---- JNI_OnLoad: load the 32-bit engine + wire everything ---- */
static int load_engine_bytes(void){
    Dl_info di; if(!dladdr((void*)&shim_call,&di) || !di.dli_fname) return -1;
    char path[1024];
    /* Reject a TRUNCATED path outright. Without this, a path >= sizeof(path) would be silently
     * cut and we would go on to open some other file (or fail confusingly). */
    if(snprintf(path,sizeof path,"%s",di.dli_fname) >= (int)sizeof path){ LOGE("engine path too long"); return -1; }
    char*sl=strrchr(path,'/'); if(!sl) return -1;
    /* Bound the basename swap. This was an unbounded strcpy into a fixed buffer: with a directory
     * part within 15 bytes of the end it would write past `path`. Android's native-lib dir is far
     * shorter than that, so it was not reachable in practice - but the shipping loader should not
     * depend on the platform's path length to stay in bounds. */
    if(sizeof path - (size_t)(sl+1-path) < sizeof "libengine32.so"){ LOGE("engine path too long"); return -1; }
    memcpy(sl+1,"libengine32.so",sizeof "libengine32.so");
    int fd=open(path,O_RDONLY); if(fd<0){ LOGE("open %s failed",path); return -1; }
    /* fstat was unchecked: on failure st.st_size is indeterminate and we would mmap a garbage
     * length. Also reject a zero/negative size rather than handing mmap a length of 0. */
    struct stat st;
    if(fstat(fd,&st) || st.st_size<=0){ LOGE("fstat %s failed",path); close(fd); return -1; }
    G.engine_len=(size_t)st.st_size;
    void*m=mmap(0,G.engine_len,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(m==MAP_FAILED){ LOGE("mmap engine failed"); return -1; }
    G.engine=(uint8_t*)m; LOG("engine %zu bytes from %s",G.engine_len,path); return 0;
}

/* DIAGNOSTIC (temporary): watch writes to the game-ready globals [g+0x7c]/[g+0x80]
 * (RG_ENGINE+0xabb124/0xabb128) so the emulator's REAL-lifecycle boot shows whether the
 * game object + ready flag get set with a real app object + real JNI. Logs to logcat. */
static void gobj_wp_shim(uc_engine*uc, uc_mem_type ty, uint64_t addr, int sz, int64_t val, void*u){
    (void)ty;(void)sz;(void)u; static int n=0; if(n++>=48) return;
    uint32_t pc=0; uc_reg_read(uc,UC_ARM_REG_PC,&pc);
    LOG("gobj WRITE [engine+0x%x]=0x%x from engine+0x%x",
        (uint32_t)(addr-RG_ENGINE), (uint32_t)(val&0xffffffffu), pc>=RG_ENGINE?pc-RG_ENGINE:pc);
}
/* DIAGNOSTIC (temporary): log every guest C++ throw (type name) + trace the scene-ctor path that
 * sets [g+0x80]. __cxa_throw@engine+0x85a1d0; F@0x1de254 factory 0x72680 ctor 0x6f370. */
#define DIAG_E(off) (RG_ENGINE + (uint64_t)(off))
static void diag_read_str(uc_engine*uc, uint64_t gptr, char*out, int max){
    int i=0; out[0]=0; if(!gptr) return;
    for(; i<max-1; i++){ uint8_t c=0; if(uc_mem_read(uc,gptr+(unsigned)i,&c,1)) break; if(!c) break; out[i]=(char)c; }
    out[i]=0;
}
/* DIAG: ring of the last 256 basic-block PCs; dumped when nativeResume first enters (right after
 * nativeInit returns) so the tail shows where 0x118f30's subtree diverged + unwound back. */
static uint32_t g_ring[256]; static unsigned g_ringpos=0; static int g_ringon=0, g_ringdumped=0;
static unsigned char g_seen[0xB00000u/16u];   /* 1-bit per 2-byte engine offset (~700KB BSS) */
static uint32_t g_fwd[1024]; static unsigned g_fwdn=0;   /* first N distinct blocks, buffered (no logd flood) */
static uint32_t g_minsp=0xffffffffu, g_minsp_pc=0;      /* deepest SP during the ctor window */
static unsigned long g_visit=0;                          /* 0x7d2b28 per-node visitor call count */
static uint32_t g_vnode[24]; static int g_vn=0;          /* first 24 visited node pointers (cycle check) */
static uint32_t g_rnode[24], g_rchild[24], g_rtype[24]; static unsigned g_rn=0;  /* last 24 recursion nodes */
static uint32_t g_rres[24], g_rshift[24], g_rarr[24]; static unsigned g_lastpos=0;  /* resolver output + hash inputs */
static uint32_t g_rlr[24]; static uint8_t g_rcolor[24];  /* caller LR + [node+5] color at each recursion */
static uint32_t g_gen=0xffffffffu; static int g_scanned=0;  /* [ctx+0x14] generation (once) + stack-scan-done */
static uint32_t g_w_arg0[32], g_w_m[32]; static uint8_t g_w_ty[32]; static unsigned g_wmn=0;  /* W(0x7d3be0) recursion M+type ring */
static unsigned long g_wcount=0; static uint32_t g_wsp0=0, g_wspmin=0xffffffffu;  /* W entry depth probe */
static uint32_t g_wk_m[32], g_wk_node[32]; static uint8_t g_wk_mc[32], g_wk_nc[32]; static int g_wk_cnt[32]; static unsigned g_wkn=0;  /* walk color-guard ring */
static void diag_ring_dump(void){
    char line[256]; int c; unsigned i;
    LOG("FWD: first %u distinct blocks from ctor entry (engine-rel):", g_fwdn);
    c=0; line[0]=0;
    for(i=0;i<g_fwdn;i++){ c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"%x ",g_fwd[i]);
        if((i&7u)==7u){ LOG("  %s",line); c=0; line[0]=0; } }
    if(c) LOG("  %s",line);
    LOG("MINSP: deepest guest SP during ctor = 0x%x (at engine+0x%x); stack base RG_STACK=0x%x",
        g_minsp, g_minsp_pc, (unsigned)RG_STACK);
    LOG("RECURSE: 0x7c53fc(node) called %lu times (huge => corrupted/cyclic tree; ~stackfill); generation(ctx+0x14)=%u", g_visit, g_gen);
    LOG("W(0x7d3be0) entries=%lu wsp0=0x%x wspmin=0x%x (depth<<overflow => FAULT not stackfill); walk(0x7d2d18) calls=%u",
        g_wcount, g_wsp0, g_wspmin, g_wkn);
    { unsigned cn=g_wmn<32u?g_wmn:32u, st=g_wmn<32u?0u:(g_wmn&31u);
      for(unsigned k=0;k<cn;k++){ unsigned i=(st+k)&31u;
        LOG("  W[-%u] arg0=0x%x M=0x%x type[M+0x15]=%u", cn-k, g_w_arg0[i], g_w_m[i], g_w_ty[i]); } }
    { unsigned cnt = g_rn<24u?g_rn:24u, st = g_rn<24u?0u:(g_rn & 23u);
      for(unsigned k=0;k<cnt;k++){ unsigned i=(st+k)&23u;
        uint32_t mask=(g_rshift[i]?((1u<<g_rshift[i])-1u):0u);
        uint32_t idxA=(mask?(g_rchild[i]&mask):0u), expA=g_rarr[i]+idxA*20u;   /* type2 &-hash expected node */
        LOG("  recurse[-%u] node=0x%x type=%u [node]=0x%x color[+5]=%u caller-LR=0x%x -> RESOLVED=0x%x  (shift=%u mask=0x%x arr=0x%x &-exp=0x%x)",
            cnt-k, g_rnode[i], g_rtype[i], g_rchild[i], g_rcolor[i], g_rlr[i], g_rres[i], g_rshift[i], mask, g_rarr[i], expA); } }
    unsigned n = g_ringpos<256u?g_ringpos:256u, start = g_ringpos<256u?0u:(g_ringpos & 255u);
    LOG("RING: last %u blocks before nativeInit returned (chronological, engine-rel):", n);
    c=0; line[0]=0;
    for(i=0;i<n;i++){ uint32_t v=g_ring[(start+i)&255u];
        c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"%x ",v);
        if((i&7u)==7u){ LOG("  %s",line); c=0; line[0]=0; } }
    if(c) LOG("  %s",line);
}
static void diag_pc_hook(uc_engine*uc, uint64_t addr, uint32_t size, void*u){
    (void)size;(void)u; uint32_t off=(uint32_t)(addr-RG_ENGINE);
    if(addr==DIAG_E(0x85a1d0)){ /* __cxa_throw(r0=exc,r1=std::type_info*,r2=dtor) */
        static int nt=0; if(nt++>=40) return;
        uint32_t ti=0,name=0; char nm[96]="?"; uc_reg_read(uc,UC_ARM_REG_R1,&ti);
        if(ti){ uc_mem_read(uc,ti+4,&name,4); if(name) diag_read_str(uc,name,nm,sizeof nm); }
        uint32_t lr=0; uc_reg_read(uc,UC_ARM_REG_LR,&lr);       /* thrower = where __cxa_throw was called from */
        LOG("THROW #%d type=%s (ti=engine+0x%x) thrower=+0x%x", nt, nm, ti>=RG_ENGINE?ti-(uint32_t)RG_ENGINE:ti, lr>=RG_ENGINE?lr-RG_ENGINE:lr);
        if(ti==DIAG_E(0xaa6d30)){   /* io::IOException — #11 escapes -> engine FatalError -> SIGABRT. Backtrace to find the failing VFS op + its caller (which subsystem's read fails). */
            uint32_t exc=0; uc_reg_read(uc,UC_ARM_REG_R0,&exc);   /* the exception object: find its message std::string (holds "Invalid scheme '{s}' in URI: {u}") */
            for(int off=4; off<=20; off+=4){ uint32_t mp=0; uc_mem_read(uc,exc+(uint32_t)off,&mp,4);
                if(mp>=RG_HEAP && mp<RG_STACK){ char m[200]="?"; diag_read_str(uc,mp,m,sizeof m); if(m[0] && (m[0]>=32&&m[0]<127)) LOG("  ioex-msg[+%d]='%s'",off,m); } }
            uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp);
            char l[256]; int cc=0, shown=0;
            for(uint32_t o=0;o<0x400u && shown<20;o+=4){ uint32_t w=0; uc_mem_read(uc,sp+o,&w,4);
                if(w>=RG_ENGINE && w<RG_ENGINE+0x966000u){ cc+=snprintf(l+cc,(size_t)(sizeof l-(size_t)cc),"+0x%x ",w-RG_ENGINE); shown++;
                    if(shown%8==0){ LOG("  ioex-bt: %s",l); cc=0; l[0]=0; } } }
            if(cc) LOG("  ioex-bt: %s",l);
        }
        if(ti==DIAG_E(0xaaea04)){   /* St11logic_error — the LEVEL-END ad-config missing-key crash (guard'd empty->{} then a REQUIRED key is absent). Capture what()+backtrace to pin the ad caller to neutralize. */
            uint32_t exc=0; uc_reg_read(uc,UC_ARM_REG_R0,&exc);
            for(int off=4; off<=20; off+=4){ uint32_t mp=0; uc_mem_read(uc,exc+(uint32_t)off,&mp,4);
                if(mp>=RG_HEAP && mp<RG_STACK){ char m[200]="?"; diag_read_str(uc,mp,m,sizeof m); if(m[0] && (m[0]>=32&&m[0]<127)) LOG("  logicerr-msg[+%d]='%s'",off,m); } }
            uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp);
            char l[256]; int cc=0, shown=0;
            for(uint32_t o=0;o<0x400u && shown<24;o+=4){ uint32_t w=0; uc_mem_read(uc,sp+o,&w,4);
                if(w>=RG_ENGINE && w<RG_ENGINE+0x966000u){ cc+=snprintf(l+cc,(size_t)(sizeof l-(size_t)cc),"+0x%x ",w-RG_ENGINE); shown++;
                    if(shown%8==0){ LOG("  logicerr-bt: %s",l); cc=0; l[0]=0; } } }
            if(cc) LOG("  logicerr-bt: %s",l);
        }
        return;
    }
    switch(off){
      case 0x1de294: LOG("F@0x1de294 empty-vector check"); break;
      case 0x1de2f8: LOG("F@0x1de2f8 loop-exit reached"); break;
      case 0x1de308:{ uint32_t r1=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); LOG("F@0x1de308 [g+0x7c]=0x%x (skip->0x1de4f0 if 0)",r1);} break;
      case 0x1de310: LOG("F@0x1de310 -> call scene factory 0x72680"); break;
      case 0x1de318:{ uint32_t r0=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); LOG("F@0x1de318 STORE [g+0x80]=0x%x  <<< scene set",r0);} break;
      case 0x1de364: LOG("F@0x1de364 empty-vector EXIT (no scene)"); break;
      case 0x1de4f0: LOG("F@0x1de4f0 [g+0x7c]==0 SKIP (no scene)"); break;
      case 0x726a0: LOG("factory@0x726a0 -> ctor 0x6f370"); g_ringon=1; break;
      case 0x1df5b4: if(!g_ringdumped){ g_ringdumped=1; g_ringon=0; diag_ring_dump(); } break; /* nativeResume entry */
      case 0x726a4: LOG("factory@0x726a4 ctor RETURNED ok"); break;
      case 0x726ac: LOG("factory@0x726ac ctor THREW -> delete+rethrow"); break;
    }
}
/* DIAG: UC_HOOK_BLOCK scoped to the scene ctor 0x6f370..0x6f928 — logs only the ctor's OWN blocks,
 * so the LAST one before the frame loop = the call site after which control never returned. */
static void diag_ctor_bb(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)uc;(void)sz;(void)u; static int n=0; if(n++<220) LOG("ctorBB @0x%x", (uint32_t)(addr-RG_ENGINE));
}
static void diag_stackscan(uc_engine*uc);   /* fwd */
static void diag_ring_bb(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)sz;(void)u; if(!g_ringon) return;
    uint32_t off=(uint32_t)(addr-RG_ENGINE);
    if(off<0xB00000u){ unsigned bit=off>>1, idx=bit>>3, msk=1u<<(bit&7u);
        if(!(g_seen[idx]&msk)){ g_seen[idx]|=(unsigned char)msk; if(g_fwdn<1024u) g_fwd[g_fwdn++]=off; } }
    { uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); if(sp<g_minsp){ g_minsp=sp; g_minsp_pc=off; } }
    /* (low-SP STACKSCAN trigger removed: ctor runs on WORKER slot-1 whose stack 0x7f5ff000-0x7f7ff000
     * is naturally below the threshold => it fired prematurely. The real capture is at the mem-fault.) */
    g_ring[g_ringpos & 255u]=off; g_ringpos++;
}
/* DIAG: one-time scan of the live guest stack DURING the recursion (triggered from diag_recurse at
 * a fixed depth). Return addresses land in the engine CODE range [RG_ENGINE, +0xB00000); data ptrs
 * are heap(0x50..)/stack(0x70..) and get filtered out. The REPEATING sequence of engine addrs = the
 * recursive call-chain; its PERIOD names the mutually-recursive functions to inspect for the guard. */
static void diag_stackscan(uc_engine*uc){
    uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp);
    LOG("STACKSCAN sp=0x%x (stack_lo=0x7f7ff000)  W(0x7d3be0) entries=%lu wsp0=0x%x wspmin=0x%x avgframe=%ldB",
        sp, g_wcount, g_wsp0, g_wspmin, g_wcount?(long)((g_wsp0-g_wspmin)/g_wcount):0);
    char line[256]; int c=0; line[0]=0; unsigned found=0;
    LOG("  (return-addr chain up from SP, engine-rel, innermost first — PERIOD = recursion cycle):");
    for(unsigned off=0; off<0x10000u && found<120u; off+=4){       /* scan 64KB up from SP */
        uint32_t w=0; if(uc_mem_read(uc,sp+off,&w,4)) break;
        if(w>=RG_ENGINE && w<RG_ENGINE+0xB00000u){               /* engine-code-range => likely return addr */
            c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"%x ",(uint32_t)(w-RG_ENGINE)); found++;
            if((found&9u)==9u){ LOG("  ra: %s",line); c=0; line[0]=0; } }
    }
    if(c) LOG("  ra: %s",line);
    LOG("  walk(0x7d2d18) last %u calls  Mcolor=[M+0x14] nodecolor=[node+5] (guard: PROCESS iff (Mc^3)&(nc^3)==0):",
        g_wkn<32u?g_wkn:32u);
    { unsigned cn=g_wkn<32u?g_wkn:32u, st=g_wkn<32u?0u:(g_wkn&31u);
      for(unsigned k=0;k<cn;k++){ unsigned i=(st+k)&31u; uint8_t mc=g_wk_mc[i],nc=g_wk_nc[i];
        int proc=(((mc^3)&(nc^3))==0);
        LOG("    wk[-%u] M=0x%x Mcolor=%u node=0x%x nodecolor=%u cnt=%d -> %s",
            cn-k, g_wk_m[i], mc, g_wk_node[i], nc, g_wk_cnt[i], proc?"PROCESS":"skip/recolor"); } }
}
/* DIAG: the stack-filling recursion is 0x7c53fc(node) — record r1(node)+[node]+[node+4]+caller-LR+
 * [node+5]color into a ring of the last 24 so the CYCLE (a repeating node ptr) is visible; also
 * captures [ctx+0x14] generation. At a fixed depth, scans the live stack for the call-chain. */
static void diag_recurse(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; uint32_t n=0,child=0,ty=0,ctx=0,shift=0,arr=0,lr=0; uint8_t color=0,gen=0;
    uc_reg_read(uc,UC_ARM_REG_R1,&n); uc_reg_read(uc,UC_ARM_REG_R0,&ctx); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uc_mem_read(uc,n,&child,4); uc_mem_read(uc,n+4,&ty,4); uc_mem_read(uc,n+5,&color,1);
    uc_mem_read(uc,ctx+7,&shift,1); uc_mem_read(uc,ctx+0x10,&arr,4); uc_mem_read(uc,ctx+0x14,&gen,1);
    if(g_gen==0xffffffffu) g_gen=gen;
    unsigned i=g_rn++ & 23u; g_rnode[i]=n; g_rchild[i]=child; g_rtype[i]=ty;
    g_rshift[i]=shift&0xff; g_rarr[i]=arr; g_rres[i]=0; g_lastpos=i; g_visit++;
    g_rlr[i]=lr>=RG_ENGINE?(uint32_t)(lr-RG_ENGINE):lr; g_rcolor[i]=color;
    if(g_visit==2000u && !g_scanned){ g_scanned=1; diag_stackscan(uc); }   /* deep enough to be steady-state */
}
/* 0x7c5504 = right after `bl 0x7c53f8`(resolver) in 0x7c54d0 — r0 = the RESOLVED node. Pair with
 * the last diag_recurse: if resolved is an ANCESTOR => the back-edge; compare to array+idx*20. */
static void diag_resolve_ret(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; uint32_t r0=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); g_rres[g_lastpos]=r0;
}
/* DIAG: W=0x7d3be0 entry — the deep recursive graph-processor. Track entry count + SP to prove the
 * recursion (SP monotonically dropping) and measure frame size (avgframe = (wsp0-wspmin)/count). */
static void diag_w(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; uint32_t sp=0,a0=0,m=0; uint8_t ty=0;
    uc_reg_read(uc,UC_ARM_REG_SP,&sp); uc_reg_read(uc,UC_ARM_REG_R0,&a0);
    uc_mem_read(uc,a0+0x10,&m,4); if(m) uc_mem_read(uc,m+0x15,&ty,1);
    if(!g_wcount) g_wsp0=sp; if(sp<g_wspmin) g_wspmin=sp; g_wcount++;
    unsigned i=g_wmn++ & 31u; g_w_arg0[i]=a0; g_w_m[i]=m; g_w_ty[i]=ty;
}
/* DIAG: walk=0x7d2d18 entry — record the color-guard inputs (Mcolor vs each slot node's color) so the
 * dump can show whether the visited-mark actually SKIPS re-visits or the guard is bypassed. */
static void diag_walk(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; uint32_t a0=0,slot=0,cnt=0,m=0,node=0; uint8_t mc=0,nc=0;
    uc_reg_read(uc,UC_ARM_REG_R0,&a0); uc_reg_read(uc,UC_ARM_REG_R1,&slot); uc_reg_read(uc,UC_ARM_REG_R2,&cnt);
    uc_mem_read(uc,a0+0x10,&m,4); if(m) uc_mem_read(uc,m+0x14,&mc,1);
    uc_mem_read(uc,slot,&node,4); if(node) uc_mem_read(uc,node+5,&nc,1);
    unsigned i=g_wkn++ & 31u; g_wk_m[i]=m; g_wk_node[i]=node; g_wk_mc[i]=mc; g_wk_nc[i]=nc; g_wk_cnt[i]=(int)cnt;
}

/* DIAG BYPASS: the vector<COW-string> dtor loop @0x616158 decrements [str-4] without null-checking
 * str; a NULL element (shim-produced) => [0xfffffffc] fault @0x616188. Hook 0x616180 (the decrement
 * block entry, r3=str ptr): if str==NULL, redirect PC to 0x616168 (loop continue) to SKIP the
 * decrement+destroy. This is a DIAGNOSTIC: if the scene then renders, the fault was THE blocker and
 * the 41 NULL strings were non-critical; if still draws=0, the NULL data itself is needed. */
static void diag_skipnull(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; uint32_t r3=0; uc_reg_read(uc,UC_ARM_REG_R3,&r3);
    static unsigned long nskip=0;
    if(r3==0u){ uint32_t npc=RG_ENGINE+0x616168u; uc_reg_write(uc,UC_ARM_REG_PC,&npc);
        if(nskip++<8) LOG("[skipnull] skipped NULL-string decrement @0x616180 (#%lu)", nskip); }
}
/* DIAG: lua_load ENTRY (0x7cc104) — r0=L,r1=reader,r2=data(LoadS{const char* s; size_t size}),
 * r3=chunkname. For luaL_loadbuffer the decrypted script buffer is [r2] (size [r2+4]). Dump each
 * decrypted script (name + full hex, 16KB cap) so it can be reconstructed + decompiled offline to
 * read findScriptPath + the script_paths contract. The buffer is Lua 5.1 BYTECODE ('\x1bLuaQ'). */
static void diag_luaload(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0; if(n>=25) return; n++;
    uint32_t r1=0,r3=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_R3,&r3);   /* r1=ZIO* Z, r3=name */
    char nm[128]; unsigned i=0; for(;i<127;i++){ uint8_t ch=0; if(uc_mem_read(uc,r3+i,&ch,1)||!ch) break; nm[i]=(ch>=32&&ch<127)?(char)ch:'.'; } nm[i]=0;
    /* Lua5.1 ZIO { size_t n; const char* p; lua_Reader reader; void* data; lua_State* L; } */
    uint32_t zn=0,zp=0,zreader=0,zdata=0, s=0,ssize=0;
    uc_mem_read(uc,r1,&zn,4); uc_mem_read(uc,r1+4,&zp,4); uc_mem_read(uc,r1+8,&zreader,4); uc_mem_read(uc,r1+12,&zdata,4);
    if(zdata){ uc_mem_read(uc,zdata,&s,4); uc_mem_read(uc,zdata+4,&ssize,4); }   /* LoadS { const char* s; size_t size } */
    uint32_t bufp = zp;                                      /* ZIO.p = current pos in the decrypted buffer (heap) */
    uint32_t bcap = zn+16u;  if(bcap==0||bcap>16384u) bcap=16384u;
    LOG("[luaload #%d] name='%s' ZIO{n=0x%x p=0x%x reader=+0x%x data=0x%x} LoadS{s=0x%x size=%u} buf=0x%x cap=%u DUMP>>>",
        n, nm, zn, zp, zreader>=RG_ENGINE?(uint32_t)(zreader-RG_ENGINE):zreader, zdata, s, ssize, bufp, bcap);
    char line[224]; int c=0; unsigned printed=0;
    for(unsigned k=0;k<bcap;k++){ uint8_t b=0; if(uc_mem_read(uc,bufp+k,&b,1)) break;
        c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"%02x",b); printed++;
        if(printed%100u==0){ LOG("LX%d %s",n,line); c=0; line[0]=0; } }
    if(c) LOG("LX%d %s",n,line);
    LOG("[luaload #%d] <<<END name='%s'", n, nm);
}
/* DIAG: util::JSON::parse ENTRY (0x69814c) — r1=&{begin,end}. Log the first bytes of each JSON input
 * + caller LR, to pin which asset each JSON ParseError/BadType throw comes from (the throw follows
 * the parse of the offending content). Light: fires only on JSON parses. */
static void diag_jsonparse(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; uint32_t r1=0,begin=0,end=0,lr=0;
    uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uc_mem_read(uc,r1,&begin,4); uc_mem_read(uc,r1+4,&end,4);
    uint32_t total=(end>begin)?(end-begin):0, len=total>64u?64u:total;
    char buf[68]; unsigned k=0; for(;k<len;k++){ uint8_t c=0; uc_mem_read(uc,begin+k,&c,1); buf[k]=(c>=32&&c<127)?(char)c:'.'; } buf[k]=0;
    LOG("[jsonparse] caller=+0x%x len=%u '%s'", lr>=RG_ENGINE?(uint32_t)(lr-RG_ENGINE):lr, total, buf);
}
/* GUARD (+diag) at the UTF-8 std::string -> UTF-16 std::u16string converter @0x727ec0. r7 = source
 * COW-string _M_p; the loop copies r6=[r7-0xc]=_Rep._M_length char16_t. Observed on-device: ONE
 * device-info string (in the analytics/crash-report HTTP payload path — 0x460xxx builds the
 * Content-type: application/json request; a phone-home) has its `_M_length` slot CLOBBERED by a
 * stray pointer (e.g. 0x5011a208), while `_M_capacity`/`_M_refcount`/content stay intact. That
 * builds a runaway ~63MB u16string -> heap-guard fault -> nativeInit aborts -> draws=0.
 * A COW std::string ALWAYS has _M_length <= _M_capacity, so len>cap (or an absurd len) is provably
 * corrupt: repair `_M_length` in guest memory to the real NUL-terminated content length BEFORE the
 * ldr at 0x727ec0 reads it, so the conversion proceeds correctly. (Symptom guard — the stray _Rep
 * write is a deeper bug still to root-cause; the payload is network-blocked regardless.) */
static void diag_u16conv(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    static int n=0, dumped=0;
    uint32_t mp=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R7,&mp); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uint32_t len=0,cap=0; uc_mem_read(uc,mp-12u,&len,4); uc_mem_read(uc,mp-8u,&cap,4);
    if(len<=cap && len<=0x100000u){ if(n++<14) LOG("[u16conv] src _M_p=0x%x len=%u cap=%u caller=+0x%x", mp, len, cap, lr>=RG_ENGINE?lr-RG_ENGINE:lr); return; }
    /* ---- corrupt: one-time full diagnostic dump ---- */
    if(!dumped){ dumped=1;
        uint32_t rc=0; uc_mem_read(uc,mp-4u,&rc,4);
        char buf[49]; unsigned k=0; for(;k<48u;k++){ uint8_t c=0; uc_mem_read(uc,mp+k,&c,1); buf[k]=(c>=32&&c<127)?(char)c:'.'; } buf[k]=0;
        uint32_t r[13]; for(int i=0;i<13;i++){ r[i]=0; uc_reg_read(uc,UC_ARM_REG_R0+i,&r[i]); }
        LOG("[u16conv CORRUPT] src _M_p=0x%x len=%u(0x%x) cap=%u refc=%d caller=+0x%x first48='%s'",
            mp,len,len,cap,(int)rc, lr>=RG_ENGINE?lr-RG_ENGINE:lr, buf);
        LOG("  r0=0x%x r1=0x%x r2=0x%x r3=0x%x r4=0x%x r5=0x%x r6=0x%x r8=0x%x r9=0x%x r10=0x%x r11=0x%x",
            r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[8],r[9],r[10],r[11]);
        uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp);
        char l[256]; int cc=0, shown=0;                    /* DEEP scan: capture the full chain up to nativeInit -> find the RCS-subsystem entry to neutralize */
        for(uint32_t o=0;o<0x1800u && shown<48;o+=4){ uint32_t w=0; uc_mem_read(uc,sp+o,&w,4);
            if(w>=RG_ENGINE && w<RG_ENGINE+0x966000u){ cc+=snprintf(l+cc,(size_t)(sizeof l-(size_t)cc),"+0x%x ",w-RG_ENGINE); shown++;
                if(shown%8==0){ LOG("  bt: %s",l); cc=0; l[0]=0; } } }
        if(cc) LOG("  bt: %s",l);
        char h[128]; int hc=0; for(int o=-16;o<16;o+=4){ uint32_t w=0; uc_mem_read(uc,mp+(uint32_t)o,&w,4); hc+=snprintf(h+hc,(size_t)(sizeof h-(size_t)hc),"[%+d]=0x%x ",o,w); }
        LOG("  _Rep bytes: %s (len slot=[-12])",h);
    }
    /* ---- repair: clamp _M_length to the real content length ---- */
    uint32_t real=0; for(; real<0x4000u; real++){ uint8_t ch=0; uc_mem_read(uc,mp+real,&ch,1); if(!ch) break; }
    if(real>=0x4000u) real=(cap<0x100000u)?cap:0u;          /* no NUL in 16KB -> fall back to capacity/0 */
    else if(cap<0x100000u && real>cap) real=cap;            /* never exceed a sane capacity */
    uc_mem_write(uc,mp-12u,&real,4);
    { static int gn=0; if(gn++<8) LOG("[u16conv GUARD] repaired corrupt _M_length 0x%x -> %u (real content len)", len, real); }
}
/* DIAG: VirtualFileSystem::open @0x6bd4f4 — log the URI + caller. The AppConfiguration (configmaster
 * remote-config) phone-home opens a network URI whose scheme isn't registered offline -> throws an
 * UNCAUGHT io::IOException ("Invalid scheme in URI") -> engine FatalError -> SIGABRT. This names the
 * exact URI and the caller to neutralize. */
static void diag_vfsopen(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    static int n=0; if(n++>=80) return;
    uint32_t r1=0,r2=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_R2,&r2); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    char s[160]="?"; uint32_t mp=0; uc_mem_read(uc,r1,&mp,4);
    if(mp>=RG_HEAP && mp<RG_STACK) diag_read_str(uc,mp,s,sizeof s);   /* r1=&std::string -> [r1]=_M_p */
    else diag_read_str(uc,r1,s,sizeof s);                              /* r1=_M_p (char*) */
    LOG("[vfsopen] caller=+0x%x r2=0x%x uri='%s'", lr>=RG_ENGINE?lr-RG_ENGINE:lr, r2, s);
}
/* DIAG (draws=0, NEW blocker): image-format dispatcher @0x67a034 (out r0, source r1, format-enum r2);
 * throws "Unsupported image file format" @0x67a3cc when *r1==0. FONT_BASIC.pvr (PVR v3, magic
 * 0x03525650, uncompressed rgba4444) fails here as the FIRST/only image. Dump the format enum + the
 * source object's first words + the caller to pin whether the magic->format detection (0x6b2f20) or
 * the source data-flow diverged in the shim (native loads this .pvr fine). */
static void diag_imgfmt(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0; if(n++>=6) return;
    uint32_t r0=0,r1=0,r2=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_R1,&r1);
    uc_reg_read(uc,UC_ARM_REG_R2,&r2); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uint32_t w[6]={0}; for(int i=0;i<6;i++) uc_mem_read(uc,r1+(uint32_t)(i*4),&w[i],4);
    LOG("[imgfmt] caller=+0x%x fmt(r2)=%u src(r1)=0x%x [r1..]=%08x %08x %08x %08x %08x %08x",
        lr>=RG_ENGINE?lr-RG_ENGINE:lr, r2, r1, w[0],w[1],w[2],w[3],w[4],w[5]);
    /* [r1] is an io::*InputStream reader: {vtable, dataptr@+4, size@+8, pos@+0xc}. Dump the reader
     * object AND deref its dataptr to show the ACTUAL file magic (should be 50 56 52 03 = "PVR\3"). */
    if(w[0]>=RG_HEAP && w[0]<RG_STACK){ uint32_t rd[4]={0}; for(int i=0;i<4;i++) uc_mem_read(uc,w[0]+(uint32_t)(i*4),&rd[i],4);
        LOG("[imgfmt] reader@0x%x vtbl=+0x%x data=0x%x size=%u pos=0x%x", w[0],
            rd[0]>=RG_ENGINE?rd[0]-RG_ENGINE:rd[0], rd[1], rd[2], rd[3]);
        uint32_t dp=rd[1]; if(dp>=RG_HEAP && dp<RG_STACK){ uint8_t m[16]={0}; uc_mem_read(uc,dp,m,16);
            LOG("[imgfmt] data@0x%x magic= %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
                dp, m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]); } }
}
/* DIAG (draws=0): the image-format MAPPER 0x6b2f20 (r0=source stream) decides the format enum by
 * reading the magic from the source via its VIRTUAL methods (vtable[0x14]/[0x18], called in 0x6c9498).
 * It returned 0 (unknown) for FONT_BASIC.pvr. Dump the REAL source r0 (this is arg1 to the setup fn,
 * a DIFFERENT object than the dispatcher's source diag_imgfmt dumped): its object words + its vtable's
 * read-method addrs (identify the io::*InputStream class) + whether it holds the 1.1MB of file data. */
static void diag_imgsrc(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0; if(n++>=5) return;
    uint32_t r0=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uint32_t o[8]={0}; for(int i=0;i<8;i++) uc_mem_read(uc,r0+(uint32_t)(i*4),&o[i],4);
    LOG("[imgsrc] caller=+0x%x src=0x%x obj= %08x %08x %08x %08x %08x %08x %08x %08x",
        lr>=RG_ENGINE?lr-RG_ENGINE:lr, r0, o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7]);
    /* wrapper is lang::Ptr: delegate = [src+0xc]==src ? [src+8] : [src+0xc]. Follow to the real stream. */
    uint32_t dl = (o[3]==r0)? o[2] : o[3];
    if(dl>=RG_HEAP && dl<RG_STACK){ uint32_t d[8]={0}; for(int i=0;i<8;i++) uc_mem_read(uc,dl+(uint32_t)(i*4),&d[i],4);
        LOG("[imgsrc] delegate=0x%x vt=+0x%x obj= %08x %08x %08x %08x %08x %08x %08x %08x",
            dl, (d[0]>=RG_ENGINE&&d[0]<RG_ENGINE+0xb00000u)?d[0]-RG_ENGINE:d[0], d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
        /* scan the delegate's fields for a heap buffer that starts with the PVR magic 50 56 52 03 */
        for(int i=1;i<8;i++){ uint32_t p=d[i]; if(p>=RG_HEAP && p<RG_STACK){ uint8_t m[12]={0}; uc_mem_read(uc,p,m,12);
            LOG("[imgsrc]  d[%d]->0x%x: %02x %02x %02x %02x %02x %02x %02x %02x", i,p,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7]); } }
    }
}
/* DIAG (draws=0): inside 0x6c9498 (the mapper's magic reader) @0x6c94ec — right after source->read14
 * (r9=data ptr/pos) and source->avail18 (r0=available bytes) return, compared to needed (r5). Log the
 * avail count, needed count, and the actual magic bytes at r9. Decisive: avail=0 => empty stream;
 * avail>=needed + r9->"PVR\3" => data present (comparison bug); r9->garbage => wrong data delivered. */
static void diag_imgread(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0; if(n++>=4) return;
    uint32_t avail=0,need=0,dp=0; uc_reg_read(uc,UC_ARM_REG_R0,&avail); uc_reg_read(uc,UC_ARM_REG_R5,&need); uc_reg_read(uc,UC_ARM_REG_R9,&dp);
    uint8_t m[12]={0}; if(dp>=RG_HEAP && dp<RG_STACK) uc_mem_read(uc,dp,m,12);
    LOG("[imgread] avail(r0)=%u needed(r5)=%u dataptr(r9)=0x%x magic= %02x %02x %02x %02x %02x %02x %02x %02x",
        avail, need, dp, m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7]);
}
/* DIAG (draws=0): the virtual/PMF call @0xdd258 returns a std::string into sp+4; that string's _Rep is
 * corrupt (_M_length is a heap ptr). Dump this(r1)/method(r2)/object-words to identify the method that
 * returns the corrupt string. Then @0xdd25c the returned _M_p is at [sp+4] — flag when [_M_p-0xc]>=heap. */
static void diag_vcall(uc_engine*uc, uint64_t a, uint32_t s, void*u){
    (void)a;(void)s;(void)u; static int n=0; if(n++>=12) return;
    uint32_t r1=0,r2=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_R2,&r2);
    uint32_t o[4]={0}; for(int i=0;i<4;i++) uc_mem_read(uc,r1+(uint32_t)(i*4),&o[i],4);
    LOG("[vcall] this=0x%x method=+0x%x obj= %08x %08x %08x %08x", r1, r2>=RG_ENGINE?r2-RG_ENGINE:r2, o[0],o[1],o[2],o[3]);
}
/* DIAG (draws=0, scene/resolver corruption): the append 0x7ce73c(this=r0, src=r1, len=r2) forwards
 * a CORRUPT len (0x505bb598, a heap ptr) to the memcpy 0x7de880. Its lr = the caller that computed the
 * bad len. Dump this/src/len/caller + src's COW _Rep header (*(src-0xc)=_M_length, -8=cap, -4=ref):
 * if _M_length==len it's a corrupt COW std::string (same class as the u16/RCS corruptions). */
static void diag_append(uc_engine*uc, uint64_t a, uint32_t s, void*u){
    (void)a;(void)s;(void)u; static int n=0; if(n>=6) return;
    uint32_t r0=0,r1=0,r2=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_R1,&r1);
    uc_reg_read(uc,UC_ARM_REG_R2,&r2); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    if(r2<0x1000000u) return; n++;
    uint32_t rep[3]={0}; for(int i=0;i<3;i++) uc_mem_read(uc,r1-0xcu+(uint32_t)(i*4),&rep[i],4);
    LOG("[append!] this=0x%x src=0x%x len=0x%x caller=+0x%x  src[-0xc]len=0x%x [-8]cap=0x%x [-4]ref=0x%x",
        r0,r1,r2,lr>=RG_ENGINE?lr-RG_ENGINE:lr, rep[0],rep[1],rep[2]);
    uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); char line[240]; int c=0; line[0]=0; unsigned f=0;
    for(uint32_t o=0;o<0x600u&&f<18u;o+=4){ uint32_t w=0; if(uc_mem_read(uc,sp+o,&w,4))break;
        if(w>=RG_ENGINE&&w<RG_ENGINE+0xb00000u){ c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"+0x%x ",w-RG_ENGINE); f++; } }
    LOG("[append!] bt: %s", line);
}
/* GUARD (draws=0 safety net): the word-copy 0x7de880 (memcpy/memmove: r0=dst,r1=src,r2=len) is being
 * called with a POINTER-VALUED length (0x505bb598 = a corrupt COW std::string _Rep._M_length from the
 * GameLua Build.VERSION device-info path — same recurring _Rep-corruption class as u16/RCS). It reads
 * src+len-1 -> wild -> UC_ERR overrun -> nativeInit abandoned -> draws=0. Any real copy length is
 * < 256MB (< RG_HEAP=0x50000000); a len in [RG_HEAP, 0x80000000) is unambiguously a corrupt heap
 * pointer, so CLAMP it to 0 (skip the copy — the device-info string becomes empty, non-fatal) to keep
 * nativeInit alive.
 *
 * STATUS 2026-07-28: this was written as a TEMPORARY band-aid, with the note that "the real fix is
 * the upstream _Rep corruption". THE REAL FIX LANDED — the galloc quarantine now withholds freed
 * blocks, so the stale write that corrupted the COW _Rep lands harmlessly and the corrupt length is
 * never produced. Measured rather than assumed: `[guard-memcpy]` has fired **0 times across all 22
 * captured runs**, while the guards that are genuinely load-bearing fire constantly over the same
 * logs (s-construct-null-guard 69, empty-json-guard 107).
 *
 * So it is NOT a band-aid any more, and calling it one would send a reader hunting an upstream bug
 * that no longer exists. It is kept as a bounded safety net: the clamp is unreachable unless that
 * corruption class returns, it cannot fire on a legitimate length (any real copy is < 256 MB, below
 * RG_HEAP), and the A56's different timing and memory layout are exactly the conditions under which
 * a heap-corruption class could resurface. Deleting it would change a bit-reproducible, play-tested
 * deliverable to remove code that costs one compare on a path that never triggers. */
static void diag_memcpy(uc_engine*uc, uint64_t a, uint32_t s, void*u){
    (void)a;(void)s;(void)u;
    uint32_t len=0; uc_reg_read(uc,UC_ARM_REG_R2,&len);
    if(len<RG_HEAP || len>=0x80000000u) return;         /* legit length -> let the copy proceed */
    uint32_t z=0; uc_reg_write(uc,UC_ARM_REG_R2,&z);     /* corrupt pointer-length -> clamp to 0 (skip) */
    static int n=0; if(n++<8){ uint32_t src=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R1,&src); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
        LOG("[guard-memcpy] CLAMPED corrupt len=0x%x -> 0 (src=0x%x caller=+0x%x) [corrupt COW _Rep._M_length; avoids UC_ERR overrun]",
            len, src, lr>=RG_ENGINE?lr-RG_ENGINE:lr); }
}
/* DIAG (draws=0): localize the +1-per-byte key corruption ("PVR"->"QWS"). @0x6b2fc8 r4=&the built
 * "."+key std::string (should be ".PVR"); @0x6b2cf8 r1=&the substr result (should be "PVR"). */
static void diag_dumpstr(uc_engine*uc, int reg, const char*tag){
    uint32_t sp=0,so=0; uc_reg_read(uc,reg,&so); uc_reg_read(uc,UC_ARM_REG_SP,&sp); (void)sp;
    uint32_t mp=0; uc_mem_read(uc,so,&mp,4); uint32_t len=0; if(mp) uc_mem_read(uc,mp-0xcu,&len,4);
    uint8_t b[10]={0}; if(mp>=RG_HEAP&&mp<RG_STACK) uc_mem_read(uc,mp,b,9);
    LOG("[%s] obj=0x%x _Mp=0x%x len=%u bytes= %02x %02x %02x %02x %02x '%.6s'",tag,so,mp,len,b[0],b[1],b[2],b[3],b[4],b);
}
static void diag_imginput(uc_engine*uc, uint64_t a, uint32_t s, void*u){ (void)a;(void)s;(void)u; static int n=0; if(n++<3) diag_dumpstr(uc,UC_ARM_REG_R4,"imginput.dotkey"); }
static void diag_imgsubstr(uc_engine*uc, uint64_t a, uint32_t s, void*u){ (void)a;(void)s;(void)u; static int n=0; if(n++<3) diag_dumpstr(uc,UC_ARM_REG_R1,"imgsubstr.ext"); }
/* DIAG (draws=0): the format-detect LINEAR SEARCH compare @0x6b2d58 (right after blx 0x88cb9c at
 * 0x6b2d54): r5=&key std::string ("PVR" extension), r4=candidate format-ID char* ("BMP"/"PVR"/...),
 * r0=compare result (0==equal==match). Dump the two operands + result to see WHY "PVR"=="PVR" isn't 0. */
static void diag_imgcmp(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0; if(n++>=24) return;
    uint32_t r5=0,r4=0,r0=0; uc_reg_read(uc,UC_ARM_REG_R5,&r5); uc_reg_read(uc,UC_ARM_REG_R4,&r4); uc_reg_read(uc,UC_ARM_REG_R0,&r0);
    uint32_t kp=0; uc_mem_read(uc,r5,&kp,4); uint32_t klen=0; if(kp) uc_mem_read(uc,kp-0xcu,&klen,4);
    uint8_t k[10]={0},c[10]={0}; if(kp>=RG_HEAP&&kp<RG_STACK) uc_mem_read(uc,kp,k,9); if(r4) uc_mem_read(uc,r4,c,9);
    LOG("[imgcmp] key(_Mp=0x%x len=%u)= %02x %02x %02x %02x '%.5s'  cand(0x%x)= %02x %02x %02x %02x '%.5s'  cmp=%d",
        kp,klen,k[0],k[1],k[2],k[3],k, r4,c[0],c[1],c[2],c[3],c,(int)r0);
}
/* DIAG (draws=0): does the image-format registry INIT (0x6b1f68, the lazy-static populator that adds
 * BMP/PNG/PVR/... format-ID -> scheme) actually RUN? If this never fires, the __cxa_guard@[base+544]
 * read as already-initialized (a .bss zero-init gap) -> the std::map stays EMPTY -> "PVR" lookup
 * misses -> fmt=0. If it DOES fire, the map is populated and the bug is the std::map/string compare. */
static void diag_reginit(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)uc;(void)addr;(void)sz;(void)u; static int n=0; if(n++<2) LOG("[reginit] image-format registry populator 0x6b1f68 RAN (map being populated)");
}
/* DIAG (draws=0): the image-format mapper's magic KEY @0x6b2f78 — [sp+4] is the std::string built by
 * 0x6c9498 from the read magic. Dump its length ([_M_p-0xc]) + bytes: is it "PVR\3"(len 4) or 24 bytes
 * with embedded NULs? This settles whether the lookup misses due to length/embedded-NUL vs a registry bug. */
static void diag_imgkey(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0; if(n++>=4) return;
    uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp);
    uint32_t sptr=0; uc_mem_read(uc,sp+4u,&sptr,4);
    uint32_t len=0; if(sptr) uc_mem_read(uc,sptr-0xcu,&len,4);
    uint8_t b[16]={0}; if(sptr>=RG_HEAP && sptr<RG_STACK) uc_mem_read(uc,sptr,b,16);
    LOG("[imgkey] key _M_p=0x%x len=%u bytes= %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        sptr,len,b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11]);
}
/* DIAG (draws=0): the memory-stream READ @0x6c5600 (this r0, dest r1, len r2): memcpy(dest, base+cur, n)
 * where base=[this+0x10], cur=[this+0x1c], end=[this+0x14]. For small reads (the 24-byte image magic
 * probe), dump base/cur and the actual source bytes — is "PVR\3"(50 56 52 03) at base+cur, or wrong? */
static void diag_streamread(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u; static int n=0;
    uint32_t th=0,dest=0,len=0; uc_reg_read(uc,UC_ARM_REG_R0,&th); uc_reg_read(uc,UC_ARM_REG_R1,&dest); uc_reg_read(uc,UC_ARM_REG_R2,&len);
    uint32_t base=0,end=0,cur=0; uc_mem_read(uc,th+0x10u,&base,4); uc_mem_read(uc,th+0x14u,&end,4); uc_mem_read(uc,th+0x1cu,&cur,4);
    if(end<100000u) return;                             /* only large-asset reads (the 1.1MB .pvr; skip small 7z chunks) */
    if(n++>=8) return;
    uint32_t src=base+cur; uint8_t m[12]={0}; if(src>=RG_HEAP && src<RG_STACK) uc_mem_read(uc,src,m,12);
    LOG("[strrd] this=0x%x len=%u base=0x%x end=%u cur=%u src=0x%x bytes= %02x %02x %02x %02x %02x %02x %02x %02x",
        th,len,base,end,cur,src,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7]);
}
/* DE-PHONE-HOME (general): the VFS scheme-open @0x6bd4f4 dispatches by URI scheme; for an
 * unregistered scheme (http/https — the offline phone-homes: RCS/configmaster/League/Facebook/
 * smoke.rovio.com) it builds+throws an io::IOException "Invalid scheme" @0x6be064. No caller
 * catches it, so it unwinds+ABANDONS nativeInit -> the scene never activates -> draws=0. Instead,
 * make that open return a NULL stream GRACEFULLY so the caller's FailureCallback fires and the
 * engine falls back to the bundled default (native does the same when the async network read
 * fails). The fn's frame is `push{r4-r8,sb,sl,fp,lr}; sub sp,#0x254`; its sole epilogue is
 * `add sp,#0x254; pop{...pc}` @0x6bd6d0/6bd6d4 returning r0. So: hooked BEFORE the throw, balance
 * sp (+0x254 to point at the pushed regs), r0=0 (null), jump to the pop @0x6bd6d4 -> clean return.
 * (Leaks the just-new'd 24B exception + msg strings — negligible, few invalid-scheme opens.) */
static void neut_vfs_invalid_scheme(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); sp+=0x254u; uc_reg_write(uc,UC_ARM_REG_SP,&sp);
    uint32_t z=0; uc_reg_write(uc,UC_ARM_REG_R0,&z); uc_reg_write(uc,UC_ARM_REG_R3,&z);
    uint32_t ret=RG_ENGINE+0x6bd6d4u; uc_reg_write(uc,UC_ARM_REG_PC,&ret);
    static int n=0; if(n++<8) LOG("[de-phonehome] VFS 'invalid scheme' (https phone-home) -> graceful NULL stream (no throw)");
}
/* DE-PHONE-HOME + corruption-avoidance: neutralize the Rovio Cloud Services (RCS) Identity
 * login/session. The App's init calls this orchestrator @0x310b6c (backtrace: NativeApplication
 * 0x1e3aa0 -> here -> virtual login 0x310bc0 -> HTTP request build 0x460xxx -> device-info UTF-16
 * conversion). That path (a) phones home to Rovio's account servers (/session/1/apps/, /sessions,
 * RefreshToken) — which the user requires removed, and (b) is where a shim emulation-fidelity bug
 * corrupts a COW std::string _Rep (_M_length<-ptr) and a std::shared_ptr control-block (misaligned)
 * -> runaway u16 conversion + LDREX alignment fault -> nativeInit aborts -> draws=0. The engine's
 * OWN [this+0x34]!=0 branch already early-returns from this fn (login-gate), so returning at entry
 * is a behavior the code path supports. Skip it: the game plays fully offline (proven cont.58), so
 * no cloud session is needed; a return here removes the phone-home AND every downstream corruption. */
static void neut_rcs_login(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    /* Hooked at the VIRTUAL login call `blx r3` @0x310bc0. Skip ONLY the login (the phone-home +
     * corruption source: it builds the device-info HTTP request) by advancing PC past the blx to
     * 0x310bc4, so the orchestrator's post-work (operator new(16) + queue callback) still runs and
     * whatever object it creates is not left NULL (avoids a later null-deref/abort). r0 after the
     * skipped call is unused by the post-work. */
    uint32_t next=RG_ENGINE+0x310bc4u; uc_reg_write(uc,UC_ARM_REG_PC,&next);
    static int n=0; if(n++<4) LOG("[de-phonehome] skipped RCS Identity login call @0x310bc0 (kept post-work)");
}
/* DE-PHONE-HOME (general + surgical): VirtualFileSystem::addLink is @0x6bfe14 (push{r4-r11,lr};
 * sub sp,#492(0x1ec); normal epilogue `add sp,#492; pop{r4-r11,pc}` @0x6c002c). It validates a link's
 * scheme is "vfs" and, for a NON-vfs scheme, builds+throws an UNCAUGHT io::IOException ("addLink:
 * Link's scheme is not vfs" @0xa30628; throw `blx __cxa_throw` @0x6c0af4). The AppConfiguration
 * remote-config init registers SEVERAL https VFS links (configmaster@0xa23ab8, league/tournament/
 * facebook/smoke.rovio.com) this way — each throw unwinds and ABANDONS nativeInit before
 * scene-activation -> the render loop runs with no scene -> draws=0 (proven THROW #11 in the x86
 * ab-emu; msg "VirtualFileSystem::addLink: Link's scheme is not vfs"). Skipping individual config
 * call-sites is whack-a-mole (multiple links via different paths — confirmed: skipping @0x3480f4 just
 * moved the throw to another link). This is the CONVERGENT fix at the throw itself: don't throw —
 * jump to addLink's own clean epilogue @0x6c002c. The throw block reads locals via sp+off inside the
 * 492B frame, so SP is at frame-base there; no SP balancing is needed and jumping past the (valid)
 * stack canary check is safe. The non-vfs https link is simply NOT registered — exactly the desired
 * offline behavior (engine keeps the bundled default remote_configuration_default.zip; the async
 * config request then hits its FailureCallback -> default). Surgical: legit vfs:// asset addLinks
 * return via 0x6c0030 and never reach 0x6c0af4, so they are untouched. r0<-r9(=this, set @0x6bfe20
 * and restored by the pop) so a `return *this`/chaining caller sees a valid non-null result.
 * (Leaks the just-allocated exception object + temp msg strings — negligible, a handful of links.) */
static void neut_addlink_scheme(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    uint32_t self=0; uc_reg_read(uc,UC_ARM_REG_R9,&self); uc_reg_write(uc,UC_ARM_REG_R0,&self);
    uint32_t ret=RG_ENGINE+0x6c002cu; uc_reg_write(uc,UC_ARM_REG_PC,&ret);
    static int n=0; if(n++<8) LOG("[de-phonehome] VFS::addLink non-vfs scheme (https config link) -> skip throw, clean return (link not added)");
}
/* DIAG: util::JSON::BadType throw sites (0x6a8aec, 0x6aa8a4) — log the guest-stack return-address
 * chain so the CONSUMER that accessed script_paths.json with the wrong type is identified (correlate
 * the Nth BadType with the diag_jsonparse log: the script_paths.json one follows its 242KB parse). */
static int g_bt_n=0;
static void diag_badtype(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)sz;(void)u; if(g_bt_n++>=8) return;
    uint32_t sp=0,lr=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    char line[256]; int c=0; line[0]=0; unsigned found=0;
    for(unsigned off=0; off<0x600u && found<20u; off+=4){ uint32_t w=0; if(uc_mem_read(uc,sp+off,&w,4)) break;
        if(w>=RG_ENGINE && w<RG_ENGINE+0xB00000u){ c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"%x ",(uint32_t)(w-RG_ENGINE)); found++; } }
    LOG("[BadType@0x%x #%d] LR=+0x%x RAs(consumer chain): %s", (uint32_t)(addr-RG_ENGINE), g_bt_n,
        lr>=RG_ENGINE?(uint32_t)(lr-RG_ENGINE):lr, line);
}
/* DIAG: catch the ctor's actual FAULT (the RING tail 0x7d3d20->nativeResume implies a mem fault at
 * `ldr r3,[M+8]`, NOT a stack overflow — ctor runs on worker-1, only ~6KB deep). Logs the faulting
 * address+PC to logcat (the sched.c [FAULT] is SDBG->stderr, invisible here), scans the fault stack,
 * and dumps the W/walk recursion state. Returns false so the access still faults (behaviour unchanged). */
static int g_mf_dumped=0, g_vecdumped=0, g_appdumped=0;
static bool diag_memfault(uc_engine*uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void*u){
    (void)value;(void)u; uint32_t pc=0,r4=0; uc_reg_read(uc,UC_ARM_REG_PC,&pc); uc_reg_read(uc,UC_ARM_REG_R4,&r4);
    uint32_t poff = pc>=RG_ENGINE?(uint32_t)(pc-RG_ENGINE):pc;
    /* BYPASS (cheap: fires only on the fault): the vector<COW-string> dtor @0x616188 faults on a NULL
     * string's [str-4] refcount. Redirect PC to the loop-continue (0x616168) to SKIP that element's
     * decrement+destroy, then recover. Proven to advance the engine past the fault. */
    if((uint32_t)addr>=0xfffff000u){   /* NULL-relative access ([NULL-4] etc.) — map a scratch page once,
                                        * so the vector<COW-string> dtor's NULL-string refcount ops succeed
                                        * harmlessly (dummy refcount) instead of faulting @0x616188. */
        static int mapped=0; if(!mapped){ mapped=1; uc_mem_map(uc,0xfffff000u,0x1000u,UC_PROT_ALL);
            LOG("[bypass616188] mapped scratch page 0xfffff000 (NULL-string dtor tolerance); fault pc=+0x%x",poff); }
        return true;   /* retry the access — now mapped */
    }
    /* GENERAL UAF-SURVIVAL: the engine's residual std::string use-after-free occasionally yields a wild
     * pointer that reads/writes an unmapped address (e.g. a corrupt _Rb_tree child during scene/registry
     * recursion). Rather than crash the app, map a zero scratch page there so the read returns 0 / the write
     * lands harmlessly and the guest CONTINUES (the recursion sees a null child and unwinds that branch) —
     * the data path analogue of the self-healing allocator. Only DATA faults (read/write); a FETCH fault is a
     * corrupt PC we can't meaningfully paper over, so let it fault. Bounded to 32MB of scratch. */
    if(type==UC_MEM_READ_UNMAPPED || type==UC_MEM_WRITE_UNMAPPED){
        static int scratch_pages=0;
        if(scratch_pages < 8192){
            uint32_t pg=(uint32_t)addr & ~0xFFFu;
            if(uc_mem_map(uc, pg, 0x1000u, UC_PROT_READ|UC_PROT_WRITE)==UC_ERR_OK){ scratch_pages++;
                static int nn=0; if(nn++<12) LOG("[uaf-survive] wild %s @0x%llx (pc=+0x%x) -> mapped zero page 0x%x, continuing (residual std::string UAF)",
                    type==UC_MEM_WRITE_UNMAPPED?"write":"read", (unsigned long long)addr, poff, pg);
                return true;   /* retry — now mapped, reads 0 */
            }
        }
    }
    static int n=0; if(n++<20){
        LOG("MEMFAULT type=%d addr=0x%llx size=%d pc=engine+0x%x r4(M)=0x%x", (int)type,
            (unsigned long long)addr, size, poff, r4);
        if((uint32_t)addr>=RG_HEAP && (uint32_t)addr<RG_STACK)   /* heap/guard fault: report arena pressure */
            LOG("  [heap] inuse=%u of %u bytes (fault in heap window -> exhaustion if ~full)",
                galloc_inuse_bytes(G.cpu.heap), (unsigned)RG_HEAP_SZ);
    }
    if((pc>=RG_ENGINE?(uint32_t)(pc-RG_ENGINE):pc)==0x616188u && !g_vecdumped){ g_vecdumped=1;
        uint32_t r5=0,r9=0; uc_reg_read(uc,UC_ARM_REG_R5,&r5); uc_reg_read(uc,UC_ARM_REG_R9,&r9);
        LOG("VECDUMP @0x616188 (vector<COW-string> dtor): r4(iter)=0x%x r5(emptyrep-cmp)=0x%x r9(end)=0x%x  NULL was at r4-4", r4, r5, r9);
        char line[256]; int c=0; unsigned pr=0;
        for(int off=-0x40; off<0x14; off+=4){ uint32_t w=0xdeadbeef; uc_mem_read(uc,(uint32_t)((int)r4+off),&w,4);
            c+=snprintf(line+c,(size_t)(sizeof line-(size_t)c),"[%+d]=0x%x ", off, w); pr++;
            if(pr%5u==0u){ LOG("  %s",line); c=0; line[0]=0; } }
        if(c) LOG("  %s",line); }
    if(poff==0x727e40u && !g_appdumped){ g_appdumped=1;   /* the UNBOUNDED basic_string::append copy: capture the corrupt source range/length */
        uint32_t rr[13]; for(int i=0;i<13;i++){ rr[i]=0; uc_reg_read(uc,UC_ARM_REG_R0+i,&rr[i]); }
        uint32_t sp=0,lr=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
        LOG("APPENDLOOP @0x727e40 (runaway string copy -> dest=0x%llx): r0=0x%x r1=0x%x r2=0x%x r3=0x%x r4=0x%x r5=0x%x r6=0x%x r7=0x%x",
            (unsigned long long)addr, rr[0],rr[1],rr[2],rr[3],rr[4],rr[5],rr[6],rr[7]);
        LOG("  r8=0x%x r9=0x%x r10=0x%x r11=0x%x r12=0x%x sp=0x%x lr=+0x%x", rr[8],rr[9],rr[10],rr[11],rr[12],sp,
            lr>=RG_ENGINE?lr-RG_ENGINE:lr);
        char l2[256]; int cc=0; unsigned pr2=0;                         /* stack locals: src ptr + length often live here */
        for(int o=0;o<0x40;o+=4){ uint32_t w=0; uc_mem_read(uc,sp+(uint32_t)o,&w,4);
            cc+=snprintf(l2+cc,(size_t)(sizeof l2-(size_t)cc),"[sp+%d]=0x%x ",o,w); pr2++;
            if(pr2%4u==0u){ LOG("  %s",l2); cc=0; l2[0]=0; } }
        if(cc) LOG("  %s",l2); }
    if(!g_mf_dumped){ g_mf_dumped=1; if(!g_scanned){ g_scanned=1; diag_stackscan(uc); }
        LOG("MEMFAULT W(0x7d3be0) recursion nodes (M + [M+0x15]type, oldest..newest of last %u):", g_wmn<32u?g_wmn:32u);
        unsigned cn=g_wmn<32u?g_wmn:32u, st=g_wmn<32u?0u:(g_wmn&31u);
        for(unsigned k=0;k<cn;k++){ unsigned i=(st+k)&31u;
            LOG("    W[-%u] arg0=0x%x M=0x%x type[M+0x15]=%u", cn-k, g_w_arg0[i], g_w_m[i], g_w_ty[i]); } }
    return false;   /* let it fault (we only observe) */
}
/* stderr -> logcat pump (DIAG): Android sends an app's stderr to /dev/null, which swallows the
 * scheduler's per-quantum PC heartbeat (sched.c run_loop: fprintf(stderr,"[SCHED-HB] ... pc=...")).
 * Redirect fd 2 into a pipe and re-emit each line under tag abshim so an on-device spin/hang is
 * locatable (the heartbeat only fires when ONE run_loop call exceeds 20000 quanta = a real spin). */
/* GameLua device-info getter neut — fixes the char32_t-stream use-after-free at its SOURCE + de-phone-home.
 * The getters 0x1112fc/0x111368/0x1113d4 build a std::basic_string<char32_t> device-info value via a
 * char32_t ostringstream (0x615d30) and destroy the temp (0x615ec8); a mis-emulated COW temp->result
 * handoff leaves the RETURNED _Rep freed-but-still-referenced -> that use-after-free is the root of the
 * cyclic-scene-tree grind (0x7d2d18) + the fusion.registry BadType loop + the LocalNotification garbage.
 * These getters feed analytics/tracking (device model/version/UUID) = phone-home the user wants removed.
 * So RETURN A VALID EMPTY char32_t string (a permanent never-freed _Rep) and skip the buggy build:
 * kills the corruption at the source AND removes the device-info phone-home. */
static uint32_t g_empty_u32_mp = 0;   /* _M_p of a permanent empty std::basic_string<char32_t> */
static uint32_t g_json_empty_mp = 0;  /* a permanent guest "{}" (2 bytes) for the empty-JSON guard */
/* GUARD (de-phone-home completeness): util::JSON::parse @0x69814c takes r1=&{begin,end}. After the
 * ad-config https fetch is de-phone-homed (neut_vfs_invalid_scheme/neut_addlink_scheme return empty
 * streams), the engine re-parses that config at LEVEL-END with an EMPTY input (begin==end) ->
 * JSON::ParseError -> uncaught lua_longjmp -> Lua panic -> abort  => the game EXITS on level-complete
 * (empirically: play+win the tutorial -> results transition -> crash to launcher). Redirect an EMPTY
 * parse input to the permanent "{}" so it parses as an empty object (no ParseError; a subsequent
 * missing-key BadType is caught by the engine like the registry BadType). Non-empty parses untouched. */
static void guard_empty_json(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    if(!g_json_empty_mp) return;
    uint32_t r1=0,begin=0,end=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); if(!r1) return;
    uc_mem_read(uc,r1,&begin,4); uc_mem_read(uc,r1+4,&end,4);
    if(begin==end){                                          /* empty input -> would throw ParseError */
        uint32_t b=g_json_empty_mp, e=g_json_empty_mp+2u;
        uc_mem_write(uc,r1,&b,4); uc_mem_write(uc,r1+4,&e,4); /* {begin,end} := "{}" */
        static int n=0; if(n++<8) LOG("[empty-json-guard] empty JSON parse -> '{}' (prevents the level-end ParseError->Lua-panic exit)");
    }
}
static uint32_t g_empty_str_mp = 0;  /* _M_p of a permanent empty std::string<char> _Rep for the _S_construct(NULL) guard */
/* GUARD: std::string::_S_construct(beg=r0, end=r1) @0x88dee4 throws logic_error "_S_construct null not valid"
 * when beg==NULL (&& end!=beg). The de-phone-homed ad-config ({} default, guard_empty_json) makes the ad code
 * read a null/missing value and construct a std::string from it -> this throw -> uncaught -> lua_longjmp ->
 * panic == the level-end crash (cont.105). Intercept at the ENTRY (before the prologue push, so no frame to
 * unwind): if beg==NULL, return a shared empty string (r0=empty _M_p, PC=LR) instead of throwing. GENERIC +
 * safe (constructing a string from NULL is always a bug; empty is the graceful result). KEEPER. */
static void neut_s_construct_null(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    if(!g_empty_str_mp) return;
    uint32_t r0=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0);
    if(r0==0){
        uint32_t lr=0; uc_reg_read(uc,UC_ARM_REG_LR,&lr);
        uc_reg_write(uc,UC_ARM_REG_R0,&g_empty_str_mp);   /* return value = empty string _M_p */
        uc_reg_write(uc,UC_ARM_REG_PC,&lr);               /* return immediately (LR carries the Thumb bit) */
        static int n=0; if(n++<8) LOG("[s-construct-null-guard] std::string(NULL) -> empty (ad-config null value; prevents the level-end logic_error->panic)");
    }
}
static void neut_gamelua_getter(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    if(!g_empty_u32_mp) return;                       /* not set up yet -> let the original run */
    uint32_t r0=0, lr=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    if(r0) uc_mem_write(uc, r0, &g_empty_u32_mp, 4);  /* *return_slot = the empty char32_t string */
    uc_reg_write(uc, UC_ARM_REG_PC, &lr);             /* return immediately (skip the buggy build) */
    static int n=0; if(n++<8) LOG("[gamelua-getter-neut] getter -> empty char32_t string (de-phone-home device-info; avoids the char32_t-stream UAF)");
}
/* GRIND BREAKER: 0x7d2d18 is a list/tree walk `while((r4=[r6])!=0 && r5-->0){ ...; r6=r4 }`. r5=r2 = a node
 * count; the engine calls it recursively (from 0x7d2e78) with r2 = mvn#2 = 0xfffffffd, a "process ALL nodes
 * until [r6]==0" SENTINEL — legitimate on an acyclic chain (terminates when the list ends). The corruption
 * (a freed _Rep reused as a node) makes the chain CYCLIC, so [r6] never reaches 0 and the sentinel loops
 * ~4.29e9 times = the nondeterministic nativeInit grind. We can't tell an acyclic sentinel-walk from a cyclic
 * one at entry, so BOUND it: clamp a huge/sentinel count to 1,048,576. An acyclic scene-graph level is far
 * smaller than that (so valid walks are UNAFFECTED — they still hit [r6]==0 first); a cyclic walk now returns
 * after ~1M iters (~0.4s) instead of never. Band-aid for the UAF, but preserves normal traversal semantics. */
#define TREE_WALK_BOUND 0x100000u
static void guard_tree_walk(uc_engine*uc, uint64_t addr, uint32_t sz, void*u){
    (void)addr;(void)sz;(void)u;
    uint32_t r2=0; uc_reg_read(uc,UC_ARM_REG_R2,&r2);
    if(r2 > TREE_WALK_BOUND){
        uint32_t b=TREE_WALK_BOUND; uc_reg_write(uc,UC_ARM_REG_R2,&b);
        static int n=0; if(n++<12){ uint32_t r0=0,r1=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
            LOG("[tree-walk-guard] 0x7d2d18 count r2=0x%x -> bound %u (sentinel/huge; caps a cyclic-chain grind, acyclic walks unaffected; r0=0x%x r1=0x%x caller=+0x%x)", r2, TREE_WALK_BOUND, r0, r1, lr>=RG_ENGINE?lr-RG_ENGINE:lr); }
    }
}
static void *stderr_pump(void *p){ int fd=(int)(long)p; char buf[512],line[640]; int c=0;
    for(;;){ int n=(int)read(fd,buf,sizeof buf); if(n<0) continue; if(n==0) break;
        for(int i=0;i<n;i++){ char ch=buf[i];
            if(ch=='\n'||c>=(int)sizeof line-1){ line[c]=0; if(c) __android_log_print(4,"abshim","ERR %s",line); c=0; }
            else line[c++]=ch; } } return NULL; }
/* --- hang watchdog thread: reads the op markers (defined at top of file) written by the guest
 * carrier thread — NO cross-thread uc access. Distinguishes a blocking bridge/JNI call
 * (g_sched_tick frozen) from a slow run_loop spin (the lowered [sched-sample] heartbeat fires). --- */
static void *watchdog_thread(void *p){ (void)p;
    unsigned long lseq=0,ltick=0; int n=0;
    for(;;){ struct timespec ts={4,0}; nanosleep(&ts,NULL);
        unsigned long seq=g_op_seq, tick=g_sched_tick; const char*br=(const char*)g_op_bridge; int sl=g_op_slot;
        if(seq==lseq && tick==ltick){ if(++n>=2){    /* ~8s with zero op AND zero quantum progress => FROZEN */
                if(sl>=0) LOGE("[WATCHDOG] FROZEN ~%ds in native '%s': last op = JNI slot %d (blocking Call*Method into ART? seq=%lu tick=%lu)", n*4, g_cur_native, sl, seq, tick);
                else      LOGE("[WATCHDOG] FROZEN ~%ds in native '%s': last op = bridge '%s' (seq=%lu tick=%lu)", n*4, g_cur_native, br?br:"?", seq, tick);
                /* the LIVE guest PC where uc is paused = the exact call site of the blocking host call (the sched-dump resume PC is stale for the running thread) */
                { uint32_t pc=0,lr=0,r0=0,r1=0; uc_reg_read(G.cpu.uc,UC_ARM_REG_PC,&pc); uc_reg_read(G.cpu.uc,UC_ARM_REG_LR,&lr); uc_reg_read(G.cpu.uc,UC_ARM_REG_R0,&r0); uc_reg_read(G.cpu.uc,UC_ARM_REG_R1,&r1);
                  LOGE("[WATCHDOG] live uc: PC=+0x%x lr=+0x%x r0=0x%x r1=0x%x", pc>=RG_ENGINE?pc-RG_ENGINE:pc, lr>=RG_ENGINE?lr-RG_ENGINE:lr, r0, r1); }
                { static int dumped=0; if(!dumped){ dumped=1; sched_dump_state(&G.sch); } }  /* DIAG: thread states at the FIRST freeze -> deadlock/no-carrier? */
                n=0; }
        } else { if(n) LOGE("[WATCHDOG] (recovered: seq %lu->%lu tick %lu->%lu)", lseq, seq, ltick, tick); n=0; }
        lseq=seq; ltick=tick; }
    return NULL; }
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved){
    (void)reserved;
    { int pfd[2]; if(pipe(pfd)==0){ dup2(pfd[1],2); close(pfd[1]);
        pthread_t t; if(pthread_create(&t,NULL,stderr_pump,(void*)(long)pfd[0])==0) pthread_detach(t); } }
    { pthread_t wt; if(pthread_create(&wt,NULL,watchdog_thread,NULL)==0) pthread_detach(wt); }
    pthread_mutex_init(&G.bel,0);
    if(load_engine_bytes()){ return JNI_VERSION_1_6; }
    if(cpu_create(&G.cpu)){ LOGE("cpu_create failed"); return JNI_VERSION_1_6; }
    if(loader_load(&G.ld,&G.cpu,G.engine,G.engine_len)){ LOGE("loader_load failed"); return JNI_VERSION_1_6; }
    dispatch_install(&G.disp,&G.cpu,&G.ld);   /* NB: RG_STUB#20 @0x10000050 = __stack_chk_fail — the level-load
                                               * fatal is a STACK-CANARY SMASH: the UAF's corrupt string length
                                               * overflows a stack buffer at a site guard-memcpy@0x7de880 misses. */
    { static uc_hook gwp; uc_hook_add(G.cpu.uc,&gwp,UC_HOOK_MEM_WRITE,(void*)gobj_wp_shim,NULL,RG_ENGINE+0xabb120u,RG_ENGINE+0xabb12fu); } /* DIAG */
    { static uc_hook dh[12]; int dn=0; /* DIAG: throw-log + scene-ctor path trace */
      uint64_t pts[]={DIAG_E(0x85a1d0),DIAG_E(0x1de294),DIAG_E(0x1de2f8),DIAG_E(0x1de308),DIAG_E(0x1de310),
                      DIAG_E(0x1de318),DIAG_E(0x1de364),DIAG_E(0x1de4f0),DIAG_E(0x726a0),DIAG_E(0x726a4),DIAG_E(0x726ac),
                      DIAG_E(0x1df5b4)};
      for(unsigned i=0;i<sizeof(pts)/sizeof(pts[0]);i++) uc_hook_add(G.cpu.uc,&dh[dn++],UC_HOOK_CODE,(void*)diag_pc_hook,NULL,pts[i],pts[i]); }
    { static uc_hook hmf; uc_hook_add(G.cpu.uc,&hmf,UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED|UC_HOOK_MEM_READ_PROT|UC_HOOK_MEM_WRITE_PROT,
        (void*)diag_memfault,NULL,(uint64_t)1,(uint64_t)0); }   /* DIAG catch a real fault (addr+pc) — cheap (fires only on fault) */
#ifndef ABSHIM_RELEASE  /* pure-log parse diagnostics — omit from the shipping build */
    { static uc_hook hjp; uc_hook_add(G.cpu.uc,&hjp,UC_HOOK_CODE,(void*)diag_jsonparse,NULL,RG_ENGINE+0x69814cu,RG_ENGINE+0x69814cu); } /* DIAG JSON parse inputs -> pin the ParseError/BadType assets */
    { static uc_hook hll; uc_hook_add(G.cpu.uc,&hll,UC_HOOK_CODE,(void*)diag_luaload,NULL,RG_ENGINE+0x7cc104u,RG_ENGINE+0x7cc104u); } /* DIAG dump DECRYPTED Lua (lua_load) -> read findScriptPath */
#endif
    { static uc_hook hu16; uc_hook_add(G.cpu.uc,&hu16,UC_HOOK_CODE,(void*)diag_u16conv,NULL,RG_ENGINE+0x727ec0u,RG_ENGINE+0x727ec0u); } /* GUARD (KEEP): UTF8->UTF16 conv clamps the corrupt ~33M source string */
    { static uc_hook hrcs; uc_hook_add(G.cpu.uc,&hrcs,UC_HOOK_CODE,(void*)neut_rcs_login,NULL,RG_ENGINE+0x310bc0u,RG_ENGINE+0x310bc0u); } /* DE-PHONE-HOME: skip RCS Identity login call, keep post-work */
    /* Set up a permanent empty std::basic_string<char32_t> (never-freed high refcount), then neutralize
     * the 3 GameLua device-info getters to return it — fixes the char32_t-stream use-after-free at its
     * SOURCE (kills the cyclic-tree grind + registry/notification cascade) AND removes the device-info
     * phone-home. _Rep layout: [_M_p-0xc]=length, [-8]=capacity, [-4]=refcount, [_M_p]=data (null u32). */
    { uint32_t p = galloc_malloc(G.cpu.heap, 0x20u);
      if(p){ uint32_t rep[4] = { 0u, 0u, 0x40000000u, 0u }; uc_mem_write(G.cpu.uc, p, rep, sizeof rep); g_empty_u32_mp = p + 0xcu;
             LOG("[gamelua-getter-neut] empty char32_t _Rep @0x%x (_M_p=0x%x); hooking getters 0x1112fc/0x111368/0x1113d4", p, g_empty_u32_mp); }
      static uc_hook hg1,hg2,hg3;
      uc_hook_add(G.cpu.uc,&hg1,UC_HOOK_CODE,(void*)neut_gamelua_getter,NULL,RG_ENGINE+0x1112fcu,RG_ENGINE+0x1112fcu);
      uc_hook_add(G.cpu.uc,&hg2,UC_HOOK_CODE,(void*)neut_gamelua_getter,NULL,RG_ENGINE+0x111368u,RG_ENGINE+0x111368u);
      uc_hook_add(G.cpu.uc,&hg3,UC_HOOK_CODE,(void*)neut_gamelua_getter,NULL,RG_ENGINE+0x1113d4u,RG_ENGINE+0x1113d4u); }
    { static uc_hook htw; uc_hook_add(G.cpu.uc,&htw,UC_HOOK_CODE,(void*)guard_tree_walk,NULL,RG_ENGINE+0x7d2d18u,RG_ENGINE+0x7d2d18u); } /* GRIND BREAKER: clamp a corrupt pointer-valued node-count (r2) at the 0x7d2d18 list/tree walk -> the nondeterministic nativeInit grind returns instead of looping ~billions */
#ifndef ABSHIM_RELEASE
    { static uc_hook hvfs; uc_hook_add(G.cpu.uc,&hvfs,UC_HOOK_CODE,(void*)diag_vfsopen,NULL,RG_ENGINE+0x6bd4f4u,RG_ENGINE+0x6bd4f4u); } /* DIAG: log VFS-open URIs -> pin the AppConfiguration invalid-scheme phone-home */
#endif
    { static uc_hook hvsc; uc_hook_add(G.cpu.uc,&hvsc,UC_HOOK_CODE,(void*)neut_vfs_invalid_scheme,NULL,RG_ENGINE+0x6be064u,RG_ENGINE+0x6be064u); } /* DE-PHONE-HOME: VFS invalid-scheme throw -> graceful NULL stream (unblocks nativeInit past all https phone-homes) */
    { static uc_hook hcfg; uc_hook_add(G.cpu.uc,&hcfg,UC_HOOK_CODE,(void*)neut_addlink_scheme,NULL,RG_ENGINE+0x6c0af4u,RG_ENGINE+0x6c0af4u); } /* DE-PHONE-HOME: VFS::addLink 'scheme is not vfs' throw -> clean return @0x6c002c (drops ALL https config links; keeps bundled default; abandons-nativeInit -> draws=0 fix) */
    /* DE-PHONE-HOME COMPLETENESS: the two hooks above leave the ad-config JSON EMPTY; guard the empty
     * parse so it yields '{}' instead of a fatal ParseError at level-end (see guard_empty_json). KEEPER. */
    { uint32_t p = galloc_malloc(G.cpu.heap, 0x10u);
      if(p){ uint8_t jb[2]={(uint8_t)'{',(uint8_t)'}'}; uc_mem_write(G.cpu.uc,p,jb,2); g_json_empty_mp=p;
             LOG("[empty-json-guard] permanent '{}' @0x%x; hooking JSON parse 0x69814c", p); }
      static uc_hook hej; uc_hook_add(G.cpu.uc,&hej,UC_HOOK_CODE,(void*)guard_empty_json,NULL,RG_ENGINE+0x69814cu,RG_ENGINE+0x69814cu); }
    /* companion to the {} guard: the ad code reads a null value out of {} and builds std::string(NULL); guard that too */
    { uint32_t p = galloc_malloc(G.cpu.heap, 0x10u);
      if(p){ uint32_t rep[4]={0u,0u,0x40000000u,0u}; uc_mem_write(G.cpu.uc,p,rep,sizeof rep); g_empty_str_mp=p+0xcu;
             LOG("[s-construct-null-guard] empty std::string _Rep @0x%x (_M_p=0x%x); hooking _S_construct 0x88dee4", p, g_empty_str_mp); }
      static uc_hook hsc; uc_hook_add(G.cpu.uc,&hsc,UC_HOOK_CODE,(void*)neut_s_construct_null,NULL,RG_ENGINE+0x88dee4u,RG_ENGINE+0x88dee4u); }
#ifndef ABSHIM_RELEASE  /* pure-log image-load diagnostics — omit from the shipping build */
    { static uc_hook himf; uc_hook_add(G.cpu.uc,&himf,UC_HOOK_CODE,(void*)diag_imgfmt,NULL,RG_ENGINE+0x67a034u,RG_ENGINE+0x67a034u); } /* DIAG: image-format dispatcher -> pin the FONT_BASIC.pvr 'unsupported format' (new draws=0 blocker) */
    { static uc_hook hims; uc_hook_add(G.cpu.uc,&hims,UC_HOOK_CODE,(void*)diag_imgsrc,NULL,RG_ENGINE+0x6b2f20u,RG_ENGINE+0x6b2f20u); } /* DIAG: image-format MAPPER -> dump the REAL source stream (identify class + whether it holds the file data) */
    { static uc_hook himr; uc_hook_add(G.cpu.uc,&himr,UC_HOOK_CODE,(void*)diag_imgread,NULL,RG_ENGINE+0x6c94ecu,RG_ENGINE+0x6c94ecu); } /* DIAG: magic-read avail/needed/bytes -> is the stream empty or is the data wrong? */
    { static uc_hook hsr; uc_hook_add(G.cpu.uc,&hsr,UC_HOOK_CODE,(void*)diag_streamread,NULL,RG_ENGINE+0x6c5600u,RG_ENGINE+0x6c5600u); } /* DIAG: memory-stream read -> dump base+cur+magic bytes */
    { static uc_hook hik; uc_hook_add(G.cpu.uc,&hik,UC_HOOK_CODE,(void*)diag_imgkey,NULL,RG_ENGINE+0x6b2f78u,RG_ENGINE+0x6b2f78u); } /* DIAG: mapper magic key string */
    { static uc_hook hri; uc_hook_add(G.cpu.uc,&hri,UC_HOOK_CODE,(void*)diag_reginit,NULL,RG_ENGINE+0x6b1f68u,RG_ENGINE+0x6b1f68u); } /* DIAG: does the image-format registry init run? */
    { static uc_hook hic; uc_hook_add(G.cpu.uc,&hic,UC_HOOK_CODE,(void*)diag_imgcmp,NULL,RG_ENGINE+0x6b2d58u,RG_ENGINE+0x6b2d58u); } /* DIAG: format-detect string compare operands+result */
    { static uc_hook hii; uc_hook_add(G.cpu.uc,&hii,UC_HOOK_CODE,(void*)diag_imginput,NULL,RG_ENGINE+0x6b2fc8u,RG_ENGINE+0x6b2fc8u); }
#endif
    { static uc_hook hmc; uc_hook_add(G.cpu.uc,&hmc,UC_HOOK_CODE,(void*)diag_memcpy,NULL,RG_ENGINE+0x7de880u,RG_ENGINE+0x7de880u); } /* GUARD (KEEP): clamp the corrupt-length memcpy */
#ifndef ABSHIM_RELEASE  /* pure-log diagnostics (writes=0) — omit from the shipping build for speed + quiet logcat */
    { static uc_hook hap; uc_hook_add(G.cpu.uc,&hap,UC_HOOK_CODE,(void*)diag_append,NULL,RG_ENGINE+0x7ce73cu,RG_ENGINE+0x7ce73cu); } /* DIAG: append caller + src COW _Rep */
    { static uc_hook hvc; uc_hook_add(G.cpu.uc,&hvc,UC_HOOK_CODE,(void*)diag_vcall,NULL,RG_ENGINE+0xdd258u,RG_ENGINE+0xdd258u); } /* DIAG: PMF call returning corrupt string */
    { static uc_hook hss; uc_hook_add(G.cpu.uc,&hss,UC_HOOK_CODE,(void*)diag_imgsubstr,NULL,RG_ENGINE+0x6b2cf8u,RG_ENGINE+0x6b2cf8u); }
#endif
    (void)diag_skipnull;   /* hot-path bypass replaced by the cheap mem-fault bypass in diag_memfault */
    { static uc_hook hbt[2]; uint32_t bt[]={0x6a8aecu,0x6aa8a4u};   /* DIAG BadType throw sites -> consumer call-chain */
      for(unsigned i=0;i<2;i++) uc_hook_add(G.cpu.uc,&hbt[i],UC_HOOK_CODE,(void*)diag_badtype,NULL,RG_ENGINE+bt[i],RG_ENGINE+bt[i]); }
#ifdef ABSHIM_HEAVY_DIAG   /* per-instruction/per-block recursion probes: HUGE slowdown — off for speed/render runs */
    { static uc_hook cbb; uc_hook_add(G.cpu.uc,&cbb,UC_HOOK_BLOCK,(void*)diag_ctor_bb,NULL,RG_ENGINE+0x6f370u,RG_ENGINE+0x6f928u); } /* DIAG ctor block-trace */
    { static uc_hook rec; uc_hook_add(G.cpu.uc,&rec,UC_HOOK_CODE,(void*)diag_recurse,NULL,RG_ENGINE+0x7c53fcu,RG_ENGINE+0x7c53fcu); } /* DIAG resolver node-cycle */
    { static uc_hook hw; uc_hook_add(G.cpu.uc,&hw,UC_HOOK_CODE,(void*)diag_w,NULL,RG_ENGINE+0x7d3be0u,RG_ENGINE+0x7d3be0u); }        /* DIAG W deep-recursion depth */
    { static uc_hook hwk; uc_hook_add(G.cpu.uc,&hwk,UC_HOOK_CODE,(void*)diag_walk,NULL,RG_ENGINE+0x7d2d18u,RG_ENGINE+0x7d2d18u); }   /* DIAG walk color-guard state */
    { static uc_hook rr[4]; uint32_t rp[]={0x7c5448u,0x7c5480u,0x7c54a4u,0x7c54c4u};  /* resolver pop{r4,pc} return sites: type2/3/4/1 */
      for(unsigned i=0;i<4;i++) uc_hook_add(G.cpu.uc,&rr[i],UC_HOOK_CODE,(void*)diag_resolve_ret,NULL,RG_ENGINE+rp[i],RG_ENGINE+rp[i]); } /* DIAG resolver output */
    { static uc_hook rbb; uc_hook_add(G.cpu.uc,&rbb,UC_HOOK_BLOCK,(void*)diag_ring_bb,NULL,(uint64_t)1,(uint64_t)0); }     /* DIAG all-block ring (begin>end => all) */
#else
    (void)diag_ctor_bb;(void)diag_recurse;(void)diag_w;(void)diag_walk;(void)diag_resolve_ret;(void)diag_ring_bb;(void)diag_stackscan;
#endif
    sched_init(&G.sch,&G.cpu); G.disp.sch=&G.sch;       /* real guest threading (pthread_* -> GEL) */
    G.sch.fatal_ext=&G.disp.fatal;                      /* M3: abort()/__stack_chk_fail halts the scheduler */
    G.sch.jni_do_call=jni_block_do_cb; G.sch.jni_finalize=jni_block_finish_cb;  /* S2 blocking-JNI stop/restart */
    jni_install(&G.jni,&G.cpu,0);
    G.disp.jni=&G.jni;                                  /* route AAsset* to the real libandroid */
    G.jni.disp_env=env_dispatch_real; G.jni.disp_vm=vm_dispatch_real; G.jni.real_vm=vm;
    /* run C++ init_array */
    int tot=0, ran=dispatch_run_init_array(&G.disp,&tot);
    LOG("init_array %d/%d (unimpl=%d last='%s')",ran,tot,G.disp.unimpl_count,G.disp.last_unimpl);
    if(ran<tot) LOGE("init_array INCOMPLETE (%d/%d) — a C++ constructor faulted; last_unimpl='%s'",ran,tot,G.disp.last_unimpl);
    /* run the guest's own JNI_OnLoad with the guest VM sentinel */
    uint32_t gonload=resolve_guest("JNI_OnLoad");
    if(gonload){ uc_err e=cpu_call(&G.cpu,gonload,G.jni.vm,0,0,0,0); uint32_t r0=0; uc_reg_read(G.cpu.uc,UC_ARM_REG_R0,&r0); LOG("guest JNI_OnLoad -> 0x%x (%s)",r0,uc_strerror(e)); }
    G.ready=1;
    LOG("abshim ready (host pagesize=%ld)", sysconf(_SC_PAGESIZE));
    return JNI_VERSION_1_6;
}
