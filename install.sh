#!/bin/sh
set -eu

REPO_OWNER="${SIGMUND_REPO_OWNER:-RchGrav}"
REPO_NAME="${SIGMUND_REPO_NAME:-sigmund}"
GITHUB_BASE="${SIGMUND_GITHUB_BASE:-https://github.com}"
GITHUB_API="${SIGMUND_GITHUB_API:-https://api.github.com}"
INSTALLER_VERSION="${SIGMUND_INSTALLER_VERSION:-latest}"

note() {
  printf '%s\n' "$*" >&2
}

die() {
  note "sigmund installer: error: $*"
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

download() {
  url=$1
  out=$2
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$out"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$out" "$url"
  else
    die "missing curl or wget"
  fi
}

fetch_text() {
  url=$1
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO- "$url"
  else
    die "missing curl or wget"
  fi
}

hash_file() {
  path=$1
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  else
    die "missing sha256sum or shasum"
  fi
}

latest_tag() {
  fetch_text "$GITHUB_API/repos/$REPO_OWNER/$REPO_NAME/releases/latest" |
    sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
    head -n 1
}

normalize_tag() {
  value=$1
  case "$value" in
    v*) printf '%s\n' "$value" ;;
    *) printf 'v%s\n' "$value" ;;
  esac
}

normalize_os() {
  os=${SIGMUND_INSTALL_TEST_OS:-$(uname -s)}
  case "$os" in
    Darwin|darwin) printf '%s\n' macos ;;
    Linux|linux) printf '%s\n' linux ;;
    *) die "unsupported operating system: $os" ;;
  esac
}

normalize_arch() {
  arch=${SIGMUND_INSTALL_TEST_ARCH:-$(uname -m)}
  case "$arch" in
    x86_64|amd64) printf '%s\n' amd64 ;;
    arm64|aarch64) printf '%s\n' arm64 ;;
    armv7l|armv7*|armhf) printf '%s\n' armhf ;;
    mipsel) printf '%s\n' mipsel ;;
    riscv64) printf '%s\n' riscv64 ;;
    *) die "unsupported CPU architecture: $arch" ;;
  esac
}

detect_libc() {
  if [ "${SIGMUND_INSTALL_TEST_LIBC:-}" ]; then
    printf '%s\n' "$SIGMUND_INSTALL_TEST_LIBC"
    return
  fi

  if command -v ldd >/dev/null 2>&1; then
    ldd_out=$(ldd --version 2>&1 || true)
    case "$ldd_out" in
      *musl*) printf '%s\n' musl; return ;;
      *GLIBC*|*GNU\ libc*|*glibc*) printf '%s\n' gnu; return ;;
    esac
  fi

  if getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
    printf '%s\n' gnu
    return
  fi

  if ls /lib/ld-musl-*.so.1 /usr/lib/ld-musl-*.so.1 >/dev/null 2>&1; then
    printf '%s\n' musl
    return
  fi

  printf '%s\n' unknown
}

select_artifact() {
  version_no_v=$1
  os=$2
  arch=$3
  libc=$4

  if [ "$os" = macos ]; then
    case "$arch" in
      arm64) printf 'sigmund-%s-macos-arm64.tar.gz\n' "$version_no_v" ;;
      amd64) printf 'sigmund-%s-macos-x86_64.tar.gz\n' "$version_no_v" ;;
      *) die "unsupported macOS architecture: $arch" ;;
    esac
    return
  fi

  case "$arch" in
    amd64|arm64|armhf)
      case "${SIGMUND_FLAVOR:-}" in
        gnu-dynamic) printf 'sigmund-%s-linux-%s-gnu-dynamic.tar.gz\n' "$version_no_v" "$arch" ;;
        gnu-static) printf 'sigmund-%s-linux-%s-gnu-static.tar.gz\n' "$version_no_v" "$arch" ;;
        musl-static) printf 'sigmund-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
        "")
          case "$libc" in
            gnu) printf 'sigmund-%s-linux-%s-gnu-static.tar.gz\n' "$version_no_v" "$arch" ;;
            musl) printf 'sigmund-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
            *) die "could not determine Linux libc; set SIGMUND_FLAVOR=gnu-static or SIGMUND_FLAVOR=musl-static" ;;
          esac
          ;;
        *) die "unsupported SIGMUND_FLAVOR: $SIGMUND_FLAVOR" ;;
      esac
      ;;
    mipsel|riscv64)
      if [ "${SIGMUND_FLAVOR:-}" ] && [ "${SIGMUND_FLAVOR:-}" != musl-static ]; then
        die "$arch only has a musl-static Sigmund artifact"
      fi
      printf 'sigmund-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch"
      ;;
    *) die "unsupported Linux architecture: $arch" ;;
  esac
}

