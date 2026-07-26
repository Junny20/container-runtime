#include "runtime/process.h"
#include "runtime/namespaces.h"
#include "runtime/mounts.h"
#include "runtime/cgroup.h"
#include "runtime/container.h"
#include "runtime/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>

/* ---- fork-to-exec benchmarking --------------------------------------- *
 * When $RT_BENCH_FILE is set, process_create records a CLOCK_MONOTONIC
 * timestamp at the fork point, and the init grandchild records another right
 * before execvpe(). The elapsed nanoseconds are appended (one integer per line)
 * to $RT_BENCH_FILE. This measures exactly the "fork -> user process about to
 * run" interval: fork + unshare + the parent's external setup + the pipe
 * handshake + pivot_root + /proc mount. With the var unset there is zero
 * overhead (no clock reads, no writes). */

static long long g_bench_fork_ns = -1;  /* stamped at the fork point       */
static long long g_bench_idle_ns = 0;   /* time spent blocked awaiting start */
static int       g_bench_fd      = -1;  /* pre-opened $RT_BENCH_FILE fd     */

static long long monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Open the bench output file and keep the fd. Must be called BEFORE pivot_root,
 * because after the mount-namespace switch the host path is no longer
 * reachable — but an already-open fd survives. No-op unless RT_BENCH_FILE is
 * set. */
