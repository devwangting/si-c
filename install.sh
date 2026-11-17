#!/usr/bin/env bash
#
# Build si-c and install it into a directory on PATH.
#
# Usage:
#   ./install.sh                  # build, then sudo install to /usr/local/bin
#   PREFIX=$HOME/.local ./install.sh
#   SUDO_PASSWORD=... ./install.sh
#
set -euo pipefail

cd "$(dirname "$0")"

PREFIX="${PREFIX:-/usr/local}"
BINDIR="${BINDIR:-$PREFIX/bin}"
TARGET="$BINDIR/si"

if ! command -v make >/dev/null 2>&1; then
    echo "error: make not found" >&2
    exit 1
fi
if ! command -v cc >/dev/null 2>&1; then
    echo "error: cc not found" >&2
    exit 1
fi

echo "==> building si-c"
make -j

run_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif [ -n "${SUDO_PASSWORD:-}" ]; then
        printf '%s\n' "$SUDO_PASSWORD" | sudo -S -p '' "$@"
    else
        sudo "$@"
    fi
}

need_root=0
if [ "$(id -u)" -ne 0 ]; then
    if [ -d "$BINDIR" ]; then
        [ -w "$BINDIR" ] || need_root=1
    else
        [ -w "$(dirname "$BINDIR")" ] || need_root=1
    fi
fi

echo "==> installing to $TARGET"
if [ "$need_root" -eq 1 ]; then
    run_root install -d "$BINDIR"
    run_root install -m 0755 si "$TARGET"
else
    install -d "$BINDIR"
    install -m 0755 si "$TARGET"
fi

if [ ! -x "$TARGET" ]; then
    echo "error: install failed: $TARGET is not executable" >&2
    exit 1
fi

echo "==> installed: $TARGET"
case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo "warning: $BINDIR is not in PATH; add it to use 'si' from anywhere" >&2 ;;
esac
if command -v si >/dev/null 2>&1; then
    echo "==> command: $(command -v si)"
fi
