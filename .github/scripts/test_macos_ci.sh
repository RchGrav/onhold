#!/usr/bin/env bash
set -Eeuo pipefail

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

extract_call_id() {
  sed -n '/^[0-9a-f]\{64\}$/p' | head -n 1
}

run_native_smoke() {
  local tmp home out id
  tmp=$(mktemp -d)
  home="$tmp/home"
  mkdir -p "$home"
  env HOME="$home" ./hold help >/dev/null

  out=$(env HOME="$home" ./hold -d --name mac-smoke -- /bin/sh -c 'echo mac-smoke-line; sleep 2' 2>&1) || {
    printf '%s\n' "$out" >&2
    fail "macOS smoke launch failed"
  }
  id=$(printf '%s\n' "$out" | extract_call_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; fail "macOS smoke did not print the 64-hex call id"; }

  sleep 0.2
  env HOME="$home" ./hold logs -p "$id" >"$tmp/logs.out"
  grep -Fq 'mac-smoke-line' "$tmp/logs.out" || {
    cat "$tmp/logs.out" >&2
    fail "macOS smoke logs did not capture stdout"
  }
  env HOME="$home" ./hold inspect "$id" >"$tmp/inspect.json"
  grep -Fq '"name": "mac-smoke"' "$tmp/inspect.json" || {
    cat "$tmp/inspect.json" >&2
    fail "macOS smoke inspect did not preserve the call name"
  }
  env HOME="$home" ./hold list -a >"$tmp/list.out"
  grep -Eq '^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES' "$tmp/list.out" || {
    cat "$tmp/list.out" >&2
    fail "macOS smoke call table header is wrong"
  }

  env HOME="$home" ./hold save mac-smoke 2>/dev/null
  if env HOME="$home" ./hold purge mac-smoke >/dev/null 2>"$tmp/refusal.err"; then
    fail "purge of a saved call unexpectedly succeeded"
  fi
  grep -Fq 'is saved' "$tmp/refusal.err" || {
    cat "$tmp/refusal.err" >&2
    fail "saved-call purge refusal message missing"
  }
  env HOME="$home" ./hold end mac-smoke >/dev/null 2>&1 || true
  env HOME="$home" ./hold purge mac-smoke --force >/dev/null 2>&1 ||
    fail "purge --force of the saved call failed"
  rm -rf "$tmp"
}

[ "$(uname -s)" = Darwin ] || fail "expected Darwin runner, got $(uname -s)"

actual_arch=$(uname -m)
expected_arch="${HOLD_EXPECT_UNAME_M:-}"
if [ -n "$expected_arch" ] && [ "$actual_arch" != "$expected_arch" ]; then
  fail "expected runner arch $expected_arch, got $actual_arch"
fi

version=$(bash .github/scripts/resolve_version.sh --base)

make_args=(
  CC=clang
  STATIC_LDFLAGS=
  CFLAGS=-std=c11\ -Wall\ -Wextra\ -Wpedantic\ -Werror\ -O2
)

make clean
make "${make_args[@]}"

file_out=$(file ./hold)
printf '%s\n' "$file_out"
printf '%s\n' "$file_out" | grep -Fq 'Mach-O' || fail "hold is not a Mach-O binary"
case "$actual_arch" in
  arm64) printf '%s\n' "$file_out" | grep -Fq 'arm64' || fail "arm64 runner did not build arm64 binary" ;;
  x86_64) printf '%s\n' "$file_out" | grep -Fq 'x86_64' || fail "x86_64 runner did not build x86_64 binary" ;;
  *) fail "unsupported macOS runner arch: $actual_arch" ;;
esac

./hold help system | grep -Fq 'macOS: /var/db/hold' || fail "macOS system store help is missing"
./hold --version | grep -Eq "^(v?${version}|${version}-|dev|[0-9a-f]{7,40}).*$" || fail "unexpected version output"
run_native_smoke

make viewer-filter-test "${make_args[@]}"
bash tests/test_version_makefile.sh
bash tests/test_release_installer.sh

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

default_out="$tmpdir/install-default.out"
capture "$default_out" env HOLD_VERSION="$version" HOLD_INSTALL_DRY_RUN=1 sh install.sh
case "$actual_arch" in
  arm64) grep -Fq "artifact: hold-${version}-macos-arm64.tar.gz" "$default_out" || fail "default installer did not select arm64 macOS artifact" ;;
  x86_64) grep -Fq "artifact: hold-${version}-macos-x86_64.tar.gz" "$default_out" || fail "default installer did not select x86_64 macOS artifact" ;;
esac

system_out="$tmpdir/install-system.out"
capture "$system_out" env HOLD_VERSION="$version" HOLD_INSTALL_DRY_RUN=1 sh install.sh --system
grep -Fq 'mode:     system' "$system_out" || fail "system dry-run did not select system mode"
grep -Fq 'install:  /usr/local/bin/hold' "$system_out" || fail "system dry-run did not target /usr/local/bin"

arm_out="$tmpdir/install-arm64.out"
capture "$arm_out" env HOLD_VERSION="$version" HOLD_INSTALL_DRY_RUN=1 HOLD_INSTALL_TEST_OS=Darwin HOLD_INSTALL_TEST_ARCH=arm64 sh install.sh
grep -Fq "artifact: hold-${version}-macos-arm64.tar.gz" "$arm_out" || fail "arm64 installer simulation selected wrong artifact"

x64_out="$tmpdir/install-x86_64.out"
capture "$x64_out" env HOLD_VERSION="$version" HOLD_INSTALL_DRY_RUN=1 HOLD_INSTALL_TEST_OS=Darwin HOLD_INSTALL_TEST_ARCH=x86_64 sh install.sh
grep -Fq "artifact: hold-${version}-macos-x86_64.tar.gz" "$x64_out" || fail "x86_64 installer simulation selected wrong artifact"

bad_out="$tmpdir/install-bad-arch.out"
if env HOLD_VERSION="$version" HOLD_INSTALL_DRY_RUN=1 HOLD_INSTALL_TEST_OS=Darwin HOLD_INSTALL_TEST_ARCH=armv7l sh install.sh >"$bad_out" 2>&1; then
  fail "unsupported macOS arch dry-run unexpectedly succeeded"
fi
grep -Fq 'unsupported macOS architecture' "$bad_out" || fail "unsupported macOS arch error was unclear"

printf 'macos-ci: %s checks passed for %s\n' "$version" "$actual_arch"
