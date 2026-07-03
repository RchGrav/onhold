#!/bin/sh
set -eu

REPO_OWNER="${HOLD_REPO_OWNER:-RchGrav}"
REPO_NAME="${HOLD_REPO_NAME:-onhold}"
GITHUB_BASE="${HOLD_GITHUB_BASE:-https://github.com}"
GITHUB_API="${HOLD_GITHUB_API:-https://api.github.com}"
INSTALLER_VERSION="${HOLD_INSTALLER_VERSION:-latest}"
INSTALL_SYSTEM="${HOLD_INSTALL_SYSTEM:-0}"

note() {
  printf '%s\n' "$*" >&2
}

die() {
  note "hold installer: error: $*"
  exit 1
}

usage() {
  cat >&2 <<'EOF'
usage: install.sh [version] [--system]
       install.sh --uninstall [--system]

Options:
  --system         Install to (or uninstall from) /usr/local/bin, using sudo
                   if needed.
  -u, --uninstall  Remove the installed hold binary and the installer's PATH
                   block. Hold state (your calls, logs, records) is kept.
  -h, --help       Show this help.

Environment:
  HOLD_VERSION         Version or tag to install.
  HOLD_INSTALL_SYSTEM  Set to 1 to behave like --system.
  HOLD_INSTALL_DIR     Custom install directory.
EOF
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
  os=${HOLD_INSTALL_TEST_OS:-$(uname -s)}
  case "$os" in
    Darwin|darwin) printf '%s\n' macos ;;
    Linux|linux) printf '%s\n' linux ;;
    *) die "unsupported operating system: $os" ;;
  esac
}

normalize_arch() {
  arch=${HOLD_INSTALL_TEST_ARCH:-$(uname -m)}
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
  if [ "${HOLD_INSTALL_TEST_LIBC:-}" ]; then
    printf '%s\n' "$HOLD_INSTALL_TEST_LIBC"
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
      arm64) printf 'hold-%s-macos-arm64.tar.gz\n' "$version_no_v" ;;
      amd64) printf 'hold-%s-macos-x86_64.tar.gz\n' "$version_no_v" ;;
      *) die "unsupported macOS architecture: $arch" ;;
    esac
    return
  fi

  case "$arch" in
    amd64|arm64|armhf)
      case "${HOLD_FLAVOR:-}" in
        gnu-dynamic) printf 'hold-%s-linux-%s-gnu-dynamic.tar.gz\n' "$version_no_v" "$arch" ;;
        gnu-static) printf 'hold-%s-linux-%s-gnu-static.tar.gz\n' "$version_no_v" "$arch" ;;
        musl-static) printf 'hold-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
        "")
          case "$libc" in
            gnu) printf 'hold-%s-linux-%s-gnu-static.tar.gz\n' "$version_no_v" "$arch" ;;
            musl) printf 'hold-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch" ;;
            *) die "could not determine Linux libc; set HOLD_FLAVOR=gnu-static or HOLD_FLAVOR=musl-static" ;;
          esac
          ;;
        *) die "unsupported HOLD_FLAVOR: $HOLD_FLAVOR" ;;
      esac
      ;;
    mipsel|riscv64)
      if [ "${HOLD_FLAVOR:-}" ] && [ "${HOLD_FLAVOR:-}" != musl-static ]; then
        die "$arch only has a musl-static On Hold artifact"
      fi
      printf 'hold-%s-linux-%s-musl-static.tar.gz\n' "$version_no_v" "$arch"
      ;;
    *) die "unsupported Linux architecture: $arch" ;;
  esac
}

path_contains_dir() {
  dir=$1
  case ":$PATH:" in
    *":$dir:"*) return 0 ;;
    *) return 1 ;;
  esac
}

current_uid() {
  if [ "${HOLD_INSTALL_TEST_UID:-}" ]; then
    printf '%s\n' "$HOLD_INSTALL_TEST_UID"
  elif command -v id >/dev/null 2>&1; then
    id -u
  else
    printf '%s\n' 1
  fi
}

truthy() {
  case "$1" in
    1|yes|true|on) return 0 ;;
    *) return 1 ;;
  esac
}

validate_sha256_hex() {
  value=$1
  [ "${#value}" -eq 64 ] || return 1
  case "$value" in
    *[!0123456789abcdefABCDEF]*) return 1 ;;
  esac
  return 0
}

sha256_from_sums() {
  sums=$1
  artifact=$2
  awk -v artifact="$artifact" '
    ($2 == artifact || $2 == "*" artifact) {
      print $1
      found = 1
      exit
    }
    END { if (!found) exit 1 }
  ' "$sums"
}

