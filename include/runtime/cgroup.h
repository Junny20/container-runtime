/* cgroup.h - cgroup v2 subtree creation, controller writes, teardown.
 *
 * We target the cgroup v2 unified hierarchy only (mounted at
 * /sys/fs/cgroup). For a container we create a leaf subtree, enable the
 * controllers we need in the PARENT of that leaf, write the limit files, and
 * place the container's init pid into cgroup.procs. Teardown removes the
 * directory; because a cgroup dir can only be rmdir'd once empty, teardown must
 * ensure no procs remain (kill via cgroup.kill on abnormal exit).
 *
 * Ordering note: cgroup membership must be established from OUTSIDE by the
 * parent (the child cannot always move itself in before dropping privileges),
 * which is one half of why the parent/child pipe handshake exists.
 */
#ifndef RUNTIME_CGROUP_H
#define RUNTIME_CGROUP_H

#include <sys/types.h>
#include "runtime/oci.h"

/* The cgroup v2 mount point. Configurable via $RT_CGROUP_ROOT for testing;
 * defaults to "/sys/fs/cgroup". */
const char *cgroup_root(void);

/* Create the container's cgroup leaf directory at `cgroup_path` (absolute,
 * typically "<root>/oci-runtime/<id>"), enabling the cpu, memory, and pids
 * controllers in the parent cgroup's cgroup.subtree_control so they are
 * available in the leaf. Returns 0 / -1 (errno set). Idempotent for an existing
 * empty directory. */
int cgroup_create(const char *cgroup_path);

/* Write the resource limits from `res` into the leaf's controller files
 * (memory.max, cpu.max, pids.max). Unset sentinels are skipped. Must be called
 * before the user process starts. Returns 0 / -1. */
int cgroup_apply(const char *cgroup_path, const oci_resources *res);

/* Move process `pid` into the leaf by writing to cgroup.procs. Returns 0 / -1. */
int cgroup_add_pid(const char *cgroup_path, pid_t pid);

/* Kill everything in the leaf (writes "1" to cgroup.kill) then rmdir the
 * directory. Safe to call during signal-driven cleanup: it uses only
 * async-signal-safe syscalls (open/write/close/rmdir) and does not allocate.
 * Returns 0 / -1; -1 with ENOENT means already gone (treated as success by
 * callers). */
int cgroup_destroy(const char *cgroup_path);

#endif /* RUNTIME_CGROUP_H */