static void bench_open(void)
{
    const char *path = getenv("RT_BENCH_FILE");
    if (!path) return;
    g_bench_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

/* Append one elapsed-ns sample via the pre-opened fd. Called in the init
 * grandchild just before exec. The reported interval is:
 *
 *     (exec_time - fork_time) - idle_blocked_time
 *
 * i.e. the active fork+unshare+setup+pivot_root+/proc work, with the time the
 * child sat blocked between `create` and `start` removed. */
static void bench_record_exec(void)
{
    if (g_bench_fd < 0 || g_bench_fork_ns < 0)
        return;
    long long elapsed = (monotonic_ns() - g_bench_fork_ns) - g_bench_idle_ns;
    if (elapsed < 0) elapsed = 0;
    char line[64];
    int n = snprintf(line, sizeof line, "%lld\n", elapsed);
    if (n <= 0) return;
    ssize_t w = write(g_bench_fd, line, (size_t)n);
    (void)w;
    close(g_bench_fd);
    g_bench_fd = -1;
}


/* ---- sync pipe primitives -------------------------------------------- *
 * These synchronize the fork (foreground) parent with its immediate child
 * while both live in the same create invocation. They are anonymous pipes and
 * are only valid within that single process tree. */

int sync_pipe_open(sync_pipe *sp)
{
    sp->parent_to_child[0] = sp->parent_to_child[1] = -1;
    sp->child_to_parent[0] = sp->child_to_parent[1] = -1;
    if (pipe(sp->parent_to_child) != 0) { log_errno("pipe p2c"); return -1; }
    if (pipe(sp->child_to_parent) != 0) {
        log_errno("pipe c2p");
        close(sp->parent_to_child[0]);
        close(sp->parent_to_child[1]);
        sp->parent_to_child[0] = sp->parent_to_child[1] = -1;
        return -1;
    }
    return 0;
}

void sync_pipe_close_all(sync_pipe *sp)
{
    for (int i = 0; i < 2; i++) {
        if (sp->parent_to_child[i] >= 0) close(sp->parent_to_child[i]);
        if (sp->child_to_parent[i] >= 0) close(sp->child_to_parent[i]);
        sp->parent_to_child[i] = -1;
        sp->child_to_parent[i] = -1;
    }
}

static int read_byte(int fd)
{
    char b;
    ssize_t r;
    do { r = read(fd, &b, 1); } while (r < 0 && errno == EINTR);
    if (r == 1) return 0;
    if (r == 0) { errno = EPIPE; return -1; }
    return -1;
}

static int write_byte(int fd)
{
    char b = 1;
    ssize_t w;
    do { w = write(fd, &b, 1); } while (w < 0 && errno == EINTR);
    return (w == 1) ? 0 : -1;
}

int sync_wait_parent(sync_pipe *sp)  { return read_byte(sp->parent_to_child[0]); }
int sync_signal_child(sync_pipe *sp) { return write_byte(sp->parent_to_child[1]); }
int sync_signal_parent(sync_pipe *sp){ return write_byte(sp->child_to_parent[1]); }
int sync_wait_child(sync_pipe *sp)   { return read_byte(sp->child_to_parent[0]); }

/* The path of the "start" FIFO the supervisor listens on. */
static void start_fifo_path(const char *id, char *out, size_t sz)
{
    char dir[4000];
    container_state_dir(id, dir, sizeof dir);
    snprintf(out, sz, "%s/start.fifo", dir);
}

/* ---- child side ------------------------------------------------------- */

typedef struct {
    const oci_config *cfg;
    container_state  *st;
    sync_pipe        *sp;
} child_arg;

/* The container init (pid 1 in the new pid namespace). Performs the filesystem
 * setup that must happen after the pid namespace is active, then execs. */
static _Noreturn void run_init(const oci_config *cfg)
{
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    /* Open the benchmark output fd BEFORE pivot_root, while the host path is
     * still reachable; the fd survives the mount-namespace switch. No-op unless
     * benchmarking. */
    bench_open();

    if (mounts_setup_rootfs(cfg) != 0) {
        log_error("init: rootfs setup failed");
        _exit(126);
    }
    if (chdir(cfg->cwd) != 0) {
        log_errno("init: chdir(%s)", cfg->cwd);
        if (chdir("/") != 0) _exit(126);
    }

    static char *empty_env[] = { NULL };
    char **envp = cfg->env ? cfg->env : empty_env;

    /* Benchmark hook: the user process is fully set up and about to run. This is
     * the "fork -> user process running" end point. No-op unless RT_BENCH_FILE
     * is set. */
    bench_record_exec();

    execvpe(cfg->args[0], cfg->args, envp);
    log_errno("init: execvpe(%s)", cfg->args[0]);
    _exit(127);
}

/* The child: unshares, signals the parent, then blocks until the parent (after
 * doing external setup) releases it. It then sets the hostname and forks init,
 * acting as init's reaper and propagating its exit status. */
static _Noreturn void run_child(child_arg *ca)
{
    const oci_config *cfg = ca->cfg;
    sync_pipe *sp = ca->sp;

    close(sp->parent_to_child[1]); sp->parent_to_child[1] = -1;
    close(sp->child_to_parent[0]); sp->child_to_parent[0] = -1;

    if (ns_unshare(&cfg->ns) != 0)
        _exit(125);

    if (sync_signal_parent(sp) != 0) {
        log_error("child: failed to signal parent");
        _exit(125);
    }

    /* Benchmark: measure how long we sit blocked waiting for `start`, so it can
     * be subtracted from the cold-start figure. No-op unless benchmarking. */
    long long block_start = (g_bench_fork_ns >= 0) ? monotonic_ns() : 0;
    if (sync_wait_parent(sp) != 0) {
        log_error("child: parent aborted before release");
        _exit(125);
    }
    if (g_bench_fork_ns >= 0)
        g_bench_idle_ns += monotonic_ns() - block_start;

    ns_set_hostname(&cfg->ns, cfg->hostname);

    pid_t init = fork();
    if (init < 0) { log_errno("child: fork init"); _exit(125); }
    if (init == 0)
        run_init(cfg);

    int status;
    while (waitpid(init, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status))   _exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) _exit(128 + WTERMSIG(status));
    _exit(1);
}

/* ---- supervisor ------------------------------------------------------- *
 * After setup, the create process daemonizes into a supervisor. It:
 *   1. creates a FIFO under the state dir,
 *   2. blocks on opening/reading the FIFO (this is `start` releasing it),
 *   3. releases the child over the still-open anonymous pipe,
 *   4. flips the state file to RUNNING,
 *   5. waitpid()s the child, then flips the state file to STOPPED. */
