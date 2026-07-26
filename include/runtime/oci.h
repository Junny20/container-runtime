/* oci.h - the subset of the OCI runtime spec this runtime understands.
 *
 * A "bundle" on disk is a directory containing a config.json (the runtime spec)
 * and a rootfs/ directory. We parse config.json into the structs below. We do
 * NOT model the whole spec: fields we support are captured explicitly, and any
 * present-but-unsupported field is surfaced as a warning rather than silently
 * ignored (see oci_config_load).
 */
#ifndef RUNTIME_OCI_H
#define RUNTIME_OCI_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* A single id-map entry (used for uid/gid mappings in a user namespace). */
typedef struct {
    unsigned int container_id; /* first id inside the container */
    unsigned int host_id;      /* first id on the host it maps to */
    unsigned int size;         /* number of ids in the range */
} oci_id_map;

/* The Linux namespaces the container process should enter. Each flag mirrors an
 * "linux.namespaces[].type" entry in config.json. */
typedef struct {
    bool pid;
    bool mount;
    bool uts;
    bool ipc;
    bool net;
    bool user; /* user namespace; when true, uid/gid maps below apply */
} oci_namespaces;

/* cgroup v2 resource limits. A negative / zero sentinel means "unset": the
 * controller file is left at its cgroup default when the value is unset. */
typedef struct {
    long long memory_limit_bytes; /* memory.max; <0 => unset            */
    long long cpu_quota_us;       /* cpu.max quota; <0 => "max"         */
    long long cpu_period_us;      /* cpu.max period; <=0 => default 100000 */
    long long pids_limit;         /* pids.max; <0 => "max"              */
} oci_resources;

/* The parsed, validated configuration. Owns all heap allocations reachable from
 * it; free with oci_config_free. */
typedef struct {
    char *oci_version;   /* ociVersion string, e.g. "1.0.2"          */
    char *hostname;      /* UTS hostname to set inside the container */
    char *rootfs_path;   /* absolute path to the bundle rootfs       */
    bool  rootfs_readonly;

    char  **args;        /* process.args, NULL-terminated argv       */
    size_t  args_len;
    char  **env;         /* process.env, NULL-terminated environ     */
    size_t  env_len;
    char   *cwd;         /* process.cwd inside the container         */
    uid_t   uid;         /* process.user.uid                         */
    gid_t   gid;         /* process.user.gid                         */

    oci_namespaces ns;
    oci_resources  res;

    oci_id_map *uid_maps;
    size_t      uid_maps_len;
    oci_id_map *gid_maps;
    size_t      gid_maps_len;
} oci_config;

/* Load and validate config.json from a bundle directory.
 *
 * `bundle_dir` is the path containing config.json; the rootfs path in the config
 * is resolved relative to it if not absolute. On success returns a heap-owned
 * oci_config* (caller frees with oci_config_free) and *err is left NULL. On
 * failure returns NULL and, if err != NULL, sets *err to a heap-owned message
 * the caller must free(). Unsupported-but-present fields produce warnings via
 * the log module but do not fail the load. */
oci_config *oci_config_load(const char *bundle_dir, char **err);

/* Release an oci_config and everything it owns. NULL-safe. */
void oci_config_free(oci_config *cfg);

#endif /* RUNTIME_OCI_H */
