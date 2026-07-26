#include "runtime/oci.h"
#include "runtime/log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

/* ---- small helpers ---------------------------------------------------- */

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* set_err: allocate a formatted error string into *err (if err != NULL). */
static void set_err(char **err, const char *fmt, ...)
{
    if (!err) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    *err = xstrdup(buf);
}

/* Read an entire file into a heap buffer (NUL-terminated). Returns NULL on
 * error with errno set. */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); errno = ENOMEM; return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* Turn a cJSON string array into a NULL-terminated char** (argv/env style). */
static char **json_str_array(const cJSON *arr, size_t *out_len)
{
    size_t n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
        if (cJSON_IsString(it)) n++;
    char **v = calloc(n + 1, sizeof *v);
    if (!v) return NULL;
    size_t i = 0;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it)) {
            v[i] = xstrdup(it->valuestring);
            if (!v[i]) { /* best-effort cleanup */
                for (size_t j = 0; j < i; j++) free(v[j]);
                free(v);
                return NULL;
            }
            i++;
        }
    }
    v[i] = NULL;
    if (out_len) *out_len = i;
    return v;
}

static void free_str_array(char **v)
{
    if (!v) return;
    for (size_t i = 0; v[i]; i++) free(v[i]);
    free(v);
}

/* Parse an id-map array ("uidMappings"/"gidMappings"). */
static oci_id_map *json_id_maps(const cJSON *arr, size_t *out_len)
{
    size_t n = (size_t)cJSON_GetArraySize(arr);
    if (n == 0) { *out_len = 0; return NULL; }
    oci_id_map *m = calloc(n, sizeof *m);
    if (!m) return NULL;
    size_t i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "containerID");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "hostID");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "size");
        m[i].container_id = cJSON_IsNumber(c) ? (unsigned)c->valuedouble : 0;
        m[i].host_id      = cJSON_IsNumber(h) ? (unsigned)h->valuedouble : 0;
        m[i].size         = cJSON_IsNumber(s) ? (unsigned)s->valuedouble : 0;
        i++;
    }
    *out_len = i;
    return m;
}

/* Warn about a known object's keys that we do not act on, so nothing is
 * silently ignored. `known` is a NULL-terminated list of keys we DO handle. */
static void warn_unsupported(const cJSON *obj, const char *ctx,
                             const char *const *known)
{
    if (!cJSON_IsObject(obj)) return;
    const cJSON *child;
    cJSON_ArrayForEach(child, obj) {
        const char *k = child->string;
        if (!k) continue;
        bool ok = false;
        for (const char *const *p = known; *p; p++)
            if (!strcmp(k, *p)) { ok = true; break; }
        if (!ok)
            log_warn("config: unsupported field ignored: %s.%s", ctx, k);
    }
}

/* ---- namespace mapping ------------------------------------------------ */

static void parse_namespaces(const cJSON *linux_obj, oci_namespaces *ns)
{
    memset(ns, 0, sizeof *ns);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(linux_obj, "namespaces");
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "type");
        if (!cJSON_IsString(t)) continue;
        const char *type = t->valuestring;
        if      (!strcmp(type, "pid"))     ns->pid   = true;
        else if (!strcmp(type, "mount"))   ns->mount = true;
        else if (!strcmp(type, "uts"))     ns->uts   = true;
        else if (!strcmp(type, "ipc"))     ns->ipc   = true;
        else if (!strcmp(type, "network")) ns->net   = true;
        else if (!strcmp(type, "user"))    ns->user  = true;
        else if (!strcmp(type, "cgroup"))
            log_warn("config: cgroup namespace requested but not supported");
        else
            log_warn("config: unknown namespace type: %s", type);
    }
}

/* ---- resource mapping ------------------------------------------------- */

