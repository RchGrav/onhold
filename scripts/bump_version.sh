#!/usr/bin/env bash
# Update the canonical release version and current release-status wording.
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

usage() {
  echo "usage: scripts/bump_version.sh <semver>" >&2
  exit 2
}

new="${1:-}"
[ $# -eq 1 ] || usage
case "$new" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "bump_version: version must look like semver, got: $new" >&2; exit 2 ;;
esac
if ! printf '%s\n' "$new" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+([-.][0-9A-Za-z][0-9A-Za-z.-]*)?$'; then
  echo "bump_version: invalid version: $new" >&2
  exit 2
fi
old="$(bash .github/scripts/resolve_version.sh --base)"
printf '%s\n' "$new" > VERSION

python3 - "$old" "$new" <<'PY'
from pathlib import Path
import sys
old, new = sys.argv[1], sys.argv[2]
repls = {
    Path('README.md'): [
        (
            f"Current branch artifacts include both `hold` and the `hold` operator CLI; 0.4.0 has not been released while `VERSION` remains `{old}`. The installer refuses to install when checksums are missing or malformed.",
            f"Current branch artifacts are versioned from the canonical `VERSION` file (`{new}` on this branch). The installer refuses to install when checksums are missing or malformed.",
        ),
    ],
    Path('REVIEW.md'): [
        (
            f"This file replaces an older review report whose test counts and file references described a previous implementation review. Treat this document as a status tracker, not proof that the current branch is release-ready. The current branch version file remains `{old}`; 0.4.0 wording is release-plan status unless backed by fresh verification.",
            f"This file replaces an older review report whose test counts and file references described a previous implementation review. Treat this document as a status tracker, not proof that the current branch is release-ready. The current branch version is `{new}` from the canonical `VERSION` file; release-readiness wording still requires fresh verification evidence.",
        ),
    ],
}
for path, pairs in repls.items():
    if not path.exists():
        continue
    text = path.read_text()
    for before, after in pairs:
        text = text.replace(before, after)
    path.write_text(text)
PY

bash scripts/check_version_sync.sh
