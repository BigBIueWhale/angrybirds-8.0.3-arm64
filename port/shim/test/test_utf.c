/* test_utf.c — host test for the UTF-8 / modified-UTF-8 codecs. */
#include "utf.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

static void t_utf8_roundtrip(void){
    printf("[utf8 round-trip all code points]\n");
    long n=0;
    for (uint32_t cp=0; cp<=0x10FFFF; cp++){
        if (cp>=0xD800 && cp<=0xDFFF){                 /* surrogates are not scalar values */
            uint8_t o[4]; CK(utf8_encode(cp,o)==-1,"encode rejects surrogate"); continue;
        }
        uint8_t o[4]; int k=utf8_encode(cp,o);
        if (k<1){ printf("  FAIL: encode(U+%X)=%d\n",cp,k); fails++; break; }
        uint32_t dc; int dk=utf8_decode(o,(size_t)k,&dc);
        if (dk!=k || dc!=cp){ printf("  FAIL: roundtrip U+%X k=%d dk=%d dc=%X\n",cp,k,dk,dc); fails++; break; }
        n++;
    }
    printf("  %ld code points ok\n",n);
    CK(utf8_encode(0x110000,(uint8_t[4]){0})==-1,"encode rejects > U+10FFFF");
}

static void t_utf8_malformed(void){
    printf("[utf8 malformed rejection]\n");
    uint32_t cp;
    CK(utf8_decode((uint8_t[]){0xC0,0x80},2,&cp)==-1,"overlong C0 80");
    CK(utf8_decode((uint8_t[]){0xE0,0x80,0x80},3,&cp)==-1,"overlong E0 80 80");
    CK(utf8_decode((uint8_t[]){0xF0,0x80,0x80,0x80},4,&cp)==-1,"overlong 4-byte");
    CK(utf8_decode((uint8_t[]){0xED,0xA0,0x80},3,&cp)==-1,"surrogate ED A0 80");
    CK(utf8_decode((uint8_t[]){0xF4,0x90,0x80,0x80},4,&cp)==-1,"> U+10FFFF");
    CK(utf8_decode((uint8_t[]){0x80},1,&cp)==-1,"lone continuation");
    CK(utf8_decode((uint8_t[]){0xF8,0x88,0x80,0x80},4,&cp)==-1,"5-byte lead");
    CK(utf8_decode((uint8_t[]){0xE2,0x82},2,&cp)==-1,"truncated 3-byte");
    /* valid boundary cases */
    CK(utf8_decode((uint8_t[]){0x24},1,&cp)==1 && cp==0x24,"'$'");
    CK(utf8_decode((uint8_t[]){0xC2,0xA2},2,&cp)==2 && cp==0xA2,"U+00A2");
    CK(utf8_decode((uint8_t[]){0xE2,0x82,0xAC},3,&cp)==3 && cp==0x20AC,"U+20AC euro");
    CK(utf8_decode((uint8_t[]){0xF0,0x90,0x8D,0x88},4,&cp)==4 && cp==0x10348,"U+10348");
}

static void t_mutf8_units(void){
    printf("[modified-UTF-8 units]\n");
    uint8_t o[3]; uint32_t u;
    /* U+0000 -> C0 80 (never a real NUL) */
    CK(mutf8_encode_unit(0,o)==2 && o[0]==0xC0 && o[1]==0x80,"encode 0 -> C0 80");
    CK(mutf8_decode_unit((uint8_t[]){0xC0,0x80},2,&u)==2 && u==0,"decode C0 80 -> 0");
    CK(mutf8_decode_unit((uint8_t[]){0x00},1,&u)==-1,"raw 0 is not a valid lead");
    /* ASCII */
    CK(mutf8_encode_unit('A',o)==1 && o[0]=='A',"encode 'A'");
    /* 2-byte */
    CK(mutf8_encode_unit(0x7FF,o)==2,"encode U+07FF -> 2 bytes");
    /* 3-byte incl a surrogate half (mUTF-8 encodes each half separately) */
    { int k=mutf8_encode_unit(0xD800,o); CK(k==3,"encode surrogate half -> 3 bytes");
      uint32_t d; CK(mutf8_decode_unit(o,3,&d)==3 && d==0xD800,"decode surrogate half"); }
    CK(mutf8_decode_unit((uint8_t[]){0xF0,0x90,0x80,0x80},4,&u)==-1,"no 4-byte mUTF-8 form");
    /* round-trip all 16-bit units */
    long n=0; for (uint32_t v=0; v<=0xFFFF; v++){ uint8_t b[3]; int k=mutf8_encode_unit(v,b); uint32_t d; int dk=mutf8_decode_unit(b,(size_t)k,&d);
        if (dk!=k || d!=v){ printf("  FAIL mUTF-8 unit U+%X\n",v); fails++; break; } n++; }
    printf("  %ld 16-bit units round-trip ok\n",n);
}

static void t_mutf8_validate(void){
    printf("[modified-UTF-8 validate]\n");
    /* "A" + U+0000 + euro(U+20AC) as mUTF-8, NUL-terminated */
    uint8_t s[]={ 'A', 0xC0,0x80, 0xE2,0x82,0xAC, 0x00 };
    CK(mutf8_validate(s,(size_t)-1)==3,"validate 3 units (A, NUL, euro)");
    CK(mutf8_validate(s,6)==3,"validate length-bounded");
    uint8_t bad[]={ 'A', 0xE2,0x82, 0x00 };  /* truncated 3-byte before NUL */
    CK(mutf8_validate(bad,(size_t)-1)==-1,"reject malformed");
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== utf host test ===\n");
    t_utf8_roundtrip(); t_utf8_malformed(); t_mutf8_units(); t_mutf8_validate();
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
