#!/usr/bin/env bash
set -Eeuo pipefail

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Git tags are the release source of truth. Source snapshots without .git should
# be visibly non-release dev builds rather than carrying a stale VERSION file.
snapshot="$tmp/snapshot"
mkdir -p "$snapshot/.github/scripts"
cp Makefile "$snapshot/"
cp .github/scripts/resolve_version.sh "$snapshot/.github/scripts/"
snapshot_version="$(cd "$snapshot" && env -u GITHUB_SHA -u GITHUB_REF_TYPE -u GITHUB_REF_NAME bash .github/scripts/resolve_version.sh)"
if [ "$snapshot_version" != "dev" ]; then
  printf 'source snapshot resolver mismatch: got %s want dev\n' "$snapshot_version" >&2
  exit 1
fi
snapshot_make="$(cd "$snapshot" && env -u GITHUB_SHA -u GITHUB_REF_TYPE -u GITHUB_REF_NAME make -s --no-print-directory print-version)"
if [ "$snapshot_make" != "dev" ]; then
  printf 'source snapshot Makefile version mismatch: got %s want dev\n' "$snapshot_make" >&2
  exit 1
fi

# A pushed release tag is the release source of truth.
tag_value="$(GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v9.8.7 bash .github/scripts/resolve_version.sh)"
if [ "$tag_value" != "9.8.7" ]; then
  printf 'tag release version mismatch: got %s want 9.8.7\n' "$tag_value" >&2
  exit 1
fi

tag_base="$(GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v9.8.7 bash .github/scripts/resolve_version.sh --base)"
if [ "$tag_base" != "9.8.7" ]; then
  printf 'tag release base mismatch: got %s want 9.8.7\n' "$tag_base" >&2
  exit 1
fi

out="$tmp/github-output"
GITHUB_OUTPUT="$out" GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v9.8.7 bash .github/scripts/resolve_version.sh --github-output
grep -qx 'value=9.8.7' "$out"
grep -qx 'base=9.8.7' "$out"
grep -qx 'tag=v9.8.7' "$out"

gitcase="$tmp/gitcase"
mkdir -p "$gitcase/.github/scripts"
cp Makefile "$gitcase/"
cp .github/scripts/resolve_version.sh "$gitcase/.github/scripts/"
(
  cd "$gitcase"
  unset GITHUB_SHA GITHUB_REF_TYPE GITHUB_REF_NAME
  git init -q
  git config user.name test
  git config user.email test@example.invalid
  git add .
  git commit -qm base

  dev_short="$(git rev-parse --short HEAD)"
  untagged_make="$(make -s --no-print-directory print-version)"
  if [ "$untagged_make" != "dev-$dev_short" ]; then
    printf 'untagged Makefile version mismatch: got %s want dev-%s\n' "$untagged_make" "$dev_short" >&2
    exit 1
  fi

  git tag v9.8.7
  tagged_make="$(make -s --no-print-directory print-version)"
  if [ "$tagged_make" != "v9.8.7" ]; then
    printf 'exact tag Makefile version mismatch: got %s want v9.8.7\n' "$tagged_make" >&2
    exit 1
  fi
  tagged_base="$(bash .github/scripts/resolve_version.sh --base)"
  if [ "$tagged_base" != "9.8.7" ]; then
    printf 'exact tag resolver base mismatch: got %s want 9.8.7\n' "$tagged_base" >&2
    exit 1
  fi
  printf '\n# dirty\n' >> Makefile
  dirty_make="$(make -s --no-print-directory print-version)"
  if [ "$dirty_make" != "v9.8.7-dirty" ]; then
    printf 'dirty exact tag Makefile version mismatch: got %s want v9.8.7-dirty\n' "$dirty_make" >&2
    exit 1
  fi
  dirty_resolved="$(env -u GITHUB_SHA bash .github/scripts/resolve_version.sh)"
  short="$(git rev-parse --short HEAD)"
  if [ "$dirty_resolved" != "9.8.7-$short-dirty" ]; then
    printf 'dirty exact tag resolver mismatch: got %s want 9.8.7-%s-dirty\n' "$dirty_resolved" "$short" >&2
    exit 1
  fi
  git checkout -q -- Makefile

  printf 'change\n' > later.txt
  git add later.txt
  git commit -qm later
  short="$(git rev-parse --short HEAD)"
  dev_make="$(make -s --no-print-directory print-version)"
  if [ "$dev_make" != "9.8.7-$short" ]; then
    printf 'post-tag Makefile version mismatch: got %s want 9.8.7-%s\n' "$dev_make" "$short" >&2
    exit 1
  fi
)

bumps="$tmp/bumps"
mkdir -p "$bumps/scripts"
cp scripts/bump_version.sh "$bumps/scripts/bump_version.sh"
(
  cd "$bumps"
  git init -q
  git config user.name test
  git config user.email test@example.invalid
  touch file
  git add file
  git commit -qm base
  git tag v0.4.4
  [ "$(bash scripts/bump_version.sh patch)" = "v0.4.5" ]
  [ "$(bash scripts/bump_version.sh minor)" = "v0.5.0" ]
  [ "$(bash scripts/bump_version.sh major)" = "v1.0.0" ]
  [ "$(bash scripts/bump_version.sh custom 2.3.4)" = "v2.3.4" ]
)

