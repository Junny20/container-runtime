#include "runtime/container.h"
#include "runtime/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog;

static void usage(FILE *out)
{
    fprintf(out,
        "Usage:\n"
        "  %s create <id> --bundle <dir>   Create a container from a bundle\n"
        "  %s start  <id>                  Start a created container\n"
        "  %s state  <id>                  Print container state as JSON\n"
        "  %s delete <id> [--force]        Delete a container\n"
        "\n"
        "Environment:\n"
        "  RT_LOG=debug|info|warn|error    Log verbosity (default info)\n"
        "  RT_STATE_DIR=<dir>              State directory (default /run/oci-runtime)\n"
        "  RT_CGROUP_ROOT=<dir>            cgroup v2 root (default /sys/fs/cgroup)\n",
        prog, prog, prog, prog);
}

/* Find "--<name>" in argv[start..argc) and return the following value, or NULL. */
static const char *opt_value(int argc, char **argv, int start, const char *name)
{
    for (int i = start; i < argc; i++) {
        if (!strcmp(argv[i], name)) {
            if (i + 1 >= argc) {
                log_error("option %s requires a value", name);
                return NULL;
            }
            return argv[i + 1];
        }
    }
    return NULL;
}

static bool has_flag(int argc, char **argv, int start, const char *name)
{
    for (int i = start; i < argc; i++)
        if (!strcmp(argv[i], name)) return true;
    return false;
}

int main(int argc, char **argv)
{
    prog = argv[0];
    log_init(getenv("RT_LOG"));

    if (argc < 2) { usage(stderr); return 2; }

    const char *verb = argv[1];

    if (!strcmp(verb, "-h") || !strcmp(verb, "--help") || !strcmp(verb, "help")) {
        usage(stdout);
        return 0;
    }

    if (!strcmp(verb, "create")) {
        if (argc < 3) { log_error("create requires <id>"); usage(stderr); return 2; }
        const char *id = argv[2];
        const char *bundle = opt_value(argc, argv, 3, "--bundle");
        if (!bundle) {
            log_error("create requires --bundle <dir>");
            return 2;
        }
        return container_create(id, bundle);
    }

    if (!strcmp(verb, "start")) {
        if (argc < 3) { log_error("start requires <id>"); return 2; }
        return container_start(argv[2]);
    }

    if (!strcmp(verb, "state")) {
        if (argc < 3) { log_error("state requires <id>"); return 2; }
        return container_state_print(argv[2]);
    }

    if (!strcmp(verb, "delete")) {
        if (argc < 3) { log_error("delete requires <id>"); return 2; }
        bool force = has_flag(argc, argv, 3, "--force");
        return container_delete(argv[2], force);
    }

    log_error("unknown verb: %s", verb);
    usage(stderr);
    return 2;
}
