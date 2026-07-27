/* test_format.c — differential test for the printf engine.
 * Lays out varargs in guest regs/stack per the soft-float AAPCS32 (mirroring the
 * marshal pull), runs our formatter, and compares to the host snprintf with the
 * SAME values. Since our engine delegates each conversion to host libc, an exact
 * match validates the ARG EXTRACTION + ordering + 8-alignment (the soft-float
 * crux) and the snprintf truncation/return semantics. Build via run_tests.sh.
 */
#include "format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *buf; uint32_t base, size; uint32_t regs[16]; } hostmem;
static hostmem H; static guest_mem M;
static void hm_read(guest_mem*m,void*d,uint32_t a,uint32_t n){ hostmem*h=m->ctx; if(a<h->base||(uint64_t)a+n>(uint64_t)h->base+h->size){fprintf(stderr,"OOB rd 0x%08x+%u\n",a,n);abort();} memcpy(d,h->buf+(a-h->base),n); }
static void hm_write(guest_mem*m,uint32_t a,const void*s,uint32_t n){ hostmem*h=m->ctx; if(a<h->base||(uint64_t)a+n>(uint64_t)h->base+h->size){fprintf(stderr,"OOB wr 0x%08x+%u\n",a,n);abort();} memcpy(h->buf+(a-h->base),s,n); }
static uint32_t hm_rg(guest_mem*m,int i){ return ((hostmem*)m->ctx)->regs[i]; }
static void     hm_rs(guest_mem*m,int i,uint32_t v){ ((hostmem*)m->ctx)->regs[i]=v; }

#define BUF   0x50001000u
#define FMTA  0x50002000u
#define SPTOP 0x50008000u
static uint32_t strscr, VAP;
static mcur PC;

static void reset(void){
    if(!H.buf){ H.buf=calloc(1,0x10000); H.base=0x50000000u; H.size=0x10000u;
                M.read=hm_read;M.write=hm_write;M.reg_get=hm_rg;M.reg_set=hm_rs;M.ctx=&H; }
    memset(H.buf,0,0x10000); memset(H.regs,0,sizeof H.regs);
    H.regs[13]=SPTOP; H.regs[0]=BUF; H.regs[1]=FMTA;
    PC.ncrn=2; PC.nsaa=SPTOP;       /* varargs start after buf(r0),fmt(r1) */
    strscr=0x50003000u; VAP=0x50005000u;
}
static void put_fmt(const char*s){ for(uint32_t i=0;;i++){ gm_wr8(&M,FMTA+i,(uint8_t)s[i]); if(!s[i])break; } }
static uint32_t put_str(const char*s){ uint32_t a=strscr; uint32_t i=0; for(;;i++){ gm_wr8(&M,a+i,(uint8_t)s[i]); if(!s[i])break; } strscr=a+i+1; return a; }
/* register/stack placement (mirrors marshal_pull_word/dword) */
static void put_w(uint32_t v){ if(PC.ncrn<4)H.regs[PC.ncrn++]=v; else { gm_wr32(&M,PC.nsaa,v); PC.nsaa+=4; } }
static void put_d(uint64_t v){ if(PC.ncrn&1)PC.ncrn++; if(PC.ncrn<=2){H.regs[PC.ncrn]=(uint32_t)v;H.regs[PC.ncrn+1]=(uint32_t)(v>>32);PC.ncrn+=2;} else {PC.ncrn=4;PC.nsaa=(PC.nsaa+7)&~7u;gm_wr32(&M,PC.nsaa,(uint32_t)v);gm_wr32(&M,PC.nsaa+4,(uint32_t)(v>>32));PC.nsaa+=8;} }
static void put_dbl(double d){ uint64_t b; memcpy(&b,&d,8); put_d(b); }
/* va_list placement (mirrors va_word/va_dword) */
static void vput_w(uint32_t v){ gm_wr32(&M,VAP,v); VAP+=4; }
static void vput_d(uint64_t v){ VAP=(VAP+7)&~7u; gm_wr32(&M,VAP,(uint32_t)v); gm_wr32(&M,VAP+4,(uint32_t)(v>>32)); VAP+=8; }
static void vput_dbl(double d){ uint64_t b; memcpy(&b,&d,8); vput_d(b); }