releasecase="$tmp/releasecase"
mkdir -p "$releasecase/.github/scripts" "$releasecase/bin" "$releasecase/scripts"
cp scripts/release_build.sh "$releasecase/scripts/release_build.sh"
cp .github/scripts/resolve_version.sh "$releasecase/.github/scripts/resolve_version.sh"
cat >"$releasecase/.github/scripts/package_tarball.sh" <<'SHSTUB'
#!/usr/bin/env bash
set -euo pipefail
target="$1"
version="$2"
binary="$3"
out="$4"
mkdir -p "$out"
printf '%s %s %s\n' "$target" "$version" "$binary" >> "$out/packages.log"
: > "$out/hold-$version-$target.tar.gz"
SHSTUB
chmod +x "$releasecase/.github/scripts/package_tarball.sh"
cat >"$releasecase/bin/git" <<'SHSTUB'
#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = -C ]; then shift 2; fi
case "${1:-}" in
  rev-parse)
    if [ "${2:-}" = --is-inside-work-tree ]; then exit 0; fi
    if [ "${2:-}" = --short ]; then printf '%s\n' abcdef1; exit 0; fi
    printf '%s\n' abcdef1234567890abcdef1234567890abcdef12
    ;;
  describe)
    case "${FAKE_GIT_MODE:-tag-clean}" in
      tag-clean|tag-dirty) printf '%s\n' v9.8.7 ;;
      branch-clean)
        if printf '%s\n' "$*" | grep -q -- '--exact-match'; then exit 1; fi
        printf '%s\n' v9.8.7
        ;;
    esac
    ;;
  status)
    case "${FAKE_GIT_MODE:-tag-clean}" in
      tag-dirty) printf '%s\n' ' M src/example.c' ;;
    esac
    ;;
  diff) exit 0 ;;
  *) printf 'unexpected fake git command: %s\n' "$*" >&2; exit 2 ;;
esac
SHSTUB
chmod +x "$releasecase/bin/git"
cat >"$releasecase/bin/make" <<'SHSTUB'
#!/usr/bin/env bash
set -euo pipefail
case "$*" in
  clean) rm -f hold hold-dynamic ;;
  hold\ STATIC_LDFLAGS=-static|hold\ STATIC_LDFLAGS='-static')
    cat > hold <<'BIN'
#!/usr/bin/env bash
printf '%s\n' v9.8.7
BIN
    chmod +x hold
    ;;
  hold-dynamic\ STATIC_LDFLAGS=*)
    cat > hold-dynamic <<'BIN'
#!/usr/bin/env bash
printf '%s\n' v9.8.7
BIN
    chmod +x hold-dynamic
    ;;
  *) printf 'unexpected fake make args: %s\n' "$*" >&2; exit 2 ;;
esac
SHSTUB
chmod +x "$releasecase/bin/make"
cat >"$releasecase/bin/uname" <<'SHSTUB'
#!/usr/bin/env bash
case "${1:-}" in
  -s) printf '%s\n' Linux ;;
  -m) printf '%s\n' x86_64 ;;
  *) printf '%s\n' Linux ;;
esac
SHSTUB
chmod +x "$releasecase/bin/uname"
cat >"$releasecase/bin/file" <<'SHSTUB'
#!/usr/bin/env bash
printf '%s: fake executable\n' "$1"
SHSTUB
chmod +x "$releasecase/bin/file"
(
  cd "$releasecase"
  env -u GITHUB_SHA -u GITHUB_REF_TYPE -u GITHUB_REF_NAME PATH="$releasecase/bin:/usr/bin:/bin" FAKE_GIT_MODE=tag-clean bash scripts/release_build.sh >tag-clean.out 2>tag-clean.err
  grep -qx 'linux-amd64-gnu-static 9.8.7 hold' dist/packages.log
  grep -qx 'linux-amd64-gnu-dynamic 9.8.7 hold-dynamic' dist/packages.log
)
(
  cd "$releasecase"
  rm -rf dist
  env -u GITHUB_SHA -u GITHUB_REF_TYPE -u GITHUB_REF_NAME PATH="$releasecase/bin:/usr/bin:/bin" FAKE_GIT_MODE=branch-clean bash scripts/release_build.sh >branch-clean.out 2>branch-clean.err
  grep -qx 'linux-amd64-gnu-static 9.8.7-abcdef1 hold' dist/packages.log
  grep -qx 'linux-amd64-gnu-dynamic 9.8.7-abcdef1 hold-dynamic' dist/packages.log
)
(
  cd "$releasecase"
  rm -rf dist
  if env -u GITHUB_SHA -u GITHUB_REF_TYPE -u GITHUB_REF_NAME PATH="$releasecase/bin:/usr/bin:/bin" FAKE_GIT_MODE=tag-dirty bash scripts/release_build.sh >tag-dirty.out 2>tag-dirty.err; then
    printf 'release_build unexpectedly allowed dirty tagged worktree\n' >&2
    exit 1
  fi
  grep -q 'refusing to build clean release artifacts from dirty tagged worktree' tag-dirty.err
  [ ! -e dist/packages.log ]
)
