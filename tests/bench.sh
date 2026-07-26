#!/bin/sh
# bench.sh - produce the numbers for the resume/README.
#
# Measures three things and prints a summary table:
#
#   1. Cold-start latency (fork -> user process running), in milliseconds.
#      Uses the runtime's built-in RT_BENCH_FILE instrumentation, which records
#      the active fork+unshare+setup+pivot_root+/proc interval per container
#      (with idle "waiting for start" time subtracted). Reports mean/min/max/p50
#      over N iterations.
#
#   2. Steady-state memory overhead per container, in KB/MB. RSS of the
#      container's init process while it sleeps, minus the RSS of the same
#      payload run bare (so the number is runtime overhead, not the payload's
#      own footprint).
#
#   3. OCI lifecycle conformance cases passing (from the conformance script).
#
# Requires Linux with namespace support (root or unprivileged userns). cgroup v2
# is optional; without it the runtime runs without limits and the numbers still
# reflect the lifecycle cost.
#
# Usage:
#   RT_BIN=build/runtime sh tests/bench.sh [N]
# where N is the iteration count for cold-start (default 50).
set -u

RT_BIN="${RT_BIN:-build/runtime}"
N="${1:-50}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

WORK="$(mktemp -d /tmp/bench.XXXXXX)"
export RT_STATE_DIR="$WORK/state"
export RT_CGROUP_ROOT="${RT_CGROUP_ROOT:-$WORK/cgroup}"
export RT_LOG=error
export RT_DAEMON_QUIET=1
mkdir -p "$RT_STATE_DIR" "$RT_CGROUP_ROOT"

cleanup() {
    # best-effort: delete any leftover containers, then remove workdir
    for d in "$RT_STATE_DIR"/*; do
        [ -d "$d" ] || continue
        id="$(basename "$d")"
        "$RT_BIN" delete "$id" --force >/dev/null 2>&1 || true
    done
    rm -rf "$WORK" 2>/dev/null || true
    [ -n "${BENCH_BUNDLE:-}" ] && rm -rf "$BENCH_BUNDLE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# --- preconditions ----------------------------------------------------------

if [ ! -x "$RT_BIN" ]; then echo "SKIP: no runtime binary at $RT_BIN"; exit 0; fi
if [ "$(uname -s)" != "Linux" ]; then
    echo "SKIP: benchmarks require Linux (found $(uname -s))"; exit 0
fi
if [ "$(id -u)" != "0" ]; then
    if ! unshare --user --map-root-user true >/dev/null 2>&1; then
        echo "SKIP: need root or unprivileged userns support"; exit 0
    fi
fi

# --- build a benchmark bundle whose payload sleeps --------------------------
# We need a long-lived process to sample RSS. Prefer a real busybox from the
# sample bundle; otherwise compile a tiny static "sleep" payload.

BENCH_BUNDLE="$WORK/bundle"
mkdir -p "$BENCH_BUNDLE/rootfs/bin"

SAMPLE_SH="$ROOT_DIR/bundles/busybox/rootfs/bin/sh"
PAYLOAD_KIND="static-sleep"

make_sleeper() {
    CC="$(command -v cc || command -v gcc || command -v clang || true)"
    [ -z "$CC" ] && return 1
    tmp="$(mktemp "$WORK/sleep.XXXXXX.c")"
    cat > "$tmp" <<'EOF'
/* Sleeps for a fixed interval so RSS can be sampled. Static so it needs no
 * shared libs inside the minimal rootfs. */
#include <unistd.h>
int main(void) { sleep(30); return 0; }
EOF
    "$CC" -static -O2 "$tmp" -o "$BENCH_BUNDLE/rootfs/bin/sleeper" 2>/dev/null || return 1
    chmod +x "$BENCH_BUNDLE/rootfs/bin/sleeper"
    return 0
}

if ! make_sleeper; then
    echo "SKIP: need a C compiler with static libc to build the bench payload"
    exit 0
fi

