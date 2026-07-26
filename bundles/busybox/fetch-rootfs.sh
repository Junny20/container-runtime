#!/bin/sh
# fetch-rootfs.sh - populate bundles/busybox/rootfs with a minimal filesystem.
#
# The rootfs itself is not checked into git (it is binary and large-ish); this
# script builds it. Strategies tried in order:
#
#   1. If `docker` is available: export the busybox image's filesystem.
#   2. Else if a `busybox` binary is on the host: copy it and symlink applets.
#   3. Else if a C compiler is available: build a tiny static payload that
#      prints its pid/hostname/proc listing, placed at bin/sh. This lets the
#      lifecycle harness run anywhere with a compiler, even without busybox.
#
# Run from anywhere; paths are resolved relative to this script.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOTFS="$HERE/rootfs"

mkdir -p "$ROOTFS"
for d in bin sbin etc proc sys dev tmp root usr/bin usr/sbin var; do
    mkdir -p "$ROOTFS/$d"
done
chmod 1777 "$ROOTFS/tmp"

have() { command -v "$1" >/dev/null 2>&1; }

# minimal /etc so tools do not complain
[ -f "$ROOTFS/etc/passwd" ] || echo "root:x:0:0:root:/root:/bin/sh" > "$ROOTFS/etc/passwd"
[ -f "$ROOTFS/etc/group" ]  || echo "root:x:0:" > "$ROOTFS/etc/group"
[ -f "$ROOTFS/etc/hostname" ] || echo "container" > "$ROOTFS/etc/hostname"

# --- strategy 1: docker export ---------------------------------------------
if have docker; then
    echo "populating rootfs from the busybox docker image..."
    cid="$(docker create busybox:latest 2>/dev/null || docker create busybox 2>/dev/null || true)"
    if [ -n "${cid:-}" ]; then
        docker export "$cid" | tar -x -C "$ROOTFS"
        docker rm "$cid" >/dev/null 2>&1 || true
        echo "rootfs populated via docker export -> $ROOTFS"
        exit 0
    fi
    echo "docker present but could not create busybox container; falling back"
fi

# --- strategy 2: host busybox ----------------------------------------------
if have busybox; then
    echo "copying host busybox binary..."
    cp "$(command -v busybox)" "$ROOTFS/bin/busybox"
    chmod +x "$ROOTFS/bin/busybox"
    for applet in sh ls echo cat hostname mount umount ps id sleep true false env; do
        ln -sf busybox "$ROOTFS/bin/$applet"
    done
    echo "rootfs populated with host busybox -> $ROOTFS"
    exit 0
fi

# --- strategy 3: static C payload ------------------------------------------
if have cc || have gcc || have clang; then
    CC="$(command -v cc || command -v gcc || command -v clang)"
    echo "no busybox/docker; building a static test payload with $CC..."
    tmp="$(mktemp /tmp/payload.XXXXXX.c)"
    cat > "$tmp" <<'EOF'
/* Minimal static container payload: prints pid, hostname, and a bit of /proc so
 * we can visually confirm namespace + pivot_root + /proc mount worked. */
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
int main(void) {
    char host[256] = {0};
    gethostname(host, sizeof host - 1);
    printf("payload: pid=%d hostname=%s\n", (int)getpid(), host);
    DIR *d = opendir("/proc");
    if (d) {
        printf("proc entries:");
        struct dirent *e; int n = 0;
        while ((e = readdir(d)) && n < 6) { printf(" %s", e->d_name); n++; }
        printf("\n");
        closedir(d);
    } else {
        printf("(/proc not readable)\n");
    }
    fflush(stdout);
    return 0;
}
EOF
    if "$CC" -static -O2 "$tmp" -o "$ROOTFS/bin/sh" 2>/dev/null; then
        chmod +x "$ROOTFS/bin/sh"
        rm -f "$tmp"
        echo "rootfs populated with static payload -> $ROOTFS/bin/sh"
        echo "note: this payload ignores argv; it is a lifecycle smoke-test, not a real shell"
        exit 0
    fi
    rm -f "$tmp"
    echo "static build failed (no static libc?); try installing busybox" >&2
fi

echo "could not populate rootfs: need docker, busybox, or a C compiler with static libc" >&2
exit 1
