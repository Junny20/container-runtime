/* process.h - process creation and the parent<->child synchronization pipe.
 *
 * This is the crux of correct setup ordering. We fork a child; the child
 * unshares into new namespaces and then BLOCKS, waiting for the parent to
 * finish the pieces of setup that can only be done from outside the namespaces
 * (uid/gid map writes, cgroup membership). The parent, after doing that work,
 * signals the child over a pipe. Only then does the child proceed to
 * pivot_root and eventually exec the user process.
 *
 * We use two pipes so both directions can synchronize:
 *   parent_ready : parent -> child  ("setup done, proceed")
 *   child_ready  : child  -> parent ("I have unshared; my pid is stable")
 *
 * Design choice: fork+unshare over clone(2) with CLONE_NEW* flags. clone would
 * have the child born already inside the namespaces (no window between fork and
 * unshare), but sequencing and debugging the setup is easier when the steps are
 * explicit calls in the child rather than clone flags. The tradeoff is the
 * small post-fork/pre-unshare window and the fact that a new PID namespace only
 * applies to the child's children, forcing the extra init-grandchild fork.
 */
#ifndef RUNTIME_PROCESS_H
#define RUNTIME_PROCESS_H

#include <sys/types.h>
#include "runtime/oci.h"
#include "runtime/container.h"

/* A synchronization channel: two unidirectional pipes. */
typedef struct {
    int parent_to_child[2]; /* [0]=read (child), [1]=write (parent) */
    int child_to_parent[2]; /* [0]=read (parent), [1]=write (child) */
} sync_pipe;

int  sync_pipe_open(sync_pipe *sp);
void sync_pipe_close_all(sync_pipe *sp);

/* Blocking one-byte handshake primitives. Return 0 on success, -1 on error. */
int sync_wait_parent(sync_pipe *sp); /* child: block until parent signals */
int sync_signal_child(sync_pipe *sp);/* parent: release the child          */
int sync_signal_parent(sync_pipe *sp);/* child: tell parent it has unshared */
int sync_wait_child(sync_pipe *sp);  /* parent: block until child signals   */

/* Create the container process for `cfg`, recording the resulting state into
 * `st` (which must already have id/bundle/rootfs/cgroup_path filled in).
 *
 * Because the OCI verbs create and start are separate CLI invocations, an
 * anonymous pipe write-end cannot survive between them. This runtime resolves
 * that with a small SUPERVISOR model: the `create` process does the fork +
 * unshare + external setup, then daemonizes itself and stays alive holding the
 * child blocked before exec. It listens on a named FIFO under the container's
 * state dir. `start` opens that FIFO and writes one byte; the supervisor
 * releases the child, which then execs the user process. The supervisor then
 * reaps the container and updates the state file to STOPPED on exit.
 *
 * On return in the ORIGINAL (foreground) process, st->pid is the supervised
 * child and st->status is STATE_CREATED; the foreground process should persist
 * the state and exit. The daemonized supervisor continues in the background.
 *
 * Returns 0 on success, -1 on error (errno set). */
int process_create(const oci_config *cfg, container_state *st);

#endif /* RUNTIME_PROCESS_H */
