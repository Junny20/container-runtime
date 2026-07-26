/* mounts.h - filesystem view: rootfs mounting and pivot_root.
 *
 * All of this runs in the CHILD, inside a fresh mount namespace. The sequence:
 *
 *   1. Make the whole mount tree private (MS_REC|MS_PRIVATE) so our changes do
 *      not propagate back to the host and host changes do not leak in.
 *   2. Bind-mount rootfs onto itself so it becomes a mount point (pivot_root
 *      requires new_root to be a mount point distinct from the old root).
 *   3. pivot_root into rootfs, moving the old root to a subdirectory.
 *   4. chdir("/"), mount /proc (reflecting the new PID namespace), /sys, /dev,
 *      then detach and remove the old root.
 *
 * pivot_root is used rather than chroot because chroot only changes the
 * process's root dir without detaching the old filesystem tree: the old root
 * stays reachable (e.g. via an open fd or ".."), so it is escapable and leaves
 * host mounts visible. pivot_root actually swaps the root mount and lets us
 * unmount the old one.
 */
#ifndef RUNTIME_MOUNTS_H
#define RUNTIME_MOUNTS_H

#include "runtime/oci.h"

/* Enter the container filesystem view described by `cfg`, using pivot_root.
 * Must be called in the child after ns_unshare() has created the mount (and
 * pid) namespace, and after the init grandchild has been forked so that /proc
 * reflects the new pid namespace. Returns 0 / -1 (errno set). On failure the
 * mount namespace may be partially set up; the caller should abort the child. */
int mounts_setup_rootfs(const oci_config *cfg);

#endif /* RUNTIME_MOUNTS_H */
