#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

version_from_tag() {
  tag="$1"
  if [[ ! "$tag" =~ ^v ]]; then
    echo "resolve_version: release tags must start with v, got: $tag" >&2
    exit 1
  fi
  version="${tag#v}"
  if [[ ! "$version" =~ ^[0-9]+[.][0-9]+[.][0-9]+([-.][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
    echo "resolve_version: invalid release tag: $tag" >&2
    exit 1
  fi
  version_base="$version"
}

version_from_file() {
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
  tag="v${version_base}"
}

if [[ "${GITHUB_REF_TYPE:-}" == "tag" ]]; then
  version_from_tag "${GITHUB_REF_NAME:-}"
elif ! git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  version_from_file
  version="$version_base"
else
  exact_tag=$(git -C "$repo_root" describe --tags --exact-match --match 'v[0-9]*' HEAD 2>/dev/null || true)
  if [[ -n "$exact_tag" ]]; then
    version_from_tag "$exact_tag"
    if ! git -C "$repo_root" diff --quiet 2>/dev/null; then
      sha="${GITHUB_SHA:-$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo dev)}"
      version="${version_base}-${sha:0:7}-dirty"
    fi
  else
    latest_tag=$(git -C "$repo_root" describe --tags --abbrev=0 --match 'v[0-9]*' HEAD 2>/dev/null || true)
    if [[ -n "$latest_tag" ]]; then
      version_from_tag "$latest_tag"
    else
      version_from_file
    fi

    sha="${GITHUB_SHA:-$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo dev)}"
    dirty_suffix=""
    if ! git -C "$repo_root" diff --quiet 2>/dev/null; then
      dirty_suffix="-dirty"
    fi
    version="${version_base}-${sha:0:7}${dirty_suffix}"
  fi
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
