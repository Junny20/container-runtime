/* test_cgroup.c - controller write/read-back tests.
 *
 * We cannot mount a real cgroup v2 hierarchy in CI, so we point the runtime at
 * a fake root via $RT_CGROUP_ROOT (a temp directory) and pre-create the
 * controller files the code writes to. We then assert the code wrote the
 * expected contents. This tests the *formatting and file targeting* logic of
 * cgroup_apply, independent of the kernel.
 */
#include "runtime/cgroup.h"
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

static char *make_tmpdir(void)
{
    char tmpl[] = "/tmp/cgtest.XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); exit(2); }
    return strdup(d);
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) fclose(f);
}

/* Read file contents into a static buffer, trimming trailing whitespace. */
static const char *read_trim(const char *path)
{
    static char buf[256];
    FILE *f = fopen(path, "rb");
    if (!f) { buf[0] = '\0'; return buf; }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    return buf;
}

static void test_apply_writes_controllers(void)
{
    printf("test_apply_writes_controllers\n");
    char *root = make_tmpdir();
    setenv("RT_CGROUP_ROOT", root, 1);

    char leaf[4096], parent[4096];
    snprintf(parent, sizeof parent, "%s/oci-runtime", root);
    snprintf(leaf, sizeof leaf, "%s/oci-runtime/testc", root);
    mkdir(parent, 0755);
    mkdir(leaf, 0755);

    char f[4096];
    snprintf(f, sizeof f, "%s/memory.max", leaf); touch(f);
    snprintf(f, sizeof f, "%s/cpu.max", leaf);    touch(f);
    snprintf(f, sizeof f, "%s/pids.max", leaf);   touch(f);

    oci_resources res = {
        .memory_limit_bytes = 134217728,
        .cpu_quota_us       = 50000,
        .cpu_period_us      = 100000,
        .pids_limit         = 64,
    };
    int rc = cgroup_apply(leaf, &res);
    CHECK(rc == 0, "cgroup_apply returns 0");

    snprintf(f, sizeof f, "%s/memory.max", leaf);
    CHECK(strcmp(read_trim(f), "134217728") == 0, "memory.max written");

    snprintf(f, sizeof f, "%s/cpu.max", leaf);
    CHECK(strcmp(read_trim(f), "50000 100000") == 0, "cpu.max written as 'quota period'");

    snprintf(f, sizeof f, "%s/pids.max", leaf);
    CHECK(strcmp(read_trim(f), "64") == 0, "pids.max written");

    unsetenv("RT_CGROUP_ROOT");
    snprintf(f, sizeof f, "%s/memory.max", leaf); unlink(f);
    snprintf(f, sizeof f, "%s/cpu.max", leaf);    unlink(f);
    snprintf(f, sizeof f, "%s/pids.max", leaf);   unlink(f);
    rmdir(leaf); rmdir(parent); rmdir(root);
    free(root);
}

static void test_unset_limits_skip_writes(void)
{
    printf("test_unset_limits_skip_writes\n");
    char *root = make_tmpdir();
    setenv("RT_CGROUP_ROOT", root, 1);

    char leaf[4096], parent[4096];
    snprintf(parent, sizeof parent, "%s/oci-runtime", root);
    snprintf(leaf, sizeof leaf, "%s/oci-runtime/testc2", root);
    mkdir(parent, 0755);
    mkdir(leaf, 0755);

    /* Do NOT create controller files. With all-unset limits, cgroup_apply must
     * not attempt any writes. */
    oci_resources res = {
        .memory_limit_bytes = -1,
        .cpu_quota_us       = -1,
        .cpu_period_us      = 0,
        .pids_limit         = -1,
    };
    int rc = cgroup_apply(leaf, &res);
    CHECK(rc == 0, "apply with unset limits returns 0");

    char f[4096];
    snprintf(f, sizeof f, "%s/memory.max", leaf);
    CHECK(access(f, F_OK) != 0, "memory.max not created when unset");

    unsetenv("RT_CGROUP_ROOT");
    rmdir(leaf); rmdir(parent); rmdir(root);
    free(root);
}

int main(void)
{
    log_init("error");
    test_apply_writes_controllers();
    test_unset_limits_skip_writes();

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
