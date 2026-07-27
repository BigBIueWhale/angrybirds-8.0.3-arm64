/* test_marshal.c — host test for the AAPCS32 soft-float arg-walker/return-writer.
 * Exercises Audit 01's worked cases exactly. Build via run_tests.sh (ASan+UBSan).
 */
#include "marshal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- flat-buffer guest_mem backend WITH a register file ---- */
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
static void reset(uint32_t sp){
    if(!H.buf){ H.buf=calloc(1,0x10000); H.base=0x50000000u; H.size=0x10000u; }
    memset(H.buf,0,0x10000); memset(H.regs,0,sizeof H.regs);
    H.regs[13]=sp;
    M.read=hm_read; M.write=hm_write; M.reg_get=hm_reg_get; M.reg_set=hm_reg_set; M.ctx=&H;
}
static void R(int i,uint32_t v){ H.regs[i]=v; }
static void STK(uint32_t a,uint32_t v){ gm_wr32(&M,a,v); }

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)
#define CKEQ(a,b,msg) do{ uint64_t _a=(a),_b=(b); if(_a!=_b){ printf("  FAIL: %s (got 0x%llx want 0x%llx)\n",msg,(unsigned long long)_a,(unsigned long long)_b); fails++; } }while(0)

/* pow(double,double)->double : arg0 r0:r1, arg1 r2:r3 (Audit01 D4) */
static void t_pow(void){
    printf("[pow(d,d)]\n");
    reset(0x50008000u);
    uint64_t a=0x3ff0000000000000ULL /*1.0*/, b=0x4000000000000000ULL /*2.0*/;
    R(0,(uint32_t)a); R(1,(uint32_t)(a>>32)); R(2,(uint32_t)b); R(3,(uint32_t)(b>>32));
    mdesc d={.ret=A_F64,.nargs=2,.args={{A_F64,0,0,0},{A_F64,0,0,0}}};
    garg g[2]; marshal_read_args(&M,&d,g);
    CKEQ(g[0].v,a,"pow arg0=r0:r1"); CKEQ(g[1].v,b,"pow arg1=r2:r3");
    /* return-writer: a double result lands in r0:r1 (D2) */
    uint64_t res=0x4008000000000000ULL /*3.0*/;
    marshal_write_ret(&M,A_F64,res);
    CKEQ(((uint64_t)H.regs[0]|((uint64_t)H.regs[1]<<32)),res,"pow ret r0:r1");
}

/* lseek64(int fd, off64_t off, int whence)->off64_t :
   fd=r0; off is 8-aligned -> NCRN 1->2 -> r2:r3 (r1 padding); whence at SP */
static void t_lseek64(void){
    printf("[lseek64(i,i64,i)]\n");
    reset(0x50008000u);
    R(0,7);                    /* fd */
    R(1,0xDEADBEEF);           /* r1 padding (must be skipped) */
    uint64_t off=0x0000001200000034ULL;
    R(2,(uint32_t)off); R(3,(uint32_t)(off>>32));
    STK(0x50008000u,2);        /* whence at [SP] */
    mdesc d={.ret=A_I64,.nargs=3,.args={{A_I32,0,0,0},{A_I64,0,0,0},{A_I32,0,0,0}}};
    garg g[3]; marshal_read_args(&M,&d,g);
    CKEQ(g[0].v,7,"lseek fd=r0"); CKEQ(g[1].v,off,"lseek off=r2:r3 (r1 skipped)"); CKEQ(g[2].v,2,"lseek whence=[SP]");
}

/* glTexImage2D(9 words): r0-r3 then SP+0,+4,+8,+12,+16 (Audit01 D1) */
static void t_teximage(void){
    printf("[glTexImage2D/9]\n");
    reset(0x50008000u);
    uint32_t v[9]={0x3553/*TEXTURE_2D*/,0,0x1908/*RGBA*/,256,256,0,0x1908,0x1401/*UBYTE*/,0x50001000/*pixels*/};
    for(int i=0;i<4;i++) R(i,v[i]);
    for(int i=4;i<9;i++) STK(0x50008000u+(i-4)*4, v[i]);
    mdesc d={.ret=A_VOID,.nargs=9,.args={{A_U32,0,0,0},{A_I32,0,0,0},{A_I32,0,0,0},{A_I32,0,0,0},{A_I32,0,0,0},{A_I32,0,0,0},{A_U32,0,0,0},{A_U32,0,0,0},{A_PTR,D_IN,0,0}}};
    garg g[9]; marshal_read_args(&M,&d,g);
    for(int i=0;i<9;i++) CKEQ(g[i].v,v[i],"teximage arg");
}

/* sprintf(buf, "%d %f", i, d) variadic:
   fixed buf=r0, fmt=r1 (NCRN=2); %d -> r2 (NCRN=3); %f (double) 8-aligns
   NCRN 3->4 -> 8-aligned stack at [SP] (r3 = padding) — Audit01 D5 / worked case */
