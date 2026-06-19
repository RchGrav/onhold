#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo_root"

fail() {
  printf 'macos-ci: error: %s\n' "$*" >&2
  exit 1
}

capture() {
  local out="$1"
  shift
  "$@" >"$out" 2>&1
}

[ "$(uname -s)" = Darwin ] || fail "expected Darwin runner, got $(uname -s)"

actual_arch=$(uname -m)
expected_arch="${SIGMUND_EXPECT_UNAME_M:-}"
if [ -n "$expected_arch" ] && [ "$actual_arch" != "$expected_arch" ]; then
  fail "expected runner arch $expected_arch, got $actual_arch"
fi

version=$(sed -n '1s/[[:space:]]*$//p' VERSION)
[ -n "$version" ] || fail "missing VERSION"

make_args=(
  CC=clang
  STATIC_LDFLAGS=
  CFLAGS=-std=c11\ -Wall\ -Wextra\ -Wpedantic\ -Werror\ -O2
)

make clean
make "${make_args[@]}"

file_out=$(file ./sigmund)
printf '%s\n' "$file_out"
printf '%s\n' "$file_out" | grep -Fq 'Mach-O' || fail "sigmund is not a Mach-O binary"
case "$actual_arch" in
  arm64) printf '%s\n' "$file_out" | grep -Fq 'arm64' || fail "arm64 runner did not build arm64 binary" ;;
  x86_64) printf '%s\n' "$file_out" | grep -Fq 'x86_64' || fail "x86_64 runner did not build x86_64 binary" ;;
  *) fail "unsupported macOS runner arch: $actual_arch" ;;
esac

./sigmund help system | grep -Fq 'macOS: /var/db/sigmund' || fail "macOS system store help is missing"
./sigmund --version | grep -Eq "^(v?${version}|${version}-|dev|[0-9a-f]{7,40}).*$" || fail "unexpected version output"

make clean
make test "${make_args[@]}"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

default_out="$tmpdir/install-default.out"
capture "$default_out" env SIGMUND_VERSION="$version" SIGMUND_INSTALL_DRY_RUN=1 sh install.sh
case "$actual_arch" in
  arm64) grep -Fq "artifact: sigmund-${version}-macos-arm64.tar.gz" "$default_out" || fail "default installer did not select arm64 macOS artifact" ;;
  x86_64) grep -Fq "artifact: sigmund-${version}-macos-x86_64.tar.gz" "$default_out" || fail "default installer did not select x86_64 macOS artifact" ;;
esac

system_out="$tmpdir/install-system.out"
capture "$system_out" env SIGMUND_VERSION="$version" SIGMUND_INSTALL_DRY_RUN=1 sh install.sh --system
grep -Fq 'mode:     system' "$system_out" || fail "system dry-run did not select system mode"
grep -Fq 'install:  /usr/local/bin/sigmund' "$system_out" || fail "system dry-run did not target /usr/local/bin"

arm_out="$tmpdir/install-arm64.out"
capture "$arm_out" env SIGMUND_VERSION="$version" SIGMUND_INSTALL_DRY_RUN=1 SIGMUND_INSTALL_TEST_OS=Darwin SIGMUND_INSTALL_TEST_ARCH=arm64 sh install.sh
grep -Fq "artifact: sigmund-${version}-macos-arm64.tar.gz" "$arm_out" || fail "arm64 installer simulation selected wrong artifact"

x64_out="$tmpdir/install-x86_64.out"
capture "$x64_out" env SIGMUND_VERSION="$version" SIGMUND_INSTALL_DRY_RUN=1 SIGMUND_INSTALL_TEST_OS=Darwin SIGMUND_INSTALL_TEST_ARCH=x86_64 sh install.sh
grep -Fq "artifact: sigmund-${version}-macos-x86_64.tar.gz" "$x64_out" || fail "x86_64 installer simulation selected wrong artifact"

bad_out="$tmpdir/install-bad-arch.out"
if env SIGMUND_VERSION="$version" SIGMUND_INSTALL_DRY_RUN=1 SIGMUND_INSTALL_TEST_OS=Darwin SIGMUND_INSTALL_TEST_ARCH=armv7l sh install.sh >"$bad_out" 2>&1; then
  fail "unsupported macOS arch dry-run unexpectedly succeeded"
fi
grep -Fq 'unsupported macOS architecture' "$bad_out" || fail "unsupported macOS arch error was unclear"

printf 'macos-ci: %s checks passed for %s\n' "$version" "$actual_arch"
