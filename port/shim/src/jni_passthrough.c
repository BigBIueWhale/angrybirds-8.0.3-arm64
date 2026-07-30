/* jni_passthrough.c — guest JNIEnv/JavaVM sentinels + slot dispatch. See .h.
 * HOST-FAKE backend: return plausible tokens so nativeInit progresses; the
 * DEVICE-REAL backend swaps the leaf action for a call to the real env. */
#include "jni_passthrough.h"
#include "regions.h"
#include "marshal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* GUESTDATA sub-layout for the JNI structures (above the loader mirrors and the
 * dispatch errno/locale slots) */
#define JD_ENV_VT   (RG_GUESTDATA + 0x8000u)   /* 260-word env vtable */
#define JD_ENV_PTR  (RG_GUESTDATA + 0x8420u)   /* holds env vtable addr; env = &this */
#define JD_VM_VT    (RG_GUESTDATA + 0x8500u)   /* 16-word VM vtable */
#define JD_VM_PTR   (RG_GUESTDATA + 0x8560u)
#define JD_SCRATCH  (RG_GUESTDATA + 0x9000u)   /* string/array host->guest copies */
#define JD_SCRATCH_END (RG_GUESTDATA + 0xE000u)
#define JNI_VM_OFF  0x4000u                     /* VM trampolines at RG_JNI+this */

static void set_r0(cpu_t*c,uint32_t v){ uc_reg_write(c->uc,UC_ARM_REG_R0,&v); }
static void set_r1(cpu_t*c,uint32_t v){ uc_reg_write(c->uc,UC_ARM_REG_R1,&v); }
static int em_str(cpu_t*c,uint32_t p,char*o,int max){ int i=0; if(!p){o[0]=0;return 0;} for(;i<max-1;i++){ uint8_t ch=0; if(uc_mem_read(c->uc,p+i,&ch,1)!=UC_ERR_OK||!ch) break; o[i]=(char)ch; } o[i]=0; return i; }
static void*fakeptr(jni_state*J){ return (void*)(uintptr_t)(0x50000000u + (++J->fake_next)); }
static int store_sig(jni_state*J,const char*s){ if(J->nsig<1024){ snprintf(J->sigs[J->nsig],80,"%s",s); return J->nsig++; } return -1; }
static uint32_t scratch_alloc(jni_state*J,uint32_t n){ n=(n+7)&~7u; if(J->scratch+n>J->scratch_end) J->scratch=JD_SCRATCH; uint32_t p=J->scratch; J->scratch+=n; return p; }

/* host-fake string round-trip: map a jstring token -> its C value */
static const char* fstr_get(jni_state*J,uint32_t tok){ for(int i=0;i<J->nfstr;i++) if(J->fstr[i].tok==tok) return J->fstr[i].val; return NULL; }
static uint32_t fstr_put(jni_state*J,uint32_t tok,const char*val){ if(J->nfstr<128){ char *d=strdup(val?val:"");
        /* strdup was unchecked and the slot counter advanced regardless, burning one of the 128
         * entries on a NULL value. Only commit the slot if the copy succeeded. */
        if(d){ J->fstr[J->nfstr].tok=tok; J->fstr[J->nfstr].val=d; J->nfstr++; } } return tok; }
uint32_t jni_fake_intern_string(jni_state*J,const char*val){ uint32_t tok=ht_new_ref(J->ht,HK_LOCAL,fakeptr(J)); return fstr_put(J,tok,val); }