static void parse_resources(const cJSON *linux_obj, oci_resources *res)
{
    /* sentinels: <0 means "unset / max" */
    res->memory_limit_bytes = -1;
    res->cpu_quota_us        = -1;
    res->cpu_period_us       = 0;   /* 0 => default period 100000 */
    res->pids_limit          = -1;

    const cJSON *r = cJSON_GetObjectItemCaseSensitive(linux_obj, "resources");
    if (!cJSON_IsObject(r)) return;

    const cJSON *mem = cJSON_GetObjectItemCaseSensitive(r, "memory");
    if (cJSON_IsObject(mem)) {
        const cJSON *limit = cJSON_GetObjectItemCaseSensitive(mem, "limit");
        if (cJSON_IsNumber(limit))
            res->memory_limit_bytes = (long long)limit->valuedouble;
    }
    const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(r, "cpu");
    if (cJSON_IsObject(cpu)) {
        const cJSON *quota  = cJSON_GetObjectItemCaseSensitive(cpu, "quota");
        const cJSON *period = cJSON_GetObjectItemCaseSensitive(cpu, "period");
        if (cJSON_IsNumber(quota))
            res->cpu_quota_us = (long long)quota->valuedouble;
        if (cJSON_IsNumber(period))
            res->cpu_period_us = (long long)period->valuedouble;
    }
    const cJSON *pids = cJSON_GetObjectItemCaseSensitive(r, "pids");
    if (cJSON_IsObject(pids)) {
        const cJSON *lim = cJSON_GetObjectItemCaseSensitive(pids, "limit");
        if (cJSON_IsNumber(lim))
            res->pids_limit = (long long)lim->valuedouble;
    }
}

/* Resolve rootfs path relative to the bundle directory if not absolute. */
static char *resolve_rootfs(const char *bundle_dir, const char *rootfs)
{
    if (!rootfs) rootfs = "rootfs";
    if (rootfs[0] == '/')
        return xstrdup(rootfs);
    char joined[PATH_MAX];
    snprintf(joined, sizeof joined, "%s/%s", bundle_dir, rootfs);
    char *abs = realpath(joined, NULL);
    if (abs) return abs;           /* canonical if it exists */
    return xstrdup(joined);        /* otherwise keep the joined path */
}

/* ---- public API ------------------------------------------------------- */

