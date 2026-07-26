#!/bin/sh
# harness.sh - end-to-end lifecycle test.
#
# Spins up the busybox bundle, walks the container through
# create -> state(created) -> start -> state(running/stopped) -> delete, and
# checks the state transitions. Requires Linux with unprivileged user
# namespaces OR root, plus a populated rootfs. On systems lacking these it
# exits 0 with a skip message so `make test` stays green in constrained CI.
#
# Usage: RT_BIN=build/runtime sh tests/harness.sh
set -u

RT_BIN="${RT_BIN:-build/runtime}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="$ROOT_DIR/bundles/busybox"
CID="harness-$$"

WORK="$(mktemp -d /tmp/harness.XXXXXX)"
export RT_STATE_DIR="$WORK/state"
export RT_CGROUP_ROOT="$WORK/cgroup"   # fake; keeps us off the host hierarchy
export RT_LOG="${RT_LOG:-warn}"
mkdir -p "$RT_STATE_DIR" "$RT_CGROUP_ROOT"

FAILED=0
pass() { echo "  ok:   $1"; }
fail() { echo "  FAIL: $1"; FAILED=1; }

cleanup() {
    "$RT_BIN" delete "$CID" --force >/dev/null 2>&1 || true
    rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# --- preconditions ----------------------------------------------------------

if [ ! -x "$RT_BIN" ]; then
    echo "SKIP: runtime binary not found at $RT_BIN"; exit 0
fi
if [ "$(uname -s)" != "Linux" ]; then
    echo "SKIP: lifecycle harness requires Linux (found $(uname -s))"; exit 0
fi
if [ ! -x "$BUNDLE/rootfs/bin/sh" ] && [ ! -L "$BUNDLE/rootfs/bin/sh" ]; then
    echo "SKIP: no rootfs at $BUNDLE/rootfs (run bundles/busybox/fetch-rootfs.sh)"; exit 0
fi
if [ "$(id -u)" != "0" ]; then
    if ! unshare --user --map-root-user true >/dev/null 2>&1; then
        echo "SKIP: need root or unprivileged userns support"; exit 0
    fi
fi

echo "harness: bundle=$BUNDLE id=$CID state=$RT_STATE_DIR"

status_of() {
    "$RT_BIN" state "$CID" 2>/dev/null | tr -d ' \t\n' | \
        sed -n 's/.*"status":"\([a-z]*\)".*/\1/p'
}

# --- create -----------------------------------------------------------------

if "$RT_BIN" create "$CID" --bundle "$BUNDLE"; then
    pass "create returned 0"
else
    fail "create failed"; exit 1
fi

st="$(status_of)"
[ "$st" = "created" ] && pass "state is 'created'" || fail "state is '$st' (want created)"

# --- start ------------------------------------------------------------------

if "$RT_BIN" start "$CID"; then
    pass "start returned 0"
else
    fail "start failed"
fi

sleep 1
st="$(status_of)"
if [ "$st" = "running" ] || [ "$st" = "stopped" ]; then
    pass "state is '$st' after start"
else
    fail "state is '$st' after start (want running/stopped)"
fi

# --- wait for stop ----------------------------------------------------------

for _ in 1 2 3 4 5; do
    st="$(status_of)"
    [ "$st" = "stopped" ] && break
    sleep 1
done
[ "$st" = "stopped" ] && pass "container reached 'stopped'" \
                       || fail "container did not stop (state '$st')"

# --- delete -----------------------------------------------------------------

if "$RT_BIN" delete "$CID"; then
    pass "delete returned 0"
else
    fail "delete failed"
fi

if "$RT_BIN" state "$CID" >/dev/null 2>&1; then
    fail "state still present after delete"
else
    pass "container gone after delete"
fi

echo ""
if [ "$FAILED" = "0" ]; then
    echo "harness PASSED"; exit 0
else
    echo "harness FAILED"; exit 1
fi
