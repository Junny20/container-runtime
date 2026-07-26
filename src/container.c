#include "runtime/container.h"
#include "runtime/process.h"
#include "runtime/cgroup.h"
#include "runtime/mounts.h"
#include "runtime/log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- state directory / file layout ----------------------------------- */

const char *container_state_base(void)
{
    const char *env = getenv("RT_STATE_DIR");
    return (env && *env) ? env : "/run/oci-runtime";
}

int container_state_dir(const char *id, char *out, size_t out_sz)
{
    int n = snprintf(out, out_sz, "%s/%s", container_state_base(), id);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

static int state_file_path(const char *id, char *out, size_t out_sz)
{
    char dir[4096];
    if (container_state_dir(id, dir, sizeof dir) != 0) return -1;
    int n = snprintf(out, out_sz, "%s/state.json", dir);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}

/* mkdir -p for a directory path. */
static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[4096];
    size_t len = strnlen(path, sizeof tmp);
    if (len >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

const char *container_status_str(container_status s)
{
    switch (s) {
    case STATE_CREATING: return "creating";
    case STATE_CREATED:  return "created";
    case STATE_RUNNING:  return "running";
    case STATE_STOPPED:  return "stopped";
    default:             return "unknown";
    }
}

static container_status status_from_str(const char *s)
{
    if (!s) return STATE_STOPPED;
    if (!strcmp(s, "creating")) return STATE_CREATING;
    if (!strcmp(s, "created"))  return STATE_CREATED;
    if (!strcmp(s, "running"))  return STATE_RUNNING;
    return STATE_STOPPED;
}

/* ---- state persistence ----------------------------------------------- */

int container_state_save(const container_state *st)
{
    char dir[4096], path[4096], tmp[4128];
    if (container_state_dir(st->id, dir, sizeof dir) != 0) { errno = ENAMETOOLONG; return -1; }
    if (mkdir_p(dir, 0700) != 0) { log_errno("mkdir_p %s", dir); return -1; }
    if (state_file_path(st->id, path, sizeof path) != 0) { errno = ENAMETOOLONG; return -1; }

    cJSON *o = cJSON_CreateObject();
    if (!o) { errno = ENOMEM; return -1; }
    cJSON_AddStringToObject(o, "ociVersion", "1.0.2");
    cJSON_AddStringToObject(o, "id", st->id);
    cJSON_AddStringToObject(o, "status", container_status_str(st->status));
    cJSON_AddNumberToObject(o, "pid", (double)st->pid);
    cJSON_AddStringToObject(o, "bundle", st->bundle);
    cJSON_AddStringToObject(o, "rootfs", st->rootfs);
    cJSON_AddStringToObject(o, "cgroupPath", st->cgroup_path);
    cJSON_AddNumberToObject(o, "createdAt", (double)st->created_at);

    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) { errno = ENOMEM; return -1; }

    /* Write to a temp file then rename for atomicity. */
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { log_errno("open %s", tmp); free(text); return -1; }
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    int saved = errno;
    close(fd);
    free(text);
    if (w != (ssize_t)len) { errno = saved; log_errno("write %s", tmp); return -1; }
    if (rename(tmp, path) != 0) { log_errno("rename %s -> %s", tmp, path); return -1; }
    return 0;
}

int container_state_load(const char *id, container_state *out)
{
    char path[4096];
    if (state_file_path(id, path, sizeof path) != 0) { errno = ENAMETOOLONG; return -1; }

    FILE *f = fopen(path, "rb");
    if (!f) return -1; /* errno == ENOENT means "no such container" */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); errno = ENOMEM; return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *o = cJSON_Parse(buf);
    free(buf);
    if (!o) { errno = EINVAL; return -1; }

    memset(out, 0, sizeof *out);
    const cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(o, "id");
    if (cJSON_IsString(j)) snprintf(out->id, sizeof out->id, "%s", j->valuestring);
    j = cJSON_GetObjectItemCaseSensitive(o, "status");
    out->status = status_from_str(cJSON_IsString(j) ? j->valuestring : NULL);
    j = cJSON_GetObjectItemCaseSensitive(o, "pid");
    out->pid = cJSON_IsNumber(j) ? (pid_t)j->valuedouble : -1;
    j = cJSON_GetObjectItemCaseSensitive(o, "bundle");
    if (cJSON_IsString(j)) snprintf(out->bundle, sizeof out->bundle, "%s", j->valuestring);
    j = cJSON_GetObjectItemCaseSensitive(o, "rootfs");
    if (cJSON_IsString(j)) snprintf(out->rootfs, sizeof out->rootfs, "%s", j->valuestring);
    j = cJSON_GetObjectItemCaseSensitive(o, "cgroupPath");
    if (cJSON_IsString(j)) snprintf(out->cgroup_path, sizeof out->cgroup_path, "%s", j->valuestring);
    j = cJSON_GetObjectItemCaseSensitive(o, "createdAt");
    out->created_at = cJSON_IsNumber(j) ? (long long)j->valuedouble : 0;

    cJSON_Delete(o);
    return 0;
}

