#include "runtime/namespaces.h"
#include "runtime/log.h"

#include <sched.h>       /* unshare, CLONE_NEW* */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/utsname.h>

/* Build the unshare flag mask from the requested namespaces. */
static int ns_flags(const oci_namespaces *ns)
{
    int flags = 0;
    if (ns->pid)   flags |= CLONE_NEWPID;
    if (ns->mount) flags |= CLONE_NEWNS;
    if (ns->uts)   flags |= CLONE_NEWUTS;
    if (ns->ipc)   flags |= CLONE_NEWIPC;
    if (ns->net)   flags |= CLONE_NEWNET;
    if (ns->user)  flags |= CLONE_NEWUSER;
    return flags;
}

int ns_unshare(const oci_namespaces *ns)
{
    int flags = ns_flags(ns);
    if (flags == 0)
        return 0; /* nothing requested */

    /* When a user namespace is requested it must be created first (in the same
     * unshare call is fine: the kernel creates userns before the others so the
     * unprivileged caller gains the capabilities needed for the rest). */
    if (unshare(flags) != 0) {
        log_errno("unshare(0x%x) failed", flags);
        return -1;
    }
    log_debug("unshared namespaces: 0x%x", flags);
    return 0;
}

/* Write `data` (len bytes) to `path`. Returns 0 / -1. */
static int write_file(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_errno("open %s", path);
        return -1;
    }
    ssize_t w = write(fd, data, len);
    int saved = errno;
    close(fd);
    if (w != (ssize_t)len) {
        errno = saved;
        log_errno("write %s", path);
        return -1;
    }
    return 0;
}

/* Format an id-map array into the multi-line form the kernel expects:
 *   "<container_id> <host_id> <size>\n" per entry. */
static int format_id_map(const oci_id_map *maps, size_t n, char *buf, size_t sz)
{
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        int r = snprintf(buf + off, sz - off, "%u %u %u\n",
                         maps[i].container_id, maps[i].host_id, maps[i].size);
        if (r < 0 || (size_t)r >= sz - off) { errno = ENOSPC; return -1; }
        off += (size_t)r;
    }
    return (int)off;
}

int ns_write_id_maps(pid_t child_pid, const oci_config *cfg)
{
    if (!cfg->ns.user)
        return 0;

    char path[128], buf[4096];
    int len;

    /* setgroups must be "deny" before writing gid_map for an unprivileged
     * userns; ignore ENOENT on very old kernels. */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child_pid);
    if (write_file(path, "deny", 4) != 0 && errno != ENOENT)
        return -1;

    /* uid_map */
    len = format_id_map(cfg->uid_maps, cfg->uid_maps_len, buf, sizeof buf);
    if (len < 0) { log_error("uid_map too large"); return -1; }
    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child_pid);
    if (write_file(path, buf, (size_t)len) != 0)
        return -1;

    /* gid_map */
    len = format_id_map(cfg->gid_maps, cfg->gid_maps_len, buf, sizeof buf);
    if (len < 0) { log_error("gid_map too large"); return -1; }
    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child_pid);
    if (write_file(path, buf, (size_t)len) != 0)
        return -1;

    log_debug("wrote uid/gid maps for pid %d", (int)child_pid);
    return 0;
}

int ns_set_hostname(const oci_namespaces *ns, const char *hostname)
{
    if (!ns->uts || !hostname || !*hostname)
        return 0;
    if (sethostname(hostname, strlen(hostname)) != 0) {
        log_errno("sethostname(%s)", hostname);
        return -1;
    }
    log_debug("set hostname: %s", hostname);
    return 0;
}
