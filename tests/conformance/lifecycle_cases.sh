#!/bin/sh
# tests/conformance/lifecycle_cases.sh
#
# A focused set of OCI lifecycle conformance checks that assert the state
# machine's *rules*. The privilege-free cases (1-4) always run; the cases that
# need to actually create a container (5-6) run only where namespaces are
# available, and are skipped otherwise.
#
#   1. `state` on a nonexistent container fails.
#   2. `start` on a nonexistent container fails.
#   3. `delete` on a nonexistent container fails.
#   4. `create` with a bad bundle path fails cleanly.
#   5. double `create` of the same id fails (no clobber).
#   6. `start` may only be issued once (second start rejected).
#
# Usage: RT_BIN=build/runtime sh tests/conformance/lifecycle_cases.sh
set -u

RT_BIN="${RT_BIN:-build/runtime}"
WORK="$(mktemp -d /tmp/conf.XXXXXX)"
export RT_STATE_DIR="$WORK/state"
export RT_CGROUP_ROOT="$WORK/cgroup"
export RT_LOG=error
mkdir -p "$RT_STATE_DIR" "$RT_CGROUP_ROOT"

PASS=0; FAIL=0
ok()  { echo "  ok:   $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

cleanup() { rm -rf "$WORK" 2>/dev/null || true; }
trap cleanup EXIT

if [ ! -x "$RT_BIN" ]; then echo "SKIP: no binary at $RT_BIN"; exit 0; fi

# 1. state on missing container
if "$RT_BIN" state ghost >/dev/null 2>&1; then bad "state(missing) should fail"
else ok "state on missing container fails"; fi

# 2. start on missing container
if "$RT_BIN" start ghost >/dev/null 2>&1; then bad "start(missing) should fail"
else ok "start on missing container fails"; fi

# 3. delete on missing container
if "$RT_BIN" delete ghost >/dev/null 2>&1; then bad "delete(missing) should fail"
else ok "delete on missing container fails"; fi

# 4. create with a bad bundle path
if "$RT_BIN" create c1 --bundle /nonexistent/bundle >/dev/null 2>&1; then
    bad "create(bad bundle) should fail"
    "$RT_BIN" delete c1 --force >/dev/null 2>&1 || true
else
    ok "create with bad bundle fails cleanly"
fi

# 5-6 need a runnable container. Use a minimal bundle with /bin/true.
have_ns=0
if [ "$(id -u)" = "0" ]; then have_ns=1
elif unshare --user --map-root-user true >/dev/null 2>&1; then have_ns=1; fi

if [ "$have_ns" = "1" ] && [ "$(uname -s)" = "Linux" ]; then
    BUNDLE="$WORK/bundle"
    mkdir -p "$BUNDLE/rootfs/bin"
    # Provide a real static payload if the busybox bundle already has one.
    SRC_SH="$(cd "$(dirname "$0")/../.." && pwd)/bundles/busybox/rootfs/bin/sh"
    if [ -x "$SRC_SH" ]; then cp "$SRC_SH" "$BUNDLE/rootfs/bin/sh"; fi
    cat > "$BUNDLE/config.json" <<'JSON'
{
  "ociVersion": "1.0.2",
  "process": { "args": ["/bin/sh"], "cwd": "/" },
  "root": { "path": "rootfs" },
  "linux": { "namespaces": [ { "type": "uts" }, { "type": "pid" }, { "type": "mount" } ] }
}
JSON

    if [ -x "$BUNDLE/rootfs/bin/sh" ] && "$RT_BIN" create c2 --bundle "$BUNDLE" >/dev/null 2>&1; then
        # 5. no-clobber
        if "$RT_BIN" create c2 --bundle "$BUNDLE" >/dev/null 2>&1; then
            bad "second create of same id should fail"
        else
            ok "double create of same id fails"
        fi
        # 6. single start
        "$RT_BIN" start c2 >/dev/null 2>&1
        sleep 1
        if "$RT_BIN" start c2 >/dev/null 2>&1; then
            bad "second start should fail"
        else
            ok "start may only be issued once"
        fi
        "$RT_BIN" delete c2 --force >/dev/null 2>&1 || true
    else
        echo "  skip: cases 5-6 (could not create test container)"
    fi
else
    echo "  skip: cases 5-6 (no namespace support)"
fi

echo ""
echo "conformance: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
