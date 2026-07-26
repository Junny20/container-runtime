#include "runtime/cgroup.h"
#include "runtime/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

const char *cgroup_root(void)
{
    const char *env = getenv("RT_CGROUP_ROOT");
    return (env && *env) ? env : "/sys/fs/cgroup";
}

/* Write a NUL-terminated string to `path`. Returns 0 / -1 (errno set). Not
 * async-signal-safe (uses strlen); the signal-safe variant is below. */
static int write_str(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    size_t len = strlen(val);
    ssize_t w = write(fd, val, len);
    int saved = errno;
    close(fd);
    if (w != (ssize_t)len) { errno = saved; return -1; }
    return 0;
}

/* Enable a controller in a cgroup's cgroup.subtree_control by writing "+name".
 * Failing to enable is a soft error (logged) because some controllers may be
 * unavailable depending on kernel config or delegation. */
static void enable_controller(const char *parent_dir, const char *name)
{
    char path[PATH_MAX], val[64];
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", parent_dir);
    snprintf(val, sizeof val, "+%s", name);
    if (write_str(path, val) != 0)
        log_warn("could not enable controller %s in %s: %s",
                 name, parent_dir, strerror(errno));
}

/* Extract the parent directory of `path` into `out`. */
static void parent_of(const char *path, char *out, size_t sz)
{
    snprintf(out, sz, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = '\0';
}

int cgroup_create(const char *cgroup_path)
{
    /* Ensure the intermediate "<root>/oci-runtime" parent exists so we can
     * enable controllers there before creating the leaf. */
    char parent[PATH_MAX];
    parent_of(cgroup_path, parent, sizeof parent);

    if (mkdir(parent, 0755) != 0 && errno != EEXIST) {
        log_errno("mkdir cgroup parent %s", parent);
        return -1;
    }
    /* Enable the controllers we intend to write in the leaf. */
    enable_controller(parent, "cpu");
    enable_controller(parent, "memory");
    enable_controller(parent, "pids");

    if (mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) {
        log_errno("mkdir cgroup leaf %s", cgroup_path);
        return -1;
    }
    log_debug("created cgroup %s", cgroup_path);
    return 0;
}

int cgroup_apply(const char *cgroup_path, const oci_resources *res)
{
    char path[PATH_MAX], val[128];

    /* memory.max */
    if (res->memory_limit_bytes >= 0) {
        snprintf(path, sizeof path, "%s/memory.max", cgroup_path);
        snprintf(val, sizeof val, "%lld", res->memory_limit_bytes);
        if (write_str(path, val) != 0)
            log_warn("write memory.max: %s", strerror(errno));
    }

    /* cpu.max: "<quota> <period>" or "max <period>" when quota unset. */
    if (res->cpu_quota_us >= 0) {
        long long period = res->cpu_period_us > 0 ? res->cpu_period_us : 100000;
        snprintf(path, sizeof path, "%s/cpu.max", cgroup_path);
        snprintf(val, sizeof val, "%lld %lld", res->cpu_quota_us, period);
        if (write_str(path, val) != 0)
            log_warn("write cpu.max: %s", strerror(errno));
    }

    /* pids.max */
    if (res->pids_limit >= 0) {
        snprintf(path, sizeof path, "%s/pids.max", cgroup_path);
        snprintf(val, sizeof val, "%lld", res->pids_limit);
        if (write_str(path, val) != 0)
            log_warn("write pids.max: %s", strerror(errno));
    }

    log_debug("applied cgroup limits to %s", cgroup_path);
    return 0;
}

int cgroup_add_pid(const char *cgroup_path, pid_t pid)
{
    char path[PATH_MAX], val[32];
    snprintf(path, sizeof path, "%s/cgroup.procs", cgroup_path);
    snprintf(val, sizeof val, "%d", (int)pid);
    if (write_str(path, val) != 0) {
        log_errno("write cgroup.procs (%s)", path);
        return -1;
    }
    log_debug("added pid %d to %s", (int)pid, cgroup_path);
    return 0;
}

/* ---- signal-safe teardown -------------------------------------------- *
 * cgroup_destroy may run from a signal-driven cleanup path, so it avoids the
 * stdio/strlen helpers above and uses only open/write/close/rmdir with a
 * fixed-size buffer built by a tiny async-signal-safe path concatenation. */

/* Append src to dst starting at *pos (bounded by cap). */
static void safe_append(char *dst, size_t cap, size_t *pos, const char *src)
{
    size_t i = 0;
    while (src[i] && *pos + 1 < cap) dst[(*pos)++] = src[i++];
    dst[*pos] = '\0';
}

static int safe_write_one(const char *path, const char *val, size_t vlen)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t w = write(fd, val, vlen);
    int saved = errno;
    close(fd);
    if (w != (ssize_t)vlen) { errno = saved; return -1; }
    return 0;
}

int cgroup_destroy(const char *cgroup_path)
{
    char path[PATH_MAX];
    size_t pos;

    /* Kill everything still in the cgroup: echo 1 > cgroup.kill. Best-effort;
     * cgroup.kill exists on modern kernels. */
    pos = 0;
    safe_append(path, sizeof path, &pos, cgroup_path);
    safe_append(path, sizeof path, &pos, "/cgroup.kill");
    (void)safe_write_one(path, "1", 1);

    /* Retry rmdir a few times since procs exit asynchronously after
     * cgroup.kill. */
    for (int attempt = 0; attempt < 50; attempt++) {
        if (rmdir(cgroup_path) == 0)
            return 0;
        if (errno == ENOENT)
            return 0; /* already gone */
        if (errno != EBUSY) {
            log_errno("rmdir cgroup %s", cgroup_path);
            return -1;
        }
        /* EBUSY: procs not yet reaped; brief spin. */
        usleep(2000);
    }
    log_error("cgroup %s still busy after retries", cgroup_path);
    errno = EBUSY;
    return -1;
}
