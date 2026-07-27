/* fdtable.h — fd/FILE model + sandbox path router (Audit 06 L5-L8/M8, Audit 09 S8).
 *
 * Two pieces of pure, security-critical logic (the actual host open/read/write
 * live in the device bridge and use these):
 *   1. an fd-slot table mapping small guest fds (>=3; 0/1/2 reserved) to a host
 *      resource + kind, with reuse;
 *   2. a path router that classifies a guest path into: a sandbox host file, a
 *      synthetic special file (/dev/urandom, /proc entries), or an asset — and, for
 *      sandbox files, canonicalises the path so it can NEVER escape the sandbox
 *      root (path-traversal via ".." is neutralised — no writes ever leave the
 *      sandbox, which matters both for Audit-06 correctness and this box's
 *      public-IPv4 exposure). */
#ifndef ABSHIM_FDTABLE_H
#define ABSHIM_FDTABLE_H
#include <stdint.h>
#include <stddef.h>

/* ---- fd slot table ---- */
enum fd_kind { FDK_FREE=0, FDK_FILE, FDK_URANDOM, FDK_PROC, FDK_ASSET };
typedef struct fdtable fdtable;

fdtable *fdt_create(void);
void     fdt_destroy(fdtable *t);
/* Allocate a fresh guest fd (>=3) bound to (kind, res, aux). Returns fd or -1. */
int      fdt_alloc(fdtable *t, int kind, void *res, int aux);
/* Look up a live fd; returns kind (FDK_FREE if not live); res/aux filled if non-NULL. */
int      fdt_get  (fdtable *t, int fd, void **res, int *aux);
/* Release an fd; returns 1 if it was live. */
int      fdt_free (fdtable *t, int fd);
uint32_t fdt_live (fdtable *t);   /* test introspection */

/* ---- path router ---- */
enum fd_route_kind { ROUTE_DENY=0, ROUTE_SANDBOX, ROUTE_URANDOM, ROUTE_PROC, ROUTE_ASSET };
enum fd_proc_id    { PROC_NONE=0, PROC_CPUINFO, PROC_MEMINFO, PROC_AUXV, PROC_SOCINFO };

typedef struct {
    int  kind;              /* enum fd_route_kind */
    int  proc_id;           /* enum fd_proc_id when kind==ROUTE_PROC */
    char host_path[1024];   /* resolved, sandbox-contained host path when SANDBOX */
    char asset_rel[1024];   /* asset-relative path when ROUTE_ASSET */
} fd_route_t;

/* Classify `path`. `sandbox_root` is the app's writable dir; `asset_prefix`
 * (may be NULL) is a guest path prefix served read-only from the asset store. */
int fd_route(const char *sandbox_root, const char *asset_prefix, const char *path, fd_route_t *out);

/* Canonicalise `path` under `root` so the result stays inside root ("." and ""
 * dropped, ".." can never rise above root, leading "/" stripped). Writes into
 * out[0..outsz). Returns 0 on success, -1 on overflow. Exposed for testing. */
int fd_sandbox_resolve(const char *root, const char *path, char *out, size_t outsz);

#endif /* ABSHIM_FDTABLE_H */
