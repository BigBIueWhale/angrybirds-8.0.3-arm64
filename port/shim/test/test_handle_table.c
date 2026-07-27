/* test_handle_table.c — host test for the JNI 32<->64 handle table. */
#include "handle_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

/* fake 64-bit "real" pointers (never dereferenced) */
static void *P(uintptr_t x){ return (void*)x; }

static void t_basic(void){
    printf("[basic refs]\n");
    handle_table*t=ht_create();
    CK(ht_new_ref(t,HK_LOCAL,NULL)==HT_NULL,"local(NULL)->HT_NULL");
    CK(ht_resolve(t,HT_NULL)==NULL,"resolve(0)->NULL");

    uint32_t a=ht_new_ref(t,HK_LOCAL,P(0x1000));
    uint32_t b=ht_new_ref(t,HK_GLOBAL,P(0x2000));
    uint32_t c=ht_new_ref(t,HK_WEAK,P(0x3000));
    CK(a&&b&&c,"tokens nonzero");
    CK(ht_kind(a)==HK_LOCAL && ht_kind(b)==HK_GLOBAL && ht_kind(c)==HK_WEAK,"kind bits");
    CK(ht_resolve(t,a)==P(0x1000)&&ht_resolve(t,b)==P(0x2000)&&ht_resolve(t,c)==P(0x3000),"resolve");

    /* no dedup for object refs: same real -> distinct tokens, both resolve */
    uint32_t a2=ht_new_ref(t,HK_LOCAL,P(0x1000));
    CK(a2!=a,"same real -> distinct token (no object dedup)");
    CK(ht_resolve(t,a2)==P(0x1000),"a2 resolves");

    /* delete a global -> stale */
    CK(ht_delete_ref(t,b)==1,"delete global");
    CK(ht_resolve(t,b)==NULL,"deleted -> NULL");
    CK(ht_delete_ref(t,b)==0,"double-delete -> 0");
    ht_destroy(t);
}

static void t_ids(void){
    printf("[id dedup]\n");
    handle_table*t=ht_create();
    const char*dA="(I)V", *dB="(Ljava/lang/String;)Z";
    uint32_t i1=ht_intern_id(t,P(0xAAAA),dA);
    uint32_t i2=ht_intern_id(t,P(0xAAAA),dA);   /* same real -> same token */
    uint32_t j =ht_intern_id(t,P(0xBBBB),dB);
    CK(ht_kind(i1)==HK_ID,"id kind");
    CK(i1==i2,"id dedup: same real -> same token");
    CK(j!=i1,"different real -> different token");
    CK(ht_id_desc(t,i1)==dA && ht_id_desc(t,j)==dB,"id descriptors stored");
    CK(ht_id_desc(t,i1) != dB, "descriptors distinct");
    /* IDs are not deletable as refs */
    CK(ht_delete_ref(t,i1)==0,"id not deletable via delete_ref");
    CK(ht_resolve(t,i1)==P(0xAAAA),"id resolves like a ref");
    ht_destroy(t);
}

static void t_frames(void){
    printf("[local frames]\n");
    handle_table*t=ht_create();
    uint32_t g=ht_new_ref(t,HK_GLOBAL,P(0x9000));       /* survives frame pops */
    uint32_t mark=ht_frame_mark(t);
    uint32_t l[100];
    for(int i=0;i<100;i++) l[i]=ht_new_ref(t,HK_LOCAL,P(0x10000+i));
    CK(ht_live(t,HK_LOCAL)==100,"100 locals live");
    /* explicitly delete a few before pop (must be tolerated) */
    ht_delete_ref(t,l[10]); ht_delete_ref(t,l[20]);
    CK(ht_live(t,HK_LOCAL)==98,"98 after 2 explicit deletes");
    ht_frame_pop(t,mark);
    CK(ht_live(t,HK_LOCAL)==0,"frame pop frees all locals");
    for(int i=0;i<100;i++) CK(ht_resolve(t,l[i])==NULL,"popped local -> NULL");
    CK(ht_resolve(t,g)==P(0x9000),"global survives frame pop");
    CK(ht_live(t,HK_GLOBAL)==1,"1 global live");
    ht_destroy(t);
}

/* index reuse must not resurrect a stale token's data */
static void t_reuse(void){
    printf("[index reuse]\n");
    handle_table*t=ht_create();
    uint32_t a=ht_new_ref(t,HK_LOCAL,P(0x1111));
    ht_delete_ref(t,a);
    uint32_t b=ht_new_ref(t,HK_LOCAL,P(0x2222));   /* reuses index -> same token value */
    CK(b==a,"index reused (token value recycled, fd-style)");
    CK(ht_resolve(t,b)==P(0x2222),"reused token resolves to NEW real");
    ht_destroy(t);
}

/* torture: interleave new/delete/intern; verify resolve integrity + no leak */
static void t_torture(void){
    printf("[torture]\n");
    handle_table*t=ht_create();
    enum{N=4096};
    uint32_t tok[N]; void* real[N]; int live[N]; memset(live,0,sizeof live);
    uint32_t rng=0x2468ace0u;
    #define RND (rng^=rng<<13,rng^=rng>>17,rng^=rng<<5,rng)
    for(int it=0;it<300000;it++){
        int i=RND%N;
        if(!live[i]){
            void*r=P(0x40000000u + (uintptr_t)(RND|1));   /* nonzero fake ptr */
            int kind=HK_LOCAL+(RND%3);
            tok[i]=ht_new_ref(t,kind,r); real[i]=r; live[i]=1;
            if(ht_resolve(t,tok[i])!=r){ printf("  FAIL torture resolve it=%d\n",it); fails++; break; }
        } else {
            if(ht_resolve(t,tok[i])!=real[i]){ printf("  FAIL torture stale it=%d\n",it); fails++; break; }
            ht_delete_ref(t,tok[i]); live[i]=0;
        }
    }
    /* free the rest */
    for(int i=0;i<N;i++) if(live[i]) ht_delete_ref(t,tok[i]);
    CK(ht_live(t,HK_LOCAL)+ht_live(t,HK_GLOBAL)+ht_live(t,HK_WEAK)==0,"torture: all refs freed");
    ht_destroy(t);
    #undef RND
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== handle_table host test ===\n");
    t_basic(); t_ids(); t_frames(); t_reuse(); t_torture();
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
