#!/usr/bin/env bash
# Run the full Linux CI mirror locally, on any host with Apple `container`,
# docker, or podman. Builds the committed CI image (docker/Dockerfile.linux-ci),
# mounts the repo read-only, copies it inside (host tree never touched), and runs:
#
#   scripts/linux.sh                 # scripts/ci.sh  (builds, suite, sanitizers, cppcheck, lint)
#   scripts/linux.sh root            # scripts/test_root.sh  (root/sudoers/private-store lane)
#   scripts/linux.sh release [dir]   # scripts/release_build.sh -> Linux artifacts in <dir> (default dist/)
#   scripts/linux.sh -- <cmd>        # an arbitrary command in the container
#
# This replaces any author-only local setup: "Linux parity" is reproducible from
# the committed Dockerfile for every contributor.
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
repo="$(pwd)"
image="sigmund-linux-ci"
dockerfile="docker/Dockerfile.linux-ci"

rt=""
for cand in container docker podman; do
  command -v "$cand" >/dev/null 2>&1 && { rt="$cand"; break; }
done
[ -n "$rt" ] || { echo "linux.sh: need Apple 'container', docker, or podman in PATH" >&2; exit 1; }

# what to run inside; `release` also bind-mounts a host output dir at /out so the
# packaged tarballs land back on the host (everything else writes only inside).
out_host=""
case "${1:-}" in
  root)            inner_cmd='chown -R ci:ci /work/sigmund && exec sudo -u ci -H bash /work/sigmund/scripts/test_root.sh' ;;
  release)         shift
                   out_host="${1:-dist}"
                   mkdir -p "$out_host"
                   out_host="$(cd "$out_host" && pwd)"
                   inner_cmd='bash scripts/release_build.sh /out' ;;
  --)              shift; inner_cmd="$*" ;;
  '')              inner_cmd='bash scripts/ci.sh' ;;
  *)               inner_cmd="$*" ;;
esac

echo "linux.sh: runtime=$rt  image=$image  cmd=[$inner_cmd]${out_host:+  out=$out_host}"
"$rt" build -t "$image" -f "$dockerfile" docker

# Copy the read-only-mounted repo into the container fs so the build and any
# test-user actor write only inside the container, leaving the host tree clean.
inner="set -e
cp -a /src /work/sigmund
cd /work/sigmund
# clear only build artifacts (not untracked source) so uncommitted changes are
# testable; the copy is fresh each run anyway.
rm -rf obj obj-test sigmund sigmund-dynamic hash-vector 2>/dev/null || true
$inner_cmd"

if [ -n "$out_host" ]; then
  "$rt" run --rm -v "$repo:/src" -v "$out_host:/out" "$image" bash -c "$inner"
else
  "$rt" run --rm -v "$repo:/src" "$image" bash -c "$inner"
fi
