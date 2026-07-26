/* namespaces.h - namespace entry and uid/gid map plumbing.
 *
 * The design deliberately uses fork + unshare rather than clone(2) with
 * namespace flags: the child forks first, then unshare()s into its new
 * namespaces, then waits on a pipe while the parent does the setup that must
 * happen from OUTSIDE the namespaces (writing uid/gid maps, cgroup membership).
 * See process.h for the handshake itself.
 */
#ifndef RUNTIME_NAMESPACES_H
#define RUNTIME_NAMESPACES_H

#include <sys/types.h>
#include "runtime/oci.h"

/* Called in the CHILD, immediately after fork, to enter the namespaces named in
 * `ns`. Uses unshare(2). Note the PID namespace only takes effect for the
 * child's children (unshare does not move the caller into a new pid ns), which
 * is why the child then forks its "init" grandchild before mounting /proc.
 * Returns 0 on success, -1 on error (errno set). */
int ns_unshare(const oci_namespaces *ns);

/* Called in the PARENT, once the child exists, to write the uid/gid maps for a
 * user namespace. `child_pid` is the child's pid in the parent's pid namespace.
 * Writing gid_map additionally requires "deny" to be written to setgroups
 * first, which this handles. No-op if ns->user is false. Returns 0 / -1. */
int ns_write_id_maps(pid_t child_pid, const oci_config *cfg);

/* Called in the CHILD after entering the UTS namespace, to set the hostname.
 * No-op if hostname is NULL/empty or the uts namespace was not requested.
 * Returns 0 / -1. */
int ns_set_hostname(const oci_namespaces *ns, const char *hostname);

#endif /* RUNTIME_NAMESPACES_H */
