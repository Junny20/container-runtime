/* test_oci.c - unit tests for the config.json parser.
 *
 * These run anywhere (no privileges, no Linux-specific syscalls) so the parser
 * and validation logic can be checked in CI under ASan/UBSan. We write sample
 * bundles into a temp dir and assert on the parsed oci_config.
 */
#include "runtime/oci.h"
#include "runtime/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } \
    else         { printf("  ok:   %s\n", msg); } \
} while (0)

/* Write `contents` to <dir>/config.json. */
static void write_config(const char *dir, const char *contents)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/config.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(2); }
    fputs(contents, f);
    fclose(f);
}

static char *make_tmpdir(void)
{
    char tmpl[] = "/tmp/ocitest.XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); exit(2); }
    return strdup(d);
}

static void cleanup_dir(char *dir)
{
    char p[4096]; snprintf(p, sizeof p, "%s/config.json", dir); unlink(p);
    rmdir(dir); free(dir);
}

static void test_minimal_valid(void)
{
    printf("test_minimal_valid\n");
    char *dir = make_tmpdir();
    write_config(dir,
        "{"
        "\"ociVersion\":\"1.0.2\","
        "\"hostname\":\"testbox\","
        "\"root\":{\"path\":\"rootfs\",\"readonly\":false},"
        "\"process\":{"
          "\"args\":[\"/bin/sh\",\"-c\",\"echo hi\"],"
          "\"env\":[\"PATH=/bin\",\"HOME=/root\"],"
          "\"cwd\":\"/\","
          "\"user\":{\"uid\":0,\"gid\":0}"
        "},"
        "\"linux\":{"
          "\"namespaces\":[{\"type\":\"pid\"},{\"type\":\"mount\"},"
                          "{\"type\":\"uts\"},{\"type\":\"ipc\"}],"
          "\"resources\":{"
            "\"memory\":{\"limit\":134217728},"
            "\"cpu\":{\"quota\":50000,\"period\":100000},"
            "\"pids\":{\"limit\":64}"
          "}"
        "}"
        "}");

    char *err = NULL;
    oci_config *cfg = oci_config_load(dir, &err);
    CHECK(cfg != NULL, "config loads");
    if (cfg) {
        CHECK(strcmp(cfg->oci_version, "1.0.2") == 0, "ociVersion parsed");
        CHECK(cfg->hostname && strcmp(cfg->hostname, "testbox") == 0, "hostname parsed");
        CHECK(cfg->args_len == 3, "three args");
        CHECK(strcmp(cfg->args[0], "/bin/sh") == 0, "arg0 is /bin/sh");
        CHECK(cfg->args[3] == NULL, "argv NULL-terminated");
        CHECK(cfg->env_len == 2, "two env vars");
        CHECK(strcmp(cfg->cwd, "/") == 0, "cwd parsed");
        CHECK(cfg->ns.pid && cfg->ns.mount && cfg->ns.uts && cfg->ns.ipc,
              "namespaces flagged");
        CHECK(!cfg->ns.net && !cfg->ns.user, "unrequested namespaces off");
        CHECK(cfg->res.memory_limit_bytes == 134217728, "memory limit parsed");
        CHECK(cfg->res.cpu_quota_us == 50000, "cpu quota parsed");
        CHECK(cfg->res.cpu_period_us == 100000, "cpu period parsed");
        CHECK(cfg->res.pids_limit == 64, "pids limit parsed");
        CHECK(strstr(cfg->rootfs_path, "rootfs") != NULL, "rootfs resolved");
        oci_config_free(cfg);
    }
    free(err);
    cleanup_dir(dir);
}

static void test_missing_ociversion(void)
{
    printf("test_missing_ociversion\n");
    char *dir = make_tmpdir();
    write_config(dir, "{\"process\":{\"args\":[\"/bin/true\"]}}");
    char *err = NULL;
    oci_config *cfg = oci_config_load(dir, &err);
    CHECK(cfg == NULL, "load fails without ociVersion");
    CHECK(err != NULL, "error message set");
    free(err);
    cleanup_dir(dir);
}

static void test_missing_args(void)
{
    printf("test_missing_args\n");
    char *dir = make_tmpdir();
    write_config(dir, "{\"ociVersion\":\"1.0.2\",\"process\":{\"args\":[]}}");
    char *err = NULL;
    oci_config *cfg = oci_config_load(dir, &err);
    CHECK(cfg == NULL, "load fails with empty args");
    free(err);
    cleanup_dir(dir);
}

static void test_userns_requires_maps(void)
{
    printf("test_userns_requires_maps\n");
    char *dir = make_tmpdir();
    write_config(dir,
        "{\"ociVersion\":\"1.0.2\","
        "\"process\":{\"args\":[\"/bin/true\"]},"
        "\"linux\":{\"namespaces\":[{\"type\":\"user\"}]}}");
    char *err = NULL;
    oci_config *cfg = oci_config_load(dir, &err);
    CHECK(cfg == NULL, "user namespace without maps rejected");
    free(err);

    /* Now with maps: should succeed. */
    write_config(dir,
        "{\"ociVersion\":\"1.0.2\","
        "\"process\":{\"args\":[\"/bin/true\"]},"
        "\"linux\":{"
          "\"namespaces\":[{\"type\":\"user\"}],"
          "\"uidMappings\":[{\"containerID\":0,\"hostID\":1000,\"size\":1}],"
          "\"gidMappings\":[{\"containerID\":0,\"hostID\":1000,\"size\":1}]"
        "}}");
    err = NULL;
    cfg = oci_config_load(dir, &err);
    CHECK(cfg != NULL, "user namespace with maps accepted");
    if (cfg) {
        CHECK(cfg->uid_maps_len == 1, "one uid map");
        CHECK(cfg->uid_maps && cfg->uid_maps[0].host_id == 1000, "uid map host id");
        oci_config_free(cfg);
    }
    free(err);
    cleanup_dir(dir);
}

static void test_malformed_json(void)
{
    printf("test_malformed_json\n");
    char *dir = make_tmpdir();
    write_config(dir, "{ this is not json ");
    char *err = NULL;
    oci_config *cfg = oci_config_load(dir, &err);
    CHECK(cfg == NULL, "malformed json rejected");
    CHECK(err != NULL, "parse error reported");
    free(err);
    cleanup_dir(dir);
}

int main(void)
{
    log_init("error"); /* keep unrelated warnings quiet during tests */
    test_minimal_valid();
    test_missing_ociversion();
    test_missing_args();
    test_userns_requires_maps();
    test_malformed_json();

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