int container_state_remove(const char *id)
{
    char dir[4096], path[4096], fifo[4128];
    if (state_file_path(id, path, sizeof path) != 0) { errno = ENAMETOOLONG; return -1; }
    if (container_state_dir(id, dir, sizeof dir) != 0) { errno = ENAMETOOLONG; return -1; }
    snprintf(fifo, sizeof fifo, "%s/start.fifo", dir);

    unlink(fifo);            /* best-effort */
    if (unlink(path) != 0 && errno != ENOENT) { log_errno("unlink %s", path); return -1; }
    if (rmdir(dir) != 0 && errno != ENOENT) { log_errno("rmdir %s", dir); return -1; }
    return 0;
}

/* Is a pid still alive? kill(pid,0) probes without sending a signal. */
static bool pid_alive(pid_t pid)
{
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM; /* alive but not ours to signal */
}

/* Refresh the status field to reflect reality: if a container that claims to be
 * CREATED or RUNNING no longer has a live init pid, it is actually STOPPED.
 *
 * Checking CREATED as well as RUNNING matters for the start path: a very
 * short-lived process can have already exited (supervisor flipped to STOPPED)
 * OR the supervisor may not yet have observed the exit. Either way, if the pid
 * is gone the container cannot be (re)started, so we correct the status here so
 * `start` and `delete` make the right decision even under that race. */
static container_status reconcile(container_state *st)
{
    if ((st->status == STATE_RUNNING || st->status == STATE_CREATED) &&
        !pid_alive(st->pid))
        st->status = STATE_STOPPED;
    return st->status;
}

/* ---- verbs ------------------------------------------------------------ */

int container_create(const char *id, const char *bundle_dir)
{
    /* Refuse to clobber an existing container. */
    container_state existing;
    if (container_state_load(id, &existing) == 0) {
        log_error("container %s already exists", id);
        return 1;
    }

    char *err = NULL;
    oci_config *cfg = oci_config_load(bundle_dir, &err);
    if (!cfg) {
        log_error("config load failed: %s", err ? err : "unknown");
        free(err);
        return 1;
    }

    container_state st;
    memset(&st, 0, sizeof st);
    snprintf(st.id, sizeof st.id, "%s", id);

    char *bundle_abs = realpath(bundle_dir, NULL);
    snprintf(st.bundle, sizeof st.bundle, "%s", bundle_abs ? bundle_abs : bundle_dir);
    free(bundle_abs);
    snprintf(st.rootfs, sizeof st.rootfs, "%s", cfg->rootfs_path);
    st.created_at = (long long)time(NULL);
    st.status = STATE_CREATING;
    st.pid = -1;

    /* Set up the cgroup leaf before creating the process so we can add the
     * child pid immediately. */
    snprintf(st.cgroup_path, sizeof st.cgroup_path,
             "%s/oci-runtime/%s", cgroup_root(), id);
    if (cgroup_create(st.cgroup_path) != 0) {
        log_warn("cgroup create failed; continuing without limits");
        st.cgroup_path[0] = '\0';
    } else {
        cgroup_apply(st.cgroup_path, &cfg->res);
    }

    int rc = process_create(cfg, &st);
    if (rc != 0) {
        log_error("process creation failed");
        if (st.cgroup_path[0]) cgroup_destroy(st.cgroup_path);
        container_state_remove(id);
        oci_config_free(cfg);
        return 1;
    }

    /* process_create already persisted CREATED state before daemonizing. */
    log_info("created container %s (pid %d)", id, (int)st.pid);
    oci_config_free(cfg);
    return 0;
}

