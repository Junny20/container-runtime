#include "runtime/mounts.h"
#include "runtime/log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/limits.h>

/* glibc historically lacks a pivot_root wrapper; call it directly. */
static int pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

/* mkdir -p style helper for a single component under the (already-current)
 * root; ignores EEXIST. */
static int ensure_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        log_errno("mkdir %s", path);
        return -1;
    }
    return 0;
}

/* Mount a pseudo-filesystem, creating its mountpoint first. */
static int mount_pseudo(const char *src, const char *target, const char *fstype,
                        unsigned long flags, const char *data)
{
    if (ensure_dir(target, 0755) != 0)
        return -1;
    if (mount(src, target, fstype, flags, data) != 0) {
        log_errno("mount %s -> %s (%s)", src, target, fstype);
        return -1;
    }
    return 0;
}

int mounts_setup_rootfs(const oci_config *cfg)
{
    const char *rootfs = cfg->rootfs_path;

    /* 1. Make the whole tree private so nothing propagates to/from the host. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        log_errno("make-rprivate /");
        return -1;
    }

    /* 2. Bind-mount rootfs onto itself so it is a distinct mount point (a
     *    prerequisite for pivot_root). */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) != 0) {
        log_errno("bind rootfs %s", rootfs);
        return -1;
    }

    /* Optionally remount read-only. */
    if (cfg->rootfs_readonly) {
        if (mount(NULL, rootfs, NULL,
                  MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0) {
            log_errno("remount ro rootfs %s", rootfs);
            return -1;
        }
    }

    /* 3. chdir into new root, create a place for the old root, pivot. */
    if (chdir(rootfs) != 0) {
        log_errno("chdir %s", rootfs);
        return -1;
    }
    if (ensure_dir(".old_root", 0700) != 0)
        return -1;
    if (pivot_root(".", ".old_root") != 0) {
        log_errno("pivot_root");
        return -1;
    }

    /* Now "/" is the new root and the old root is at "/.old_root". */
    if (chdir("/") != 0) {
        log_errno("chdir / after pivot");
        return -1;
    }

    /* 4. Mount fresh /proc (reflects the new PID namespace), /sys, /dev, and a
     *    couple of the usual pseudo-fs entries. /proc mount will fail with
     *    EPERM if the caller is not in a fresh pid namespace with the right
     *    privileges; that is surfaced by the log. */
    if (mount_pseudo("proc", "/proc", "proc",
                     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
        return -1;
    if (mount_pseudo("sysfs", "/sys", "sysfs",
                     MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, NULL) != 0)
        log_warn("sysfs mount failed (continuing)");
    if (mount_pseudo("tmpfs", "/dev", "tmpfs",
                     MS_NOSUID | MS_STRICTATIME, "mode=755") != 0)
        log_warn("/dev tmpfs mount failed (continuing)");

    /* 5. Detach and remove the old root so no host mount remains reachable. */
    if (umount2("/.old_root", MNT_DETACH) != 0)
        log_errno("umount2 /.old_root (continuing)");
    if (rmdir("/.old_root") != 0)
        log_errno("rmdir /.old_root (continuing)");

    log_debug("rootfs set up via pivot_root: %s", rootfs);
    return 0;
}