validate_archive_layout() {
  archive=$1
  tar -tzf "$archive" | awk '
    {
      p = $0
      if (p == "" || p ~ /^\//) bad = 1
      n = split(p, parts, "/")
      for (i = 1; i <= n; i++) {
        if (parts[i] == "..") bad = 1
      }
      sub(/^\.\//, "", p)
      if (p == "hold") found_hold = 1
    }
    END {
      if (bad || !found_hold) exit 1
    }
  '
}

can_write_dir() {
  dir=$1
  [ -d "$dir" ] && [ -w "$dir" ] && [ -x "$dir" ]
}

default_install_dir() {
  if [ "$(current_uid)" -eq 0 ]; then
    printf '%s\n' /usr/local/bin
  elif can_write_dir /usr/local/bin; then
    printf '%s\n' /usr/local/bin
  else
    [ -n "${HOME:-}" ] || die "HOME is not set; set HOLD_INSTALL_DIR"
    printf '%s\n' "$HOME/.local/bin"
  fi
}

ensure_install_dir() {
  dir=$1
  created=0
  if [ ! -d "$dir" ]; then
    created=1
  fi
  mkdir -p "$dir" || die "could not create install directory: $dir"
  [ -d "$dir" ] || die "install path is not a directory: $dir"
  [ -w "$dir" ] || die "install directory is not writable: $dir"
  [ -x "$dir" ] || die "install directory is not searchable: $dir"
  if [ "$created" -eq 1 ]; then
    chmod 0755 "$dir" 2>/dev/null || true
  fi
}

install_needs_sudo() {
  dir=$1
  if [ "$(current_uid)" -eq 0 ]; then
    return 1
  fi
  can_write_dir "$dir" && return 1
  return 0
}

install_binary_user() {
  bin=$1
  target=$2
  install_dir=$3

  ensure_install_dir "$install_dir"
  cp "$bin" "$target.tmp.$$"
  chmod 0755 "$target.tmp.$$"
  if [ "$(current_uid)" -eq 0 ] && [ "$install_dir" = /usr/local/bin ]; then
    chown 0:0 "$target.tmp.$$" 2>/dev/null || true
  fi
  mv "$target.tmp.$$" "$target"
}

install_binary_sudo() {
  bin=$1
  target=$2
  install_dir=$3
  tmp_target="$target.tmp.$$"

  need_cmd sudo
  sudo -v
  if ! sudo test -d "$install_dir"; then
    sudo mkdir -p "$install_dir"
    sudo chmod 0755 "$install_dir" 2>/dev/null || true
  fi
  sudo test -d "$install_dir" || die "install path is not a directory: $install_dir"
  sudo test -w "$install_dir" || die "install directory is not writable through sudo: $install_dir"
  sudo rm -f "$tmp_target"
  sudo cp "$bin" "$tmp_target"
  sudo chmod 0755 "$tmp_target"
  sudo chown 0:0 "$tmp_target" 2>/dev/null || true
  sudo mv "$tmp_target" "$target"
}

shell_quote() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

write_env_file() {
  env_file=$1
  hold_bin_path=$2
  install_dir=$3
  mkdir -p "$(dirname "$env_file")"
  {
    printf 'export HOLD_BIN=%s\n' "$(shell_quote "$hold_bin_path")"
    printf 'export PATH=%s:"$PATH"\n' "$(shell_quote "$install_dir")"
  } >"$env_file"
}

profile_path() {
  if [ "${HOLD_PROFILE:-}" ]; then
    printf '%s\n' "$HOLD_PROFILE"
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
  [ "${HOLD_UPDATE_PROFILE:-1}" = 1 ] || return 0
  [ "$(current_uid)" -ne 0 ] || return 0
  [ -n "${HOME:-}" ] || return 0

  profile=$(profile_path)
  marker="# hold installer"
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

version_arg=
UNINSTALL=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --system)
      INSTALL_SYSTEM=1
      ;;
    -u|--uninstall)
      UNINSTALL=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      [ "$#" -eq 0 ] || die "unexpected argument after --: $1"
      break
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      [ -z "$version_arg" ] || die "multiple version arguments provided"
      version_arg=$1
      ;;
  esac
  shift
done

if [ "${HOLD_INSTALL_DIR:-}" ] && truthy "$INSTALL_SYSTEM"; then
  die "HOLD_INSTALL_DIR cannot be combined with --system or HOLD_INSTALL_SYSTEM=1"
fi

install_mode=user
if [ "${HOLD_INSTALL_DIR:-}" ]; then
  install_dir=$HOLD_INSTALL_DIR
  install_mode=custom
elif truthy "$INSTALL_SYSTEM"; then
  install_dir=/usr/local/bin
  install_mode=system
else
  install_dir=$(default_install_dir)
  if [ "$install_dir" = /usr/local/bin ]; then
    install_mode=system
  fi
fi
target="$install_dir/hold"
use_sudo=0
if [ "$install_mode" = system ] && install_needs_sudo "$install_dir"; then
  use_sudo=1
fi
privilege_note=
if [ "$use_sudo" -eq 1 ]; then
  privilege_note=" with sudo"
fi

if [ "$UNINSTALL" -eq 1 ]; then
  [ -z "$version_arg" ] || die "--uninstall does not take a version"
  if [ ! -e "$target" ] && [ ! -L "$target" ]; then
    note "hold is not installed at $target; nothing to remove"
    for other in /usr/local/bin/hold "${HOME:-/nonexistent}/.local/bin/hold"; do
      if [ "$other" != "$target" ] && [ -e "$other" ]; then
        note "note: found hold at $other (rerun with --system or HOLD_INSTALL_DIR to target it)"
      fi
    done
    exit 0
  fi
  if [ "$use_sudo" -eq 1 ]; then
    sudo rm -f "$target"
  else
    rm -f "$target"
  fi
  note "removed $target"
  if [ "$(current_uid)" -ne 0 ] && [ -n "${HOME:-}" ]; then
    profile=$(profile_path)
    marker="# hold installer"
    if [ -f "$profile" ] && grep -Fq "$marker" "$profile"; then
      tmp_profile="$profile.hold-uninstall.$$"
      awk -v marker="$marker" '
        $0 == marker { skip = 2 }
        skip > 0 { skip--; next }
        { print }
      ' "$profile" >"$tmp_profile" && mv "$tmp_profile" "$profile"
      note "removed the installer PATH block from $profile"
    fi
  fi
  note "kept hold state (your calls, logs, records); remove manually if you"
  note "really mean it: ~/.local/state/hold (user), /var/lib/hold (system)"
  exit 0
fi

tag=${version_arg:-${HOLD_VERSION:-$INSTALLER_VERSION}}
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

note "hold installer"
note "  detected: $os $arch${libc:+ $libc}"
note "  version:  $tag"
note "  artifact: $artifact"
note "  mode:     $install_mode$privilege_note"
note "  install:  $target"

if [ "${HOLD_INSTALL_DRY_RUN:-0}" = 1 ]; then
  note "dry run: no files changed"
  exit 0
fi

need_cmd tar
need_cmd awk
need_cmd sed
need_cmd mktemp

old_umask=$(umask)
umask 077
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hold-install.XXXXXX") || die "could not create temporary directory"
umask "$old_umask"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

archive="$tmp/$artifact"
download "$url" "$archive"

sums="$tmp/SHA256SUMS"
download "$GITHUB_BASE/$REPO_OWNER/$REPO_NAME/releases/download/$tag/SHA256SUMS" "$sums" 2>/dev/null ||
  die "missing SHA256SUMS for $tag; refusing to install"
expected=$(sha256_from_sums "$sums" "$artifact") ||
  die "SHA256SUMS does not contain $artifact; refusing to install"
validate_sha256_hex "$expected" ||
  die "malformed SHA256SUMS entry for $artifact; refusing to install"

actual=$(hash_file "$archive")
[ "$actual" = "$expected" ] || die "checksum mismatch for $artifact"

validate_archive_layout "$archive" || die "archive layout is invalid for $artifact"
extract="$tmp/extract"
mkdir -p "$extract"
tar -xzf "$archive" -C "$extract"
bin="$extract/hold"
[ -f "$bin" ] && [ ! -L "$bin" ] || die "archive did not contain expected root hold binary"

if [ "$use_sudo" -eq 1 ]; then
  install_binary_sudo "$bin" "$target" "$install_dir"
else
  install_binary_user "$bin" "$target" "$install_dir"
fi

version=$("$target" --version 2>/dev/null || true)
[ -n "$version" ] || die "installed binary did not run: $target --version"

if [ "${HOLD_ENV_FILE:-}" ]; then
  write_env_file "$HOLD_ENV_FILE" "$target" "$install_dir"
  note "wrote environment handoff: $HOLD_ENV_FILE"
fi

if ! path_contains_dir "$install_dir" && [ -z "${HOLD_ENV_FILE:-}" ]; then
  maybe_update_profile "$install_dir"
fi

note "installed hold $version"
if path_contains_dir "$install_dir"; then
  note "run: hold on"
else
  note "current shell PATH does not include $install_dir"
  note "run now: $target on"
  note "or export: PATH=$install_dir:\$PATH"
fi
installer_url="$GITHUB_BASE/$REPO_OWNER/$REPO_NAME/releases/latest/download/install.sh"
case "$install_mode" in
  system) note "uninstall: curl -LsSf $installer_url | sh -s -- --uninstall --system" ;;
  custom) note "uninstall: curl -LsSf $installer_url | HOLD_INSTALL_DIR=$(shell_quote "$install_dir") sh -s -- --uninstall" ;;
  *)      note "uninstall: curl -LsSf $installer_url | sh -s -- --uninstall" ;;
esac

# Machine handoff for scripts capturing stdout; humans already saw the path.
if [ ! -t 1 ]; then
  printf 'HOLD_BIN=%s\n' "$target"
fi
