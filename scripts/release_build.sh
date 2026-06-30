#!/usr/bin/env bash
# Build the release-artifact matrix locally and package each tarball, mirroring
# .github/workflows/release.yml so a contributor can reproduce and *test* real
# release artifacts without GitHub.
#
#   - On Linux (typically inside scripts/linux.sh's pinned CI container): builds
#     the native gnu static + dynamic binaries and, when zig is present, the full
#     musl cross matrix (amd64/arm64/armhf/mipsel/riscv64).
#   - On macOS: builds the native macOS binary. The cross targets that need a
#     different OS/arch come from `scripts/linux.sh release` or CI.
#
# Usage:
#   scripts/release_build.sh [output_dir]     # default: dist/ ; native host
#   scripts/linux.sh release  [output_dir]    # the Linux matrix, in-container
#
# Build commands are kept byte-identical to release.yml; every tarball is
# packaged with .github/scripts/package_tarball.sh (the same script CI uses) and
# the binary is run-verified whenever its arch matches the host.
set -uo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

out="${1:-dist}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
ver="$(bash .github/scripts/resolve_version.sh --base)"
os="$(uname -s)"
hostm="$(uname -m)"
pkg="./.github/scripts/package_tarball.sh"
log="${TMPDIR:-/tmp}/release_build.$$.log"

ok=0; fail=0; skip=0
section() { printf '\n----- %s -----\n' "$*"; }
show()    { file "$1" | sed 's/^/    /'; }
runv()    { ./"$1" --version | sed "s|^|    runs ($1): |"; }
emit() { # target  binary
  if "$pkg" "$1" "$ver" "$2" "$out" >/dev/null; then
    printf '    packaged: hold-%s-%s.tar.gz\n' "$ver" "$1"; ok=$((ok + 1))
  else
    printf '    PACKAGE FAILED: %s\n' "$1" >&2; fail=$((fail + 1))
  fi
}
bfail() { printf '    BUILD FAILED:\n'; tail -15 "$log" | sed 's/^/      /'; fail=$((fail + 1)); }

case "$os" in
Darwin)
  section "macOS native ($hostm)"
  make clean >/dev/null
  if make CC=clang STATIC_LDFLAGS='' \
       CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2' >"$log" 2>&1; then
    show hold; runv hold; emit "macos-$hostm" hold
  else bfail; fi
  ;;
Linux)
  case "$hostm" in
  aarch64) gnu_arch=arm64 ;;
  x86_64)  gnu_arch=amd64 ;;
  *)       gnu_arch="$hostm" ;;
  esac

  section "Linux gnu native ($hostm -> $gnu_arch) static"
  make clean >/dev/null
  if make hold STATIC_LDFLAGS='-static' >"$log" 2>&1; then
    show hold; runv hold; emit "linux-$gnu_arch-gnu-static" hold
  else bfail; fi

  section "Linux gnu native ($gnu_arch) dynamic"
  make clean >/dev/null
  if make hold-dynamic STATIC_LDFLAGS='' >"$log" 2>&1; then
    show hold-dynamic; emit "linux-$gnu_arch-gnu-dynamic" hold-dynamic
  else bfail; fi

  if command -v zig >/dev/null 2>&1; then
    for pair in \
      x86_64-linux-musl:linux-amd64-musl-static \
      aarch64-linux-musl:linux-arm64-musl-static \
      arm-linux-musleabihf:linux-armhf-musl-static \
      mipsel-linux-musl:linux-mipsel-musl-static \
      riscv64-linux-musl:linux-riscv64-musl-static; do
      zt="${pair%%:*}"; target="${pair##*:}"
      section "Linux musl $target  (zig cc -target $zt)"
      make clean >/dev/null
      if make CC="zig cc -target $zt" \
           CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O2 -s' \
           STATIC_LDFLAGS='-static' >"$log" 2>&1; then
        show hold
        [ "$target" = "linux-$gnu_arch-musl-static" ] && runv hold
        emit "$target" hold
      else bfail; fi
    done
  else
    section "Linux musl matrix"
    echo "    SKIPPED: zig not found — run 'scripts/linux.sh release' (its image ships pinned zig)"
    skip=$((skip + 1))
  fi
  ;;
*)
  echo "release_build: unsupported OS '$os'" >&2
  exit 1
  ;;
esac

rm -f "$log"
section "artifacts in $out"
ls -la "$out"/hold-"$ver"-*.tar.gz 2>/dev/null | awk '{print "   ", $NF, $5"B"}'
printf '\n===== release build: %d ok, %d failed, %d skipped =====\n' "$ok" "$fail" "$skip"
[ "$fail" -eq 0 ]
