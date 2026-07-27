/* fdtable.c — fd/FILE model + sandbox path router. See fdtable.h. */
#include "fdtable.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { int kind; void *res; int aux; } fdslot;
struct fdtable { fdslot *v; int cap, n; };   /* index 0..2 reserved for stdio */

fdtable *fdt_create(void){
    fdtable *t = (fdtable*)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cap = 8; t->n = 3;                      /* fds 0,1,2 reserved */
    t->v = (fdslot*)calloc(t->cap, sizeof(fdslot));
    return t;
}
void fdt_destroy(fdtable *t){ if (t){ free(t->v); free(t); } }

int fdt_alloc(fdtable *t, int kind, void *res, int aux){
    int fd = -1;
    for (int i = 3; i < t->n; i++) if (t->v[i].kind == FDK_FREE){ fd = i; break; }
    if (fd < 0){
        if (t->n >= t->cap){ int nc = t->cap*2; fdslot *nv = (fdslot*)realloc(t->v, nc*sizeof(fdslot));
            if (!nv) return -1; memset(nv+t->cap, 0, (nc-t->cap)*sizeof(fdslot)); t->v=nv; t->cap=nc; }
        fd = t->n++;
    }
    t->v[fd].kind = kind; t->v[fd].res = res; t->v[fd].aux = aux;
    return fd;
}
int fdt_get(fdtable *t, int fd, void **res, int *aux){
    if (fd < 0 || fd >= t->n || t->v[fd].kind == FDK_FREE) return FDK_FREE;
    if (res) *res = t->v[fd].res;
    if (aux) *aux = t->v[fd].aux;
    return t->v[fd].kind;
}
int fdt_free(fdtable *t, int fd){
    if (fd < 3 || fd >= t->n || t->v[fd].kind == FDK_FREE) return 0;
    t->v[fd].kind = FDK_FREE; t->v[fd].res = NULL; t->v[fd].aux = 0;
    return 1;
}
uint32_t fdt_live(fdtable *t){ uint32_t c=0; for (int i=3;i<t->n;i++) if (t->v[i].kind!=FDK_FREE) c++; return c; }

/* Canonicalise into a component stack; ".." never rises above root. */
int fd_sandbox_resolve(const char *root, const char *path, char *out, size_t outsz){
    const char *comp[256]; int clen[256]; int depth = 0;
    const char *p = path;
    while (*p){
        while (*p == '/') p++;                 /* skip separators */
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);
        if (len == 1 && start[0] == '.') continue;                 /* "." */
        if (len == 2 && start[0] == '.' && start[1] == '.'){       /* ".." */
            if (depth > 0) depth--;            /* pop, but never above root */
            continue;
        }
        if (depth < 256){ comp[depth] = start; clen[depth] = len; depth++; }
    }
    /* assemble root + "/" + components */
    size_t rl = strlen(root);
    if (rl + 1 >= outsz) return -1;
    memcpy(out, root, rl); size_t o = rl;
    for (int i = 0; i < depth; i++){
        if (o + 1 + (size_t)clen[i] + 1 >= outsz) return -1;
        out[o++] = '/';
        memcpy(out + o, comp[i], clen[i]); o += clen[i];
    }
    out[o] = 0;
    return 0;
}

static int proc_match(const char *path){
    if (!strcmp(path,"/proc/cpuinfo"))       return PROC_CPUINFO;
    if (!strcmp(path,"/proc/meminfo"))       return PROC_MEMINFO;
    if (!strcmp(path,"/proc/self/auxv"))     return PROC_AUXV;
    if (!strcmp(path,"/proc/socinfo") ||
        !strcmp(path,"/proc/cpu/alignment")) return PROC_SOCINFO;
    return PROC_NONE;
}

int fd_route(const char *sandbox_root, const char *asset_prefix, const char *path, fd_route_t *out){
    memset(out, 0, sizeof *out);
    if (!path || !*path){ out->kind = ROUTE_DENY; return -1; }

    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")){ out->kind = ROUTE_URANDOM; return 0; }
    int pid = proc_match(path);
    if (pid != PROC_NONE){ out->kind = ROUTE_PROC; out->proc_id = pid; return 0; }

    /* asset prefix -> read-only asset store (device serves it) */
    if (asset_prefix && *asset_prefix){
        size_t al = strlen(asset_prefix);
        if (!strncmp(path, asset_prefix, al)){
            const char *rel = path + al; while (*rel == '/') rel++;
            if (strlen(rel) < sizeof out->asset_rel){ out->kind = ROUTE_ASSET; strcpy(out->asset_rel, rel); return 0; }
        }
    }

    /* everything else -> a sandbox file (contained; never escapes root) */
    if (fd_sandbox_resolve(sandbox_root, path, out->host_path, sizeof out->host_path) != 0){ out->kind = ROUTE_DENY; return -1; }
    out->kind = ROUTE_SANDBOX;
    return 0;
}