/* env-vtable slot dispatch (host-fake backend; device overrides via J->disp_env) */
static void env_dispatch_fake(jni_state*J, uint32_t slot){
    cpu_t*c=J->cpu; mcur cur; marshal_cur_init(&c->mem,&cur);
    (void)marshal_pull_word(&c->mem,&cur);                 /* env (r0) */
    #define W marshal_pull_word(&c->mem,&cur)
    /* Call<T>MethodV families: env(35..62), static(115..142). type index -> ret. */
    if ((slot>=34 && slot<=63) || (slot>=114 && slot<=143)){
        int base = (slot<=63)?34:114; int type=(slot-base)/3;   /* 0 Obj..9 Void */
        (void)W; (void)W; (void)W;                              /* obj/clazz, mid, va */
        switch(type){
        case 0: set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break;  /* Object -> fake local */
        case 6: set_r0(c,0); set_r1(c,0); break;                          /* Long -> r0:r1 */
        case 9: break;                                                    /* Void */
        default: set_r0(c,0); break;                                      /* Boolean/Byte/.../Double */
        }
        return;
    }
    switch(slot){
    case 4:  set_r0(c,0x00010006u); break;                 /* GetVersion */
    case 6:  { (void)W; set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break; }        /* FindClass */
    case 13: (void)W; break;                               /* Throw */
    case 14: { (void)W;(void)W; set_r0(c,0); break; }      /* ThrowNew */
    case 15: set_r0(c,0); break;                           /* ExceptionOccurred -> NULL */
    case 16: break;                                        /* ExceptionDescribe */
    case 17: break;                                        /* ExceptionClear */
    case 18: set_r0(c,0); break;                           /* FatalError (host: ignore) */
    case 19: (void)W; set_r0(c,0); break;                  /* PushLocalFrame */
    case 20: { (void)W; set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break; }        /* PopLocalFrame */
    case 21: { uint32_t o=W; void*r=ht_resolve(J->ht,o); set_r0(c, ht_new_ref(J->ht,HK_GLOBAL, r?r:fakeptr(J))); break; } /* NewGlobalRef */
    case 22: { uint32_t o=W; ht_delete_ref(J->ht,o); set_r0(c,0); break; }                /* DeleteGlobalRef */
    case 23: { uint32_t o=W; ht_delete_ref(J->ht,o); set_r0(c,0); break; }                /* DeleteLocalRef */
    case 24: { uint32_t a=W,b=W; set_r0(c, ht_resolve(J->ht,a)==ht_resolve(J->ht,b)?1:0); break; } /* IsSameObject */
    case 25: { uint32_t o=W; void*r=ht_resolve(J->ht,o); set_r0(c, ht_new_ref(J->ht,HK_LOCAL, r?r:fakeptr(J))); break; }  /* NewLocalRef */
    case 29: { (void)W;(void)W; set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break; }/* NewObjectV */
    case 31: { (void)W; set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break; }        /* GetObjectClass */
    case 32: { (void)W;(void)W; set_r0(c,1); break; }      /* IsInstanceOf -> yes */
    case 33: case 113: { (void)W; uint32_t nm=W,sig=W; (void)nm; char s[80]; em_str(c,sig,s,sizeof s); int si=store_sig(J,s);
                         set_r0(c, ht_intern_id(J->ht, fakeptr(J), (const void*)(intptr_t)si)); break; } /* Get(Static)MethodID + sig */
    case 94: case 144: { (void)W;(void)W;(void)W; set_r0(c, ht_intern_id(J->ht, fakeptr(J), NULL)); break; } /* Get(Static)FieldID */
    case 164: { uint32_t js=W; const char*v=fstr_get(J,js); set_r0(c, v?(uint32_t)strlen(v):0); break; }   /* GetStringLength */
    case 167: { uint32_t gp=W; char s[1024]; em_str(c,gp,s,sizeof s); uint32_t tok=ht_new_ref(J->ht,HK_LOCAL,fakeptr(J)); fstr_put(J,tok,s); set_r0(c,tok); break; } /* NewStringUTF: store bytes */
    case 168: { uint32_t js=W; const char*v=fstr_get(J,js); set_r0(c, v?(uint32_t)strlen(v):0); break; }   /* GetStringUTFLength */
    case 169: { uint32_t js=W; (void)W; const char*v=fstr_get(J,js); uint32_t n=v?(uint32_t)strlen(v):0;   /* GetStringUTFChars: return stored value (or "") */
                uint32_t p=scratch_alloc(J,n+1); if(v&&n) uc_mem_write(c->uc,p,v,n); uint8_t z=0; uc_mem_write(c->uc,p+n,&z,1); set_r0(c,p); break; }
    case 170: break;                                       /* ReleaseStringUTFChars */
    case 171: { (void)W; set_r0(c,0); break; }             /* GetArrayLength -> 0 */
    case 172: { (void)W;(void)W;(void)W; set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break; } /* NewObjectArray */
    case 173: { (void)W;(void)W; set_r0(c, ht_new_ref(J->ht,HK_LOCAL,fakeptr(J))); break; } /* GetObjectArrayElement */
    case 174: { (void)W;(void)W;(void)W; set_r0(c,0); break; }                            /* SetObjectArrayElement */
    case 184: { (void)W;(void)W; uint32_t p=scratch_alloc(J,64); set_r0(c,p); break; }    /* GetByteArrayElements -> scratch */
    case 192: break;                                       /* ReleaseByteArrayElements */
    case 219: { uint32_t pvm=W; if(pvm) gm_wr32(&c->mem,pvm,J->vm); set_r0(c,0); break; } /* GetJavaVM */
    case 221: { (void)W;(void)W;(void)W; uint32_t buf=W; if(buf){ uint8_t z=0; uc_mem_write(c->uc,buf,&z,1);} break; } /* GetStringUTFRegion */
    case 228: set_r0(c,0); break;                          /* ExceptionCheck -> JNI_FALSE (host: no exception) */
    default:  set_r0(c,0); break;
    }
    #undef W
}

