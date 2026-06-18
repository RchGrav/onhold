#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <release-tag> <output-dir>" >&2
  exit 2
fi

tag="$1"
output_dir="$2"

if [[ ! "$tag" =~ ^v[0-9]+[.][0-9]+[.][0-9]+([-.][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
  echo "prepare_installer: invalid release tag: $tag" >&2
  exit 1
fi

mkdir -p "$output_dir"

sed "s/^INSTALLER_VERSION=.*/INSTALLER_VERSION=\"\${SIGMUND_INSTALLER_VERSION:-${tag}}\"/" \
  install.sh >"${output_dir}/install.sh"
chmod 0755 "${output_dir}/install.sh"
