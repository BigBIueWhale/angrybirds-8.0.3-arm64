/* test_fdtable.c — host test for the fd table + sandbox path router.
 *
 * READ THIS BEFORE COUNTING THESE PASSES AS COVERAGE. The module under test is NOT wired into the
 * shipped shim. Every exported function of fdtable.c — fdt_create/destroy/alloc/get/free/live,
 * fd_route, fd_sandbox_resolve — has zero callers anywhere in port/shim/src outside the module
 * itself. (Contrast handle_table.c, the live JNI handle table, called throughout: ht_new_ref 14
 * times, ht_resolve 10.) Production file I/O is bridge_file.c's f_fopen/f_open, which hand the
 * guest's path to the host verbatim, with no routing, no sandbox root and no /proc special-casing.
 *
 * So the containment cases below do NOT establish that the shim contains a path escape. This header
 * used to read "the containment cases are security-critical: a guest path must NEVER resolve to a
 * host path outside the sandbox root", which states a property of the ARTIFACT that the artifact
 * does not have. What actually confines the app is Android's own UID sandbox, and the engine is
 * Rovio's own code rather than adversarial input — see port/OPEN_FINDINGS.md R8.
 *
 * The tests are kept: the module is still compiled into the shim (it is in MODS), and if it is ever
 * wired up these are the cases that would have to hold. They are labelled — in the OUTPUT, not just
 * here — because a green check for code the product never calls is assurance about nothing, and the
 * output is what someone reads when they count passes. */
#include "fdtable.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

static void t_fdtable(void){
    printf("[fd table]  (NOTE: fdtable.c is not wired into the shipped shim — no caller in\n");
    printf("             port/shim/src; production I/O is bridge_file.c. These passes are not\n");
    printf("             coverage of the shim's real file path. See OPEN_FINDINGS.md R8.)\n");
    fdtable*t=fdt_create();
    CK(fdt_live(t)==0,"start empty");
    int a=fdt_alloc(t,FDK_FILE,(void*)0x1000,7);
    int b=fdt_alloc(t,FDK_URANDOM,NULL,0);
    CK(a==3 && b==4,"fds start at 3, increment");
    void*res; int aux;
    CK(fdt_get(t,a,&res,&aux)==FDK_FILE && res==(void*)0x1000 && aux==7,"get file slot");
    CK(fdt_get(t,b,NULL,NULL)==FDK_URANDOM,"get urandom slot");
    CK(fdt_get(t,99,NULL,NULL)==FDK_FREE,"get unknown -> FREE");
    CK(fdt_free(t,0)==0 && fdt_free(t,2)==0,"cannot free reserved 0/1/2");
    CK(fdt_free(t,a)==1,"free file");
    CK(fdt_get(t,a,NULL,NULL)==FDK_FREE,"freed -> FREE");
    int c=fdt_alloc(t,FDK_PROC,NULL,PROC_CPUINFO);
    CK(c==3,"freed fd reused");
    CK(fdt_live(t)==2,"live count");
    fdt_destroy(t);
}

static void rescheck(const char*path,const char*want){
    char out[1024];
    int rc=fd_sandbox_resolve("/sb",path,out,sizeof out);
    CK(rc==0,"resolve ok");
    if(rc==0 && strcmp(out,want)){ printf("  FAIL resolve(\"%s\"): got \"%s\" want \"%s\"\n",path,out,want); fails++; }
    /* containment invariant: result always under root */
    if(rc==0){ CK(!strncmp(out,"/sb",3) && (out[3]=='/'||out[3]==0),"stays under /sb"); }
}
static void t_containment(void){
    /* "must be neutralised" was a claim about the shim. It is a claim about fd_route(), which the
     * shim does not call — so the wording says whose property it is. */
    printf("[sandbox containment — fd_route() neutralises escapes; UNUSED by the shim]\n");
    rescheck("a/b/c",              "/sb/a/b/c");
    rescheck("a/../b",             "/sb/b");
    rescheck("./x/./y",            "/sb/x/y");
    rescheck("a/b/../../c",        "/sb/c");
    rescheck("",                   "/sb");
    rescheck(".",                  "/sb");
    rescheck("..",                 "/sb");               /* cannot rise above root */
    rescheck("../../../etc/passwd","/sb/etc/passwd");    /* escape neutralised */
    rescheck("/etc/passwd",        "/sb/etc/passwd");    /* absolute treated relative */
    rescheck("a/b/../../../../c",  "/sb/c");             /* surplus .. ignored */
    rescheck("foo//bar///baz",     "/sb/foo/bar/baz");   /* empty components dropped */
    rescheck("save/game.dat",      "/sb/save/game.dat");
}

static void t_router(void){
    printf("[path router]\n");
    fd_route_t r;
    CK(fd_route("/sb","assets/","/dev/urandom",&r)==0 && r.kind==ROUTE_URANDOM,"/dev/urandom");
    CK(fd_route("/sb","assets/","/proc/cpuinfo",&r)==0 && r.kind==ROUTE_PROC && r.proc_id==PROC_CPUINFO,"/proc/cpuinfo");
    CK(fd_route("/sb","assets/","/proc/meminfo",&r)==0 && r.proc_id==PROC_MEMINFO,"/proc/meminfo");
    CK(fd_route("/sb","assets/","/proc/self/auxv",&r)==0 && r.proc_id==PROC_AUXV,"/proc/self/auxv");
    CK(fd_route("/sb","assets/","assets/levels/1.lua",&r)==0 && r.kind==ROUTE_ASSET && !strcmp(r.asset_rel,"levels/1.lua"),"asset prefix");
    CK(fd_route("/sb","assets/","assetsfoo/x",&r)==0 && r.kind==ROUTE_SANDBOX,"false asset prefix -> sandbox");
    CK(fd_route("/sb","assets/","save/hi.dat",&r)==0 && r.kind==ROUTE_SANDBOX && !strcmp(r.host_path,"/sb/save/hi.dat"),"sandbox file");
    /* an escape attempt still lands in the sandbox */
    CK(fd_route("/sb","assets/","../../secret",&r)==0 && r.kind==ROUTE_SANDBOX && !strncmp(r.host_path,"/sb/",4),"escape -> contained sandbox");
    CK(fd_route("/sb","assets/","",&r)==-1 && r.kind==ROUTE_DENY,"empty path -> deny");
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== fdtable host test ===\n");
    t_fdtable(); t_containment(); t_router();
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
