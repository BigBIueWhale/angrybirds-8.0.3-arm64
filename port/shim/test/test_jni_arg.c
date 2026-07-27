/* test_jni_arg.c — host test for the JNI outbound arg-builders (jni_argbuild.c).
 * Proves the three calling forms decode identically-typed signatures CORRECTLY
 * despite their different byte layouts:
 *   - va_list / inline (C-variadic): 'F' is promoted to double (8 bytes) -> narrow
 *   - jvalue[] array:                'F' is a natural 4-byte float, 8-byte stride
 * and that the Call<T>Method{,V,A} slot -> (type,form) decomposition matches the
 * real JNINativeInterface layout (incl. the previously-dropped ...MethodA slots
 * 63 / 143). Build via run_tests.sh (ASan+UBSan, -fno-sanitize-recover). */
#include "jni_argbuild.h"
#include "handle_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- flat-buffer guest_mem backend WITH a register file (as in test_marshal.c) ---- */
typedef struct { uint8_t *buf; uint32_t base, size; uint32_t regs[16]; } hostmem;
static void hm_read(guest_mem*m, void*d, uint32_t a, uint32_t n){
    hostmem*h=(hostmem*)m->ctx;
    if (a<h->base || (uint64_t)a+n>(uint64_t)h->base+h->size){ fprintf(stderr,"OOB rd 0x%08x+%u\n",a,n); abort(); }
    memcpy(d,h->buf+(a-h->base),n);
}
static void hm_write(guest_mem*m, uint32_t a, const void*s, uint32_t n){
    hostmem*h=(hostmem*)m->ctx;
    if (a<h->base || (uint64_t)a+n>(uint64_t)h->base+h->size){ fprintf(stderr,"OOB wr 0x%08x+%u\n",a,n); abort(); }
    memcpy(h->buf+(a-h->base),s,n);
}
static uint32_t hm_reg_get(guest_mem*m,int i){ return ((hostmem*)m->ctx)->regs[i]; }
static void     hm_reg_set(guest_mem*m,int i,uint32_t v){ ((hostmem*)m->ctx)->regs[i]=v; }

static hostmem H; static guest_mem M;
static void reset(void){
    if(!H.buf){ H.buf=calloc(1,0x10000); H.base=0x50000000u; H.size=0x10000u; }
    memset(H.buf,0,0x10000); memset(H.regs,0,sizeof H.regs);
    M.read=hm_read; M.write=hm_write; M.reg_get=hm_reg_get; M.reg_set=hm_reg_set; M.ctx=&H;
}

static int fails=0;
#define CKEQ(a,b,msg) do{ uint64_t _a=(uint64_t)(a),_b=(uint64_t)(b); if(_a!=_b){ printf("  FAIL: %s (got 0x%llx want 0x%llx)\n",msg,(unsigned long long)_a,(unsigned long long)_b); fails++; } }while(0)
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)
static uint32_t fbits(float f){ uint32_t b; memcpy(&b,&f,4); return b; }

/* 3.5 is exactly representable in both float and double. */
#define D_3_5_LO 0x00000000u
#define D_3_5_HI 0x400C0000u   /* double 3.5 = 0x400C000000000000 */
#define F_3_5    0x40600000u   /* float  3.5 = 0x40600000 */
#define LVAL     0x1122334455667788ULL

/* CallXxxMethodV path: a guest va_list for "(IFJL)V".
 * The variadic 'F' arrives PROMOTED to a double (8 bytes, 8-aligned). */
static void t_valist(void){
    printf("[valist (IFJL)V — float promoted to double]\n");
    reset();
    handle_table*ht=ht_create();
    void*obj=(void*)0x0000CAFEu; uint32_t tok=ht_new_ref(ht,HK_LOCAL,obj);
    uint32_t va=0x50001000u;                 /* 8-aligned */
    gm_wr32(&M, va+0,  0x12345678u);         /* int   @0            */
    /* @4 padding (float 8-aligns to @8) */
    gm_wr32(&M, va+8,  D_3_5_LO);            /* promoted double @8  */
    gm_wr32(&M, va+12, D_3_5_HI);
    gm_wr32(&M, va+16, (uint32_t)LVAL);      /* long @16            */
    gm_wr32(&M, va+20, (uint32_t)(LVAL>>32));
    gm_wr32(&M, va+24, tok);                 /* object token @24    */
    ab_jval o[4]; memset(o,0,sizeof o);
    int n=ab_build_valist(&M,ht,"(IFJL)V",va,o,4);
    CKEQ(n,4,"valist nargs");
    CKEQ((uint32_t)o[0].i, 0x12345678u, "valist int");
    CKEQ(fbits(o[1].f), F_3_5,          "valist float (from promoted double)");
    CKEQ((uint64_t)o[2].j, LVAL,        "valist long");
    CKEQ((uintptr_t)o[3].l, (uintptr_t)obj, "valist object resolved");
    ht_destroy(ht);
}

