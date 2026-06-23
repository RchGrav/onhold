#!/usr/bin/env bash
# Privilege-delegation lane: run the regression suite as a NON-root user that can
# elevate via passwordless sudo, and let the suite elevate per-test.
#
# This mirrors how sigmund is actually used and must be tested: you invoke it as
# yourself, and it sudo-elevates only the operations that need root — and the
# elevated process must know WHICH non-root user invoked it (the SUDO_UID/
# SUDO_USER provenance), not merely that it is running as root. Running the whole
# suite as root would erase that distinction (and leave root-owned build
# artifacts a non-root cleanup cannot remove, which is exactly what used to break
# this lane). The suite's as_user/as_root/as_sudo_from_user helpers do the
# per-test elevation; SIGMUND_REQUIRE_ROOT_TESTS=1 turns any skip into a failure,
# so this lane proves the elevated tests really executed (that is its job, vs the
# user-runner which merely permits them to skip).
#
# Shared verbatim by the GitHub CI lane (runs as the non-root 'runner') and by
# scripts/linux.sh (runs this as the container's non-root 'ci' sudoer).
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
  echo "test_root.sh: run me as a NON-root user with passwordless sudo, not as root." >&2
  echo "  This lane models the real 'invoke as a user, then sudo-elevate' flow so the" >&2
  echo "  elevated tests see a genuine SUDO_UID/SUDO_USER. In the container use" >&2
  echo "  'scripts/linux.sh root' (it runs this as the non-root 'ci' sudoer)." >&2
  exit 1
fi
if ! command -v sudo >/dev/null 2>&1 || ! sudo -n true >/dev/null 2>&1; then
  echo "test_root.sh: need passwordless sudo — the elevated tests must be able to run," >&2
  echo "  and SIGMUND_REQUIRE_ROOT_TESTS=1 makes a missing root actor a hard failure." >&2
  exit 1
fi

src="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
werror="${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -Werror -O2}"

# Build + run in a private temp copy so the host/source tree is never mutated.
# Everything here runs as this one non-root user, so the EXIT-trap cleanup owns
# what it removes.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/src"
# Anchor excludes to repo-root paths: a bare --exclude=sigmund would also drop
# the include/sigmund/ header directory.
tar --exclude='./.git' --exclude='./sigmund' --exclude='./sigmund-dynamic' \
    --exclude='./hash-vector' --exclude='./obj' --exclude='./obj-test' \
    --exclude='./sigmund.dSYM' --exclude='./sigmund-dynamic.dSYM' \
    -C "$src" -cf - . | tar -xf - -C "$tmp/src"

cd "$tmp/src"
umask 022
make clean
SIGMUND_REQUIRE_ROOT_TESTS=1 make test CFLAGS="$werror"