static void vm_dispatch_fake(jni_state*J, uint32_t slot){
    cpu_t*c=J->cpu; mcur cur; marshal_cur_init(&c->mem,&cur);
    (void)marshal_pull_word(&c->mem,&cur);                 /* vm */
    switch(slot){
    case 4: case 6: case 7: {                              /* AttachCurrentThread(AsDaemon) / GetEnv */
        uint32_t penv=marshal_pull_word(&c->mem,&cur);
        if (penv) gm_wr32(&c->mem, penv, J->env);
        set_r0(c,0); break; }                              /* JNI_OK */
    case 5: set_r0(c,0); break;                            /* DetachCurrentThread */
    default: set_r0(c,0); break;
    }
}

/* env-gated JNI trace (ABSHIM_JNI_TRACE=1): name the class/method/field lookups + calls the
 * engine makes, so the boot's Java-API sequence is visible. Peeks registers; no side effects. */
static void jni_trace(jni_state*J, uint32_t slot){
    static int on=-1; if(on<0){ const char*e=getenv("ABSHIM_JNI_TRACE"); on=e&&*e?1:0; }
    if(!on) return;
    cpu_t*c=J->cpu; uint32_t r1=0,r2=0,r3=0; char nm[160]="",sg[96]="";
    uc_reg_read(c->uc,UC_ARM_REG_R1,&r1); uc_reg_read(c->uc,UC_ARM_REG_R2,&r2); uc_reg_read(c->uc,UC_ARM_REG_R3,&r3);
    if(slot==6){ em_str(c,r1,nm,sizeof nm); fprintf(stderr,"[jni] FindClass \"%s\"\n",nm); }
    else if(slot==33||slot==113){ em_str(c,r2,nm,sizeof nm); em_str(c,r3,sg,sizeof sg); fprintf(stderr,"[jni] Get%sMethodID %s %s\n",slot==113?"Static":"",nm,sg); }
    else if(slot==94||slot==144){ em_str(c,r2,nm,sizeof nm); em_str(c,r3,sg,sizeof sg); fprintf(stderr,"[jni] Get%sFieldID %s %s\n",slot==144?"Static":"",nm,sg); }
    else if((slot>=34&&slot<=63)||(slot>=114&&slot<=143)) fprintf(stderr,"[jni] Call slot %u\n",slot);
    else if((slot>=95&&slot<=112)||(slot>=145&&slot<=162)) fprintf(stderr,"[jni] Get/SetField slot %u\n",slot);
}
/* This callback makes REAL JNI calls into ART, so the slice timer must never stop inside it:
 * uc_emu_stop makes a UC_HOOK_CODE callback RE-FIRE at the same address on resume, and a doubled
 * ART call is what produced gr::GraphicsException in the previous attempt (a GL/surface call run
 * twice). Declared OUTSIDE the conditional below because BOTH build variants need the guard -- in the
 * previous attempt the guard landed in the profiling-only branch and the shipping build had none. */
extern volatile int abshim_in_nonidem_cb;

