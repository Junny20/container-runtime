# OCI-Compliant Container Runtime (C, Linux)

A small `runc`-style container runtime written from scratch in C. It turns an
OCI bundle on disk (a root filesystem plus a `config.json`) into an isolated
process, and manages that process through the OCI lifecycle verbs
`create` / `start` / `state` / `delete`.

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