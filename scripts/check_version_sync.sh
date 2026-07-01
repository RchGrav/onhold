#!/usr/bin/env bash
# Fail when current release/status files drift from the shared tag-aware version resolver.
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

base="$(bash .github/scripts/resolve_version.sh --base)"

case "$base" in
  *[!0-9A-Za-z.-]*|'')
    echo "check_version_sync: invalid canonical version: $base" >&2
    exit 1
    ;;
esac

fail=0
note_fail() {
  printf 'check_version_sync: %s\n' "$*" >&2
  fail=1
}

make_version="$(make -s --no-print-directory print-version)"
case "$make_version" in
  "$base"|"$base"-*|"v$base"|"v$base"-*) ;;
  *) note_fail "Makefile print-version ($make_version) is not derived from resolved base ($base)" ;;
esac

# Current user-facing release/status files must not keep stale VERSION warnings.
for file in README.md REVIEW.md; do
  [ -f "$file" ] || continue
  if grep -Eq 'VERSION (still says|remains|is still|file remains) `?0[.]3[.]9`?|0[.]4[.]0 has not been released while `?VERSION`? remains `?0[.]3[.]9`?' "$file"; then
    note_fail "$file contains stale 0.3.9 current-version wording"
  fi
  if grep -Eq 'Current branch version file remains|VERSION remains `?0[.]3[.]9`?' "$file"; then
    note_fail "$file contains stale current branch version wording"
  fi
done

# Local release builds mirror CI naming from the shared resolver's full value,
# so branch/snapshot artifacts include the same commit suffix as the binary.
if ! grep -Fq '.github/scripts/resolve_version.sh)' scripts/release_build.sh; then
  note_fail "scripts/release_build.sh does not use the shared full-version resolver"
fi
if ! grep -Fq 'refusing to build clean release artifacts from dirty tagged worktree' scripts/release_build.sh; then
  note_fail "scripts/release_build.sh does not guard dirty tagged release builds"
fi
if ! grep -Fq '.github/scripts/resolve_version.sh --base' .github/scripts/test_macos_ci.sh; then
  note_fail ".github/scripts/test_macos_ci.sh does not use the shared version resolver"
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
printf 'version sync: %s\n' "$base"