#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
/* Time spent in guest->JVM JNI passthrough. This arrives on its own UC_HOOK_CODE at RG_JNI, NOT
 * through dispatch.c's stub hook, so the bridge timer there cannot see it. Without this the JNI
 * cost would be silently attributed to "emulation" in the frame-time split. Depth-guarded and
 * globally accumulated for the same reasons documented at g_stub_ns in dispatch.c. */
#include <time.h>
static __thread uint64_t g_jnip_ns = 0, g_jnip_n = 0;
static __thread int g_jnip_depth = 0;
uint64_t jni_pass_ns(void){ return g_jnip_ns; }
uint64_t jni_pass_calls(void){ return g_jnip_n; }
static uint64_t jnip_now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static void jni_hook_cb_inner(uc_engine *uc, uint64_t address, uint32_t size, void *ud);
static void jni_hook_cb(uc_engine *uc, uint64_t address, uint32_t size, void *ud){
    if (g_jnip_depth++){ jni_hook_cb_inner(uc,address,size,ud); g_jnip_depth--; return; }
    abshim_in_nonidem_cb++;
    uint64_t _j0 = jnip_now_ns();
    jni_hook_cb_inner(uc,address,size,ud);
    g_jnip_ns += jnip_now_ns() - _j0; g_jnip_n++; g_jnip_depth--;
    abshim_in_nonidem_cb--;
}
static void jni_hook_cb_inner(uc_engine *uc, uint64_t address, uint32_t size, void *ud){
#else
/* Release has no timing wrapper, so add a minimal one purely for the guard, and bracket the body so the
 * depth is decremented on every exit path. */
static void jni_hook_cb_body(uc_engine *uc, uint64_t address, uint32_t size, void *ud);
static void jni_hook_cb(uc_engine *uc, uint64_t address, uint32_t size, void *ud){
    abshim_in_nonidem_cb++;
    jni_hook_cb_body(uc, address, size, ud);
    abshim_in_nonidem_cb--;
}
static void jni_hook_cb_body(uc_engine *uc, uint64_t address, uint32_t size, void *ud){
#endif
    (void)uc;(void)size;
    jni_state *J=(jni_state*)ud;
    uint32_t a=(uint32_t)address;
    if (a >= RG_JNI + JNI_VM_OFF){ J->disp_vm(J, (a - RG_JNI - JNI_VM_OFF)/4); return; }
    uint32_t slot=(a - RG_JNI)/4;
    if (slot<260){ J->call_count[slot]++; J->slots_used[slot]=1; }
    jni_trace(J, slot);
    J->disp_env(J, slot);
}

int jni_install(jni_state *J, cpu_t *cpu, int host_fake){
    memset(J,0,sizeof *J);
    J->cpu=cpu; J->host_fake=host_fake; J->scratch=JD_SCRATCH; J->scratch_end=JD_SCRATCH_END;
    J->disp_env=env_dispatch_fake; J->disp_vm=vm_dispatch_fake;   /* device overrides after install */
    J->ht=ht_create(); if(!J->ht) return -1;
    /* env vtable: 260 slots -> RG_JNI trampolines */
    for (uint32_t i=0;i<260;i++) gm_wr32(&cpu->mem, JD_ENV_VT + i*4, RG_JNI + i*4);
    gm_wr32(&cpu->mem, JD_ENV_PTR, JD_ENV_VT);            /* JNIEnv* = &(ptr-to-vtable) */
    J->env = JD_ENV_PTR;
    /* VM vtable: 16 slots -> RG_JNI + JNI_VM_OFF trampolines */
    for (uint32_t i=0;i<16;i++) gm_wr32(&cpu->mem, JD_VM_VT + i*4, RG_JNI + JNI_VM_OFF + i*4);
    gm_wr32(&cpu->mem, JD_VM_PTR, JD_VM_VT);
    J->vm = JD_VM_PTR;
    return uc_hook_add(cpu->uc, &J->hook, UC_HOOK_CODE, (void*)jni_hook_cb, J, RG_JNI, RG_JNI + RG_JNI_SZ);
}

void jni_free(jni_state *J){ if(J){ for(int i=0;i<J->nfstr;i++){ free(J->fstr[i].val); J->fstr[i].val=NULL; } J->nfstr=0; if(J->ht){ ht_destroy(J->ht); J->ht=NULL; } } }
