#!/usr/bin/env bash
# Root-runner lane: build and run the regression suite as root, so the
# sudo/sudoers/private-store tests that the user-runner can only skip actually
# execute. The tree is copied into a world-readable temp dir first, because the
# harness creates an unprivileged test-user actor that must read/execute the
# build. SIGMUND_REQUIRE_ROOT_TESTS=1 makes any skip a hard failure, so this lane
# can never silently no-op. Shared verbatim by the GitHub CI root-runner job and
# by scripts/linux.sh.
set -euo pipefail
src="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
export SIGMUND_WERROR="${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -Werror -O2}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/src"
# Anchor excludes to repo-root paths: a bare --exclude=sigmund would also drop
# the include/sigmund/ header directory.
tar --exclude='./.git' --exclude='./sigmund' --exclude='./sigmund-dynamic' \
    --exclude='./hash-vector' --exclude='./obj' --exclude='./obj-test' \
    --exclude='./sigmund.dSYM' --exclude='./sigmund-dynamic.dSYM' \
    -C "$src" -cf - . | tar -xf - -C "$tmp/src"
chmod -R a+rX "$tmp"

run_cmd='cd "$1"; umask 022; make clean; SIGMUND_REQUIRE_ROOT_TESTS=1 make test CFLAGS="$SIGMUND_WERROR"'
if [ "$(id -u)" -eq 0 ]; then
  bash -c "$run_cmd" bash "$tmp/src"
else
  command -v sudo >/dev/null 2>&1 || { echo "test_root.sh: needs root (no sudo found)" >&2; exit 1; }
  sudo -E bash -c "$run_cmd" bash "$tmp/src"
fi