cat > "$BENCH_BUNDLE/config.json" <<'JSON'
{
  "ociVersion": "1.0.2",
  "hostname": "bench",
  "root": { "path": "rootfs" },
  "process": { "args": ["/bin/sleeper"], "cwd": "/" },
  "linux": {
    "namespaces": [
      { "type": "pid" }, { "type": "mount" }, { "type": "uts" },
      { "type": "ipc" }, { "type": "network" }
    ],
    "resources": {
      "memory": { "limit": 134217728 },
      "cpu": { "quota": 50000, "period": 100000 },
      "pids": { "limit": 64 }
    }
  }
}
JSON

echo "benchmark bundle: $BENCH_BUNDLE (payload: $PAYLOAD_KIND)"
echo "iterations: $N"
echo ""

# ============================================================================
# 1. COLD-START LATENCY
# ============================================================================
echo "[1/3] cold-start latency (fork -> user process running) ..."

BENCH_FILE="$WORK/coldstart.ns"
: > "$BENCH_FILE"
export RT_BENCH_FILE="$BENCH_FILE"

i=0; started=0
while [ "$i" -lt "$N" ]; do
    id="cs$i"
    if "$RT_BIN" create "$id" --bundle "$BENCH_BUNDLE" >/dev/null 2>&1; then
        if "$RT_BIN" start "$id" >/dev/null 2>&1; then
            started=$((started + 1))
        fi
        # Let init reach execvpe and write its sample before we tear down.
        sleep 0.05
        "$RT_BIN" delete "$id" --force >/dev/null 2>&1
    fi
    i=$((i + 1))
done
unset RT_BENCH_FILE

# Reduce the ns samples -> ms stats with awk.
if [ -s "$BENCH_FILE" ]; then
    awk -v started="$started" '
    { v[NR] = $1; sum += $1 }
    END {
        n = NR
        if (n == 0) { print "  no samples"; exit }
        # sort for median
        for (i = 1; i <= n; i++)
            for (j = i + 1; j <= n; j++)
                if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
        mean = sum / n
        p50  = v[int((n + 1) / 2)]
        printf "  samples : %d (of %d started)\n", n, started
        printf "  mean    : %.2f ms\n", mean / 1e6
        printf "  p50     : %.2f ms\n", p50  / 1e6
        printf "  min     : %.2f ms\n", v[1] / 1e6
        printf "  max     : %.2f ms\n", v[n] / 1e6
        printf "COLDSTART_MEAN_MS=%.2f\n", mean / 1e6 > "/dev/stderr"
        printf "COLDSTART_P50_MS=%.2f\n", p50 / 1e6 > "/dev/stderr"
    }' "$BENCH_FILE" 2> "$WORK/cs_summary"
    CS_MEAN="$(sed -n 's/COLDSTART_MEAN_MS=//p' "$WORK/cs_summary")"
    CS_P50="$(sed -n 's/COLDSTART_P50_MS=//p' "$WORK/cs_summary")"
else
    echo "  no cold-start samples captured (instrumentation may not have run)"
    CS_MEAN="n/a"
    CS_P50="n/a"
fi
echo ""

# ============================================================================
# 2. MEMORY OVERHEAD PER CONTAINER
# ============================================================================
echo "[2/3] steady-state memory overhead per container ..."

rss_kb_of() {  # $1 = pid
    awk '/^VmRSS:/ { print $2 }' "/proc/$1/status" 2>/dev/null
}

# 2a. Baseline: run the payload bare (no runtime), average RSS over a few reads.
"$BENCH_BUNDLE/rootfs/bin/sleeper" &
BARE_PID=$!
sleep 0.3
bsum=0; bn=0
for _ in 1 2 3; do
    r="$(rss_kb_of "$BARE_PID")"
    [ -n "$r" ] && { bsum=$((bsum + r)); bn=$((bn + 1)); }
    sleep 0.1
done
kill "$BARE_PID" 2>/dev/null; wait "$BARE_PID" 2>/dev/null
BARE_RSS=$([ "$bn" -gt 0 ] && echo $((bsum / bn)) || echo "")

