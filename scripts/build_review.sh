#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

stamp="$(date +%Y%m%d-%H%M%S)"
commit="$(git rev-parse --short=12 HEAD 2>/dev/null || printf unknown)"
branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || printf unknown)"
out_base="${1:-review-builds}"
outdir="$out_base/hold-${stamp}-${commit}"

mkdir -p "$outdir"

# Static/default artifact. On glibc this is a GNU-static build with NSS caveats;
# use the dynamic artifact for normal local review if that matters.
make clean >/dev/null 2>&1 || true
make
cp hold "$outdir/hold-static"

# Dynamic artifact for local review on the build host/distro.
make clean >/dev/null 2>&1 || true
STATIC_LDFLAGS= make
cp hold "$outdir/hold-dynamic"

"$outdir/hold-static" --version > "$outdir/version.txt"
"$outdir/hold-dynamic" --version > "$outdir/version-dynamic.txt"

{
  echo "branch=$branch"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || printf unknown)"
  echo "built_at=$(date -Iseconds)"
  echo "version=$(cat "$outdir/version.txt")"
  echo
  file "$outdir"/hold-static "$outdir"/hold-dynamic
  echo
  echo "dynamic dependencies:"
  ldd "$outdir/hold-dynamic" || true
  echo
  sha256sum "$outdir"/hold-static "$outdir"/hold-dynamic "$outdir"/version.txt "$outdir"/version-dynamic.txt
} > "$outdir/BUILD-INFO.txt"
sha256sum "$outdir"/* > "$outdir/SHA256SUMS"
tar -C "$out_base" -czf "$outdir.tar.gz" "$(basename "$outdir")"
sha256sum "$outdir.tar.gz" > "$outdir.tar.gz.sha256"

printf 'Review build created:\n'
printf '  directory: %s\n' "$outdir"
printf '  static:    %s/hold-static\n' "$outdir"
printf '  dynamic:   %s/hold-dynamic\n' "$outdir"
printf '  tarball:   %s.tar.gz\n' "$outdir"
printf '  sha256:    %s.tar.gz.sha256\n' "$outdir"