embedded_checksum() {
  key=$1
  case "$key" in
    v0.3.0:sigmund-0.3.0-linux-amd64-gnu-dynamic.tar.gz) printf '%s\n' 9bab289d444a75e41007ef1fbf5655f9ba06355c81457c18c0e1eedc74b9e09c ;;
    v0.3.0:sigmund-0.3.0-linux-amd64-gnu-static.tar.gz) printf '%s\n' 57bf0ec8ba6f1a3b1813f563c3e6c2663777b57286dea148ae052e7365ed7c82 ;;
    v0.3.0:sigmund-0.3.0-linux-amd64-musl-static.tar.gz) printf '%s\n' 46f44de89cd830bba39c90e71f61d84da313ff661f004fac4a52c25e9b9b7fce ;;
    v0.3.0:sigmund-0.3.0-linux-arm64-gnu-dynamic.tar.gz) printf '%s\n' 00a189c8e40dbcd4a4f4bebc95c4b7da9ef8ceb9caedc9f9911799d37928b5a7 ;;
    v0.3.0:sigmund-0.3.0-linux-arm64-gnu-static.tar.gz) printf '%s\n' 893b552fc3037ca7131349d8068b16de155272e262ebd94e2b1fb0f3a34d8f45 ;;
    v0.3.0:sigmund-0.3.0-linux-arm64-musl-static.tar.gz) printf '%s\n' df29e41bbe4787cd274325d20eeb44ea3a97e758595b40482a01142db896828c ;;
    v0.3.0:sigmund-0.3.0-linux-armhf-gnu-dynamic.tar.gz) printf '%s\n' eabb648577717c6ae8188ca63b090af7e1e911dd6a30acf4822a595077a25309 ;;
    v0.3.0:sigmund-0.3.0-linux-armhf-gnu-static.tar.gz) printf '%s\n' bb193688043731f1dcea1e9061e931ef14a60c9d085a6af4087ee7eae2aae135 ;;
    v0.3.0:sigmund-0.3.0-linux-armhf-musl-static.tar.gz) printf '%s\n' 678cd05a643dc9f4127989c24da4ef1b5127eadc7a3880e74eb4b6c45efff4d0 ;;
    v0.3.0:sigmund-0.3.0-linux-mipsel-musl-static.tar.gz) printf '%s\n' 1b283553a0819e8cd80e5ee253cb9d0eba56943875697ae002f157246e0f8862 ;;
    v0.3.0:sigmund-0.3.0-linux-riscv64-musl-static.tar.gz) printf '%s\n' 8ce78f295fe3d162037e20f0cea326cb88308fa8a9a938abd40f5e19927030ab ;;
    v0.3.0:sigmund-0.3.0-macos-arm64.tar.gz) printf '%s\n' 07fd9af4d5ca8337689e0cae16e2b4f54d40981a75fe3cce7c9850bcf1633cc5 ;;
    v0.3.0:sigmund-0.3.0-macos-x86_64.tar.gz) printf '%s\n' 05c96d06177688f5ead010b87b4c1aa2a44e22b25dfcf98e39cb34ce718bfa87 ;;
    *) return 1 ;;
  esac
}

release_body_checksum() {
  tag=$1
  artifact=$2
  fetch_text "$GITHUB_API/repos/$REPO_OWNER/$REPO_NAME/releases/tags/$tag" 2>/dev/null |
    awk -v artifact="$artifact" '
      { data = data $0 "\n" }
      END {
        gsub(/\\r\\n|\\n|\\r/, "\n", data)
        n = split(data, lines, "\n")
        seen = 0
        for (i = 1; i <= n; i++) {
          if (index(lines[i], artifact)) {
            seen = 1
          }
          if (seen && index(lines[i], "sha256:")) {
            s = lines[i]
            sub(/^.*sha256:[[:space:]]*/, "", s)
            sub(/[^0-9a-fA-F].*$/, "", s)
            print tolower(s)
            exit
          }
        }
      }
    '
}

path_contains_dir() {
  dir=$1
  case ":$PATH:" in
    *":$dir:"*) return 0 ;;
    *) return 1 ;;
  esac
}

current_uid() {
  if [ "${SIGMUND_INSTALL_TEST_UID:-}" ]; then
    printf '%s\n' "$SIGMUND_INSTALL_TEST_UID"
  elif command -v id >/dev/null 2>&1; then
    id -u
  else
    printf '%s\n' 1
  fi
}

shell_quote() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

write_env_file() {
  env_file=$1
  bin_path=$2
  install_dir=$3
  mkdir -p "$(dirname "$env_file")"
  {
    printf 'export SIGMUND_BIN=%s\n' "$(shell_quote "$bin_path")"
    printf 'export PATH=%s:"$PATH"\n' "$(shell_quote "$install_dir")"
  } >"$env_file"
}

profile_path() {
  if [ "${SIGMUND_PROFILE:-}" ]; then
    printf '%s\n' "$SIGMUND_PROFILE"
    return
  fi
  shell_name=$(basename "${SHELL:-sh}")
  case "$shell_name" in
    zsh) printf '%s\n' "$HOME/.zshrc" ;;
    bash) printf '%s\n' "$HOME/.bashrc" ;;
    *) printf '%s\n' "$HOME/.profile" ;;
  esac
}

