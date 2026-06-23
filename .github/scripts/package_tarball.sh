#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "usage: $0 <target> <version> [binary_path] [output_dir]" >&2
  exit 1
fi

target="$1"
version="$2"
binary_path="${3:-sigmund}"
output_dir="${4:-dist}"
stage_dir="${output_dir}/package-${target}"
archive_name="sigmund-${version}-${target}.tar.gz"

rm -rf "${stage_dir}"
mkdir -p "${stage_dir}" "${output_dir}"
cp "${binary_path}" "${stage_dir}/sigmund"
cp "${binary_path}" "${stage_dir}/mund"
chmod +x "${stage_dir}/sigmund" "${stage_dir}/mund"

shopt -s nullglob
for file in LICENSE* README*; do
  cp -a "${file}" "${stage_dir}/"
done
shopt -u nullglob

archive_path="${output_dir}/${archive_name}"
if tar --version 2>/dev/null | grep -qi 'gnu tar'; then
  tar -C "${stage_dir}" \
    --sort=name \
    --mtime='UTC 1970-01-01' \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    -cf - . | gzip -n > "${archive_path}"
else
  tar -C "${stage_dir}" -czf "${archive_path}" .
fi

echo "${archive_path}"