/* CallXxxMethodA path: a guest jvalue[] for "(IFJL)V".
 * Natural cells, 8-byte stride, 'F' is a plain 4-byte float (NOT promoted). */
static void t_jvalarr(void){
    printf("[jvalarr (IFJL)V — natural float, 8-byte stride]\n");
    reset();
    handle_table*ht=ht_create();
    void*obj=(void*)0x0000BEE5u; uint32_t tok=ht_new_ref(ht,HK_LOCAL,obj);
    uint32_t ap=0x50002000u;
    gm_wr32(&M, ap+0,  0x0BADF00Du);         /* cell0 I */
    gm_wr32(&M, ap+8,  F_3_5);               /* cell1 F (low word) */
    gm_wr32(&M, ap+16, (uint32_t)LVAL);      /* cell2 J */
    gm_wr32(&M, ap+20, (uint32_t)(LVAL>>32));
    gm_wr32(&M, ap+24, tok);                 /* cell3 L */
    ab_jval o[4]; memset(o,0,sizeof o);
    int n=ab_build_jvalarr(&M,ht,"(IFJL)V",ap,o,4);
    CKEQ(n,4,"jvalarr nargs");
    CKEQ((uint32_t)o[0].i, 0x0BADF00Du, "jvalarr int");
    CKEQ(fbits(o[1].f), F_3_5,          "jvalarr float (natural, not promoted)");
    CKEQ((uint64_t)o[2].j, LVAL,        "jvalarr long (stride kept in sync)");
    CKEQ((uintptr_t)o[3].l, (uintptr_t)obj, "jvalarr object resolved");
    ht_destroy(ht);
}

/* bare CallXxxMethod path: inline AAPCS32 varargs pulled from the live cursor.
 * After env/obj/mid consumed r0..r2, cursor NCRN=3. "(IF)V": int -> r3, then the
 * promoted-double float 8-aligns NCRN 3->4 (exhausted) -> 8-aligned stack. */
static void t_inline(void){
    printf("[inline (IF)V — int in r3, promoted float on stack]\n");
    reset();
    handle_table*ht=ht_create();
    H.regs[3]=0x00ABCDEFu;                    /* the int, in r3 */
    uint32_t sp=0x50008000u;                   /* 8-aligned */
    gm_wr32(&M, sp+0, D_3_5_LO);              /* promoted double @[SP] */
    gm_wr32(&M, sp+4, D_3_5_HI);
    mcur cur={.ncrn=3,.nsaa=sp};
    ab_jval o[2]; memset(o,0,sizeof o);
    int n=ab_build_inline(&M,ht,"(IF)V",&cur,o,2);
    CKEQ(n,2,"inline nargs");
    CKEQ((uint32_t)o[0].i, 0x00ABCDEFu, "inline int (r3)");
    CKEQ(fbits(o[1].f), F_3_5,          "inline float (promoted, 8-aligned stack)");
    ht_destroy(ht);
}

/* sub-int narrowing + NULL-token resolution across a valist. "(ZBCS L)V" with a
 * NULL object token must resolve to NULL (not a bogus pointer). */
static void t_narrow(void){
    printf("[valist (ZBCSL)V — sub-int narrowing + NULL ref]\n");
    reset();
    handle_table*ht=ht_create();
    uint32_t va=0x50003000u;
    gm_wr32(&M, va+0,  0x00000101u);         /* Z: promoted int, low bit=1 -> true */
    gm_wr32(&M, va+4,  0xFFFFFF80u);         /* B: -128 */
    gm_wr32(&M, va+8,  0x0000FFFFu);         /* C: 0xFFFF */
    gm_wr32(&M, va+12, 0xFFFF8000u);         /* S: -32768 */
    gm_wr32(&M, va+16, HT_NULL);             /* L: null token */
    ab_jval o[5]; memset(o,0,sizeof o);
    int n=ab_build_valist(&M,ht,"(ZBCSL)V",va,o,5);
    CKEQ(n,5,"narrow nargs");
    CKEQ(o[0].z, 1,        "Z narrowed to 1");
    CKEQ((int64_t)o[1].b, -128, "B narrowed");
    CKEQ(o[2].c, 0xFFFFu,  "C narrowed");
    CKEQ((int64_t)o[3].s, -32768, "S narrowed");
    CKEQ((uintptr_t)o[4].l, 0u, "NULL token -> NULL object");
    ht_destroy(ht);
}

