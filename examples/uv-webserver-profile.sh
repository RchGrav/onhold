#!/bin/sh
set -eu

if (set -o pipefail) 2>/dev/null; then
  set -o pipefail
fi

say() {
  printf '%s\n' "$*"
}

say_err() {
  printf '%s\n' "$*" >&2
}

die() {
  say_err "uv-webserver-profile: error: $*"
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

download() {
  url=$1
  out=$2
  if command -v curl >/dev/null 2>&1; then
    curl -LsSf "$url" -o "$out"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$out" "$url"
  else
    die "missing curl or wget"
  fi
}

abs_path() {
  path=$1
  if command -v realpath >/dev/null 2>&1; then
    realpath "$path"
    return
  fi
  dir=$(dirname "$path")
  base=$(basename "$path")
  (cd "$dir" && printf '%s/%s\n' "$(pwd -P)" "$base")
}

find_on_path() {
  name=$1
  found=$(command -v "$name" 2>/dev/null || true)
  [ -n "$found" ] || return 1
  abs_path "$found"
}

wait_for_url() {
  url=$1
  attempts=${2:-40}
  i=1
  while [ "$i" -le "$attempts" ]; do
    if curl -fsS "$url" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
    i=$((i + 1))
  done
  return 1
}

ROOT=$(abs_path "${HOLD_DEMO_ROOT:-$PWD/.hold-demo}")
TOOLS_DIR="$ROOT/tools"
SITE_DIR="$ROOT/site"
OTHER_DIR="$ROOT/from-another-directory"
HOLD_ENV="$ROOT/hold.env"
PORT=${HOLD_DEMO_PORT:-8765}
PROFILE=${HOLD_DEMO_PROFILE:-uv-web-demo}
HOLD_INSTALL_URL=${HOLD_INSTALL_URL:-https://github.com/RchGrav/hold/releases/latest/download/install.sh}
UV_INSTALL_URL=${UV_INSTALL_URL:-https://astral.sh/uv/install.sh}
ACTIVE_RUNS=""

cleanup_on_error() {
  rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${HOLD_BIN:-}" ]; then
    for id in $ACTIVE_RUNS; do
      "$HOLD_BIN" dump "$id" >/dev/null 2>&1 || true
      "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
      "$HOLD_BIN" prune "$id" >/dev/null 2>&1 || true
    done
  fi
  exit "$rc"
}
trap cleanup_on_error EXIT HUP INT TERM

mkdir -p "$TOOLS_DIR" "$SITE_DIR" "$OTHER_DIR"
cat >"$SITE_DIR/index.html" <<'HTML'
<!doctype html>
<html>
  <head><meta charset="utf-8"><title>On Hold uv demo</title></head>
  <body>On Hold started this uv-backed server.</body>
</html>
HTML

if [ -n "${HOLD_BIN:-}" ]; then
  case "$HOLD_BIN" in
    */*) HOLD_BIN=$(abs_path "$HOLD_BIN") ;;
    *) HOLD_BIN=$(find_on_path "$HOLD_BIN") ;;
  esac
elif HOLD_BIN=$(find_on_path hold); then
  :
else
  installer="$ROOT/install-hold.sh"
  say_err "Installing On Hold into $TOOLS_DIR"
  download "$HOLD_INSTALL_URL" "$installer"
  HOLD_INSTALL_DIR="$TOOLS_DIR" HOLD_ENV_FILE="$HOLD_ENV" sh "$installer"
  . "$HOLD_ENV"
fi
[ -x "$HOLD_BIN" ] || die "HOLD_BIN is not executable: $HOLD_BIN"

if [ -n "${UV_BIN:-}" ]; then
  case "$UV_BIN" in
    */*) UV_BIN=$(abs_path "$UV_BIN") ;;
    *) UV_BIN=$(find_on_path "$UV_BIN") ;;
  esac
elif UV_BIN=$(find_on_path uv); then
  :
else
  installer="$ROOT/install-uv.sh"
  say_err "Installing uv into $TOOLS_DIR"
  download "$UV_INSTALL_URL" "$installer"
  UV_UNMANAGED_INSTALL="$TOOLS_DIR" UV_NO_MODIFY_PATH=1 sh "$installer"
  UV_BIN="$TOOLS_DIR/uv"
fi
[ -x "$UV_BIN" ] || die "UV_BIN is not executable: $UV_BIN"
need_cmd curl

URL="http://127.0.0.1:$PORT/"

say_err "Starting initial server through On Hold"
RUN_ID=$("$HOLD_BIN" "$UV_BIN" run --python 3 python -m http.server "$PORT" --bind 127.0.0.1 --directory "$SITE_DIR")
ACTIVE_RUNS="$ACTIVE_RUNS $RUN_ID"
wait_for_url "$URL" 60 || die "server did not become reachable at $URL"

say_err "Creating profile '$PROFILE' from recorded run $RUN_ID"
"$HOLD_BIN" profile save "$RUN_ID" as "$PROFILE"

say_err "Stopping initial run before starting the profile"
"$HOLD_BIN" stop "$RUN_ID"
"$HOLD_BIN" prune "$RUN_ID" || true
ACTIVE_RUNS=""

say_err "Starting profile from $OTHER_DIR"
PROFILE_RUN_ID=$(cd "$OTHER_DIR" && "$HOLD_BIN" start "$PROFILE")
ACTIVE_RUNS="$PROFILE_RUN_ID"
wait_for_url "$URL" 60 || die "profile-backed server did not become reachable at $URL"

trap - EXIT HUP INT TERM

cat <<EOF
On Hold + uv demo is running.

URL:         $URL
profile:     $PROFILE
run ID:      $PROFILE_RUN_ID
On Hold bin: $HOLD_BIN
uv bin:      $UV_BIN

Inspect:
  "$HOLD_BIN" list
  "$HOLD_BIN" dump "$PROFILE"

Stop and clean up:
  "$HOLD_BIN" stop "$PROFILE"
  "$HOLD_BIN" prune "$PROFILE_RUN_ID"
EOF
