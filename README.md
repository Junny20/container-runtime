# OCI-Compliant Container Runtime (C, Linux)

A small `runc`-style container runtime written from scratch in C. It turns an
OCI bundle on disk (a root filesystem plus a `config.json`) into an isolated
process, and manages that process through the OCI lifecycle verbs
`create` / `start` / `state` / `delete`.

The point of the project is to work directly against the syscalls the container
abstraction is actually made of — namespaces, `pivot_root`, and cgroup v2 —
rather than treating a container as a magic kernel object. A container here is
just an ordinary process that has been placed in its own namespaces, given a
private filesystem view, and constrained by a cgroup.

> **Status:** builds clean under `-Wall -Wextra` and additional warnings; all
> unit tests, lifecycle conformance cases, and the end-to-end harness pass,
> including under AddressSanitizer + UndefinedBehaviorSanitizer. See
> [Testing](#testing).

## What it does

- **Lifecycle state machine** over the OCI verbs, with state persisted to disk
  so separate CLI invocations can find and act on a container:

  ```
  (none) --create--> CREATED --start--> RUNNING --(process exits)--> STOPPED
      ^                 |                                              |
      +----- delete ----+--------------------- delete ----------------+
  ```

- **Namespace isolation**: PID, mount, UTS, IPC, and network namespaces give the
  container its own process tree, mounts, hostname, IPC objects, and network
  stack.
- **Filesystem view**: mounts the bundle rootfs and switches into it with
  `pivot_root`, then mounts a fresh `/proc` so the isolated PID namespace is
  reflected correctly.
- **Resource limits**: creates a cgroup v2 subtree and writes the CPU, memory,
  and PID controllers before the user process starts; tears the cgroup down on
  `delete` and on abnormal exit.
- **Config parsing**: parses and validates the supported subset of
  `config.json` with a vendored JSON parser, and *surfaces* unsupported-but-
  present fields as warnings rather than silently ignoring them.

## Deliberate simplifications

Two choices favor clarity and debuggability over matching every `runc` detail.
Both are defensible and both have a natural "what would you change" follow-up.

- **`fork` + `unshare` instead of `clone(2)` with namespace flags.** The child
  forks first, then `unshare()`s into its new namespaces, so the setup steps are
  explicit calls that are easy to sequence and debug. The tradeoff: a small
  window exists between `fork` and `unshare`, and because `unshare(CLONE_NEWPID)`
  only affects *future* children, the child must fork an "init" grandchild that
  becomes PID 1 in the new namespace. `clone(2)` would have the child born
  already inside the namespaces with no such window.

- **cgroup v2 only.** The unified hierarchy is simpler than v1's per-controller
  mounts and is the modern default.

## The parent/child handshake

Correct setup ordering is the crux of the design. Some steps must happen from
*outside* the namespaces (writing uid/gid maps, cgroup membership) and some from
*inside* (hostname, `pivot_root`, `/proc`). The runtime coordinates this with a
pipe handshake:

```
parent (create)                         child
  fork ───────────────────────────────▶ unshare(namespaces)
  wait on child_ready  ◀─────────────── signal "unshared"
  write uid/gid maps
  add child to cgroup
  signal "proceed" ──────────────────▶ wait on parent_ready
                                         set hostname
                                         fork init grandchild ──▶ pivot_root
                                                                  mount /proc
                                                                  execvpe(user process)
```

### create/start across two processes: the supervisor

The OCI model makes `create` and `start` **separate CLI invocations**. An
anonymous pipe write-end cannot survive between two unrelated processes, so the
`create` process **daemonizes into a supervisor** that holds the child blocked
before `exec` and listens on a named FIFO in the container's state directory.
`start` opens that FIFO and writes one byte; the supervisor releases the child,
flips the state file to `running`, then `waitpid`s the container and flips it to
`stopped`. The FIFO is unlinked as soon as it is consumed, so a container can
only be started once.

*(This is the main deviation from the simplest single-process pipe story, and is
the obvious "what would you change" — e.g. a persistent daemon or an
`fd`-passing socket instead of re-daemonizing per container.)*

## `pivot_root` vs `chroot`

`chroot` only changes a process's root directory; it does not detach the old
filesystem tree, which stays reachable (via an open fd, `..`, etc.), making it
escapable and leaving host mounts visible. `pivot_root` swaps the root mount and
lets the runtime unmount and remove the old root entirely, so no host mount
remains reachable inside the container.

## Layout

```
container-runtime/
├── Makefile
├── include/runtime/       public headers (one per subsystem)
├── src/
│   ├── main.c             CLI dispatch: create/start/state/delete
│   ├── oci.c              parse + validate config.json
│   ├── container.c        lifecycle state machine, state-file I/O
│   ├── process.c          fork+unshare, parent/child sync pipe, supervisor
│   ├── namespaces.c       unshare, uid/gid map writes, hostname
│   ├── mounts.c           bind rootfs, pivot_root, mount /proc etc.
│   ├── cgroup.c           cgroup v2: create, write controllers, teardown
│   └── log.c
├── third_party/cJSON/     vendored JSON parser (MIT)
├── tests/
│   ├── test_oci.c         config parsing unit tests
│   ├── test_cgroup.c      controller write/read-back tests
│   ├── conformance/       lifecycle-rule conformance cases
│   └── harness.sh         end-to-end create→start→state→delete
└── bundles/busybox/       sample OCI bundle (config.json + rootfs builder)
```

## Building

```sh
make            # release build       -> build/runtime
make debug      # ASan/UBSan build    -> build/runtime-debug
make test       # unit + conformance + harness
make bench      # measure cold-start, memory overhead, conformance
make clean
```

Requires a C11 compiler and, to *run*, Linux with cgroup v2 and either root or
unprivileged user-namespace support. The parser and state code compile and are
unit-tested on any platform.

## Running

First populate the sample rootfs (uses docker, a host `busybox`, or a static C
smoke-test payload — whichever is available):

```sh
./bundles/busybox/fetch-rootfs.sh
```

Then walk a container through its lifecycle:

```sh
sudo ./build/runtime create demo --bundle bundles/busybox
sudo ./build/runtime state  demo         # -> "status": "created"
sudo ./build/runtime start  demo         # user process runs
sudo ./build/runtime state  demo         # -> "running" / "stopped"
sudo ./build/runtime delete demo
```

Environment variables (handy for rootless testing without touching host paths):

| Variable          | Default            | Purpose                      |
|-------------------|--------------------|------------------------------|
| `RT_LOG`          | `info`             | `debug`/`info`/`warn`/`error`|
| `RT_STATE_DIR`    | `/run/oci-runtime` | where state files live       |
| `RT_CGROUP_ROOT`  | `/sys/fs/cgroup`   | cgroup v2 mount point         |

## Testing

`make test` runs three layers:

1. **Unit tests** (`test_oci`, `test_cgroup`), built with **ASan + UBSan**.
   They cover config parsing/validation (including the user-namespace-needs-maps
   rule and malformed input) and cgroup controller-file formatting against a
   fake cgroup root. *ASan caught a real use-after-free during development —
   `cJSON_GetErrorPtr()` returns a pointer into the JSON text buffer, which was
   being read after the buffer was freed.*
2. **Lifecycle conformance** (`tests/conformance/lifecycle_cases.sh`): asserts
   the state-machine rules — missing-container verbs fail, bad bundles fail
   cleanly, no id clobbering, start-only-once.
3. **End-to-end harness** (`tests/harness.sh`): runs the real
   `create → start → state → delete` cycle against the sample bundle and checks
   every transition. On the reference run the payload prints `pid=1` and the
   container hostname, and lists a freshly mounted `/proc`, confirming the PID
   and UTS namespaces and `pivot_root` + `/proc` are all working.

The conformance and harness scripts self-skip (exit 0) where namespaces or a
rootfs are unavailable, so `make test` stays green in constrained CI while still
running the sanitized unit tests.

## What to measure

Run the benchmark harness to get real numbers for your host:

```sh
make bench          # 50 cold-start iterations (default)
make bench N=200    # more iterations for a tighter estimate
```

It measures and prints a summary of three things:

- **Cold-start latency** (fork → user process running): the runtime is
  instrumented (behind `$RT_BENCH_FILE`, zero overhead when unset) to record a
  `CLOCK_MONOTONIC` timestamp at the fork point and another in the init
  grandchild right before `execvpe`, with the idle "waiting for `start`"
  interval subtracted. Reports mean / p50 / min / max in ms. **Prefer p50** for
  a resume figure — it is robust to the occasional scheduling outlier on a
  virtualized host.
- **Memory overhead per container**: RSS of the container's init process while
  it sleeps, minus the same payload run bare (isolation overhead), plus the
  supervisor process's RSS (the persistent per-container bookkeeping cost). The
  supervisor dominates; the isolation overhead on the workload itself is ~0.
- **Conformance cases passing**: from the lifecycle conformance script (**6**).

Reference run on the development host (values are host-specific — regenerate on
yours):

```
  cold-start (fork->exec) : ~2.9 ms mean / ~2.7 ms p50
  memory overhead / ctr   : ~5 MB (almost entirely the supervisor process)
  conformance cases       : 6 passing
```

When comparing to `runc`, scope the comparison honestly: this runtime omits
seccomp, AppArmor, and user-namespace ID mapping in the default path, so it
starts faster precisely because it does less. Frame any comparison as
**core-lifecycle only**. The ~5 MB is dominated by the supervisor daemon; a
production design would amortize that (a shared daemon or fd-passing socket
rather than one supervisor per container).

## Not implemented (by design)

seccomp filters, AppArmor/SELinux, capability dropping, device cgroup rules,
`mounts[]` beyond the rootfs and `/proc`,`/sys`,`/dev`, and a real init/reaper
beyond the single grandchild. Each is a reasonable next increment.

## License

The vendored `cJSON` parser is under the MIT license (see
`third_party/cJSON/LICENSE`). The runtime code itself is provided as-is for
study and portfolio use.
