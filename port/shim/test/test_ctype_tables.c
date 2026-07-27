/* test_ctype_tables.c — validate the GUESTDATA mirrors against host libc. */
#include "ctype_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

/* flat guest_mem backend */
typedef struct { uint8_t*buf; uint32_t base,size; } hm;
static void r(guest_mem*m,void*d,uint32_t a,uint32_t n){ hm*h=m->ctx; memcpy(d,h->buf+(a-h->base),n); }
static void w(guest_mem*m,uint32_t a,const void*s,uint32_t n){ hm*h=m->ctx; memcpy(h->buf+(a-h->base),s,n); }
static uint32_t rg(guest_mem*m,int i){(void)m;(void)i;return 0;}
static void sg(guest_mem*m,int i,uint32_t v){(void)m;(void)i;(void)v;}

static int16_t rd16(const uint8_t*p){ return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t rd32(const uint8_t*p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    setlocale(LC_ALL,"C");
    printf("=== ctype_tables host test ===\n");

    uint32_t sz,off;
    const uint8_t *ct = ctype_mirror("_ctype_",&sz,&off);
    CK(ct && sz==257 && off==0,"_ctype_ 257 bytes, sym_off 0");

    /* differential vs host libc for the C locale, all bytes */
    printf("[_ctype_ vs host libc]\n");
    for (int c=0;c<=255;c++){
        uint8_t b = ct[c+1];
        CK(!!(b&_CT_S)==!!isspace(c),"isspace");
        CK(!!(b&_CT_N)==!!isdigit(c),"isdigit");
        CK(!!(b&_CT_U)==!!isupper(c),"isupper");
        CK(!!(b&_CT_L)==!!islower(c),"islower");
        CK(!!(b&_CT_C)==!!iscntrl(c),"iscntrl");
        CK(!!(b&_CT_P)==!!ispunct(c),"ispunct");
        CK(!!(b&(_CT_N|_CT_X))==!!isxdigit(c),"isxdigit");
        CK(!!(b&(_CT_U|_CT_L))==!!isalpha(c),"isalpha");
        CK(!!(b&(_CT_U|_CT_L|_CT_N))==!!isalnum(c),"isalnum");
        CK(!!(b&(_CT_P|_CT_U|_CT_L|_CT_N))==!!isgraph(c),"isgraph");
        CK(!!(b&(_CT_P|_CT_U|_CT_L|_CT_N|_CT_B))==!!isprint(c),"isprint");
        if (fails) { printf("  (first mismatch at c=%d '%c')\n",c, (c>=32&&c<127)?c:'.'); break; }
    }
    CK(ct[0]==0,"_ctype_[0] EOF slot = 0");

    /* case-mapping tables */
    printf("[tolower/toupper]\n");
    const uint8_t *tl = ctype_mirror("_tolower_tab_",&sz,&off);
    /* sym_off 0: the engine accesses element c+1 as `ldrh [ptr + c*2 + 2]` (disasm @engine 0x7253b4),
     * so the pointer variable resolves to &T[0] (the base) and the engine adds the +2 itself. */
    CK(sz==2*257 && off==0,"_tolower_tab_ 257 shorts, sym_off 0 (&T[0]; engine adds +2)");
    CK(rd16(tl+0)==-1,"tolower[0] EOF -> -1");
    const uint8_t *tu = ctype_mirror("_toupper_tab_",&sz,&off);
    CK(sz==2*257 && off==0,"_toupper_tab_ shape");
    for (int c=0;c<=255;c++){
        CK(rd16(tl+2*(c+1))==(int16_t)tolower(c),"tolower[c+1]");
        CK(rd16(tu+2*(c+1))==(int16_t)toupper(c),"toupper[c+1]");
        if (fails){ printf("  (mismatch at c=%d)\n",c); break; }
    }

    /* __sF: _file @0xe within each 84-byte struct */
    printf("[__sF]\n");
    const uint8_t *sf = ctype_mirror("__sF",&sz,&off);
    CK(sz==3*84 && off==0,"__sF 3x84, sym_off 0");
    for (int i=0;i<3;i++) CK(rd16(sf+i*84+0x0e)==i,"__sF[i]._file @0xe == i");
    CK(rd16(sf+0*84+0x0c)==0x0004,"stdin _flags __SRD");
    CK(rd16(sf+1*84+0x0c)==0x0008,"stdout _flags __SWR");

    /* canary: nonzero with a NUL byte */
    printf("[__stack_chk_guard]\n");
    const uint8_t *g = ctype_mirror("__stack_chk_guard",&sz,&off);
    CK(sz==4 && off==0,"guard 4 bytes");
    uint32_t gv=(uint32_t)g[0]|((uint32_t)g[1]<<8)|((uint32_t)g[2]<<16)|((uint32_t)g[3]<<24);
    CK(gv!=0,"guard nonzero");
    CK(g[0]==0||g[1]==0||g[2]==0||g[3]==0,"guard has a NUL byte");

    /* unknown name */
    CK(ctype_mirror("nope",&sz,&off)==NULL,"unknown -> NULL");

    /* write to guest memory + symbol resolution offset */
    printf("[write to guest mem]\n");
    hm h; h.buf=calloc(1,0x2000); h.base=0x12000000u; h.size=0x2000u;
    guest_mem m={r,w,rg,sg,&h};
    /* pointer-variable symbols: ctype_write_mirror writes the table at base and a
     * 4-byte pointer word past it; the symbol resolves to that pointer word, and the
     * pointer word holds &table[sym_off]. The engine does ldr ptr;[ptr+idx]. */
    uint32_t cons=0;
    uint32_t sym = ctype_write_mirror(&m,0x12000000u,"_tolower_tab_",&cons);
    CK(sym>=0x12000000u+514u && cons>=514u+4u,"tolower symbol is a ptr word past the 514-byte table");
    CK(rd32(h.buf + (sym-0x12000000u))==0x12000000u,"tolower ptr word holds base+0 (&T[0]; engine adds +2)");
    CK(rd16(h.buf + 2*(('A')+1))==(int16_t)'a',"written tolower['A'+1]=='a'");
    uint32_t sym2 = ctype_write_mirror(&m,0x12001000u,"_ctype_",&cons);
    CK(sym2>=0x12001000u+257u && cons>=257u+4u,"_ctype_ symbol is a ptr word past the 257-byte table");
    CK(rd32(h.buf + (sym2-0x12000000u))==0x12001000u,"_ctype_ ptr word holds base (&T[0])");
    CK((h.buf[0x1000 + '0'+1] & _CT_N)!=0,"written _ctype_['0'+1] has _N");
    free(h.buf);

    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
