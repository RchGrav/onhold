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

ver="$(bash .github/scripts/resolve_version.sh)"

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  exact_tag="$(git describe --tags --exact-match --match 'v[0-9]*' HEAD 2>/dev/null || true)"
  if [ -n "$exact_tag" ] && [ -n "$(git status --porcelain --untracked-files=all)" ]; then
    echo "release_build: refusing to build clean release artifacts from dirty tagged worktree ($exact_tag)" >&2
    echo "release_build: commit or stash changes, then rerun from the clean tag/CI checkout" >&2
    exit 1
  fi
fi

out="${1:-dist}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
os="$(uname -s)"
hostm="$(uname -m)"
pkg="./.github/scripts/package_tarball.sh"
log="${TMPDIR:-/tmp}/release_build.$$.log"

ok=0; fail=0; skip=0
section() { printf '\n----- %s -----\n' "$*"; }
show()    { file "$1" | sed 's/^/    /'; }
runv() { # binary
  local out
  out="${log}.run"
  if ./"$1" --version >"$out" 2>&1; then
    sed "s|^|    runs ($1): |" "$out"
    return 0
  fi
  printf '    RUN FAILED (%s):\n' "$1" >&2
  sed 's/^/      /' "$out" >&2
  return 1
}
emit() { # target  binary
  if "$pkg" "$1" "$ver" "$2" "$out" >/dev/null; then
    printf '    packaged: hold-%s-%s.tar.gz\n' "$ver" "$1"; ok=$((ok + 1))
  else
    printf '    PACKAGE FAILED: %s\n' "$1" >&2; fail=$((fail + 1))
  fi
}
bfail() { printf '    BUILD FAILED:\n'; tail -15 "$log" | sed 's/^/      /'; fail=$((fail + 1)); }
verify_emit() { # target binary should_run
  if [ "$3" = 1 ] && ! runv "$2"; then
    fail=$((fail + 1))
    return
  fi
  emit "$1" "$2"
}

case "$os" in
Darwin)
  section "macOS native ($hostm)"
  make clean >/dev/null
  if make CC=clang STATIC_LDFLAGS='' \
       CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2' >"$log" 2>&1; then
    show hold; verify_emit "macos-$hostm" hold 1
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
    show hold; verify_emit "linux-$gnu_arch-gnu-static" hold 1
  else bfail; fi

  section "Linux gnu native ($gnu_arch) dynamic"
  make clean >/dev/null
  if make hold-dynamic STATIC_LDFLAGS='' >"$log" 2>&1; then
    show hold-dynamic; verify_emit "linux-$gnu_arch-gnu-dynamic" hold-dynamic 1
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
        if [ "$target" = "linux-$gnu_arch-musl-static" ]; then
          verify_emit "$target" hold 1
        else
          emit "$target" hold
        fi
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

rm -f "$log" "${log}.run"
section "artifacts in $out"
ls -la "$out"/hold-"$ver"-*.tar.gz 2>/dev/null | awk '{print "   ", $NF, $5"B"}'
printf '\n===== release build: %d ok, %d failed, %d skipped =====\n' "$ok" "$fail" "$skip"
[ "$fail" -eq 0 ]