# 2b. Under the runtime: create+start, then sample the container init's RSS and
#     the supervisor's RSS. The supervisor is the persistent per-container
#     bookkeeping process; container-init RSS minus bare RSS is the isolation
#     overhead on the workload itself.
"$RT_BIN" create memc --bundle "$BENCH_BUNDLE" >/dev/null 2>&1
"$RT_BIN" start  memc >/dev/null 2>&1
sleep 0.4
CPID="$("$RT_BIN" state memc 2>/dev/null | tr -d ' \t\n' | sed -n 's/.*"pid":\([0-9]*\).*/\1/p')"

CONT_RSS=""; SUP_RSS=""
if [ -n "$CPID" ]; then
    csum=0; cn=0
    for _ in 1 2 3; do
        r="$(rss_kb_of "$CPID")"
        [ -n "$r" ] && { csum=$((csum + r)); cn=$((cn + 1)); }
        sleep 0.1
    done
    CONT_RSS=$([ "$cn" -gt 0 ] && echo $((csum / cn)) || echo "")

    # The supervisor is the parent of the container-init pid (CPID). Find it.
    SPID="$(awk '/^PPid:/ { print $2 }' "/proc/$CPID/status" 2>/dev/null)"
    [ -n "$SPID" ] && SUP_RSS="$(rss_kb_of "$SPID")"
fi
"$RT_BIN" delete memc --force >/dev/null 2>&1

if [ -n "${BARE_RSS:-}" ] && [ -n "${CONT_RSS:-}" ]; then
    OVERHEAD_KB=$((CONT_RSS - BARE_RSS))
    [ "$OVERHEAD_KB" -lt 0 ] && OVERHEAD_KB=0
    printf "  payload RSS bare        : %s KB\n" "$BARE_RSS"
    printf "  container init RSS      : %s KB\n" "$CONT_RSS"
    printf "  isolation overhead      : %s KB (%.2f MB)\n" \
        "$OVERHEAD_KB" "$(awk "BEGIN{print $OVERHEAD_KB/1024}")"
    if [ -n "${SUP_RSS:-}" ]; then
        printf "  supervisor RSS          : %s KB (%.2f MB, persistent/ctr)\n" \
            "$SUP_RSS" "$(awk "BEGIN{print $SUP_RSS/1024}")"
        TOTAL_KB=$((OVERHEAD_KB + SUP_RSS))
    else
        TOTAL_KB=$OVERHEAD_KB
    fi
    printf "  total overhead / ctr    : %s KB (%.2f MB)\n" \
        "$TOTAL_KB" "$(awk "BEGIN{print $TOTAL_KB/1024}")"
    MEM_MB="$(awk "BEGIN{printf \"%.2f\", $TOTAL_KB/1024}")"
else
    echo "  could not sample RSS (container pid: ${CPID:-none})"
    MEM_MB="n/a"
fi
echo ""

# ============================================================================
# 3. CONFORMANCE CASES
# ============================================================================
echo "[3/3] conformance cases ..."
CONF_OUT="$(RT_BIN="$RT_BIN" sh "$ROOT_DIR/tests/conformance/lifecycle_cases.sh" 2>/dev/null | tail -1)"
echo "  $CONF_OUT"
CONF_N="$(echo "$CONF_OUT" | sed -n 's/conformance: \([0-9]*\) passed.*/\1/p')"
[ -z "$CONF_N" ] && CONF_N="n/a"
echo ""

# ============================================================================
# SUMMARY
# ============================================================================
echo "======================================================"
echo " RESUME NUMBERS (fill the [X] placeholders with these)"
echo "======================================================"
printf "  cold-start (fork->exec) : %s ms mean / %s ms p50  (%s runs)\n" "${CS_MEAN:-n/a}" "${CS_P50:-n/a}" "$N"
printf "  memory overhead / ctr   : %s MB\n" "${MEM_MB:-n/a}"
printf "  conformance cases       : %s passing\n" "${CONF_N:-n/a}"
echo "======================================================"
echo ""
echo "tip: prefer p50 for the resume number — it is robust to the occasional"
echo "scheduling outlier that inflates the mean on a busy/virtualized host."
echo ""
echo "note: measured on THIS host. cold-start is deliberately low because the"
echo "runtime omits seccomp/AppArmor/cap-drop; frame comparisons to runc as"
echo "core-lifecycle only."