static void t_sprintf_varargs(void){
    printf("[sprintf(\"%%d %%f\") variadic]\n");
    reset(0x50008000u);
    R(0,0x50002000u);          /* buf */
    R(1,0x50003000u);          /* fmt */
    R(2,42);                   /* the %d int */
    R(3,0xBADBADBA);           /* r3 = padding, must NOT be read as the double */
    uint64_t dv=0x4045000000000000ULL /*42.0*/;
    STK(0x50008000u,(uint32_t)dv); STK(0x50008004u,(uint32_t)(dv>>32));
    /* emulate the printf engine: init cursor, pull the two fixed ptrs, then tail */
    mcur c; marshal_cur_init(&M,&c);
    uint32_t buf=marshal_pull_word(&M,&c);   /* r0 */
    uint32_t fmt=marshal_pull_word(&M,&c);   /* r1 */
    uint32_t di =marshal_pull_word(&M,&c);   /* %d -> r2 */
    uint64_t df =marshal_pull_dword(&M,&c);  /* %f -> 8-align 3->4 -> [SP] */
    CKEQ(buf,0x50002000u,"buf=r0"); CKEQ(fmt,0x50003000u,"fmt=r1");
    CKEQ(di,42,"%d=r2"); CKEQ(df,dv,"%f from 8-aligned stack (r3 padding skipped)");
    CK(c.ncrn==4,"cursor NCRN clamped to 4 after stack spill");
}

/* return-writer scalar coverage */
static void t_ret(void){
    printf("[write_ret]\n");
    reset(0x50008000u);
    R(0,0x11111111);R(1,0x22222222);
    marshal_write_ret(&M,A_VOID,0xDEAD); CKEQ(H.regs[0],0x11111111,"VOID leaves r0"); CKEQ(H.regs[1],0x22222222,"VOID leaves r1");
    marshal_write_ret(&M,A_I32,0xFFFFFFFFABCDEF01ULL); CKEQ(H.regs[0],0xABCDEF01u,"I32 -> r0 low"); CKEQ(H.regs[1],0x22222222,"I32 leaves r1");
    marshal_write_ret(&M,A_F32,0x40490FDBULL); CKEQ(H.regs[0],0x40490FDBu,"F32 bits -> r0");
    marshal_write_ret(&M,A_U64,0x1122334455667788ULL); CKEQ(H.regs[0],0x55667788u,"U64 lo->r0"); CKEQ(H.regs[1],0x11223344u,"U64 hi->r1");
}

/* entry-side placement is the exact inverse of the pull side (thunk marshalling
   matches what the guest import-read expects) — real nativeInput/nativeMixData
   arg shapes */
static void t_place_roundtrip(void){
    printf("[place<->pull round-trip (nativeInput/nativeMixData shapes)]\n");
    /* nativeInput: env, thiz, jint, jfloat, jfloat, jint (VIFFI + env/thiz) */
    reset(0x50008000u);
    mcur p={.ncrn=0,.nsaa=0x50008000u};
    marshal_place_word (&M,&p,0xE0);            /* env  -> r0 */
    marshal_place_word (&M,&p,0x71);            /* thiz -> r1 */
    marshal_place_word (&M,&p,42);              /* int  -> r2 */
    marshal_place_word (&M,&p,0x40490FDB);      /* float-> r3 */
    marshal_place_word (&M,&p,0x3F800000);      /* float-> [SP] */
    marshal_place_word (&M,&p,99);              /* int  -> [SP+4] */
    mcur q={.ncrn=0,.nsaa=0x50008000u};
    CKEQ(marshal_pull_word(&M,&q),0xE0,"rt env"); CKEQ(marshal_pull_word(&M,&q),0x71,"rt thiz");
    CKEQ(marshal_pull_word(&M,&q),42,"rt int"); CKEQ(marshal_pull_word(&M,&q),0x40490FDB,"rt float r3");
    CKEQ(marshal_pull_word(&M,&q),0x3F800000,"rt float stack"); CKEQ(marshal_pull_word(&M,&q),99,"rt int2");

    /* nativeMixData: env, thiz, jlong, jobject, jint (VJLI) — long 8-aligns to r2:r3 */
    reset(0x50008000u);
    mcur p2={.ncrn=0,.nsaa=0x50008000u};
    marshal_place_word (&M,&p2,0xE0);           /* env  -> r0 */
    marshal_place_word (&M,&p2,0x71);           /* thiz -> r1 */
    marshal_place_dword(&M,&p2,0x1122334455667788ULL); /* long -> r2:r3 (even) */
    marshal_place_word (&M,&p2,0x10000001);     /* obj token -> [SP] */
    marshal_place_word (&M,&p2,4096);           /* int -> [SP+4] */
    mcur q2={.ncrn=0,.nsaa=0x50008000u};
    CKEQ(marshal_pull_word(&M,&q2),0xE0,"mix env"); CKEQ(marshal_pull_word(&M,&q2),0x71,"mix thiz");
    CKEQ(marshal_pull_dword(&M,&q2),0x1122334455667788ULL,"mix long r2:r3");
    CKEQ(marshal_pull_word(&M,&q2),0x10000001,"mix obj stack"); CKEQ(marshal_pull_word(&M,&q2),4096,"mix int stack");
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== marshal host test ===\n");
    t_pow(); t_lseek64(); t_teximage(); t_sprintf_varargs(); t_ret(); t_place_roundtrip();
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