maybe_update_profile() {
  install_dir=$1
  [ "${SIGMUND_UPDATE_PROFILE:-1}" = 1 ] || return 0
  [ "$(current_uid)" -ne 0 ] || return 0
  [ -n "${HOME:-}" ] || return 0

  profile=$(profile_path)
  marker="# sigmund installer"
  mkdir -p "$(dirname "$profile")"
  touch "$profile"
  if grep -Fq "$marker" "$profile"; then
    return 0
  fi
  {
    printf '\n%s\n' "$marker"
    printf 'export PATH=%s:"$PATH"\n' "$(shell_quote "$install_dir")"
  } >>"$profile"
  note "updated shell profile for future shells: $profile"
}

tag=${1:-${SIGMUND_VERSION:-$INSTALLER_VERSION}}
if [ "$tag" = latest ]; then
  tag=$(latest_tag)
  [ -n "$tag" ] || die "could not resolve latest release tag"
else
  tag=$(normalize_tag "$tag")
fi
version_no_v=${tag#v}

os=$(normalize_os)
arch=$(normalize_arch)
libc=
if [ "$os" = linux ]; then
  libc=$(detect_libc)
fi
artifact=$(select_artifact "$version_no_v" "$os" "$arch" "$libc")
url="$GITHUB_BASE/$REPO_OWNER/$REPO_NAME/releases/download/$tag/$artifact"

if [ "${SIGMUND_INSTALL_DIR:-}" ]; then
  install_dir=$SIGMUND_INSTALL_DIR
elif [ "$(current_uid)" -eq 0 ]; then
  install_dir=/usr/local/bin
else
  install_dir=$HOME/.local/bin
fi
target="$install_dir/sigmund"

note "sigmund installer"
note "  detected: $os $arch${libc:+ $libc}"
note "  version:  $tag"
note "  artifact: $artifact"
note "  install:  $target"

if [ "${SIGMUND_INSTALL_DRY_RUN:-0}" = 1 ]; then
  note "dry run: no files changed"
  exit 0
fi

need_cmd tar
need_cmd awk
need_cmd sed

tmp=${TMPDIR:-/tmp}/sigmund-install.$$
rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

archive="$tmp/$artifact"
download "$url" "$archive"

expected=$(embedded_checksum "$tag:$artifact" || true)
if [ -z "$expected" ]; then
  expected=$(release_body_checksum "$tag" "$artifact" || true)
fi
if [ -n "$expected" ] && [ "${#expected}" -ne 64 ]; then
  expected=
fi
if [ -z "$expected" ]; then
  sums="$tmp/SHA256SUMS"
  if download "$GITHUB_BASE/$REPO_OWNER/$REPO_NAME/releases/download/$tag/SHA256SUMS" "$sums" 2>/dev/null; then
    expected=$(awk -v artifact="$artifact" '$2 == artifact || $2 == "*" artifact { print $1; exit }' "$sums")
  fi
fi
if [ -n "$expected" ] && [ "${#expected}" -ne 64 ]; then
  expected=
fi
if [ -z "$expected" ]; then
  sidecar="$tmp/$artifact.sha256"
  if download "$url.sha256" "$sidecar" 2>/dev/null; then
    expected=$(awk '{print $1; exit}' "$sidecar")
  fi
fi
if [ -n "$expected" ] && [ "${#expected}" -ne 64 ]; then
  expected=
fi
[ -n "$expected" ] || die "no checksum available for $artifact; refusing to install"

actual=$(hash_file "$archive")
[ "$actual" = "$expected" ] || die "checksum mismatch for $artifact"

extract="$tmp/extract"
mkdir -p "$extract"
tar -xzf "$archive" -C "$extract"
bin=$(find "$extract" -type f -name sigmund | head -n 1)
[ -n "$bin" ] || die "archive did not contain sigmund binary"

mkdir -p "$install_dir"
cp "$bin" "$target.tmp.$$"
chmod 0755 "$target.tmp.$$"
mv "$target.tmp.$$" "$target"
if [ "$(current_uid)" -eq 0 ]; then
  chown 0:0 "$target" 2>/dev/null || true
fi

version=$("$target" --version 2>/dev/null || true)
[ -n "$version" ] || die "installed binary did not run: $target --version"

if [ "${SIGMUND_ENV_FILE:-}" ]; then
  write_env_file "$SIGMUND_ENV_FILE" "$target" "$install_dir"
  note "wrote environment handoff: $SIGMUND_ENV_FILE"
fi

if ! path_contains_dir "$install_dir" && [ -z "${SIGMUND_ENV_FILE:-}" ]; then
  maybe_update_profile "$install_dir"
fi

note "installed sigmund $version"
note "binary: $target"
if path_contains_dir "$install_dir"; then
  note "run: sigmund --help"
else
  note "current shell PATH does not include $install_dir"
  note "run now: $target --help"
  note "or export: PATH=$install_dir:\$PATH"
fi

printf 'SIGMUND_BIN=%s\n' "$target"