static _Noreturn void run_supervisor(container_state *st, sync_pipe *sp)
{
    char fifo[4096];
    start_fifo_path(st->id, fifo, sizeof fifo);

    /* Create the start FIFO (idempotent). */
    if (mkfifo(fifo, 0600) != 0 && errno != EEXIST) {
        log_errno("supervisor: mkfifo %s", fifo);
        kill(st->pid, SIGKILL);
        _exit(1);
    }

    /* Block until `start` opens the FIFO for writing and sends a byte. Opening
     * a FIFO read side blocks until a writer appears, which is exactly the
     * release signal we want. */
    int ffd = open(fifo, O_RDONLY);
    if (ffd < 0) {
        log_errno("supervisor: open fifo");
        kill(st->pid, SIGKILL);
        _exit(1);
    }
    char b;
    ssize_t rr = read(ffd, &b, 1);
    (void)rr; /* a writer appeared; content is irrelevant, EOF is fine too */
    close(ffd);

    /* Remove the FIFO immediately so a second `start` observes ENOENT and is
     * rejected: the container may only be started once. */
    unlink(fifo);

    /* Release the blocked child so it execs the user process. */
    if (sync_signal_child(sp) != 0) {
        log_error("supervisor: failed to release child");
        kill(st->pid, SIGKILL);
        _exit(1);
    }
    sync_pipe_close_all(sp);

    /* Mark RUNNING in the state file. */
    container_state running = *st;
    running.status = STATE_RUNNING;
    container_state_save(&running);

    /* Reap the container. */
    int status;
    while (waitpid(st->pid, &status, 0) < 0 && errno == EINTR)
        ;

    /* Mark STOPPED. The cgroup/mount teardown happens in `delete`; here we only
     * update status so `state` reports correctly. */
    container_state stopped = *st;
    stopped.status = STATE_STOPPED;
    container_state_save(&stopped);

    log_debug("supervisor: container %s stopped (status=%d)", st->id, status);
    _exit(0);
}

/* Daemonize the current process into the background supervisor: detach from the
 * controlling terminal and redirect std fds. */
static void daemonize_supervisor(void)
{
    if (setsid() < 0)
        log_errno("supervisor: setsid (continuing)");

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        if (getenv("RT_DAEMON_QUIET"))
            dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
}

/* ---- parent side ------------------------------------------------------ */

int process_create(const oci_config *cfg, container_state *st)
{
    sync_pipe sp;
    if (sync_pipe_open(&sp) != 0)
        return -1;

    /* Benchmark hook: stamp the fork point. Inherited by the child/init across
     * fork(). No-op (just an unused value) unless RT_BENCH_FILE is set. */
    if (getenv("RT_BENCH_FILE"))
        g_bench_fork_ns = monotonic_ns();

    pid_t child = fork();
    if (child < 0) {
        log_errno("fork child");
        sync_pipe_close_all(&sp);
        return -1;
    }
    if (child == 0) {
        child_arg ca = { .cfg = cfg, .st = st, .sp = &sp };
        run_child(&ca);
    }

    /* Parent keeps only its ends. */
    close(sp.parent_to_child[0]); sp.parent_to_child[0] = -1;
    close(sp.child_to_parent[1]); sp.child_to_parent[1] = -1;

    st->pid = child;

    /* Wait for the child to confirm it has unshared. */
    if (sync_wait_child(&sp) != 0) {
        log_error("parent: child failed during unshare");
        goto abort_child;
    }

    /* External setup performed by the parent, in order:
     *   (a) uid/gid maps (only with a user namespace),
     *   (b) cgroup membership. */
    if (ns_write_id_maps(child, cfg) != 0) {
        log_error("parent: writing id maps failed");
        goto abort_child;
    }
    if (st->cgroup_path[0] &&
        cgroup_add_pid(st->cgroup_path, child) != 0) {
        /* Non-fatal: a container can run without cgroup accounting (e.g. when
         * cgroup v2 is unavailable or not delegated). Warn, drop the cgroup
         * path so teardown does not try to rmdir a partial tree, and proceed. */
        log_warn("parent: could not add child to cgroup; continuing without limits");
        st->cgroup_path[0] = '\0';
    }

    st->status = STATE_CREATED;

    /* Persist CREATED state before we fork off the supervisor so that even if
     * the caller exits immediately, `state`/`start` can find the container. */
    if (container_state_save(st) != 0) {
        log_errno("parent: saving created state");
        goto abort_child;
    }

    /* Fork the supervisor. The original foreground process returns to the
     * caller (which will exit), reporting CREATED. The supervisor keeps the
     * child blocked, waiting for `start`. */
    pid_t sup = fork();
    if (sup < 0) {
        log_errno("fork supervisor");
        goto abort_child;
    }
    if (sup == 0) {
        daemonize_supervisor();
        run_supervisor(st, &sp); /* never returns */
    }

    /* Foreground: it must NOT hold the pipe write end open, or the child could
     * never distinguish release from foreground exit. Close our copies; the
     * supervisor holds the authoritative ones. */
    sync_pipe_close_all(&sp);
    return 0;

abort_child:
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
        ;
    sync_pipe_close_all(&sp);
    st->status = STATE_STOPPED;
    st->pid = -1;
    return -1;
}
