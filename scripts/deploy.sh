#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: bash scripts/deploy.sh [version|tag] [options]

Create and push a release tag, then optionally watch the GitHub release workflow.
The GitHub Actions release workflow builds artifacts, generates SHA256SUMS, and
publishes the GitHub release from the pushed tag.

Examples:
  bash scripts/deploy.sh
  bash scripts/deploy.sh --skip-tests
  bash scripts/deploy.sh v1.2.3 --dry-run

Options:
  --remote <name>    Git remote to push the tag to (default: origin)
  --skip-tests       Do not run "make clean && make test" before tagging
  --no-watch         Do not wait for the GitHub Actions release run
  --no-install-test  Do not run the GitHub one-line installer smoke test
  --dry-run          Validate and print the planned deployment, but do not tag
  --yes              Do not prompt before creating and pushing the tag
  -h, --help         Show this help
EOF
}

note() {
  printf '%s\n' "$*" >&2
}

die() {
  note "deploy: error: $*"
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

remote=origin
run_tests=1
watch_run=1
install_test=1
dry_run=0
assume_yes=0
tag=

while [[ $# -gt 0 ]]; do
  case "$1" in
    --remote)
      [[ $# -ge 2 ]] || die "--remote requires a value"
      remote=$2
      shift 2
      ;;
    --skip-tests)
      run_tests=0
      shift
      ;;
    --no-watch)
      watch_run=0
      shift
      ;;
    --no-install-test)
      install_test=0
      shift
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    --yes)
      assume_yes=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      [[ -z "$tag" ]] || die "only one version/tag argument is allowed"
      tag=$1
      shift
      ;;
  esac
done

need_cmd git
need_cmd sed
need_cmd gh
if [[ "$install_test" -eq 1 ]]; then
  if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    die "missing curl or wget"
  fi
fi
if [[ "$run_tests" -eq 1 ]]; then
  need_cmd make
fi

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || die "not inside a git repository"
cd "$repo_root"

[[ -f .github/workflows/release.yml ]] || die "missing .github/workflows/release.yml"
[[ -f .github/scripts/resolve_version.sh ]] || die "missing .github/scripts/resolve_version.sh"

if [[ -z "$tag" ]]; then
  tag=$(bash .github/scripts/resolve_version.sh --base)
fi

case "$tag" in
  v*) ;;
  *) tag="v${tag}" ;;
esac

if [[ ! "$tag" =~ ^v[0-9]+[.][0-9]+[.][0-9]+([-.][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
  die "release tag must look like vMAJOR.MINOR.PATCH, got: $tag"
fi
version=${tag#v}

git remote get-url "$remote" >/dev/null 2>&1 || die "unknown git remote: $remote"
gh auth status >/dev/null 2>&1 || die "gh is not authenticated; run: gh auth login"

if [[ -n "$(git status --porcelain)" ]]; then
  die "working tree is not clean; commit or stash changes before deploying"
fi

current_branch=$(git branch --show-current)
[[ -n "$current_branch" ]] || die "deploy from a branch, not a detached HEAD"

version_base=$(bash .github/scripts/resolve_version.sh --base)
if [[ "$version_base" != "$version" ]]; then
  die "VERSION is $version_base, but deploy tag is $tag"
fi

if ! grep -Eq "^##[[:space:]]+${version//./\\.}([[:space:]-]|$)" CHANGELOG.md 2>/dev/null; then
  note "deploy: warning: CHANGELOG.md does not appear to have a $version section"
fi

note "deploy: fetching $remote"
git fetch "$remote" --tags
git fetch "$remote" "+refs/heads/${current_branch}:refs/remotes/${remote}/${current_branch}"

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
  die "local tag already exists: $tag"
fi

if git ls-remote --exit-code --tags "$remote" "refs/tags/$tag" >/dev/null 2>&1; then
  die "remote tag already exists on $remote: $tag"
fi

remote_ref="refs/remotes/${remote}/${current_branch}"
git show-ref --verify --quiet "$remote_ref" || die "remote branch not found: $remote/$current_branch"

head_sha=$(git rev-parse HEAD)
remote_sha=$(git rev-parse "$remote_ref")
if [[ "$head_sha" != "$remote_sha" ]]; then
  die "HEAD ($head_sha) differs from $remote/$current_branch ($remote_sha); push or pull first"
fi

note "deploy: release tag  $tag"
note "deploy: source       $current_branch @ $head_sha"
note "deploy: remote       $remote"

if [[ "$run_tests" -eq 1 ]]; then
  note "deploy: running local release smoke test"
  make clean
  make test
else
  note "deploy: skipping local tests"
fi

if [[ "$dry_run" -eq 1 ]]; then
  note "deploy: dry run complete"
  note "deploy: would run: git tag -a $tag -m 'Release $tag'"
  note "deploy: would run: git push $remote refs/tags/$tag"
  exit 0
fi

if [[ "$assume_yes" -ne 1 ]]; then
  if [[ ! -t 0 ]]; then
    die "non-interactive deploy requires --yes"
  fi
  read -r -p "Deploy $tag from $current_branch to $remote? [y/N] " answer
  case "$answer" in
    y|Y|yes|YES) ;;
    *) die "deployment cancelled" ;;
  esac
fi

git tag -a "$tag" -m "Release $tag"
git push "$remote" "refs/tags/$tag"

note "deploy: pushed $tag; GitHub Actions will build and publish the release"

if [[ "$watch_run" -eq 1 ]]; then
  note "deploy: waiting for release workflow run"
  run_id=
  for _ in {1..60}; do
    run_id=$(gh run list \
      --workflow release.yml \
      --limit 20 \
      --json databaseId,headBranch,headSha \
      --jq ".[] | select(.headBranch == \"$tag\" and .headSha == \"$head_sha\") | .databaseId" |
      head -n 1)
    [[ -n "$run_id" ]] && break
    sleep 5
  done

  [[ -n "$run_id" ]] || die "could not find GitHub Actions run for $tag"
  gh run watch "$run_id" --exit-status
  gh release view "$tag" --json url --jq '.url'
else
  note "deploy: not watching workflow; check GitHub Actions for $tag"
  if [[ "$install_test" -eq 1 ]]; then
    note "deploy: skipping GitHub installer smoke test because --no-watch was used"
    install_test=0
  fi
fi

if [[ "$install_test" -eq 1 ]]; then
  case "$(uname -s)" in
    Linux|Darwin) ;;
    *)
      note "deploy: skipping GitHub installer smoke test on unsupported installer host: $(uname -s)"
      exit 0
      ;;
  esac

  repo=$(gh repo view --json nameWithOwner --jq '.nameWithOwner')
  installer_url="https://github.com/${repo}/releases/download/${tag}/install.sh"
  install_tmp=$(mktemp -d)
  trap 'rm -rf "$install_tmp"' EXIT

  note "deploy: smoke-testing one-line installer from $installer_url"
  if command -v curl >/dev/null 2>&1; then
    curl -LsSf "$installer_url" |
      SIGMUND_INSTALL_DIR="$install_tmp/bin" SIGMUND_UPDATE_PROFILE=0 sh
  else
    wget -qO- "$installer_url" |
      SIGMUND_INSTALL_DIR="$install_tmp/bin" SIGMUND_UPDATE_PROFILE=0 sh
  fi

  installed_version=$("$install_tmp/bin/sigmund" --version)
  if [[ "$installed_version" != "$tag" && "$installed_version" != "$version" ]]; then
    die "installed version $installed_version did not match $tag"
  fi
  note "deploy: GitHub one-line installer installed $installed_version"
fi
