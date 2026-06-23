#!/usr/bin/env bash
set -Eeuo pipefail

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

make_fake_release() {
  local root="$1" artifact="$2" mode="${3:-good}" release_dir stage
  release_dir="$root/RchGrav/sigmund/releases/download/v1.2.3"
  stage="$root/stage-$mode"
  mkdir -p "$release_dir" "$stage"
  case "$mode" in
    good)
      cat >"$stage/sigmund" <<'SH'
#!/bin/sh
if [ "${1:-}" = "--version" ]; then
  printf '%s\n' 'v1.2.3'
  exit 0
fi
exit 0
SH
      cp "$stage/sigmund" "$stage/mund"
      chmod 0755 "$stage/sigmund" "$stage/mund"
      ;;
    missing-root-binary)
      mkdir -p "$stage/bin"
      cat >"$stage/bin/sigmund" <<'SH'
#!/bin/sh
printf '%s\n' 'wrong layout'
SH
      chmod 0755 "$stage/bin/sigmund"
      ;;
  esac
  tar -C "$stage" -czf "$release_dir/$artifact" .
}

run_installer() {
  local root="$1" install_dir="$2" extra_env="${3:-}"
  env \
    SIGMUND_GITHUB_BASE="file://$root" \
    SIGMUND_INSTALL_TEST_OS=linux \
    SIGMUND_INSTALL_TEST_ARCH=amd64 \
    SIGMUND_INSTALL_TEST_LIBC=gnu \
    SIGMUND_INSTALL_DIR="$install_dir" \
    SIGMUND_UPDATE_PROFILE=0 \
    $extra_env \
    sh ./install.sh 1.2.3
}

expect_fail() {
  local desc="$1"; shift
  if "$@" >"$tmp/$desc.out" 2>"$tmp/$desc.err"; then
    printf 'expected installer failure for %s\n' "$desc" >&2
    cat "$tmp/$desc.out" "$tmp/$desc.err" >&2
    exit 1
  fi
}

test_installer_fail_closed() {
  local root release_dir artifact sum install_dir
  artifact="sigmund-1.2.3-linux-amd64-gnu-static.tar.gz"

  root="$tmp/missing-sums"
  install_dir="$tmp/install-missing"
  make_fake_release "$root" "$artifact" good
  expect_fail missing-sums run_installer "$root" "$install_dir"
  grep -q 'missing SHA256SUMS' "$tmp/missing-sums.err"

  root="$tmp/malformed-sums"
  install_dir="$tmp/install-malformed"
  make_fake_release "$root" "$artifact" good
  release_dir="$root/RchGrav/sigmund/releases/download/v1.2.3"
  printf 'not-a-sha  %s\n' "$artifact" >"$release_dir/SHA256SUMS"
  expect_fail malformed-sums run_installer "$root" "$install_dir"
  grep -q 'malformed SHA256SUMS' "$tmp/malformed-sums.err"

  root="$tmp/mismatch-sums"
  install_dir="$tmp/install-mismatch"
  make_fake_release "$root" "$artifact" good
  release_dir="$root/RchGrav/sigmund/releases/download/v1.2.3"
  printf '%064d  %s\n' 0 "$artifact" >"$release_dir/SHA256SUMS"
  expect_fail mismatch-sums run_installer "$root" "$install_dir"
  grep -q 'checksum mismatch' "$tmp/mismatch-sums.err"

  root="$tmp/bad-layout"
  install_dir="$tmp/install-layout"
  make_fake_release "$root" "$artifact" missing-root-binary
  release_dir="$root/RchGrav/sigmund/releases/download/v1.2.3"
  sum="$(sha256_file "$release_dir/$artifact")"
  printf '%s  %s\n' "$sum" "$artifact" >"$release_dir/SHA256SUMS"
  expect_fail bad-layout run_installer "$root" "$install_dir"
  grep -q 'archive layout is invalid' "$tmp/bad-layout.err"

  expect_fail unsupported-platform env SIGMUND_INSTALL_TEST_OS=SunOS SIGMUND_INSTALL_DRY_RUN=1 sh ./install.sh 1.2.3
  grep -q 'unsupported operating system' "$tmp/unsupported-platform.err"

  root="$tmp/good"
  install_dir="$tmp/install-good"
  make_fake_release "$root" "$artifact" good
  release_dir="$root/RchGrav/sigmund/releases/download/v1.2.3"
  sum="$(sha256_file "$release_dir/$artifact")"
  printf '%s  %s\n' "$sum" "$artifact" >"$release_dir/SHA256SUMS"
  run_installer "$root" "$install_dir" >"$tmp/good.out" 2>"$tmp/good.err"
  [ "$("$install_dir/sigmund" --version)" = "v1.2.3" ]
  [ "$("$install_dir/mund" --version)" = "v1.2.3" ]
}

test_package_tarball_deterministic_when_gnu_tar() {
  if ! tar --version 2>/dev/null | grep -qi 'gnu tar'; then
    return 0
  fi
  local bin out1 out2 a1 a2 s1 s2
  bin="$tmp/fake-sigmund"
  cat >"$bin" <<'SH'
#!/bin/sh
exit 0
SH
  chmod 0755 "$bin"
  out1="$tmp/pkg1"
  out2="$tmp/pkg2"
  a1="$(.github/scripts/package_tarball.sh linux-amd64-gnu-static 1.2.3 "$bin" "$out1")"
  tar -tzf "$a1" | sed 's#^./##' | sort >"$tmp/pkg-files.txt"
  grep -qx 'sigmund' "$tmp/pkg-files.txt"
  grep -qx 'mund' "$tmp/pkg-files.txt"
  sleep 1
  a2="$(.github/scripts/package_tarball.sh linux-amd64-gnu-static 1.2.3 "$bin" "$out2")"
  s1="$(sha256_file "$a1")"
  s2="$(sha256_file "$a2")"
  [ "$s1" = "$s2" ] || {
    printf 'package_tarball is not deterministic: %s != %s\n' "$s1" "$s2" >&2
    exit 1
  }
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

test_installer_fail_closed
test_package_tarball_deterministic_when_gnu_tar