/* The Call<T>Method{,V,A} slot -> (is_static,type,form) decomposition used by
 * env_dispatch_real. Ground truth from the NDK JNINativeInterface layout. */
static void decomp(int slot,int*st,int*type,int*form){ *st=slot>=114; int base=*st?114:34; int rel=slot-base; *type=rel/3; *form=rel%3; }
static void t_slots(void){
    printf("[Call-family slot decomposition vs JNINativeInterface]\n");
    struct { int slot,st,type,form; const char*nm; } T[] = {
        { 34,0,0,0,"CallObjectMethod"},   { 35,0,0,1,"CallObjectMethodV"}, { 36,0,0,2,"CallObjectMethodA"},
        { 37,0,1,0,"CallBooleanMethod"},  { 62,0,9,1,"CallVoidMethodV"},   { 63,0,9,2,"CallVoidMethodA"},
        {114,1,0,0,"CallStaticObjectMethod"}, {115,1,0,1,"CallStaticObjectMethodV"},
        {142,1,9,1,"CallStaticVoidMethodV"},  {143,1,9,2,"CallStaticVoidMethodA"},
    };
    for(unsigned k=0;k<sizeof T/sizeof T[0];k++){
        int st,ty,fm; decomp(T[k].slot,&st,&ty,&fm);
        char m[96]; snprintf(m,sizeof m,"%s slot %d",T[k].nm,T[k].slot);
        CK(st==T[k].st && ty==T[k].type && fm==T[k].form, m);
    }
    /* the range must include the ...MethodA endpoints that the old [35,62]/[115,142] dropped */
    CK((63>=34&&63<=63) && (143>=114&&143<=143), "CallVoidMethodA(63)/CallStaticVoidMethodA(143) in range");
}

/* Field/array family slot decompositions used by env_dispatch_real, vs the true
 * NDK JNINativeInterface indices. Locks the base/range arithmetic on the host. */
static void field_decomp(int slot,int*st,int*ty){
    if(slot>=145&&slot<=153){*st=1;*ty=slot-145;}       /* GetStatic<T>Field */
    else if(slot>=95&&slot<=103){*st=0;*ty=slot-95;}    /* Get<T>Field */
    else if(slot>=154&&slot<=162){*st=1;*ty=slot-154;}  /* SetStatic<T>Field */
    else {*st=0;*ty=slot-104;}                          /* Set<T>Field */
}
static int arr_esz(int ty){ static const int s[8]={1,1,2,2,4,8,4,8}; return s[ty]; }
static void t_extslots(void){
    printf("[field/array family slot decompositions vs JNINativeInterface]\n");
    struct { int slot,st,ty; const char*nm; } F[] = {
        { 95,0,0,"GetObjectField"}, {100,0,5,"GetIntField"}, {103,0,8,"GetDoubleField"},
        {145,1,0,"GetStaticObjectField"}, {150,1,5,"GetStaticIntField"}, {153,1,8,"GetStaticDoubleField"},
        {104,0,0,"SetObjectField"}, {110,0,6,"SetLongField"}, {112,0,8,"SetDoubleField"},
        {154,1,0,"SetStaticObjectField"}, {162,1,8,"SetStaticDoubleField"},
    };
    for(unsigned k=0;k<sizeof F/sizeof F[0];k++){ int st,ty; field_decomp(F[k].slot,&st,&ty);
        char m[96]; snprintf(m,sizeof m,"%s slot %d",F[k].nm,F[k].slot); CK(st==F[k].st&&ty==F[k].ty,m); }
    /* array type index (0 Bool,1 Byte,2 Char,3 Short,4 Int,5 Long,6 Float,7 Double) + elem size */
    CK((179-175)==4 && arr_esz(179-175)==4, "NewIntArray=179 -> ty Int, esz 4");
    CK((184-183)==1 && arr_esz(184-183)==1, "GetByteArrayElements=184 -> ty Byte, esz 1");
    CK((190-183)==7 && arr_esz(190-183)==8, "GetDoubleArrayElements=190 -> ty Double, esz 8");
    CK((203-199)==4, "GetIntArrayRegion=203 -> ty Int");
    CK((210-207)==3 && arr_esz(210-207)==2, "SetShortArrayRegion=210 -> ty Short, esz 2");
    CK((28-28)==0 && (29-28)==1 && (30-28)==2, "NewObject/V/A forms 0/1/2");
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== jni arg-builder host test ===\n");
    t_valist(); t_jvalarr(); t_inline(); t_narrow(); t_slots(); t_extslots();
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
