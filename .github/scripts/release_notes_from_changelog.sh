#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <release-tag-or-version> <output-file>" >&2
  exit 2
fi

version="${1#v}"
output_file="$2"

mkdir -p "$(dirname "$output_file")"

awk -v version="$version" '
  $0 ~ "^##[[:space:]]+" version "([[:space:]-]|$)" {
    found = 1
    print
    next
  }
  found && /^##[[:space:]]+/ {
    exit
  }
  found {
    print
  }
  END {
    if (!found) {
      exit 1
    }
  }
' CHANGELOG.md >"$output_file"

if [[ ! -s "$output_file" ]]; then
  echo "release_notes_from_changelog: no notes found for $version" >&2
  exit 1
fi
