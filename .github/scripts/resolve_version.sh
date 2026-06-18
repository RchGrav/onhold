#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
version_file="${repo_root}/VERSION"

if [[ ! -f "$version_file" ]]; then
  echo "resolve_version: missing VERSION file" >&2
  exit 1
fi

version_base=$(sed -n '1s/[[:space:]]*$//p' "$version_file")
if [[ ! "$version_base" =~ ^[0-9]+[.][0-9]+[.][0-9]+([-.][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
  echo "resolve_version: invalid VERSION value: $version_base" >&2
  exit 1
fi

if [[ "${GITHUB_REF_TYPE:-}" == "tag" ]]; then
  tag="${GITHUB_REF_NAME:-}"
  if [[ ! "$tag" =~ ^v ]]; then
    echo "resolve_version: release tags must start with v, got: $tag" >&2
    exit 1
  fi
  version="${tag#v}"
  if [[ "$version" != "$version_base" ]]; then
    echo "resolve_version: tag $tag does not match VERSION $version_base" >&2
    exit 1
  fi
else
  sha="${GITHUB_SHA:-$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo dev)}"
  version="${version_base}-${sha:0:7}"
  tag="v${version_base}"
fi

case "${1:-}" in
  --github-output)
    {
      printf 'value=%s\n' "$version"
      printf 'base=%s\n' "$version_base"
      printf 'tag=%s\n' "$tag"
    } >>"$GITHUB_OUTPUT"
    ;;
  --base)
    printf '%s\n' "$version_base"
    ;;
  "")
    printf '%s\n' "$version"
    ;;
  *)
    echo "resolve_version: unknown option: $1" >&2
    exit 2
    ;;
esac