oci_config *oci_config_load(const char *bundle_dir, char **err)
{
    if (err) *err = NULL;

    char cfg_path[PATH_MAX];
    snprintf(cfg_path, sizeof cfg_path, "%s/config.json", bundle_dir);

    char *text = read_file(cfg_path, NULL);
    if (!text) {
        set_err(err, "cannot read %s: %s", cfg_path, strerror(errno));
        return NULL;
    }

    cJSON *root = cJSON_Parse(text);
    if (!root) {
        /* cJSON_GetErrorPtr() points INTO `text`, so copy the snippet before
         * freeing it. */
        const char *ep = cJSON_GetErrorPtr();
        char snippet[41] = "?";
        if (ep) { strncpy(snippet, ep, sizeof snippet - 1); snippet[40] = '\0'; }
        free(text);
        set_err(err, "config.json parse error near: %s", snippet);
        return NULL;
    }
    free(text);

    oci_config *cfg = calloc(1, sizeof *cfg);
    if (!cfg) { cJSON_Delete(root); set_err(err, "out of memory"); return NULL; }

    /* ociVersion */
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "ociVersion");
    if (!cJSON_IsString(ver)) {
        set_err(err, "config.json: missing required string 'ociVersion'");
        goto fail;
    }
    cfg->oci_version = xstrdup(ver->valuestring);

    /* hostname (optional) */
    const cJSON *hn = cJSON_GetObjectItemCaseSensitive(root, "hostname");
    if (cJSON_IsString(hn)) cfg->hostname = xstrdup(hn->valuestring);

    /* root { path, readonly } */
    const cJSON *rootobj = cJSON_GetObjectItemCaseSensitive(root, "root");
    const char *rootfs_field = NULL;
    if (cJSON_IsObject(rootobj)) {
        const cJSON *rp = cJSON_GetObjectItemCaseSensitive(rootobj, "path");
        const cJSON *ro = cJSON_GetObjectItemCaseSensitive(rootobj, "readonly");
        if (cJSON_IsString(rp)) rootfs_field = rp->valuestring;
        cfg->rootfs_readonly = cJSON_IsBool(ro) && cJSON_IsTrue(ro);
        static const char *const known[] = { "path", "readonly", NULL };
        warn_unsupported(rootobj, "root", known);
    }
    cfg->rootfs_path = resolve_rootfs(bundle_dir, rootfs_field);

    /* process { args, env, cwd, user{uid,gid} } */
    const cJSON *proc = cJSON_GetObjectItemCaseSensitive(root, "process");
    if (!cJSON_IsObject(proc)) {
        set_err(err, "config.json: missing required object 'process'");
        goto fail;
    }
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(proc, "args");
    if (!cJSON_IsArray(args) || cJSON_GetArraySize(args) == 0) {
        set_err(err, "config.json: process.args must be a non-empty array");
        goto fail;
    }
    cfg->args = json_str_array(args, &cfg->args_len);
    if (!cfg->args) { set_err(err, "out of memory (args)"); goto fail; }

    const cJSON *env = cJSON_GetObjectItemCaseSensitive(proc, "env");
    if (cJSON_IsArray(env)) {
        cfg->env = json_str_array(env, &cfg->env_len);
        if (!cfg->env) { set_err(err, "out of memory (env)"); goto fail; }
    } else {
        cfg->env = calloc(1, sizeof(char *)); /* empty, NULL-terminated */
        cfg->env_len = 0;
    }

    const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(proc, "cwd");
    cfg->cwd = xstrdup(cJSON_IsString(cwd) ? cwd->valuestring : "/");

    const cJSON *user = cJSON_GetObjectItemCaseSensitive(proc, "user");
    if (cJSON_IsObject(user)) {
        const cJSON *uid = cJSON_GetObjectItemCaseSensitive(user, "uid");
        const cJSON *gid = cJSON_GetObjectItemCaseSensitive(user, "gid");
        cfg->uid = cJSON_IsNumber(uid) ? (uid_t)uid->valuedouble : 0;
        cfg->gid = cJSON_IsNumber(gid) ? (gid_t)gid->valuedouble : 0;
        static const char *const known[] =
            { "uid", "gid", "additionalGids", "umask", NULL };
        warn_unsupported(user, "process.user", known);
    }
    {
        static const char *const known[] =
            { "args", "env", "cwd", "user", "terminal",
              "capabilities", "rlimits", "noNewPrivileges", NULL };
        warn_unsupported(proc, "process", known);
    }

    /* linux { namespaces, resources, uidMappings, gidMappings } */
    const cJSON *lin = cJSON_GetObjectItemCaseSensitive(root, "linux");
    if (cJSON_IsObject(lin)) {
        parse_namespaces(lin, &cfg->ns);
        parse_resources(lin, &cfg->res);

        const cJSON *um = cJSON_GetObjectItemCaseSensitive(lin, "uidMappings");
        const cJSON *gm = cJSON_GetObjectItemCaseSensitive(lin, "gidMappings");
        if (cJSON_IsArray(um)) cfg->uid_maps = json_id_maps(um, &cfg->uid_maps_len);
        if (cJSON_IsArray(gm)) cfg->gid_maps = json_id_maps(gm, &cfg->gid_maps_len);

        static const char *const known[] =
            { "namespaces", "resources", "uidMappings", "gidMappings",
              "seccomp", "maskedPaths", "readonlyPaths", "devices",
              "sysctl", "cgroupsPath", NULL };
        warn_unsupported(lin, "linux", known);
    } else {
        /* No linux section: default to no isolation & unset limits. */
        memset(&cfg->ns, 0, sizeof cfg->ns);
        cfg->res.memory_limit_bytes = -1;
        cfg->res.cpu_quota_us = -1;
        cfg->res.cpu_period_us = 0;
        cfg->res.pids_limit = -1;
    }

    /* Validation: user namespace requires at least a uid & gid map. */
    if (cfg->ns.user &&
        (cfg->uid_maps_len == 0 || cfg->gid_maps_len == 0)) {
        set_err(err, "config.json: user namespace requires uid/gid mappings");
        goto fail;
    }

    /* Top-level unsupported-field surfacing. */
    {
        static const char *const known[] =
            { "ociVersion", "hostname", "root", "process", "linux",
              "mounts", "annotations", NULL };
        warn_unsupported(root, "", known);
    }

    cJSON_Delete(root);
    return cfg;

fail:
    cJSON_Delete(root);
    oci_config_free(cfg);
    return NULL;
}

void oci_config_free(oci_config *cfg)
{
    if (!cfg) return;
    free(cfg->oci_version);
    free(cfg->hostname);
    free(cfg->rootfs_path);
    free(cfg->cwd);
    free_str_array(cfg->args);
    free_str_array(cfg->env);
    free(cfg->uid_maps);
    free(cfg->gid_maps);
    free(cfg);
}