int container_start(const char *id)
{
    container_state st;
    if (container_state_load(id, &st) != 0) {
        if (errno == ENOENT) log_error("no such container: %s", id);
        else log_errno("loading state for %s", id);
        return 1;
    }

    /* Reconcile first: a container whose process already exited is STOPPED, not
     * CREATED, even if the supervisor has not yet rewritten the state file.
     * This makes a second `start` (after the process ran) fail correctly. */
    reconcile(&st);

    if (st.status != STATE_CREATED) {
        log_error("cannot start container in state '%s'",
                  container_status_str(st.status));
        return 1;
    }

    /* Release the supervisor by writing to the start FIFO. If the FIFO is gone
     * (supervisor already released and cleaned up), the container has already
     * been started; treat that as an error too. */
    char dir[4096], fifo[4128];
    container_state_dir(id, dir, sizeof dir);
    snprintf(fifo, sizeof fifo, "%s/start.fifo", dir);

    int fd = open(fifo, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            log_error("container %s has already been started", id);
        else
            log_errno("open start fifo %s", fifo);
        return 1;
    }
    char b = 1;
    ssize_t w = write(fd, &b, 1);
    close(fd);
    if (w != 1) { log_errno("write start fifo"); return 1; }

    log_info("started container %s", id);
    return 0;
}

int container_state_print(const char *id)
{
    container_state st;
    if (container_state_load(id, &st) != 0) {
        if (errno == ENOENT) log_error("no such container: %s", id);
        else log_errno("loading state for %s", id);
        return 1;
    }
    reconcile(&st);

    /* Emit OCI-style state JSON on stdout. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ociVersion", "1.0.2");
    cJSON_AddStringToObject(o, "id", st.id);
    cJSON_AddStringToObject(o, "status", container_status_str(st.status));
    cJSON_AddNumberToObject(o, "pid", (double)st.pid);
    cJSON_AddStringToObject(o, "bundle", st.bundle);
    char *text = cJSON_Print(o);
    cJSON_Delete(o);
    if (!text) { errno = ENOMEM; return 1; }
    printf("%s\n", text);
    free(text);
    return 0;
}

int container_delete(const char *id, bool force)
{
    container_state st;
    if (container_state_load(id, &st) != 0) {
        if (errno == ENOENT) log_error("no such container: %s", id);
        else log_errno("loading state for %s", id);
        return 1;
    }
    reconcile(&st);

    if (st.status == STATE_RUNNING) {
        if (!force) {
            log_error("container %s is running; use --force to delete", id);
            return 1;
        }
        /* Kill the container process; the cgroup teardown below also uses
         * cgroup.kill to catch descendants. */
        if (st.pid > 0) kill(st.pid, SIGKILL);
    }

    /* Teardown: remove the cgroup (kills any stragglers, then rmdir), then the
     * state directory. This is the cleanup-guarantee path: it must succeed even
     * when the container died abnormally, so it tolerates ENOENT everywhere. */
    if (st.cgroup_path[0]) {
        if (cgroup_destroy(st.cgroup_path) != 0 && errno != ENOENT)
            log_warn("cgroup teardown incomplete for %s", id);
    }
    if (container_state_remove(id) != 0)
        log_warn("state removal incomplete for %s", id);

    log_info("deleted container %s", id);
    return 0;
}