static int fails=0;
static void gbuf(char*out,uint32_t n){ uint32_t i=0; for(;i<n-1;i++){ out[i]=(char)gm_rd8(&M,BUF+i); if(!out[i])break; } out[i]=0; }
static void cmp(const char*fmt,const char*ref,uint32_t ret){
    char got[600]; gbuf(got,sizeof got);
    if(strcmp(got,ref)){ printf("  FAIL \"%s\": got=\"%s\" ref=\"%s\"\n",fmt,got,ref); fails++; }
    if(ret!=(uint32_t)strlen(ref)){ printf("  FAIL \"%s\": ret=%u want=%zu\n",fmt,ret,strlen(ref)); fails++; }
}
/* run source A (sprintf semantics, unbounded) */
static uint32_t runA(void){ mcur c={.ncrn=2,.nsaa=SPTOP}; return fmt_to_guest(&M,BUF,0xFFFFFFFFu,FMTA,c); }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== format host test ===\n");
    char ref[600];

    /* --- scalar conversions --- */
    reset(); put_fmt("%d");                 put_w((uint32_t)-42);   snprintf(ref,sizeof ref,"%d",-42);                 cmp("%d",ref,runA());
    reset(); put_fmt("%u");                 put_w(3000000000u);     snprintf(ref,sizeof ref,"%u",3000000000u);         cmp("%u",ref,runA());
    reset(); put_fmt("%x|%X|%#x");          put_w(0xabcd);put_w(0xABCD);put_w(0x1f); snprintf(ref,sizeof ref,"%x|%X|%#x",0xabcd,0xABCD,0x1f); cmp("%x|%X|%#x",ref,runA());
    reset(); put_fmt("%o");                 put_w(0511);            snprintf(ref,sizeof ref,"%o",0511);                cmp("%o",ref,runA());
    reset(); put_fmt("%c%c");               put_w('Q');put_w('!');  snprintf(ref,sizeof ref,"%c%c",'Q','!');           cmp("%c%c",ref,runA());
    reset(); put_fmt("[%s]");               put_w(put_str("hello"));snprintf(ref,sizeof ref,"[%s]","hello");           cmp("[%s]",ref,runA());

    /* --- width / precision / flags --- */
    reset(); put_fmt("%5d|%-5d|%05d|%+d");  put_w(42);put_w(42);put_w(42);put_w(42); snprintf(ref,sizeof ref,"%5d|%-5d|%05d|%+d",42,42,42,42); cmp("flags",ref,runA());
    reset(); put_fmt("%.3s|%8.3s");         { uint32_t a=put_str("abcdef"); put_w(a); put_w(a);} snprintf(ref,sizeof ref,"%.3s|%8.3s","abcdef","abcdef"); cmp("str prec",ref,runA());

    /* --- floating point (soft-float: double pulled from core-reg/stack) --- */
    reset(); put_fmt("%.3f");               put_dbl(3.14159);       snprintf(ref,sizeof ref,"%.3f",3.14159);           cmp("%.3f",ref,runA());
    reset(); put_fmt("%8.2f|%-8.2f|%+.1f"); put_dbl(3.14159);put_dbl(3.14159);put_dbl(-2.5); snprintf(ref,sizeof ref,"%8.2f|%-8.2f|%+.1f",3.14159,3.14159,-2.5); cmp("float flags",ref,runA());
    reset(); put_fmt("%e|%g|%G");           put_dbl(12345.678);put_dbl(0.0001234);put_dbl(1e20); snprintf(ref,sizeof ref,"%e|%g|%G",12345.678,0.0001234,1e20); cmp("%e%g",ref,runA());

    /* --- 64-bit --- */
    reset(); put_fmt("%lld|%llx|%llu");     put_d((uint64_t)-1LL);put_d(0xdeadbeefcafeULL);put_d(0xffffffffffffffffULL);
             snprintf(ref,sizeof ref,"%lld|%llx|%llu",-1LL,0xdeadbeefcafeULL,0xffffffffffffffffULL); cmp("64-bit",ref,runA());

    /* --- pointer --- */
    reset(); put_fmt("%p");                 put_w(0x50001000u);     snprintf(ref,sizeof ref,"%p",(void*)(uintptr_t)0x50001000u); cmp("%p",ref,runA());

    /* --- 8-align interplay: int, double, int (the double must 8-align) --- */
    reset(); put_fmt("%d %f %d");           put_w(7);put_dbl(2.5);put_w(9); snprintf(ref,sizeof ref,"%d %f %d",7,2.5,9); cmp("align",ref,runA());

    /* --- '*' width and precision (pull an int arg each) --- */
    reset(); put_fmt("%*d|%.*f");           put_w(6);put_w(42);put_w(3);put_dbl(3.14159); snprintf(ref,sizeof ref,"%*d|%.*f",6,42,3,3.14159); cmp("star",ref,runA());

    /* --- literal %% and mixed --- */
    reset(); put_fmt("100%% done: %d/%d");  put_w(3);put_w(10);     snprintf(ref,sizeof ref,"100%% done: %d/%d",3,10); cmp("percent",ref,runA());

    /* --- snprintf truncation + return value --- */
    { reset(); put_fmt("%s"); put_w(put_str("abcdefghij"));
      mcur c={.ncrn=2,.nsaa=SPTOP}; uint32_t ret=fmt_to_guest(&M,BUF,8,FMTA,c);   /* cap=8 -> 7 chars + NUL */
      char got[64]; gbuf(got,sizeof got);
      if(strcmp(got,"abcdefg")){ printf("  FAIL trunc: got=\"%s\" want=\"abcdefg\"\n",got); fails++; }
      if(ret!=10){ printf("  FAIL trunc ret=%u want=10\n",ret); fails++; }
      printf("[snprintf truncation] got=\"%s\" ret=%u\n",got,ret); }

    /* --- va_list variant (source B) --- */
    { reset(); put_fmt("%d %s %.2f %llx");
      uint32_t va=VAP; vput_w(77); vput_w(put_str("va!")); vput_dbl(6.25); vput_d(0x1234ABCDULL);
      uint32_t ret=fmt_to_guest_va(&M,BUF,0xFFFFFFFFu,FMTA,va);
      snprintf(ref,sizeof ref,"%d %s %.2f %llx",77,"va!",6.25,0x1234ABCDULL); cmp("va_list",ref,ret);
      printf("[va_list source B] ok\n"); }

    /* --- host sink (printf path) --- */
    { reset(); put_fmt("hi %d=%s"); put_w(5); put_w(put_str("five"));
      char hb[128]; uint32_t hl=0; mcur c={.ncrn=2,.nsaa=SPTOP};
      uint32_t ret=fmt_to_host(&M,hb,sizeof hb,FMTA,c,&hl);
      snprintf(ref,sizeof ref,"hi %d=%s",5,"five");
      if(strcmp(hb,ref)||hl!=strlen(ref)||ret!=strlen(ref)){ printf("  FAIL host sink: hb=\"%s\" ref=\"%s\"\n",hb,ref); fails++; }
      printf("[host sink] \"%s\"\n",hb); }

    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
