/* container.h - lifecycle state machine and on-disk state.
 *
 * The runtime implements the OCI verbs create / start / state / delete. Because
 * each verb is a separate CLI invocation (a fresh process), the only way a later
 * invocation can find an earlier container is a small state file persisted under
 * a per-container state directory. This header defines that state and the verbs
 * that transition it.
 *
 * State transitions (OCI):
 *
 *      (none) --create--> CREATED --start--> RUNNING --(proc exits)--> STOPPED
 *          ^                 |                                            |
 *          |                 +--------------------delete------------------+
 *          +-------------------------- delete --------------------------- +
 *
 *  create : set up namespaces/rootfs/cgroup, fork the child, block it before it
 *           execs the user process. Container ends in CREATED.
 *  start  : release the blocked child so it execs the user process. RUNNING.
 *  state  : print the current status as JSON. Never mutates.
 *  delete : tear down cgroup + mounts and remove the state dir. Only valid from
 *           CREATED or STOPPED (refuse to delete a RUNNING container unless
 *           --force is given).
 */
#ifndef RUNTIME_CONTAINER_H
#define RUNTIME_CONTAINER_H

#include <stdbool.h>
#include <sys/types.h>
#include "runtime/oci.h"

typedef enum {
    STATE_CREATING = 0, /* transient: create in progress */
    STATE_CREATED,      /* set up, user process not yet started */
    STATE_RUNNING,      /* user process started */
    STATE_STOPPED       /* user process has exited */
} container_status;

/* Persisted container record. The status/pid fields change over the lifecycle;
 * bundle/rootfs/created_at are fixed at create time. */
typedef struct {
    char id[128];             /* container id (from CLI)                    */
    container_status status;
    pid_t pid;                /* pid of the container's init process (host view) */
    char bundle[4096];        /* absolute path to the bundle directory      */
    char rootfs[4096];        /* absolute path to the rootfs                */
    char cgroup_path[4096];   /* absolute path to the container's cgroup dir */
    long long created_at;     /* unix seconds at create time                */
} container_state;

/* State-file location. Containers live under a base state directory; each gets
 * its own subdir "<base>/<id>/" holding "state.json". The base is
 * $RT_STATE_DIR or, if unset, "/run/oci-runtime". */
const char *container_state_base(void);

/* Compute the per-container state directory ("<base>/<id>") into `out` (size
 * `out_sz`). Returns 0 on success, -1 if the buffer is too small. */
int container_state_dir(const char *id, char *out, size_t out_sz);

/* Persist / load / remove the state file. Return 0 on success, -1 on error
 * (errno set). container_state_load returns -1 with errno==ENOENT if no such
 * container exists. */
int container_state_save(const container_state *st);
int container_state_load(const char *id, container_state *out);
int container_state_remove(const char *id);

/* Human-readable status string ("creating"/"created"/"running"/"stopped"). */
const char *container_status_str(container_status s);

/* ---- Verb entry points. Each returns a process exit code (0 == success). ----
 * create: builds the container from `bundle_dir` under name `id`. Blocks the
 *         child before exec and returns once it is CREATED.
 * start : releases the CREATED container `id` so it execs the user process.
 * state : prints the state of `id` as JSON to stdout.
 * delete: tears down `id`. If `force` is true, a RUNNING container is killed
 *         first; otherwise deleting a RUNNING container is an error. */
int container_create(const char *id, const char *bundle_dir);
int container_start(const char *id);
int container_state_print(const char *id);
int container_delete(const char *id, bool force);

#endif /* RUNTIME_CONTAINER_H */
