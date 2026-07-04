#!/usr/bin/env bash
set -Eeuo pipefail

HOLD_REAL_BIN="${HOLD_BIN:-./hold}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

FAILS=0
SUITE_ROOT="$(mktemp -d)"
chmod 755 "$SUITE_ROOT"
TEST_ROOT=""
HOME=""
HOLD_TEST_SYSTEM_STATE_DIR=""
ROOT_HOME=""
ACTOR_HOME=""
TEST_USER=""
TEST_UID=""
TEST_GID=""
USER_ACTOR_NEEDS_SUDO=0
ROOT_ACTOR_AVAILABLE=0
SUDO_BIN="$(command -v sudo || true)"
USER_CREATED=0
ROOT_SAFE_HOLD_DIR=""
HOLD_TEST_TIMEOUT="${HOLD_TEST_TIMEOUT:-25}"

PASSES=0
SKIPS=0
pass() { echo "PASS: $1"; PASSES=$((PASSES + 1)); }
fail() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }
skip_note() { echo "SKIP: $1"; SKIPS=$((SKIPS + 1)); }
# Call `skip "<reason>"` from inside a test to report SKIP instead of a vacuous
# PASS. It returns sentinel 77, which run_test maps to SKIP (or to FAIL when
# HOLD_REQUIRE_ROOT_TESTS=1, so CI cannot silently no-op the root lane).
skip() { [ -n "${1:-}" ] && echo "  (skipped: $1)" >&2; return 77; }

# Refuse to run vacuously: a missing core tool (e.g. ps from procps) must be a
# hard, visible failure, never a quietly-green test.
require_tools() {
  local missing="" t
  for t in ps stat sed awk grep find mktemp head tr cut id chmod kill; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
  done
  if [ -n "$missing" ]; then
    echo "FATAL: required test tools missing:$missing" >&2
    echo "  install them (e.g. 'procps' provides ps) and re-run; the suite refuses to pass vacuously." >&2
    exit 1
  fi
  case "$HOLD_TEST_TIMEOUT" in
    ''|*[!0-9]*|0)
      echo "FATAL: HOLD_TEST_TIMEOUT must be a positive integer number of seconds" >&2
      exit 1
      ;;
  esac
}

suite_cleanup() {
  set +e
  if [ "$USER_CREATED" -eq 1 ] && [ -n "$TEST_USER" ]; then
    userdel "$TEST_USER" >/dev/null 2>&1 || true
  fi
  if [ -n "$ROOT_SAFE_HOLD_DIR" ]; then
    if [ "$(id -u)" -eq 0 ]; then
      rm -rf "$ROOT_SAFE_HOLD_DIR" >/dev/null 2>&1 || true
    elif [ -n "$SUDO_BIN" ]; then
      "$SUDO_BIN" -n rm -rf "$ROOT_SAFE_HOLD_DIR" >/dev/null 2>&1 || true
    fi
  fi
  remove_tree "$SUITE_ROOT"
}
trap suite_cleanup EXIT

remove_tree() {
  local path="$1"
  [ -n "$path" ] || return 0
  [ -e "$path" ] || return 0

  # Fast path: almost every test tree is user-owned and removable as-is. Avoid
  # recursive chmod/chown unless cleanup actually hits root-owned leftovers.
  rm -rf "$path" 2>/dev/null || true
  [ ! -e "$path" ] || [ -L "$path" ] || {
    if [ "$(id -u)" -eq 0 ]; then
      chmod -R u+rwX "$path" 2>/dev/null || true
      rm -rf "$path" || true
    elif [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -n "$SUDO_BIN" ]; then
      # Root-owned artifacts are expected only in the synthetic system store
      # and root-home fixtures. Normalize those directories instead of walking
      # the whole temp tree on every cleanup.
      while IFS= read -r root_owned_dir; do
        "$SUDO_BIN" -n chmod -R u+rwX "$root_owned_dir" 2>/dev/null || true
        "$SUDO_BIN" -n chown -R "$(id -u):$(id -g)" "$root_owned_dir" 2>/dev/null || true
      done < <(find "$path" -maxdepth 3 -type d \( -name system -o -name root-home \) -print 2>/dev/null || true)
      rm -rf "$path" 2>/dev/null || "$SUDO_BIN" -n rm -rf "$path" || true
    else
      rm -rf "$path" || true
    fi
  }
  return 0
}

setup_suite_actors() {
  if [ "$(id -u)" -eq 0 ]; then
    USER_ACTOR_NEEDS_SUDO=1
    ROOT_ACTOR_AVAILABLE=1
    TEST_USER="holdtest_$$"
    ACTOR_HOME="$SUITE_ROOT/user-home"
    mkdir -p "$ACTOR_HOME" || return 1
    if ! id "$TEST_USER" >/dev/null 2>&1; then
      useradd -M -d "$ACTOR_HOME" -s /bin/sh "$TEST_USER" || return 1
      USER_CREATED=1
    fi
    TEST_UID="$(id -u "$TEST_USER")"
    TEST_GID="$(id -g "$TEST_USER")"
    chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  else
    USER_ACTOR_NEEDS_SUDO=0
    TEST_USER="$(id -un)"
    TEST_UID="$(id -u)"
    TEST_GID="$(id -g)"
    if [ -n "$SUDO_BIN" ] && "$SUDO_BIN" -n true >/dev/null 2>&1; then
      ROOT_ACTOR_AVAILABLE=1
    else
      ROOT_ACTOR_AVAILABLE=0
    fi
  fi
  export HOLD_REAL_BIN SUDO_BIN TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE
}

as_user() {
  local env_args
  env_args=(
    "HOME=$ACTOR_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${env_args[@]}" "$@"
  else
    env "${env_args[@]}" "$@"
  fi
}

as_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 77
  local env_args
  env_args=(
    "HOME=$ROOT_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$(id -u)" -eq 0 ]; then
    env "${env_args[@]}" "$@"
  else
    "$SUDO_BIN" -n env "${env_args[@]}" "$@"
  fi
}

as_sudo_from_user() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 77
  local env_args
  env_args=(
    "HOME=$ROOT_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "HOLD_TEST_INVOKING_HOME=$ACTOR_HOME"
    "SUDO_UID=$TEST_UID"
    "SUDO_GID=$TEST_GID"
    "SUDO_USER=$TEST_USER"
    "PATH=$PATH"
  )
  local name
  for name in \
    HOLD_BOOT_ID_PATH \
    HOLD_TEST_HOME_ROOT \
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC; do
    if [ "${!name+x}" = x ]; then
      env_args+=("$name=${!name}")
    fi
  done
  if [ "$(id -u)" -eq 0 ]; then
    env "${env_args[@]}" "$@"
  else
    "$SUDO_BIN" -n env "${env_args[@]}" "$@"
  fi
}

write_user_actor_wrapper() {
  cat > "$TEST_ROOT/user-hold" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
env_args=(
  "HOME=$HOLD_ACTOR_HOME"
  "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
  "HOLD_TEST_INVOKING_HOME=$HOLD_ACTOR_HOME"
  "PATH=$PATH"
)
for name in \
  HOLD_BOOT_ID_PATH \
  HOLD_TEST_HOME_ROOT \
  HOLD_TEST_FAIL_RECORD_WRITE \
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
  HOLD_FAKE_SUDO_ARGV \
  HOLD_FAKE_SUDO_RC; do
  if [ "${!name+x}" = x ]; then
    env_args+=("$name=${!name}")
  fi
done
if [ "${HOLD_USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then
  exec "$HOLD_ACTOR_SUDO_BIN" -n -u "$HOLD_ACTOR_USER" env "${env_args[@]}" "$HOLD_REAL_BIN" "$@"
fi
exec env "${env_args[@]}" "$HOLD_REAL_BIN" "$@"
SH
  chmod +x "$TEST_ROOT/user-hold" || return 1
  HOLD_BIN="$TEST_ROOT/user-hold"
  export HOLD_BIN
}

new_env() {
  TEST_ROOT="$(mktemp -d "$SUITE_ROOT/test.XXXXXX")" || return 1
  chmod 755 "$TEST_ROOT" || return 1
  HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
  HOLD_BOOT_ID_PATH="$TEST_ROOT/boot_id"
  printf 'boot-main\n' >"$HOLD_BOOT_ID_PATH" || return 1
  ROOT_HOME="$TEST_ROOT/root-home"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR" "$ROOT_HOME" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" || return 1
  chmod 700 "$ROOT_HOME" || return 1

  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  else
    ACTOR_HOME="$TEST_ROOT/home"
    mkdir -p "$ACTOR_HOME" || return 1
    chmod 700 "$ACTOR_HOME" || return 1
  fi

  HOME="$ACTOR_HOME"
  export HOME TEST_ROOT HOLD_TEST_SYSTEM_STATE_DIR HOLD_BOOT_ID_PATH ROOT_HOME ACTOR_HOME
  export HOLD_ACTOR_HOME="$ACTOR_HOME"
  export HOLD_ACTOR_USER="$TEST_USER"
  export HOLD_ACTOR_SUDO_BIN="$SUDO_BIN"
  export HOLD_USER_ACTOR_NEEDS_SUDO="$USER_ACTOR_NEEDS_SUDO"
  write_user_actor_wrapper
}

cleanup_env() {
  local store ids id sys_store
  store="$HOME/.local/state/hold"
  if [ -d "$store" ]; then
    ids=$(find "$store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
    done
  fi
  sys_store="$HOLD_TEST_SYSTEM_STATE_DIR/runs"
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -d "$sys_store" ]; then
    ids=$(find "$sys_store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      as_root "$HOLD_REAL_BIN" kill "system:$id" >/dev/null 2>&1 || true
    done
  fi
  remove_tree "$TEST_ROOT"
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ] && [ -n "$ACTOR_HOME" ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
  fi
}

ensure_user_fixture_store() {
  mkdir -p "$HOME/.local/state/hold" || return 1
  chmod 700 "$HOME" "$HOME/.local" "$HOME/.local/state" "$HOME/.local/state/hold" 2>/dev/null || true
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown -R "$TEST_UID:$TEST_GID" "$HOME/.local" || return 1
  fi
}

file_mode() {
  stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"
}

resolve_path() {
  local path="$1" dir base
  if command -v realpath >/dev/null 2>&1; then
    realpath "$path"
    return
  fi
  dir="$(dirname "$path")"
  base="$(basename "$path")"
  (cd "$dir" && printf '%s/%s\n' "$(pwd -P)" "$base")
}

root_file_exists() {
  as_root test -f "$1"
}

root_path_absent() {
  as_root sh -c '[ ! -e "$1" ]' sh "$1"
}

root_file_mode() {
  as_root stat -c '%a' "$1" 2>/dev/null || as_root stat -f '%Lp' "$1"
}

root_file_owner() {
  as_root stat -c '%u:%g' "$1" 2>/dev/null || as_root stat -f '%u:%g' "$1"
}

sha256_stdin() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  else
    printf '%s\n' 'missing sha256sum or shasum for SHA-256 test helper' >&2
    return 127
  fi
}


root_grep() {
  local pattern="$1" path="$2"
  as_root grep -F -q -- "$pattern" "$path"
}

terminate_pid_tree() {
  local pid="$1" signal_name="${2:-TERM}" child
  [ -n "$pid" ] || return 0
  for child in $(ps -eo pid=,ppid= 2>/dev/null | awk -v p="$pid" '$2 == p { print $1 }'); do
    terminate_pid_tree "$child" "$signal_name"
  done
  kill "-$signal_name" "$pid" 2>/dev/null || true
}

print_timeout_diagnostics() {
  local desc="$1" fn="$2"
  {
    echo "TIMEOUT: $fn ($desc) exceeded ${HOLD_TEST_TIMEOUT}s"
    echo "TEST_ROOT: ${TEST_ROOT:-}"
    echo "--- relevant ps output ---"
    ps -eo pid=,ppid=,pgid=,stat=,etime=,args= 2>/dev/null |
      awk -v root="${TEST_ROOT:-}" -v suite="$SUITE_ROOT" '
        NR == 1 || $0 ~ /hold/ || (root != "" && index($0, root)) || index($0, suite) || $0 ~ /sleep|bash|sh/ {
          print
          shown++
          if (shown >= 120) exit
        }
      ' || true
    echo "--- bounded find listing for TEST_ROOT ---"
    if [ -n "${TEST_ROOT:-}" ] && [ -d "$TEST_ROOT" ]; then
      find "$TEST_ROOT" -maxdepth 4 -print 2>/dev/null | sort | head -n 200 || true
    else
      echo "(TEST_ROOT unavailable)"
    fi
  } >&2
}

run_test() {
  local desc="$1" fn="$2" rc="" pid deadline
  echo "RUN: $desc"
  new_env || { fail "$desc"; return; }
  export HOME TEST_ROOT HOLD_BIN HOLD_REAL_BIN HOLD_TEST_SYSTEM_STATE_DIR ROOT_HOME ACTOR_HOME
  export TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE SUDO_BIN
  export HOLD_ACTOR_HOME HOLD_ACTOR_USER HOLD_ACTOR_SUDO_BIN HOLD_USER_ACTOR_NEEDS_SUDO
  set +e
  ( set -Eeuo pipefail; "$fn" ) &
  pid=$!
  deadline=$((SECONDS + HOLD_TEST_TIMEOUT))
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
      print_timeout_diagnostics "$desc" "$fn"
      terminate_pid_tree "$pid" TERM
      sleep 0.5
      terminate_pid_tree "$pid" KILL
      wait "$pid" 2>/dev/null || true
      rc=124
      break
    fi
    sleep 0.1
  done
  if [ -z "$rc" ]; then
    wait "$pid"
    rc=$?
  fi
  set -e
  if [ "$rc" -eq 0 ]; then
    pass "$desc"
  elif [ "$rc" -eq 77 ]; then
    if [ "${HOLD_REQUIRE_ROOT_TESTS:-0}" -eq 1 ]; then
      fail "$desc (skipped, but HOLD_REQUIRE_ROOT_TESTS=1)"
    else
      skip_note "$desc"
    fi
  else
    fail "$desc"
  fi
  cleanup_env
}

setup_suite_actors
require_tools

extract_id() {
  sed -n -e '/^[0-9a-f]\{12\}$/p' -e '/^[0-9a-f]\{64\}$/p' -e 's/^hold: id=\([0-9a-f][0-9a-f]*\).*/\1/p' | head -n1 | cut -c1-12
}

find_prefixed_artifact() {
  local store="$1" id="$2" suffix="$3" path count
  [ -n "$store" ] && [ -n "$id" ] && [ -d "$store" ] || return 1
  if [ -e "$store/$id$suffix" ]; then
    printf '%s\n' "$store/$id$suffix"
    return 0
  fi
  count=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" 2>/dev/null | wc -l)
  [ "$count" = "1" ] || return 1
  path=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" -print -quit 2>/dev/null)
  [ -n "$path" ] || return 1
  printf '%s\n' "$path"
}

record_path() {
  local id="$1" store="${2:-$HOME/.local/state/hold}"
  find_prefixed_artifact "$store" "$id" ".json"
}

log_path() {
  local id="$1" store="${2:-$HOME/.local/state/hold}"
  find_prefixed_artifact "$store" "$id" ".log"
}

record_exists() {
  record_path "$1" "${2:-$HOME/.local/state/hold}" >/dev/null 2>&1
}

log_exists() {
  log_path "$1" "${2:-$HOME/.local/state/hold}" >/dev/null 2>&1
}

root_find_prefixed_artifact() {
  local store="$1" id="$2" suffix="$3"
  as_root sh -c '
store=$1
id=$2
suffix=$3
[ -n "$store" ] && [ -n "$id" ] && [ -d "$store" ] || exit 1
if [ -e "$store/$id$suffix" ]; then
  printf "%s\n" "$store/$id$suffix"
  exit 0
fi
count=$(find "$store" -maxdepth 1 -type f -name "$id*$suffix" 2>/dev/null | wc -l)
[ "$count" = "1" ] || exit 1
find "$store" -maxdepth 1 -type f -name "$id*$suffix" -print -quit 2>/dev/null
' sh "$store" "$id" "$suffix"
}

root_record_path() {
  root_find_prefixed_artifact "$2" "$1" ".json"
}

root_log_path() {
  root_find_prefixed_artifact "$2" "$1" ".log"
}

root_record_exists() {
  root_record_path "$1" "$2" >/dev/null 2>&1
}

root_log_exists() {
  root_log_path "$1" "$2" >/dev/null 2>&1
}

record_pgid() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"pgid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$record" | head -n1
}

record_sid() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"sid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$record" | head -n1
}

record_name() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record
  record=$(record_path "$id" "$store") || return 1
  sed -n 's/.*"name": "\([^"]*\)".*/\1/p' "$record" | head -n1
}

# Poll until a call's record reports the exited state, so purge/redial (which
# require an ended call) do not race the supervisor's finish write.
record_ended_soon() {
  local id="$1" store="${2:-$HOME/.local/state/hold}" record tries
  for tries in $(seq 1 60); do
    record=$(record_path "$id" "$store") || return 1
    grep -q '"state": "exited"' "$record" && return 0
    sleep 0.05
  done
  return 1
}


pid_dead_enough() {
  local p="$1" st
  if ! kill -0 "$p" 2>/dev/null; then
    return 0
  fi
  st=$(ps -o stat= -p "$p" 2>/dev/null | tr -d ' ' | cut -c1 || true)
  [ "$st" = "Z" ] || ! kill -0 "$p" 2>/dev/null
}

pgid_terminated() {
  local g="$1" tries stats
  for tries in $(seq 1 40); do
    if ! kill -0 "-$g" 2>/dev/null; then
      return 0
    fi
    stats=$(ps -o stat= -g "$g" 2>/dev/null | tr -d " " || true)
    if [ -n "$stats" ] && ! printf "%s\n" "$stats" | grep -qv "^Z"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

path_absent_soon() {
  local path="$1" tries
  for tries in $(seq 1 40); do
    [ ! -e "$path" ] && return 0
    sleep 0.05
  done
  return 1
}

test_lifecycle() {
  local out id lines
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  # Detached bare form prints exactly the 64-hex call id, nothing else.
  printf '%s\n' "$out" | grep -Eqx '[0-9a-f]{64}' || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$id" | grep -Eq '^[0-9a-f]{12}$' || return 1
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*Up "
  "$HOLD_BIN" end "$id" >/dev/null
  "$HOLD_BIN" list -a | grep -Eq "^$id[[:space:]].*Exited"
  "$HOLD_BIN" purge >/dev/null
  lines=$("$HOLD_BIN" list -a | wc -l)
  [ "$lines" -eq 1 ]
}


test_kill_subcommand() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" kill "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_group_kill_children() {
  local out id pgid children
  out=$("$HOLD_BIN" -d bash -c 'sleep 600 & sleep 601 & wait' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  sleep 0.2
  children=$(ps -eo pid=,pgid=,args= | awk -v g="$pgid" '$2==g && $1!=g && $3 ~ /^sleep$/ {print $1}')
  [ -n "$children" ] || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  sleep 0.2
  for p in $children; do
    pid_dead_enough "$p" || return 1
  done
  return 0
}

test_exec_failure_no_record() {
  local rc count
  set +e
  "$HOLD_BIN" nonexistent_binary_xyz >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  if [ -d "$HOME/.local/state/hold" ]; then
    count=$(find "$HOME/.local/state/hold" -maxdepth 1 -type f -name '*.json' | wc -l)
  else
    count=0
  fi
  [ "$count" -eq 0 ] || return 1
  count=$(find "$HOME/.local/state/hold" -maxdepth 1 -type f 2>/dev/null | wc -l)
  [ "$count" -eq 0 ]
}

test_fast_exit_record_exited() {
  local out id record
  out=$("$HOLD_BIN" -d /bin/sh -c 'exit 7' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  record=$(record_path "$id") || return 1
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 7' "$record" && break
    sleep 0.1
  done
  grep -q '"state": "exited"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"exit_code": 7' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"ended_at": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \\(7\\)"
}

test_exec_replacement_remains_controllable() {
  local out id pgid
  out=$("$HOLD_BIN" -d bash -c 'exec sleep 300' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*Up " || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_corrupt_record_handling() {
  ensure_user_fixture_store || return 1
  printf 'garbage\n' > "$HOME/.local/state/hold/badbad00cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^badbad00cafe' "$TEST_ROOT/list.out"
  ! grep -Eq '^0[[:space:]]' "$TEST_ROOT/list.out"
  grep -q 'warning: skipping corrupt record badbad00cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/hold/badbad00cafe0000000000000000000000000000000000000000000000000000.json" ]
}

test_deep_json_record_rejected_not_crashed() {
  ensure_user_fixture_store || return 1
  local json i
  json='{"junk":'
  for i in $(seq 1 80); do json="${json}["; done
  json="${json}0"
  for i in $(seq 1 80); do json="${json}]"; done
  json="${json},\"version\":1,\"id\":\"abc12445cafe\",\"pid\":12345,\"pgid\":12345,\"sid\":12345,\"start_unix_ns\":0,\"argv\":[\"x\"],\"cmdline_display\":\"x\",\"uid\":0,\"gid\":0,\"proc_starttime_ticks\":0,\"exe_dev\":0,\"exe_ino\":0}"
  printf '%s\n' "$json" > "$HOME/.local/state/hold/abc12445cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12445cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12445cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
}

test_symlinked_record_rejected() {
  ensure_user_fixture_store || return 1
  cat > "$TEST_ROOT/record-target.json" <<'JSON'
{"version":1,"id":"abc12545cafe0000000000000000000000000000000000000000000000000000","pid":12345,"pgid":12345,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  ln -s "$TEST_ROOT/record-target.json" "$HOME/.local/state/hold/abc12545cafe0000000000000000000000000000000000000000000000000000.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12545cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12545cafe0000000000000000000000000000000000000000000000000000.json' "$TEST_ROOT/list.err"
}

test_symlinked_log_rejected() {
  local out id log rc
  out=$("$HOLD_BIN" -d /bin/echo original-log-line 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log=$(log_path "$id") || return 1
  sleep 0.2
  [ -f "$log" ] || return 1
  printf 'symlink-secret\n' > "$TEST_ROOT/log-target" || return 1
  rm -f "$log" || return 1
  ln -s "$TEST_ROOT/log-target" "$log" || return 1
  set +e
  "$HOLD_BIN" logs "$id" -p >"$TEST_ROOT/dump.out" 2>"$TEST_ROOT/dump.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  ! grep -q 'symlink-secret' "$TEST_ROOT/dump.out" || return 1
  grep -q 'failed to open log' "$TEST_ROOT/dump.err"
}

test_invalid_pgid_record() {
  ensure_user_fixture_store || return 1
  cat > "$HOME/.local/state/hold/abc12345cafe0000000000000000000000000000000000000000000000000000.json" <<'JSON'
{"version":1,"id":"abc12345cafe0000000000000000000000000000000000000000000000000000","pid":12345,"pgid":0,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12345cafe' "$TEST_ROOT/list.out"
}

test_orphan_log_cleanup() {
  ensure_user_fixture_store || return 1
  : > "$HOME/.local/state/hold/a1b2c3d4e5f60000000000000000000000000000000000000000000000000000.log" || return 1
  : > "$HOME/.local/state/hold/deadbeefcafe0000000000000000000000000000000000000000000000000000.log" || return 1
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/hold/a1b2c3d4e5f60000000000000000000000000000000000000000000000000000.log" ] && [ ! -e "$HOME/.local/state/hold/deadbeefcafe0000000000000000000000000000000000000000000000000000.log" ]
}

test_id_sanitization() {
  local rc
  for bad in '../../etc/passwd' 'AABBCC' 'hello!' ''; do
    set +e
    "$HOLD_BIN" stop "$bad" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 5 ] || return 1
  done
}

test_print_signal_output() {
  local out id pgid got
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  got=$("$HOLD_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ]
  got=$("$HOLD_BIN" kill --print "$id") || return 1
  [ "$got" = "kill -KILL -- -$pgid" ]
}

test_signal_refuses_tampered_live_group_identity() {
  local out id rec real_pgid real_sid decoy_out decoy_id decoy_pgid decoy_sid rc
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  real_pgid=$(record_pgid "$id")
  real_sid=$(record_sid "$id")
  [ -n "$id" ] && [ -n "$real_pgid" ] && [ -n "$real_sid" ] || return 1

  decoy_out=$("$HOLD_BIN" -d sleep 300 2>&1) || { "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  decoy_id=$(printf '%s\n' "$decoy_out" | extract_id)
  decoy_pgid=$(record_pgid "$decoy_id")
  decoy_sid=$(record_sid "$decoy_id")
  [ -n "$decoy_id" ] && [ -n "$decoy_pgid" ] && [ -n "$decoy_sid" ] || {
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    [ -n "$decoy_id" ] && "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
    return 1
  }
  [ "$decoy_pgid" != "$real_pgid" ] && [ "$decoy_sid" != "$real_sid" ] || {
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
    return 1
  }

  rec=$(record_path "$id") || return 1
  cp "$rec" "$rec.bak" || return 1
  sed -i.tmp \
    -e "s/\"pgid\":[[:space:]]*[0-9][0-9]*/\"pgid\":$decoy_pgid/" \
    -e "s/\"sid\":[[:space:]]*[0-9][0-9]*/\"sid\":$decoy_sid/" \
    "$rec" || return 1
  rm -f "$rec.tmp"
  set +e
  "$HOLD_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/tampered-live-print.err"
  rc=$?
  set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  "$HOLD_BIN" stop "$decoy_id" >/dev/null 2>&1 || true
  [ "$rc" -eq 2 ] || { echo "--print on tampered live pgid/sid: rc=$rc (want 2 refused)" >&2; return 1; }
  grep -Eq 'process group/session differs|cannot be signaled' "$TEST_ROOT/tampered-live-print.err" || {
    cat "$TEST_ROOT/tampered-live-print.err" >&2
    return 1
  }
}

test_stop_multiple_ids() {
  local out1 out2 id1 id2 pgid1 pgid2 rc
  out1=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  out2=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid1=$(record_pgid "$id1")
  pgid2=$(record_pgid "$id2")
  [ -n "$id1" ] && [ -n "$id2" ] && [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  set +e
  "$HOLD_BIN" stop "$id1" "$id2" >/dev/null
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_argument_edges() {
  local rc out
  set +e
  printf '' | "$HOLD_BIN" >"$TEST_ROOT/bare-hold.out" 2>"$TEST_ROOT/bare-hold.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/bare-hold.out" "$TEST_ROOT/bare-hold.err" >&2; return 1; }
  grep -q 'PLACE A CALL' "$TEST_ROOT/bare-hold.out" || { cat "$TEST_ROOT/bare-hold.out" >&2; return 1; }
  set +e
  "$HOLD_BIN" purge -all >/dev/null 2>"$TEST_ROOT/prune-flag.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  grep -q "unknown flag '-all'" "$TEST_ROOT/prune-flag.err" || return 1
  set +e
  "$HOLD_BIN" end >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || return 1
  "$HOLD_BIN" --help >/dev/null || return 1
  "$HOLD_BIN" help >/dev/null || return 1
  "$HOLD_BIN" help --help >/dev/null || return 1
  "$HOLD_BIN" help targets | grep -q 'call id, leading id prefix, or call name' || return 1
  "$HOLD_BIN" help scripting | grep -q 'Exit codes:' || return 1
  "$HOLD_BIN" end -h | grep -q 'usage: hold end' || return 1
  out=$("$HOLD_BIN" --version) || return 1
  printf '%s\n' "$out" | grep -Eq '^(dev(-[0-9a-f]{7,40}(-dirty)?)?|[0-9a-f]{7,40}|v?[0-9]+\.[0-9]+\.[0-9]+.*)$'
  set +e
  "$HOLD_BIN" -l >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  set +e
  "$HOLD_BIN" --list >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  "$HOLD_BIN" -- sleep 1 >/dev/null
}

test_special_chars_args() {
  local out id json
  out=$("$HOLD_BIN" -d echo "hello world" "it's" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(record_path "$id") || return 1
  [ -f "$json" ] || return 1
  grep -Fq '"hello world"' "$json"
  grep -Fq '"it'"'"'s"' "$json"
}

test_log_capture() {
  local out id log
  out=$("$HOLD_BIN" -d bash -c 'echo out; echo err >&2; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log=$(log_path "$id") || return 1
  sleep 0.4
  [ -f "$log" ] || return 1
  [ -f "$log.idx" ] || return 1
  head -c 7 "$log.idx" | grep -q '^HLOGIDX' || return 1
  [ "$(wc -c <"$log.idx")" -gt 80 ] || { ls -l "$log.idx" >&2; return 1; }
  ! grep -q '"log"' "$log" || { cat "$log" >&2; return 1; }
  grep -q '^out$' "$log" && grep -q '^err$' "$log" || return 1
  "$HOLD_BIN" __view "$id" --filter out --limit 1 >"$TEST_ROOT/log-capture-view.out" 2>"$TEST_ROOT/log-capture-view.stats" || return 1
  grep -qx 'out' "$TEST_ROOT/log-capture-view.out" || { cat "$TEST_ROOT/log-capture-view.out" >&2; return 1; }
}



test_log_view_internal_seed_filters() {
  local out id viewed stats similar plain
  out=$("$HOLD_BIN" -d bash -c 'echo alpha; echo "needle one"; echo "warn database connection timeout retrying"; echo omega; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.4
  viewed=$("$HOLD_BIN" __view "$id" --filter needle --limit 1) || return 1
  [ "$viewed" = "needle one" ] || return 1
  plain=$("$HOLD_BIN" __view "$id" --plain --filter needle --limit 1) || return 1
  [ "$plain" = "needle one" ] || return 1
  similar=$("$HOLD_BIN" __view "$id" --similar "error database connection timeout" --limit 1) || return 1
  printf '%s\n' "$similar" | grep -q 'database connection timeout' || return 1
  stats=$("$HOLD_BIN" __view "$id" --filter needle --limit 1 --debug-stats 2>&1 >/dev/null) || return 1
  printf '%s\n' "$stats" | grep -Eq 'bytes_read=[0-9]+ .*visible=1'
  set +e
  "$HOLD_BIN" __view "$id" --interactive >"$TEST_ROOT/view-interactive.out" 2>"$TEST_ROOT/view-interactive.err"
  [ "$?" -eq 5 ] || { set -e; cat "$TEST_ROOT/view-interactive.out" "$TEST_ROOT/view-interactive.err" >&2; return 1; }
  set -e
  grep -q -- '--interactive requires a TTY' "$TEST_ROOT/view-interactive.err"
}

test_log_view_follow_filters_live_output() {
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'sleep 0.2; echo ignore-before; sleep 0.2; echo "live-needle one"; sleep 0.2; echo ignore-after; sleep 0.2; echo "live-needle two"' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  "$HOLD_BIN" __view "$id" --follow --filter live-needle >"$TEST_ROOT/view-follow.out" 2>"$TEST_ROOT/view-follow.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow.err" >&2; return 1; }
  grep -q 'live-needle one' "$TEST_ROOT/view-follow.out" || { cat "$TEST_ROOT/view-follow.out" >&2; return 1; }
  grep -q 'live-needle two' "$TEST_ROOT/view-follow.out" || { cat "$TEST_ROOT/view-follow.out" >&2; return 1; }
  ! grep -q 'ignore-before' "$TEST_ROOT/view-follow.out" || { cat "$TEST_ROOT/view-follow.out" >&2; return 1; }
  ! grep -q 'ignore-after' "$TEST_ROOT/view-follow.out" || { cat "$TEST_ROOT/view-follow.out" >&2; return 1; }
}

test_hold_logs_follow_opens_dynamic_tty_filter() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'sleep 0.3; echo logs-ignore; sleep 0.3; echo "logs-live-hit"; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  python3 -c 'import sys,time; time.sleep(0.1); sys.stdout.write("logs-live"); sys.stdout.flush(); time.sleep(1.0); sys.stdout.write("q"); sys.stdout.flush()' |
    script -qfec "$HOLD_BIN logs $id --follow --interactive --debug-stats" /dev/null >"$TEST_ROOT/logs-follow.out" 2>"$TEST_ROOT/logs-follow.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/logs-follow.err" >&2; return 1; }
  grep -q 'filter: logs-live' "$TEST_ROOT/logs-follow.out" || { cat "$TEST_ROOT/logs-follow.out" >&2; return 1; }
  grep -q 'logs-live-hit' "$TEST_ROOT/logs-follow.out" || { cat "$TEST_ROOT/logs-follow.out" >&2; return 1; }
  ! grep -q 'logs-ignore' "$TEST_ROOT/logs-follow.out" || { cat "$TEST_ROOT/logs-follow.out" >&2; return 1; }
}

test_log_view_follow_dynamic_tty_filter() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'sleep 0.3; echo boring-ignore; sleep 0.3; echo "needle-live"; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  python3 -c 'import sys,time; time.sleep(0.1); sys.stdout.write("needle"); sys.stdout.flush(); time.sleep(1.0); sys.stdout.write("q"); sys.stdout.flush()' |
    script -qfec "$HOLD_BIN __view $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-dynamic.out" 2>"$TEST_ROOT/view-follow-dynamic.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-dynamic.err" >&2; return 1; }
  grep -q 'filter: needle' "$TEST_ROOT/view-follow-dynamic.out" || { cat "$TEST_ROOT/view-follow-dynamic.out" >&2; return 1; }
  grep -q 'needle-live' "$TEST_ROOT/view-follow-dynamic.out" || { cat "$TEST_ROOT/view-follow-dynamic.out" >&2; return 1; }
  ! grep -q 'boring-ignore' "$TEST_ROOT/view-follow-dynamic.out" || { cat "$TEST_ROOT/view-follow-dynamic.out" >&2; return 1; }
}

test_log_viewer_integrated_chrome_help_and_info() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc short
  # A named call: the header shows the call's name (the human handle), while the
  # short id stays where it has always been, in the footer.
  out=$("$HOLD_BIN" --name chrome_call -d -- /bin/sh -c 'echo "chrome one"; echo "chrome two"; echo "chrome err" >&2; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  short=$(printf '%s' "$id" | cut -c1-12)
  record_ended_soon "$id" || return 1
  set +e
  # Ctrl-T cycles timestamps to time; Ctrl-Y shows the source column; Ctrl-H
  # opens the footer help; the next key dismisses it; q quits.
  python3 -c 'import sys,time; o=sys.stdout.buffer; o.write(b"\x14\x19"); o.flush(); time.sleep(0.2); o.write(b"\x08"); o.flush(); time.sleep(0.2); o.write(b"q"); o.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 10 cols 80; $HOLD_BIN logs $id --interactive" /dev/null >"$TEST_ROOT/viewer-chrome.out" 2>"$TEST_ROOT/viewer-chrome.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/viewer-chrome.err" >&2; return 1; }
  local plain="$TEST_ROOT/viewer-chrome.plain"
  python3 -c 'import re,sys; raw=open(sys.argv[1],"rb").read().decode("utf-8","ignore"); sys.stdout.write(re.sub(r"\x1b\[[0-9;?]*[A-Za-z~]","",raw).replace("\r",""))' "$TEST_ROOT/viewer-chrome.out" >"$plain"
  # Header carries the call name; footer still carries the short id.
  grep -q "hold logs: chrome_call" "$plain" || { cat "$plain" >&2; return 1; }
  grep -q "$short" "$plain" || { cat "$plain" >&2; return 1; }
  grep -q 'VIEWING EXITED (0)' "$plain" || { cat "$plain" >&2; return 1; }
  grep -q 'Ctrl-H Help' "$plain" || { cat "$plain" >&2; return 1; }
  grep -Eq 'ts:time local +src:all +wrap:off' "$plain" || { cat "$plain" >&2; return 1; }
  grep -q 'OUT | chrome one' "$plain" || { cat "$plain" >&2; return 1; }
  grep -q 'ERR | chrome err' "$plain" || { cat "$plain" >&2; return 1; }
  grep -q 'Ctrl-T time' "$plain" || { cat "$plain" >&2; return 1; }
}

test_log_viewer_space_excludes_and_ctrl_r_resets_filters() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo "example database timeout"; echo "unrelated payment ok"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  # Space excludes the selected (top) record destructively; the dissimilar line
  # stays visible and the excluded line disappears.
  set +e
  python3 -c 'import sys,time; sys.stdout.buffer.write(b" "); sys.stdout.flush(); time.sleep(0.3); sys.stdout.buffer.write(b"q"); sys.stdout.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 8 cols 90; $HOLD_BIN logs $id --interactive" /dev/null >"$TEST_ROOT/viewer-exclude.out" 2>"$TEST_ROOT/viewer-exclude.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/viewer-exclude.err" >&2; return 1; }
  python3 - "$TEST_ROOT/viewer-exclude.out" <<'PY' || { cat "$TEST_ROOT/viewer-exclude.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
final = 'hold logs:' + plain.split('hold logs:')[-1]
if 'unrelated payment ok' not in final:
    raise SystemExit('dissimilar line was not kept visible after exclude')
if 'example database timeout' in final:
    raise SystemExit('excluded line remained visible in the final frame')
PY
  # Ctrl-R restores every excluded record.
  set +e
  python3 -c 'import sys,time; sys.stdout.buffer.write(b" "); sys.stdout.flush(); time.sleep(0.2); sys.stdout.buffer.write(b"\x12"); sys.stdout.flush(); time.sleep(0.2); sys.stdout.buffer.write(b"q"); sys.stdout.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 8 cols 90; $HOLD_BIN logs $id --interactive" /dev/null >"$TEST_ROOT/viewer-reset.out" 2>"$TEST_ROOT/viewer-reset.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/viewer-reset.err" >&2; return 1; }
  python3 - "$TEST_ROOT/viewer-reset.out" <<'PY' || { cat "$TEST_ROOT/viewer-reset.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
final = 'hold logs:' + plain.split('hold logs:')[-1]
for wanted in ('example database timeout', 'unrelated payment ok'):
    if wanted not in final:
        raise SystemExit(f'Ctrl-R did not restore {wanted!r} in the final frame')
PY
}

test_log_view_selection_uses_cached_rows() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc max_gen
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'printf "needle 1\nneedle 2\nneedle 3\nneedle 4\n"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; sys.stdout.write("needlejjkkq"); sys.stdout.flush(); time.sleep(0.1)' |
    script -qfec "$HOLD_BIN __view $id --interactive --debug-stats" /dev/null >"$TEST_ROOT/view-cache-selection.out" 2>"$TEST_ROOT/view-cache-selection.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-cache-selection.err" >&2; return 1; }
  grep -q 'filter: needle' "$TEST_ROOT/view-cache-selection.out" || { cat "$TEST_ROOT/view-cache-selection.out" >&2; return 1; }
  max_gen=$(grep -ao 'scan_gen=[0-9][0-9]*' "$TEST_ROOT/view-cache-selection.out" | sed 's/scan_gen=//' | sort -n | tail -n1)
  [ -n "$max_gen" ] || { cat "$TEST_ROOT/view-cache-selection.out" >&2; return 1; }
  [ "$max_gen" -le 7 ] || { echo "scan_gen advanced during selection movement: $max_gen" >&2; cat "$TEST_ROOT/view-cache-selection.out" >&2; return 1; }
}

test_log_view_follow_pages_filtered_windows() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in 1 2 3 4 5 6 7 8 9; do echo "page-hit-$i"; done; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"page-hit"); out.flush(); time.sleep(0.1); out.write(b"\x1b[5~"); out.flush(); time.sleep(0.1); out.write(b"\x1b[6~q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 7 cols 100; $HOLD_BIN __view $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-pages.out" 2>"$TEST_ROOT/view-follow-pages.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-pages.err" >&2; return 1; }
  grep -q 'page-hit-4' "$TEST_ROOT/view-follow-pages.out" || { cat "$TEST_ROOT/view-follow-pages.out" >&2; return 1; }
  grep -q 'page-hit-9' "$TEST_ROOT/view-follow-pages.out" || { cat "$TEST_ROOT/view-follow-pages.out" >&2; return 1; }
  grep -q 'follow=browsing' "$TEST_ROOT/view-follow-pages.out" || { cat "$TEST_ROOT/view-follow-pages.out" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-pages.out" <<'PY' || { cat "$TEST_ROOT/view-follow-pages.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
if stats[-1][4] != 'browsing':
    raise SystemExit(f'PageDown after PageUp snapped back to live tail: {stats[-1]}')
PY
}

test_log_view_follow_page_up_stays_at_start() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 40); do echo "topjump-line-$i"; done; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 15); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-top.out" 2>"$TEST_ROOT/view-follow-top.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-top.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-top.out" <<'PY' || { cat "$TEST_ROOT/view-follow-top.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw)
stats = re.findall(r'scan_gen=(\d+) offset=(\d+).*?follow=(tail|browsing)', plain)
if not stats:
    raise SystemExit('missing debug stats')
max_gen = max(int(gen) for gen, _, _ in stats)
offset, follow = stats[-1][1:]
if follow != 'browsing':
    raise SystemExit(f'expected final top page to stay in browsing mode, got follow={follow}')
if offset != '0':
    raise SystemExit(f'expected final top page to anchor at offset 0, got offset={offset}')
if 'topjump-line-1' not in plain:
    raise SystemExit('top line never rendered')
if 'topjump-line-4' not in plain:
    raise SystemExit('top page did not fill downward from the first line')
if max_gen > 11:
    raise SystemExit(f'page-up at log start kept rescanning/wrapping; max scan_gen={max_gen} final offset={offset}')
PY
}


test_log_view_follow_oldest_page_does_not_wrap_to_tail() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 50); do echo "nowrap-line-$i"; done; sleep 0.5' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 20); out.flush(); time.sleep(0.1); out.write(b"\x1b[A" * 8); out.flush(); time.sleep(0.7); out.write(b"\x1b[A" * 3); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-nowrap-tail.out" 2>"$TEST_ROOT/view-follow-nowrap-tail.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-nowrap-tail.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-nowrap-tail.out" <<'PY' || { cat "$TEST_ROOT/view-follow-nowrap-tail.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] != 'browsing':
    raise SystemExit(f'oldest page wrapped away from top instead of staying browsing offset 0: {last}')
final = plain[-1200:]
for wanted in ('nowrap-line-1', 'nowrap-line-2', 'nowrap-line-3', 'nowrap-line-4'):
    if wanted not in final:
        raise SystemExit(f'final page wrapped off the oldest log line {wanted}')
if 'nowrap-line-47' in final or 'nowrap-line-50' in final:
    raise SystemExit('final page jumped back to the tail')
PY
}


test_log_view_follow_arrow_up_to_top_does_not_wrap_to_tail() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 45); do echo "arrowtop-line-$i"; done; sleep 0.3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[A" * 80); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-arrow-top.out" 2>"$TEST_ROOT/view-follow-arrow-top.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-arrow-top.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-arrow-top.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-arrow-top.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] != 'browsing':
    raise SystemExit(f'ArrowUp at oldest page wrapped away from top instead of staying browsing offset 0: {last}')
final = plain[-1400:]
for wanted in ('arrowtop-line-1', 'arrowtop-line-2', 'arrowtop-line-3', 'arrowtop-line-4'):
    if wanted not in final:
        raise SystemExit(f'final page wrapped off oldest line {wanted}')
if any(f'arrowtop-line-{i}' in final for i in range(38, 46)):
    raise SystemExit('final ArrowUp page jumped back to the live tail')
PYCHECK
}



test_log_view_follow_top_navigation_is_idempotent() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 80); do echo "idempotent-top-line-$i"; done; sleep 0.8' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 30); out.flush(); time.sleep(0.1); out.write((b"\x1b[A" * 5 + b"\x1b[5~") * 4); out.flush(); time.sleep(0.7); out.write((b"\x1b[A" * 3 + b"\x1b[5~") * 2); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-top-idempotent.out" 2>"$TEST_ROOT/view-follow-top-idempotent.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-top-idempotent.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-top-idempotent.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-top-idempotent.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
first_top = next((i for i, st in enumerate(stats) if st[1] == '0' and st[4] == 'browsing'), None)
if first_top is None:
    raise SystemExit(f'never reached a browsed top page: {stats[-8:]}')
for st in stats[first_top:]:
    if st[1] != '0' or st[4] not in ('browsing', 'exited'):
        raise SystemExit(f'top navigation was not idempotent after reaching top; offending frame={st}')
final = plain[-1600:]
for wanted in ('idempotent-top-line-1', 'idempotent-top-line-2', 'idempotent-top-line-3', 'idempotent-top-line-4'):
    if wanted not in final:
        raise SystemExit(f'final page lost top line {wanted}')
if any(f'idempotent-top-line-{i}' in final for i in range(70, 81)):
    raise SystemExit('final page looped back to the bottom/tail')
PYCHECK
}

test_log_view_follow_repeated_top_commands_ignore_live_growth() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 40); do echo "sticktop-line-$i"; done; for i in $(seq 41 90); do sleep 0.03; echo "sticktop-line-$i"; done; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.15
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 40); out.flush(); time.sleep(0.2); out.write((b"\x1b[H" + b"\x1b[5~") * 5); out.flush(); time.sleep(1.4); out.write((b"\x1b[H" + b"\x1b[5~") * 3); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-repeated-top-live.out" 2>"$TEST_ROOT/view-follow-repeated-top-live.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-repeated-top-live.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-repeated-top-live.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-repeated-top-live.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
first_top = next((i for i, st in enumerate(stats) if st[1] == '0' and st[4] in ('browsing', 'exited')), None)
if first_top is None:
    raise SystemExit(f'never reached top page: {stats[-10:]}')
for st in stats[first_top:]:
    if st[1] != '0' or st[4] not in ('browsing', 'exited'):
        raise SystemExit(f'repeated top commands during live growth looped away from top: {st}')
final = plain[-1600:]
for wanted in ('sticktop-line-1', 'sticktop-line-2', 'sticktop-line-3', 'sticktop-line-4'):
    if wanted not in final:
        raise SystemExit(f'final top-pinned page lost oldest line {wanted}')
if any(f'sticktop-line-{i}' in final for i in range(80, 91)):
    raise SystemExit('final top-pinned page looped back to the newest tail')
PYCHECK
}

test_log_view_modified_page_up_reaches_oldest_page_without_tail_loop() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 80); do echo "modpgup-line-$i"; done; sleep 0.4' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5;5~" * 30); out.flush(); time.sleep(0.2); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-modpgup.out" 2>"$TEST_ROOT/view-follow-modpgup.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-modpgup.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-modpgup.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-modpgup.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] not in ('browsing', 'exited'):
    raise SystemExit(f'modified PageUp did not reach/stay at offset 0: {last}')
final = plain[-1400:]
for wanted in ('modpgup-line-1', 'modpgup-line-2', 'modpgup-line-3', 'modpgup-line-4'):
    if wanted not in final:
        raise SystemExit(f'final modified-PageUp page lost oldest line {wanted}')
if any(f'modpgup-line-{i}' in final for i in range(70, 81)):
    raise SystemExit('modified-PageUp page looped back to the bottom/tail')
PYCHECK
}

test_log_view_modified_home_key_pins_oldest_page_without_tail_loop() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 90); do echo "modhome-line-$i"; done; sleep 0.6; echo "modhome-line-91"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[1;5H"); out.flush(); time.sleep(0.9); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-modhomepin.out" 2>"$TEST_ROOT/view-follow-modhomepin.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-modhomepin.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-modhomepin.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-modhomepin.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] not in ('browsing', 'exited'):
    raise SystemExit(f'modified Home/top navigation did not stay anchored at offset 0: {last}')
final = 'hold logs' + plain.split('hold logs')[-1]
for wanted in ('modhome-line-1', 'modhome-line-2', 'modhome-line-3', 'modhome-line-4'):
    if wanted not in final:
        raise SystemExit(f'final modified-Home page lost oldest line {wanted}')
if any(f'modhome-line-{i}' in final for i in range(80, 92)):
    raise SystemExit('modified-Home page looped back to the bottom/tail')
PYCHECK
}

test_log_view_home_key_pins_oldest_page_without_tail_loop() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 90); do echo "homepin-line-$i"; done; sleep 0.6; echo "homepin-line-91"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[H"); out.flush(); time.sleep(0.9); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-homepin.out" 2>"$TEST_ROOT/view-follow-homepin.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-homepin.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-homepin.out" <<'PY' || { cat "$TEST_ROOT/view-follow-homepin.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] not in ('browsing', 'exited'):
    raise SystemExit(f'Home/top navigation did not stay anchored at offset 0: {last}')
final = 'hold logs' + plain.split('hold logs')[-1]
for wanted in ('homepin-line-1', 'homepin-line-2', 'homepin-line-3', 'homepin-line-4'):
    if wanted not in final:
        raise SystemExit(f'final Home-pinned page lost oldest line {wanted}')
if any(f'homepin-line-{i}' in final for i in range(80, 92)):
    raise SystemExit('Home-pinned page looped back to the bottom/tail')
PY
}

test_log_view_follow_page_down_from_top_does_not_wrap_to_tail() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 70); do echo "topdown-line-$i"; done; sleep 0.3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 25); out.flush(); time.sleep(0.1); out.write(b"\x1b[6~"); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-top-down-not-tail.out" 2>"$TEST_ROOT/view-follow-top-down-not-tail.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-top-down-not-tail.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-top-down-not-tail.out" <<'PY'
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[4] != 'browsing':
    raise SystemExit(f'PageDown from oldest page re-entered tail/exited mode: {last}')
final = plain[-1400:]
if any(f'topdown-line-{i}' in final for i in range(60, 71)):
    raise SystemExit('PageDown from oldest page wrapped directly to the live tail')
if not any(f'topdown-line-{i}' in final for i in range(5, 16)):
    raise SystemExit('PageDown from oldest page did not advance to the next nearby page')
PY
}


test_log_view_follow_page_down_stops_on_last_real_page() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 12); do echo "loopend-line-$i"; done; sleep 0.3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 12); out.flush(); time.sleep(0.1); out.write(b"\x1b[6~" * 4); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-page-down-last-real.out" 2>"$TEST_ROOT/view-follow-page-down-last-real.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-page-down-last-real.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-page-down-last-real.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-page-down-last-real.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[4] != 'browsing':
    raise SystemExit(f'expected manual browsing to remain active, got {last}')
if last[1] == last[3]:
    raise SystemExit(f'PageDown advanced to an empty EOF anchor instead of stopping on the last real page: {last}')
final = plain[-1400:]
for wanted in ('loopend-line-9', 'loopend-line-10', 'loopend-line-11', 'loopend-line-12'):
    if wanted not in final:
        raise SystemExit(f'PageDown advanced past the last real log page; missing {wanted}')
if final.count('~') >= 4 and 'loopend-line-12' not in final:
    raise SystemExit('PageDown rendered an empty EOF page')
PYCHECK
}


test_log_view_follow_page_up_after_top_page_down_returns_to_top() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 60); do echo "navloop-line-$i"; done; sleep 0.3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 20); out.flush(); time.sleep(0.1); out.write(b"\x1b[6~"); out.flush(); time.sleep(0.1); out.write(b"\x1b[5~"); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-top-down-up.out" 2>"$TEST_ROOT/view-follow-top-down-up.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-top-down-up.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-top-down-up.out" <<'PY' || { cat "$TEST_ROOT/view-follow-top-down-up.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] != 'browsing':
    raise SystemExit(f'expected PageUp after PageDown from top to return to offset 0 browsing page, got {last}')
final = plain[-1200:]
for wanted in ('navloop-line-1', 'navloop-line-2', 'navloop-line-3', 'navloop-line-4'):
    if wanted not in final:
        raise SystemExit(f'final page did not return to top line {wanted}')
if 'navloop-line-8' in final and 'navloop-line-1' not in final:
    raise SystemExit('final page stayed on the newer page instead of returning to top')
PY
}



test_log_view_follow_filter_change_preserves_browsed_page() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo "target-top-one"; echo "target-top-two"; for i in $(seq 1 60); do echo "noise-middle-$i"; done; echo "target-tail-one"; echo "target-tail-two"; sleep 0.3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 25); out.flush(); time.sleep(0.1); out.write(b"target"); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-filter-preserve.out" 2>"$TEST_ROOT/view-follow-filter-preserve.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-filter-preserve.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-filter-preserve.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-filter-preserve.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
frames = plain.split('filter: target')
if len(frames) < 2:
    raise SystemExit('filter prompt never rendered')
final = frames[-1]
if 'target-top-one' not in final or 'target-top-two' not in final:
    raise SystemExit('typing a filter while browsed away did not preserve the top page')
if 'target-tail-one' in final or 'target-tail-two' in final:
    raise SystemExit('typing a filter while browsed away looped back to the bottom')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats or stats[-1][4] != 'browsing':
    raise SystemExit(f'expected browsed-away filter state, got {stats[-1] if stats else "missing stats"}')
PYCHECK
}


test_log_view_follow_exclude_preserves_browsed_page() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo "exclude-alpha-root"; for i in $(seq 1 40); do echo "keep-near-top-$i"; done; for i in $(seq 1 20); do echo "tail-after-exclude-$i"; done; sleep 0.3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[5~" * 25); out.flush(); time.sleep(0.1); out.write(b" "); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-exclude-preserve.out" 2>"$TEST_ROOT/view-follow-exclude-preserve.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-exclude-preserve.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-exclude-preserve.out" <<'PYCHECK' || { cat "$TEST_ROOT/view-follow-exclude-preserve.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
final = plain[-1600:]
if 'excluding similar' not in final:
    raise SystemExit('exclude mode was not activated')
if not any(f'keep-near-top-{i}' in final for i in range(1, 8)):
    raise SystemExit('excluding a line while browsed away did not keep nearby top lines visible')
if any(f'tail-after-exclude-{i}' in final for i in range(10, 21)):
    raise SystemExit('excluding a line while browsed away looped back to the bottom')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats or stats[-1][4] != 'browsing':
    raise SystemExit(f'expected browsed-away exclude state, got {stats[-1] if stats else "missing stats"}')
PYCHECK
}

test_log_view_follow_exclude_at_live_edge_pins_current_page() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo "drop heartbeat alpha"; echo "keep window one"; echo "keep window two"; echo "keep window three"; echo "keep window four"; echo "keep window five"; sleep 0.5; for i in $(seq 1 40); do sleep 0.03; echo "late bottom $i"; done; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.05
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b" "); out.flush(); time.sleep(1.2); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 8 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-exclude-live-pin.out" 2>"$TEST_ROOT/view-follow-exclude-live-pin.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-exclude-live-pin.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-exclude-live-pin.out" <<'PY' || { cat "$TEST_ROOT/view-follow-exclude-live-pin.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
final = plain[-1800:]
if 'excluding similar' not in final:
    raise SystemExit('space did not activate exclude mode')
if 'keep window one' not in final and 'keep window two' not in final and 'keep window three' not in final:
    raise SystemExit('excluding at the live edge did not keep the current page pinned')
if 'late bottom 35' in final or 'late bottom 40' in final:
    raise SystemExit('excluding at the live edge looped back to the newest tail')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats or stats[-1][4] != 'browsing':
    raise SystemExit(f'expected exclude-at-live to leave tail mode, got {stats[-1] if stats else "missing stats"}')
if 'newer=yes' not in final:
    raise SystemExit('pinned exclude view did not mark newer data below')
PY
}


test_log_view_follow_top_refills_as_live_log_grows() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 12); do echo "growtop-line-$i"; sleep 0.02; done; sleep 0.5' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; time.sleep(0.02); out.write(b"\x1b[5~" * 20); out.flush(); time.sleep(0.9); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-growtop.out" 2>"$TEST_ROOT/view-follow-growtop.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-growtop.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-growtop.out" <<'PY' || { cat "$TEST_ROOT/view-follow-growtop.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
if stats[-1][1] != '0' or stats[-1][2] not in ('browsing', 'exited'):
    raise SystemExit(f'expected growing top page to stay pinned at offset 0 after browsing, got {stats[-1]}')
final = plain[-1200:]
for wanted in ('growtop-line-1', 'growtop-line-2', 'growtop-line-3', 'growtop-line-4'):
    if wanted not in final:
        raise SystemExit(f'final top page did not refill downward with {wanted}')
if 'growtop-line-12' in final:
    raise SystemExit('final top page jumped back to the live tail')
PY
}

test_log_view_follow_cursor_navigation_disables_tail_yank() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 12); do echo "cursor-yank-line-$i"; done; for i in $(seq 13 40); do sleep 0.04; echo "cursor-yank-line-$i"; done; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.15
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x1b[B\x1b[B\x1b[A"); out.flush(); time.sleep(0.7); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 8 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-cursor-yank.out" 2>"$TEST_ROOT/view-follow-cursor-yank.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-cursor-yank.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-cursor-yank.out" <<'PY' || { cat "$TEST_ROOT/view-follow-cursor-yank.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
if stats[-1][4] != 'browsing':
    raise SystemExit(f'cursor navigation did not leave live-tail mode: {stats[-1]}')
final = plain[-1600:]
if 'cursor-yank-line-40' in final:
    raise SystemExit('cursor navigation was yanked back to the newest tail line')
if 'newer=yes' not in final:
    raise SystemExit('browsed-away viewer did not mark newer matching data')
PY
}


test_log_view_follow_cursor_browse_keeps_short_top_page_pinned() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in 1 2 3 4; do echo "pinshort-line-$i"; done; sleep 0.8; for i in $(seq 5 40); do sleep 0.03; echo "pinshort-line-$i"; done; sleep 3' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.15
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; time.sleep(0.25); out.write(b"\x1b[B"); out.flush(); time.sleep(1.8); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty -echo rows 8 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-pinshort.out" 2>"$TEST_ROOT/view-follow-pinshort.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-pinshort.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-pinshort.out" <<'PY' || { cat "$TEST_ROOT/view-follow-pinshort.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
last = stats[-1]
if last[1] != '0' or last[4] != 'browsing':
    raise SystemExit(f'cursor-browsed short page did not stay anchored at the oldest offset: {last}')
final = plain[-1600:]
for wanted in ('pinshort-line-1', 'pinshort-line-2', 'pinshort-line-3', 'pinshort-line-4'):
    if wanted not in final:
        raise SystemExit(f'final browsed page lost oldest visible line {wanted}')
for unwanted in ('pinshort-line-35', 'pinshort-line-40'):
    if unwanted in final:
        raise SystemExit(f'browsed page looped back to the live tail and rendered {unwanted}')
if 'newer=yes' not in final:
    raise SystemExit('browsed-away short page did not mark newer data')
PY
}


test_log_view_follow_page_up_stops_at_first_filtered_match() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 20); do echo "filtered-noise-$i"; done; for i in $(seq 1 40); do echo "filtered-target-line-$i"; done; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"filtered-target"); out.flush(); time.sleep(0.1); out.write(b"\x1b[5~" * 20); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-filtered-top.out" 2>"$TEST_ROOT/view-follow-filtered-top.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-filtered-top.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-filtered-top.out" <<'PY' || { cat "$TEST_ROOT/view-follow-filtered-top.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw)
frames = plain.split('filter: filtered-target')
if len(frames) < 2:
    raise SystemExit('filter prompt never rendered')
final = frames[-1]
if 'filtered-target-line-1' not in final:
    raise SystemExit('final top-filtered page did not keep the first matching line visible')
if re.search(r'\n~\r?\n~\r?\n~\r?\n~', final):
    raise SystemExit('final top-filtered page fell into an empty overscrolled page')
stats = re.findall(r'scan_gen=(\d+) offset=(\d+) prev=(\d+) next=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
if stats[-1][4] != 'browsing':
    raise SystemExit(f'expected final filtered top page to stay browsed away, got {stats[-1][4]}')
PY
}

test_log_view_printable_f_starts_dynamic_filter() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo "aaa unrelated"; echo "f-only-visible"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; sys.stdout.write("fq"); sys.stdout.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 100; $HOLD_BIN logs $id --interactive --debug-stats" /dev/null >"$TEST_ROOT/view-filter-f.out" 2>"$TEST_ROOT/view-filter-f.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-filter-f.err" >&2; return 1; }
  grep -q 'filter: f' "$TEST_ROOT/view-filter-f.out" || { cat "$TEST_ROOT/view-filter-f.out" >&2; return 1; }
  grep -q 'f-only-visible' "$TEST_ROOT/view-filter-f.out" || { cat "$TEST_ROOT/view-filter-f.out" >&2; return 1; }
}

test_log_view_follow_exited_page_up_keeps_backward_navigation() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in $(seq 1 40); do echo "exitpage-line-$i"; done; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  python3 -c 'import sys,time; time.sleep(0.8); out=sys.stdout.buffer; out.write(b"\x1b[5~" * 20); out.flush(); time.sleep(0.1); out.write(b"q"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 6 cols 120; $HOLD_BIN logs $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-exited-page-up.out" 2>"$TEST_ROOT/view-follow-exited-page-up.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-exited-page-up.err" >&2; return 1; }
  python3 - "$TEST_ROOT/view-follow-exited-page-up.out" <<'PY' || { cat "$TEST_ROOT/view-follow-exited-page-up.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw)
stats = re.findall(r'scan_gen=(\d+) offset=(\d+).*?follow=(tail|browsing|exited)', plain)
if not stats:
    raise SystemExit('missing debug stats')
if 'exitpage-line-1' not in plain:
    raise SystemExit('page-up after follow exit did not reach oldest rendered lines')
if re.search(r'follow=exited.*?\n~\r?\n~\r?\n~', plain, re.S):
    raise SystemExit('page-up after follow exit rendered an empty top page')
last_gen, last_offset, last_follow = stats[-1]
if last_follow != 'exited':
    raise SystemExit(f'expected exited state after run finished, got {last_follow}')
if int(last_offset) == 0 and 'exitpage-line-1' not in plain[-1000:]:
    raise SystemExit('final page fell to offset 0 without visible lines')
PY
}

test_log_view_follow_browsed_away_marks_newer_without_yank() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in 1 2 3 4 5 6; do echo "away-hit-old-$i"; done; sleep 0.8; echo "away-hit-new-tail"; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"away-hit"); out.flush(); time.sleep(0.1); out.write(b"\x1b[5~"); out.flush(); time.sleep(1.1); out.write(b"q"); out.flush()' |
    script -qfec "stty rows 7 cols 100; $HOLD_BIN __view $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-away.out" 2>"$TEST_ROOT/view-follow-away.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-away.err" >&2; return 1; }
  grep -q 'newer below' "$TEST_ROOT/view-follow-away.out" || { cat "$TEST_ROOT/view-follow-away.out" >&2; return 1; }
  grep -q 'away-hit-old' "$TEST_ROOT/view-follow-away.out" || { cat "$TEST_ROOT/view-follow-away.out" >&2; return 1; }
  ! grep -q 'away-hit-new-tail' "$TEST_ROOT/view-follow-away.out" || { cat "$TEST_ROOT/view-follow-away.out" >&2; return 1; }
}

test_log_view_follow_ignores_nonmatching_newer_data() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in 1 2 3 4 5 6; do echo "nomatch-hit-old-$i"; done; sleep 0.8; echo "unrelated-new-tail"; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"nomatch-hit"); out.flush(); time.sleep(0.1); out.write(b"\x1b[5~"); out.flush(); time.sleep(1.1); out.write(b"q"); out.flush()' |
    script -qfec "stty rows 7 cols 100; $HOLD_BIN __view $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-nonmatching-away.out" 2>"$TEST_ROOT/view-follow-nonmatching-away.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-nonmatching-away.err" >&2; return 1; }
  grep -q 'nomatch-hit-old' "$TEST_ROOT/view-follow-nonmatching-away.out" || { cat "$TEST_ROOT/view-follow-nonmatching-away.out" >&2; return 1; }
  ! grep -q 'newer below' "$TEST_ROOT/view-follow-nonmatching-away.out" || { cat "$TEST_ROOT/view-follow-nonmatching-away.out" >&2; return 1; }
  ! grep -q 'unrelated-new-tail' "$TEST_ROOT/view-follow-nonmatching-away.out" || { cat "$TEST_ROOT/view-follow-nonmatching-away.out" >&2; return 1; }
}

test_log_view_follow_finds_sparse_newer_match_after_large_burst() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'for i in 1 2 3 4 5 6; do echo "burst-hit-old-$i"; done; sleep 0.8; python3 -c '"'"'import sys; sys.stdout.write("unrelated\\n" * 40000); sys.stdout.write("burst-hit-new-tail\\n"); sys.stdout.flush()'"'"'; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"burst-hit"); out.flush(); time.sleep(0.1); out.write(b"\x1b[5~"); out.flush(); time.sleep(2.0); out.write(b"q"); out.flush()' |
    script -qfec "stty rows 7 cols 100; $HOLD_BIN __view $id --interactive --follow --debug-stats" /dev/null >"$TEST_ROOT/view-follow-large-burst.out" 2>"$TEST_ROOT/view-follow-large-burst.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/view-follow-large-burst.err" >&2; return 1; }
  grep -q 'newer below' "$TEST_ROOT/view-follow-large-burst.out" || { cat "$TEST_ROOT/view-follow-large-burst.out" >&2; return 1; }
  grep -q 'burst-hit-old' "$TEST_ROOT/view-follow-large-burst.out" || { cat "$TEST_ROOT/view-follow-large-burst.out" >&2; return 1; }
  ! grep -q 'burst-hit-new-tail' "$TEST_ROOT/view-follow-large-burst.out" || { cat "$TEST_ROOT/view-follow-large-burst.out" >&2; return 1; }
}

test_start_follow_short_form() {
  local out
  # -f keeps the foreground call streaming; foreground prints no id of its own.
  out=$("$HOLD_BIN" -f bash -c 'echo follow-short' 2>&1) || return 1
  printf '%s\n' "$out" | grep -q 'follow-short'
}



test_tail_verb_existing_id() {
  local out id tailed
  out=$("$HOLD_BIN" -d bash -c 'sleep 0.3; echo from_tail_id; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  tailed=$("$HOLD_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'from_tail_id'
}

test_persistent_stale_records() {
  local out id store bootfile oldboot list_out stale_id stale_log
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" -d bash -c 'echo stale-line; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  store="$HOME/.local/state/hold"
  stale_log=$(log_path "$id" "$store") || return 1
  [ -f "$stale_log" ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list >/dev/null || return 1
  printf 'boot-b\n' >"$bootfile" || return 1
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Stale"
  set +e
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null 2>"$TEST_ROOT/stop.err"
  [ "$?" -eq 2 ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" kill "$id" >/dev/null 2>"$TEST_ROOT/kill.err"
  [ "$?" -eq 2 ] || return 1
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/print.err"
  [ "$?" -eq 2 ] || return 1
  set -e
  grep -q 'stale' "$TEST_ROOT/stop.err"
  grep -q 'stale' "$TEST_ROOT/kill.err"
  grep -q 'stale' "$TEST_ROOT/print.err"
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/tail.out"
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" logs "$id" -p >"$TEST_ROOT/dump.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/dump.out"
  record_exists "$id" "$store" && log_exists "$id" "$store"
}


test_boot_unavailable_does_not_force_stale() {
  local out id bootfile list_out rc pgid got
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" -d sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  rm -f "$bootfile"
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up " || return 1
  ! printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Stale" || return 1
  set +e
  got=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" 2>"$TEST_ROOT/missing-boot-print.err")
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "stop --print with missing boot source: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/missing-boot-print.err" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { printf 'got: %s\nwant: kill -TERM -- -%s\n' "$got" "$pgid" >&2; return 1; }
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null
}

test_leader_zombie_group_still_running() {
  local id list_out
  id=$("$HOLD_BIN" -d bash -c 'sleep 60 & exit 0' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  sleep 0.3
  list_out=$("$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up " || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
}

test_tail_finished_log_prints_existing_output() {
  local out id tailed
  out=$("$HOLD_BIN" -d bash -c 'echo finished-tail-line' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  tailed=$("$HOLD_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'finished-tail-line'
}

test_console_does_not_require_external_attach_tool() {
  local fake_path out id store record sock
  fake_path="$TEST_ROOT/no-tools"
  mkdir -p "$fake_path" || return 1
  out=$(as_user /usr/bin/env PATH="$fake_path" "$HOLD_REAL_BIN" --console /bin/sh -c 'read line; echo "native:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  printf 'ping\n' | as_user /usr/bin/env PATH="$fake_path" "$HOLD_REAL_BIN" console "$id" >"$TEST_ROOT/console-native.out" 2>"$TEST_ROOT/console-native.err" || {
    cat "$TEST_ROOT/console-native.out" "$TEST_ROOT/console-native.err" >&2
    return 1
  }
  grep -q 'native:ping' "$TEST_ROOT/console-native.out" || { cat "$TEST_ROOT/console-native.out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_reports_non_console_run() {
  local out id rc
  out=$("$HOLD_BIN" -d sleep 30 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-none.out" 2>"$TEST_ROOT/console-none.err"
  rc=$?
  [ "$rc" -eq 0 ] || return 1
  grep -q "has no console" "$TEST_ROOT/console-none.err" || { cat "$TEST_ROOT/console-none.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_console_round_trip_and_log_tee() {
  local out id store record sock
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "got:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" list >"$TEST_ROOT/console-list.out" || return 1
  grep -Eq "^$id[[:space:]].*Up " "$TEST_ROOT/console-list.out" || {
    cat "$TEST_ROOT/console-list.out" >&2
    return 1
  }
  printf 'ping\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console.out" 2>"$TEST_ROOT/console.err" || {
    cat "$TEST_ROOT/console.out" "$TEST_ROOT/console.err" >&2
    return 1
  }
  grep -q 'got:ping' "$TEST_ROOT/console.out" || { cat "$TEST_ROOT/console.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'got:ping' "$logf" || { cat "$logf" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_exit_code_is_recorded() {
  local out id store record rc
  out=$("$HOLD_BIN" --console /bin/sh -c 'read line; echo "console-exit:$line"; exit 7' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1

  set +e
  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-exit.out" 2>"$TEST_ROOT/console-exit.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || [ "$rc" -eq 7 ] || {
    echo "console attach rc=$rc (want 0 or target exit 7)" >&2
    cat "$TEST_ROOT/console-exit.out" "$TEST_ROOT/console-exit.err" >&2
    return 1
  }
  grep -q 'console-exit:done' "$TEST_ROOT/console-exit.out" || { cat "$TEST_ROOT/console-exit.out" >&2; return 1; }

  for _ in $(seq 1 50); do
    grep -q '"exit_code": 7' "$record" && break
    sleep 0.1
  done
  grep -q '"state": "exited"' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"exit_code": 7' "$record" || { cat "$record" >&2; return 1; }
  grep -q '"ExitCode": 7' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \(7\)" || return 1
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
}

test_end_console_call_signals_target() {
  # 'hold end' on a console call must end the HELD group, not the broker. The
  # target ignores SIGHUP so an incidental PTY hangup cannot masquerade as the
  # group signal: under the pre-anchoring bug the record named the broker, the
  # group survived, and the record fabricated Exited (0). No red test caught this
  # because the best-effort 'stop' cleanups elsewhere (2015-2037, 2068) silently
  # leaked the target; this test is the pin.
  local out id store record pgid sock got ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" HUP; sleep 45' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  # Target-anchoring: pid==pgid==sid names the held sh/sleep, not a hold broker.
  # Under the bug the recorded group is the broker's and holds no sleep.
  ps -o args= -g "$pgid" >"$TEST_ROOT/end-signal-group.txt" 2>/dev/null || true
  grep -q 'sleep 45' "$TEST_ROOT/end-signal-group.txt" || {
    echo "record pgid $pgid is not the held group:" >&2
    cat "$TEST_ROOT/end-signal-group.txt" >&2
    return 1
  }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  # The honest --print surface names the real group.
  got=$("$HOLD_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ] || {
    echo "stop --print: [$got] want [kill -TERM -- -$pgid]" >&2
    return 1
  }
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived end" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  # Poll the record; never grep list before this (the zombie window renders
  # Exited (0) transiently until the broker's mark lands with 143).
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 143' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 143:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 15' "$record" || { cat "$record" >&2; return 1; }
  path_absent_soon "$sock" || { ls -la "$(dirname "$sock")" >&2 || true; return 1; }
  "$HOLD_BIN" ps -a | grep -Eq "^$id[[:space:]].*Exited \(143\)" || { "$HOLD_BIN" ps -a >&2; return 1; }
}

test_end_console_call_escalates_to_kill() {
  # SPEC 'end: TERM, then KILL' holds for consoles too. The leader traps both TERM
  # and HUP and keeps a child alive across TERM, so only the KILL escalation can
  # bring the group down; the record must reach exit_code 137 (128+SIGKILL).
  local out id store record pgid ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" TERM HUP; while :; do sleep 30; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  "$HOLD_BIN" end "$id" >/dev/null || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived end escalation" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 137' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 137:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 9' "$record" || { cat "$record" >&2; return 1; }
}

test_console_broker_term_forwards_to_target() {
  # A raw SIGTERM to the broker pid itself (not routed through 'hold end') must
  # forward to the held group and clean up the socket. The record no longer names
  # the broker, so without the forwarder a bare kill would orphan the target.
  local out id store record pgid sock broker ok _
  out=$("$HOLD_BIN" --console /bin/sh -c 'trap "" HUP; sleep 45' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  pgid=$(record_pgid "$id" "$store") || return 1
  [ -n "$pgid" ] || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  # The broker is the parent of the target group leader (it forked the target).
  broker=$(ps -o ppid= -p "$pgid" 2>/dev/null | tr -d ' ')
  [ -n "$broker" ] && [ "$broker" -gt 1 ] || { echo "could not find broker for pgid $pgid" >&2; return 1; }
  kill -TERM "$broker" || return 1
  pgid_terminated "$pgid" || { echo "held group $pgid survived broker TERM" >&2; return 1; }
  record_ended_soon "$id" "$store" || { cat "$record" >&2; return 1; }
  ok=0
  for _ in $(seq 1 50); do
    grep -q '"exit_code": 143' "$record" && { ok=1; break; }
    sleep 0.1
  done
  [ "$ok" -eq 1 ] || { echo "record did not reach exit_code 143 after broker TERM:" >&2; cat "$record" >&2; return 1; }
  grep -q '"term_signal": 15' "$record" || { cat "$record" >&2; return 1; }
  path_absent_soon "$sock" || { ls -la "$(dirname "$sock")" >&2 || true; return 1; }
}

test_console_socket_lives_in_store_dir() {
  # The console socket must live inside the store's console directory, never in
  # /tmp, even though the harness's store path is longer than the AF_UNIX
  # sun_path limit (~104). This exercises the relative-bind path.
  local out id store record sock
  out=$("$HOLD_BIN" --console /bin/sh -c 'while read line; do [ "$line" = noop ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  case "$sock" in
    "$store/console/$id"*.sock) ;;
    *)
    echo "console socket not in store console dir (got: $sock)" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
      ;;
  esac
  if [ ! -S "$sock" ]; then
    echo "no socket present at $sock" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The socket must be owner-only (0600): it is an exec channel into the run.
  sock_mode=$(file_mode "$sock") || { "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  if [ "$sock_mode" != 600 ]; then
    echo "console socket mode=$sock_mode (want 600)" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The relative path must also be connectable when the store path is long.
  printf 'noop\n' | "$HOLD_BIN" console "$id" >/dev/null 2>"$TEST_ROOT/console-store.err" || {
    cat "$TEST_ROOT/console-store.err" >&2
    "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  return 0
}

test_console_target_runs_in_caller_cwd() {
  # Relative-bind temporarily chdir()s into the console dir to bind the socket;
  # it must restore the cwd so the launched process still runs in the caller's
  # working directory, not the console directory.
  local out id store logf workdir i ok
  workdir="$TEST_ROOT/console-cwd"
  mkdir -p "$workdir" || return 1
  : > "$workdir/sentinel-file" || return 1
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    chown -R "$TEST_UID:$TEST_GID" "$workdir" || return 1
  fi

  out=$( cd "$workdir" && "$HOLD_BIN" --console /bin/sh -c \
    'if [ -e sentinel-file ]; then echo CWD_OK; else echo CWD_BAD; fi; sleep 30' 2>&1 ) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  logf=$(log_path "$id" "$store") || return 1
  ok=0
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if grep -q CWD_OK "$logf" 2>/dev/null; then ok=1; break; fi
    if grep -q CWD_BAD "$logf" 2>/dev/null; then ok=0; break; fi
    sleep 0.2
  done
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  if [ "$ok" -ne 1 ]; then
    echo "target did not run in caller cwd; log:" >&2
    cat "$logf" >&2 2>/dev/null || true
    return 1
  fi
  return 0
}

build_console_protocol_helper() {
  local helper="$TEST_ROOT/console_protocol_helper"
  [ -x "$helper" ] && { printf '%s\n' "$helper"; return 0; }
  cat >"$TEST_ROOT/console_protocol_helper.c" <<'EOF'
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *)buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 0;
}

static int write_frame(int fd, unsigned char type, const char *payload) {
  size_t len = payload ? strlen(payload) : 0;
  unsigned char header[3];
  if (len > UINT16_MAX) return -1;
  header[0] = type;
  header[1] = (unsigned char)((len >> 8) & 0xff);
  header[2] = (unsigned char)(len & 0xff);
  if (write_all(fd, header, sizeof(header)) != 0) return -1;
  return len == 0 ? 0 : write_all(fd, payload, len);
}

int main(int argc, char **argv) {
  if (argc != 4) return 64;
  const char *sock_path = argv[1];
  const char *input = argv[2];
  const char *expect = argv[3];

  /* Connect via a short name relative to the socket's directory, mirroring
   * hold's own client, so a long store path stays within sun_path. */
  const char *slash = strrchr(sock_path, '/');
  if (!slash || !slash[1]) return 3;
  char dir[4096];
  size_t dlen = (size_t)(slash - sock_path);
  if (dlen >= sizeof(dir)) return 3;
  memcpy(dir, sock_path, dlen);
  dir[dlen] = '\0';
  const char *name = slash + 1;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 2;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(name) >= sizeof(addr.sun_path)) return 3;
  strcpy(addr.sun_path, name);
  if (chdir(dir) != 0) return 4;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 4;
  if (write_all(fd, "holdv1\0\0", 8) != 0) return 5;
  if (write_frame(fd, 'D', input) != 0) return 6;

  char seen[8192];
  size_t seen_len = 0;
  memset(seen, 0, sizeof(seen));
  for (;;) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sr < 0 && errno == EINTR) continue;
    if (sr <= 0) return 7;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 8;
    if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) return 9;
    size_t copy = (size_t)n;
    if (copy > sizeof(seen) - seen_len - 1) copy = sizeof(seen) - seen_len - 1;
    memcpy(seen + seen_len, buf, copy);
    seen_len += copy;
    seen[seen_len] = '\0';
    if (strstr(seen, expect)) {
      if (strcmp(expect, "attach denied") == 0) {
        close(fd);
        return 0;
      }
      break;
    }
  }

  if (write_frame(fd, 'X', NULL) != 0) return 10;
  close(fd);
  return 0;
}
EOF
  "${CC:-cc}" -std=c99 -Wall -Wextra -Werror "$TEST_ROOT/console_protocol_helper.c" -o "$helper"
  printf '%s\n' "$helper"
}

test_console_rejects_unrelated_peer_uid_before_replay() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  [ -n "$SUDO_BIN" ] || skip "sudo unavailable"
  id nobody >/dev/null 2>&1 || skip "no nobody user"
  local attacker_uid
  attacker_uid=$(id -u nobody)
  [ "$attacker_uid" != "0" ] && [ "$attacker_uid" != "$TEST_UID" ] || skip "nobody is not an unrelated user"
  "$SUDO_BIN" -n -u nobody true >/dev/null 2>&1 || skip "cannot run as nobody"

  local out id record sock helper rc log
  out=$("$HOLD_BIN" --console /bin/sh -c 'echo replay-secret; while read line; do echo "got:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -S "$sock" ] || return 1
  log=$(log_path "$id") || return 1

  # Deliberately relax filesystem traversal and socket mode in this temp store
  # so the kernel accepts the unrelated connection attempt; the broker's peer
  # credential check must still reject it before replaying output or forwarding
  # input.
  chmod 0711 "$ACTOR_HOME" "$ACTOR_HOME/.local" "$ACTOR_HOME/.local/state" \
    "$HOME/.local/state/hold" "$HOME/.local/state/hold/console" || return 1
  chmod 0666 "$sock" || return 1

  helper=$(build_console_protocol_helper) || return 1
  mkdir -p "$TEST_ROOT/nobody-home" || return 1
  chmod 755 "$TEST_ROOT/nobody-home" || return 1
  set +e
  "$SUDO_BIN" -n -u nobody env HOME="$TEST_ROOT/nobody-home" "$helper" "$sock" 'steal
' 'attach denied' >"$TEST_ROOT/unauth-console.out" 2>"$TEST_ROOT/unauth-console.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/unauth-console.out" "$TEST_ROOT/unauth-console.err" >&2; return 1; }
  grep -q 'attach denied' "$TEST_ROOT/unauth-console.out" || { cat "$TEST_ROOT/unauth-console.out" >&2; return 1; }
  ! grep -q 'replay-secret' "$TEST_ROOT/unauth-console.out" || { echo "unauthorized attach received replay" >&2; return 1; }
  sleep 0.2
  ! grep -q 'got:steal' "$log" || { cat "$log" >&2; return 1; }

  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/authorized-console.out" 2>"$TEST_ROOT/authorized-console.err" || {
    cat "$TEST_ROOT/authorized-console.out" "$TEST_ROOT/authorized-console.err" >&2
    return 1
  }
  grep -q 'got:done' "$TEST_ROOT/authorized-console.out" || { cat "$TEST_ROOT/authorized-console.out" >&2; return 1; }
}

test_console_can_reattach_after_detach() {
  local out id store record sock helper
  out=$("$HOLD_BIN" --console /bin/sh -c 'while read line; do echo "seen:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/hold"
  record=$(record_path "$id" "$store") || return 1
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }

  helper=$(build_console_protocol_helper) || return 1
  as_user "$helper" "$sock" 'first
' 'seen:first' >"$TEST_ROOT/console-first.out" 2>"$TEST_ROOT/console-first.err" || {
    cat "$TEST_ROOT/console-first.out" "$TEST_ROOT/console-first.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-first.out" || { cat "$TEST_ROOT/console-first.out" >&2; return 1; }

  printf 'done\n' | "$HOLD_BIN" console "$id" >"$TEST_ROOT/console-second.out" 2>"$TEST_ROOT/console-second.err" || {
    cat "$TEST_ROOT/console-second.out" "$TEST_ROOT/console-second.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  grep -q 'seen:done' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  sleep 0.2
  logf=$(log_path "$id" "$store") || return 1
  grep -q 'seen:first' "$logf" || { cat "$logf" >&2; return 1; }
  grep -q 'seen:done' "$logf" || { cat "$logf" >&2; return 1; }
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_save_marks_record_and_is_idempotent() {
  local id name out
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  [ -n "$name" ] || { echo "run has no generated name" >&2; return 1; }
  # First save: stdout empty, stderr confirms, record flagged.
  out=$("$HOLD_BIN" save "$id" 2>"$TEST_ROOT/save1.err") || { echo "save rc nonzero" >&2; cat "$TEST_ROOT/save1.err" >&2; return 1; }
  [ -z "$out" ] || { echo "save wrote to stdout: [$out]" >&2; return 1; }
  grep -qF "hold: saved $id ($name)" "$TEST_ROOT/save1.err" || { cat "$TEST_ROOT/save1.err" >&2; return 1; }
  grep -q '"Saved": true' "$(record_path "$id")" || { echo "record not marked saved" >&2; return 1; }
  # Second save is idempotent: still exit 0, same note, no stdout.
  out=$("$HOLD_BIN" save "$name" 2>"$TEST_ROOT/save2.err") || { echo "idempotent save rc nonzero" >&2; return 1; }
  [ -z "$out" ] || { echo "idempotent save wrote to stdout: [$out]" >&2; return 1; }
  grep -qF "hold: saved $id ($name)" "$TEST_ROOT/save2.err" || { cat "$TEST_ROOT/save2.err" >&2; return 1; }
}

test_purge_sweep_skips_saved() {
  local saved_id plain_id
  saved_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  plain_id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$saved_id" ] && [ -n "$plain_id" ] || return 1
  "$HOLD_BIN" save "$saved_id" >/dev/null 2>&1 || return 1
  record_ended_soon "$saved_id" || return 1
  record_ended_soon "$plain_id" || return 1
  "$HOLD_BIN" purge >/dev/null 2>&1 || return 1
  record_exists "$saved_id" || { echo "sweeping purge removed a saved call" >&2; return 1; }
  ! record_exists "$plain_id" || { echo "sweeping purge left an unsaved ended call" >&2; return 1; }
  # -a (include stale) must also skip the saved call.
  "$HOLD_BIN" purge -a >/dev/null 2>&1 || return 1
  record_exists "$saved_id" || { echo "purge -a removed a saved call" >&2; return 1; }
}

test_purge_targeted_refuses_saved_exact_wording() {
  local id name pfx rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  # Refusal by name: line 1 identifies by name, line 2 echoes the typed token.
  set +e; "$HOLD_BIN" purge "$name" >"$TEST_ROOT/refuse.out" 2>"$TEST_ROOT/refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "targeted refusal rc=$rc (want 2)" >&2; cat "$TEST_ROOT/refuse.err" >&2; return 1; }
  [ ! -s "$TEST_ROOT/refuse.out" ] || { echo "refusal wrote to stdout" >&2; return 1; }
  printf "hold: '%s' is saved — purging a saved call requires --force\n  hold purge %s --force\n" "$name" "$name" >"$TEST_ROOT/refuse.want"
  diff -u "$TEST_ROOT/refuse.want" "$TEST_ROOT/refuse.err" || { echo "refusal wording mismatch (by name)" >&2; return 1; }
  # Refusal by id prefix: line 2 echoes exactly the prefix the user typed.
  pfx=$(printf '%s' "$id" | cut -c1-6)
  set +e; "$HOLD_BIN" purge "$pfx" 2>"$TEST_ROOT/refuse2.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "prefix refusal rc=$rc (want 2)" >&2; return 1; }
  printf "hold: '%s' is saved — purging a saved call requires --force\n  hold purge %s --force\n" "$name" "$pfx" >"$TEST_ROOT/refuse2.want"
  diff -u "$TEST_ROOT/refuse2.want" "$TEST_ROOT/refuse2.err" || { echo "refusal wording mismatch (by prefix)" >&2; return 1; }
  record_exists "$id" || { echo "refused purge still removed the call" >&2; return 1; }
}

test_purge_force_removes_saved_ended() {
  local id name
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  # The copy-paste command from the refusal must succeed on an ended saved call.
  "$HOLD_BIN" purge "$name" --force >/dev/null 2>&1 || { echo "force purge of saved ended call failed" >&2; return 1; }
  ! record_exists "$id" || { echo "force did not remove the saved call" >&2; return 1; }
}

test_purge_force_removes_live_saved() {
  local id name pgid
  id=$("$HOLD_BIN" -d sleep 300 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  name=$(record_name "$id") || return 1
  pgid=$(record_pgid "$id") || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  # --force must end the live call and remove it despite the saved flag.
  # Retry briefly: under load the TERM->KILL escalation can outlive one
  # purge invocation by a scheduler beat; a real regression still fails
  # every attempt for the full window.
  local tries=0
  while :; do
    "$HOLD_BIN" purge "$name" --force >/dev/null 2>&1
    if ! record_exists "$id" && pgid_terminated "$pgid"; then
      return 0
    fi
    tries=$((tries + 1))
    [ "$tries" -lt 20 ] || break
    sleep 0.1
  done
  ! record_exists "$id" || { echo "force did not remove the live saved call" >&2; return 1; }
  pgid_terminated "$pgid" || { echo "force purge left the process group alive" >&2; return 1; }
  return 1
}

test_rename_of_saved_call_keeps_protection() {
  local id newname rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  "$HOLD_BIN" save "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || return 1
  "$HOLD_BIN" rename "$id" keptsafe >/dev/null 2>&1 || { echo "rename failed" >&2; return 1; }
  grep -q '"Saved": true' "$(record_path "$id")" || { echo "rename cleared the saved flag" >&2; return 1; }
  set +e; "$HOLD_BIN" purge keptsafe 2>"$TEST_ROOT/rename-refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "renamed saved call not protected: rc=$rc" >&2; return 1; }
  grep -qF "hold: 'keptsafe' is saved" "$TEST_ROOT/rename-refuse.err" || { cat "$TEST_ROOT/rename-refuse.err" >&2; return 1; }
  "$HOLD_BIN" purge keptsafe --force >/dev/null 2>&1 || true
}

test_rename_saves_an_unsaved_call() {
  local id rc
  id=$("$HOLD_BIN" -d true 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Naming a call declares intent to keep it: rename implies save.
  "$HOLD_BIN" rename "$id" nowkept 2>"$TEST_ROOT/rename-save.err" || { echo "rename failed" >&2; return 1; }
  grep -qF "(saved)" "$TEST_ROOT/rename-save.err" || { cat "$TEST_ROOT/rename-save.err" >&2; return 1; }
  grep -q '"Saved": true' "$(record_path "$id")" || { echo "rename did not save the call" >&2; return 1; }
  set +e; "$HOLD_BIN" purge nowkept 2>"$TEST_ROOT/rename-save-refuse.err"; rc=$?; set -e
  [ "$rc" -eq 2 ] || { echo "renamed call not protected: rc=$rc" >&2; return 1; }
  "$HOLD_BIN" purge nowkept --force >/dev/null 2>&1 || true
}

test_redial_honors_recorded_foreground_mode() {
  local out
  "$HOLD_BIN" --name fgredial -- /bin/sh -c 'echo fg-redial-hit' >/dev/null 2>&1 || return 1
  # Foreground recipe: redial streams the output and prints no id of its own.
  out=$("$HOLD_BIN" fgredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -q 'fg-redial-hit' || { echo "foreground redial did not stream output: [$out]" >&2; return 1; }
  ! printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "foreground redial printed a detached id" >&2; return 1; }
  # An explicit -d on the redial overrides the recipe and detaches.
  out=$("$HOLD_BIN" -d fgredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "-d override did not detach: [$out]" >&2; return 1; }
}

test_redial_honors_recorded_detached_mode() {
  local id out
  id=$("$HOLD_BIN" -d --name dredial /bin/sh -c 'echo d-redial; sleep 0.1' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Detached recipe: bare redial detaches and prints the 64-hex id.
  out=$("$HOLD_BIN" dredial 2>/dev/null) || return 1
  printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "detached redial did not print a bare id: [$out]" >&2; return 1; }
}

test_redial_honors_recorded_console_mode() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  local sm id out
  script -qec "$HOLD_BIN -it --name itredial /bin/sh -c 'echo it-live-hit; sleep 0.4'" /dev/null >/dev/null 2>&1 || true
  sm=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"SessionMode": "\([a-z]*\)".*/\1/p' | head -n1)
  [ "$sm" = "console" ] || { echo "itredial SessionMode=$sm (want console)" >&2; return 1; }
  id=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"id": "\([0-9a-f]*\)".*/\1/p' | head -n1)
  [ -n "$id" ] || return 1
  record_ended_soon "$id" || return 1
  # Console recipe: bare redial replays on a PTY (attaches) rather than detaching.
  out=$(script -qec "$HOLD_BIN itredial" /dev/null 2>&1) || true
  printf '%s\n' "$out" | grep -q 'it-live-hit' || { printf '%s\n' "$out" >&2; echo "console redial did not replay on a PTY" >&2; return 1; }
  ! printf '%s\n' "$out" | grep -qE '^[0-9a-f]{64}$' || { echo "console redial detached instead of attaching" >&2; return 1; }
  record_ended_soon "$id" || return 1
  sm=$("$HOLD_BIN" inspect itredial 2>/dev/null | sed -n 's/.*"SessionMode": "\([a-z]*\)".*/\1/p' | head -n1)
  [ "$sm" = "console" ] || { echo "console redial rewrote SessionMode=$sm (want console)" >&2; return 1; }
}

test_prune_by_id() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d true 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/hold"
  "$HOLD_BIN" prune "$id1" >/dev/null || return 1
  ! record_exists "$id1" "$store" && ! log_exists "$id1" "$store" || return 1
  record_exists "$id2" "$store" && log_exists "$id2" "$store"
}

test_prune_all_keeps_running() {
  local out1 out2 id1 id2 store
  out1=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  out2=$("$HOLD_BIN" -d true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/hold"
  "$HOLD_BIN" prune all >/dev/null || return 1
  record_exists "$id1" "$store" || return 1
  ! record_exists "$id2" "$store" || return 1
  ! log_exists "$id2" "$store" || return 1
}

test_transactional_record_write_failure() {
  local rc pids
  set +e
  HOLD_TEST_FAIL_RECORD_WRITE=1 "$HOLD_BIN" -d bash -c 'exec -a hold_txn_test_sleep sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  # Detect a leaked process via ps (a required tool) rather than pgrep with
  # `|| true`, which would pass vacuously if pgrep were absent.
  pids=$(ps -eo pid=,args= | awk '/hold_txn_test_sleep/ && !/awk/ {print $1}')
  [ -z "$pids" ]
}


make_fake_sudo() {
  mkdir -p "$TEST_ROOT/fakebin" || return 1
  cat > "$TEST_ROOT/fakebin/sudo" <<'SH'
#!/usr/bin/env bash
: "${HOLD_FAKE_SUDO_ARGV:?}"
printf '%s\n' "$@" > "$HOLD_FAKE_SUDO_ARGV"
exit "${HOLD_FAKE_SUDO_RC:-77}"
SH
  chmod 755 "$TEST_ROOT/fakebin" "$TEST_ROOT/fakebin/sudo" || return 1
  export HOLD_FAKE_SUDO_ARGV="$TEST_ROOT/sudo.argv"
  : > "$HOLD_FAKE_SUDO_ARGV" || return 1
  chmod 0666 "$HOLD_FAKE_SUDO_ARGV" || return 1
  export HOLD_FAKE_SUDO_RC=77
  export PATH="$TEST_ROOT/fakebin:$PATH"
}

write_public_index_fixture() {
  local id="$1" state="${2:-running}" started="${3:-2026-06-15T18:42:11Z}" alias="${4:-}"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  local alias_json=""
  if [ -n "$alias" ]; then
    alias_json="\"alias\":\"$alias\","
  fi
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<JSON
{"id":"$id","root_managed":true,"requires_elevation":true,${alias_json}"state_hint":"$state","started_at":"$started","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
}













test_root_capability_drop_all_then_add_preserves_added_cap() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  [ -r /proc/self/status ] || skip "/proc status unavailable"
  local out cap_hex
  out=$(as_root "$HOLD_REAL_BIN" --rm --cap-drop ALL --cap-add NET_BIND_SERVICE -- /bin/sh -c 'grep "^CapEff:" /proc/self/status' 2>&1) || {
    printf '%s
' "$out" >&2
    return 1
  }
  cap_hex=$(printf '%s
' "$out" | awk '/^CapEff:/ { print $2; exit }')
  [ -n "$cap_hex" ] || { printf '%s
' "$out" >&2; return 1; }
  python3 - "$cap_hex" <<'PY'
import sys
cap_eff = int(sys.argv[1], 16)
net_bind_service = 1 << 10
if cap_eff != net_bind_service:
    raise SystemExit(f"expected only CAP_NET_BIND_SERVICE after cap-drop ALL + cap-add NET_BIND_SERVICE, got 0x{cap_eff:x}")
PY
}

test_direct_capability_metadata_projects_to_inspect() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id
  out=$(as_root "$HOLD_REAL_BIN" -d --cap-add NET_BIND_SERVICE --cap-drop ALL -- /bin/sleep 60 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" inspect "$id" >"$TEST_ROOT/direct-cap-inspect.json" || return 1
  python3 - "$TEST_ROOT/direct-cap-inspect.json" <<'PY' || { cat "$TEST_ROOT/direct-cap-inspect.json" >&2; return 1; }
import json, sys
r = json.load(open(sys.argv[1], encoding="utf-8"))[0]
cfg = r.get("Config") or {}
if cfg.get("CapAdd") != ["NET_BIND_SERVICE"]:
    raise SystemExit(f"CapAdd mismatch: {cfg!r}")
if cfg.get("CapDrop") != ["ALL"]:
    raise SystemExit(f"CapDrop mismatch: {cfg!r}")
PY
  as_root "$HOLD_REAL_BIN" stop "$id" >/dev/null || return 1
}

test_restart_worker_applies_capability_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  [ -r /proc/self/status ] || skip "/proc status unavailable"
  local script out id count logs
  script="$TEST_ROOT/restart-cap.sh"
  cat >"$script" <<'EOS' || return 1
#!/bin/sh
grep '^CapEff:' /proc/self/status
exit 7
EOS
  chmod 755 "$script" || return 1
  out=$(as_root "$HOLD_REAL_BIN" -d --restart on-failure:1 --restart-delay 0 \
      --cap-drop ALL --cap-add NET_BIND_SERVICE -- \
      "$script" 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  for _ in $(seq 1 40); do
    logs=$(as_root "$HOLD_REAL_BIN" logs "$id" --plain -n 50 2>/dev/null || true)
    count=$(printf '%s\n' "$logs" | grep -c '^CapEff:' || true)
    [ "$count" -ge 2 ] && break
    sleep 0.1
  done
  [ "$count" -ge 2 ] || { echo "restart capability attempts: $count" >&2; as_root "$HOLD_REAL_BIN" logs "$id" --plain -n 50 >&2 || true; return 1; }
  logs=$(as_root "$HOLD_REAL_BIN" logs "$id" --plain -n 50) || return 1
  printf '%s\n' "$logs" >"$TEST_ROOT/restart-cap.log" || return 1
  python3 - "$TEST_ROOT/restart-cap.log" <<'PY' || { printf '%s\n' "$logs" >&2; return 1; }
import sys
lines = [line.split()[1] for line in open(sys.argv[1], encoding="utf-8") if line.startswith("CapEff:")]
if len(lines) < 2:
    raise SystemExit(f"expected at least 2 CapEff lines, got {lines!r}")
want = 1 << 10
for line in lines:
    got = int(line, 16)
    if got != want:
        raise SystemExit(f"restart worker capabilities not constrained: got 0x{got:x}, want 0x{want:x}")
PY
  as_root "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true
}




test_docker_rm_removes_run_artifacts_after_exit() {
  local out id store json log
  store="$HOME/.local/state/hold"
  out=$("$HOLD_BIN" -d --rm -- /bin/sh -c 'echo rm-gone; sleep 0.3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  log=$(log_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$json" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$log" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  sleep 0.1
  [ -f "$json" ] || { echo "--rm removed record before process exit" >&2; return 1; }
  path_absent_soon "$json" || { echo "--rm did not remove record $json" >&2; cat "$json" >&2 2>/dev/null || true; return 1; }
  path_absent_soon "$log" || { echo "--rm did not remove log $log" >&2; return 1; }
}

test_env_flag_is_recipe_data_only() {
  local out id store fake record log
  store="$HOME/.local/state/hold"
  fake="$TEST_ROOT/fake-home"
  mkdir -p "$fake" || return 1
  out=$("$HOLD_BIN" -d -e HOME="$fake" -e HOLD_MARKER=recipe -- /bin/sh -c 'echo "home:$HOME marker:$HOLD_MARKER"' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id" "$store") || { echo "-e HOME relocated the record store" >&2; return 1; }
  [ -f "$record" ] || { echo "-e HOME relocated the record store" >&2; return 1; }
  [ ! -d "$fake/.local/state/hold" ] || { echo "-e HOME leaked into hold's own environment" >&2; return 1; }
  record_ended_soon "$id" || return 1
  log=$(log_path "$id" "$store") || return 1
  grep -q "home:$fake marker:recipe" "$log" || { cat "$log" >&2; return 1; }
  out=$("$HOLD_BIN" -d -e PATH=/nonexistent -- sleep 0.1 2>&1) || { echo "-e PATH broke invoking-PATH command resolution" >&2; printf '%s\n' "$out" >&2; return 1; }
}





















test_raw_start_does_not_steal_trailing_system() {
  local out id log
  out=$("$HOLD_BIN" -d sh -c 'printf "arg:%s\n" "$1"' sh --system 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  "$HOLD_BIN" logs --plain "$id" >"$TEST_ROOT/raw-start.log" || return 1
  grep -q '^arg:--system$' "$TEST_ROOT/raw-start.log"
}

test_public_root_index_list_is_redacted() {
  write_public_index_fixture abc12345cafe0000000000000000000000000000000000000000000000000000 running 2026-06-15T18:42:11Z || return 1
  # Public root-index rows appear with -a in redacted form: the USER and COMMAND
  # cells both read the literal "hidden" (exists-but-not-yours-to-see, not "-"),
  # and the STATUS reflects the projected state.
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  # Projected rows speak the table's Docker phrasing: "Up <age>", not the raw hint.
  grep -Eq '^abc12345cafe[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*[[:space:]]Up ' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

test_public_root_index_list_reads_projected_state() {
  local id=deadbeefcafe0000000000000000000000000000000000000000000000000000
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{
  "id": "deadbeefcafe0000000000000000000000000000000000000000000000000000",
  "root_managed": true,
  "requires_elevation": true,
  "created_at": "2026-06-15T18:42:10Z",
  "State": {
    "Status": "exited",
    "Running": false,
    "Paused": false,
    "Restarting": false,
    "Dead": false,
    "Pid": 0,
    "Pgid": 0,
    "Sid": 0,
    "ExitCode": 7,
    "Error": "",
    "StartedAt": "2026-06-15T18:42:11Z",
    "FinishedAt": "2026-06-15T18:42:12Z"
  },
  "argv": ["secret"],
  "cmdline_display": "secret command"
}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  # The projected State.Status ("exited") drives the STATUS column; USER and
  # COMMAND stay redacted to the literal "hidden". Ended public rows need -a.
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  grep -Eq '^deadbeefcafe[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*[[:space:]]exited([[:space:]]|$)' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

# `hold list` is the ledger: a user's own calls, live and past, by default, with
# a USER column naming the caller; -l/--live narrows it to the running ones.
test_list_default_is_full_ledger_live_narrows() {
  local live_out ended_out live_id ended_id me
  me=$(id -un)
  live_out=$("$HOLD_BIN" -d --name ledger_live sleep 300 2>&1) || return 1
  live_id=$(printf '%s\n' "$live_out" | extract_id); [ -n "$live_id" ] || return 1
  ended_out=$("$HOLD_BIN" -d --name ledger_past /bin/sh -c 'exit 4' 2>&1) || return 1
  ended_id=$(printf '%s\n' "$ended_out" | extract_id); [ -n "$ended_id" ] || return 1
  record_ended_soon "$ended_id" || return 1
  # Default view: the USER column, then both the live and the past call, each
  # attributed to the calling user.
  "$HOLD_BIN" list >"$TEST_ROOT/ledger.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  grep -Eq "^$live_id[[:space:]]+$me[[:space:]].*Up " "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  grep -Eq "^$ended_id[[:space:]]+$me[[:space:]].*Exited \\(4\\)" "$TEST_ROOT/ledger.out" || { cat "$TEST_ROOT/ledger.out" >&2; return 1; }
  # -l (and its --live long form) drop the past call, keep the live one.
  local flag
  for flag in -l --live; do
    "$HOLD_BIN" list "$flag" >"$TEST_ROOT/live.out" 2>&1 || return 1
    grep -Eq "^$live_id[[:space:]].*Up " "$TEST_ROOT/live.out" || { cat "$TEST_ROOT/live.out" >&2; return 1; }
    ! grep -q "^$ended_id" "$TEST_ROOT/live.out" || { echo "$flag still showed the past call" >&2; cat "$TEST_ROOT/live.out" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$live_id" >/dev/null 2>&1 || true
}

# `hold list <name>` narrows the ledger to the call whose NAME matches. Regression
# for the filter matching the deleted profile-era alias field, which nothing set,
# so a named lookup of a real call printed an empty table.
test_list_name_filter_matches_call_name() {
  local out webby_id other_id
  out=$("$HOLD_BIN" -d --name webby sleep 300 2>&1) || return 1
  webby_id=$(printf '%s\n' "$out" | extract_id); [ -n "$webby_id" ] || return 1
  out=$("$HOLD_BIN" -d --name other_call sleep 300 2>&1) || return 1
  other_id=$(printf '%s\n' "$out" | extract_id); [ -n "$other_id" ] || return 1
  "$HOLD_BIN" list webby >"$TEST_ROOT/named.out" 2>&1 || return 1
  grep -Eq "^$webby_id[[:space:]].*[[:space:]]webby$" "$TEST_ROOT/named.out" || { cat "$TEST_ROOT/named.out" >&2; return 1; }
  ! grep -q 'other_call' "$TEST_ROOT/named.out" || { echo "name filter leaked another call" >&2; cat "$TEST_ROOT/named.out" >&2; return 1; }
  "$HOLD_BIN" stop "$webby_id" >/dev/null 2>&1 || true
  "$HOLD_BIN" stop "$other_id" >/dev/null 2>&1 || true
}

# list -a renders the observed ports root projected into a global call's public
# entry, in the same redacted row (USER and COMMAND both "hidden").
test_list_all_renders_global_ports_projection() {
  local id=facefeed12340000000000000000000000000000000000000000000000000000
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{"id":"facefeed12340000000000000000000000000000000000000000000000000000","root_managed":true,"name":"global_web","state_hint":"running","created_at":"2026-06-15T18:42:11Z","observed_ports":"127.0.0.1:8080/tcp, [::]:9090/tcp","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  "$HOLD_BIN" list -a >"$TEST_ROOT/list.out" 2>&1 || return 1
  # The redacted row shows the generated name and the projected ports; USER and
  # COMMAND read "hidden" and the command line never appears.
  grep -Eq "^${id:0:12}[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*127\\.0\\.0\\.1:8080/tcp.*global_web$" "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

# `hold list -s/--system` (and the equivalent `--system list`) is exactly the
# redacted global view: no personal calls leak into it.
test_system_list_is_redacted_global_only() {
  local out personal_id gid=beadfeed56780000000000000000000000000000000000000000000000000000 form
  out=$("$HOLD_BIN" -d --name my_personal sleep 300 2>&1) || return 1
  personal_id=$(printf '%s\n' "$out" | extract_id); [ -n "$personal_id" ] || return 1
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" <<'JSON'
{"id":"beadfeed56780000000000000000000000000000000000000000000000000000","root_managed":true,"name":"global_only","state_hint":"running","created_at":"2026-06-15T18:42:11Z","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" || return 1
  # -s, --system, and the pre-command --system spelling all mean the same view.
  for form in "list -s" "list --system" "--system list"; do
    # shellcheck disable=SC2086
    "$HOLD_BIN" $form >"$TEST_ROOT/system-list.out" 2>&1 || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    grep -Eq "^${gid:0:12}[[:space:]]+hidden[[:space:]]+hidden[[:space:]].*global_only$" "$TEST_ROOT/system-list.out" || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    ! grep -q "^$personal_id" "$TEST_ROOT/system-list.out" || { echo "form '$form' leaked a personal call" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
    ! grep -q 'my_personal' "$TEST_ROOT/system-list.out" || { echo "form: $form" >&2; cat "$TEST_ROOT/system-list.out" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$personal_id" >/dev/null 2>&1 || true
}

# ps is Docker's machine-wide running view: running calls from both scopes, the
# global ones redacted, no USER column; -a adds ended; non-Docker flags reject.
test_ps_is_docker_machine_wide_running_view() {
  local live_out live_id ended_out ended_id gid=c1cadafeed990000000000000000000000000000000000000000000000000000 rc
  live_out=$("$HOLD_BIN" -d --name ps_live sleep 300 2>&1) || return 1
  live_id=$(printf '%s\n' "$live_out" | extract_id); [ -n "$live_id" ] || return 1
  ended_out=$("$HOLD_BIN" -d --name ps_past /bin/sh -c 'exit 2' 2>&1) || return 1
  ended_id=$(printf '%s\n' "$ended_out" | extract_id); [ -n "$ended_id" ] || return 1
  record_ended_soon "$ended_id" || return 1
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" <<'JSON'
{"id":"c1cadafeed990000000000000000000000000000000000000000000000000000","root_managed":true,"name":"ps_global","state_hint":"running","created_at":"2026-06-15T18:42:11Z","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/$gid.json" || return 1
  # ps: Docker's header (no USER column), running personal + global, past hidden.
  "$HOLD_BIN" ps >"$TEST_ROOT/ps.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  grep -Eq "^$live_id[[:space:]].*Up .*ps_live$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  grep -Eq "^${gid:0:12}[[:space:]]+hidden[[:space:]].*ps_global$" "$TEST_ROOT/ps.out" || { cat "$TEST_ROOT/ps.out" >&2; return 1; }
  ! grep -q "^$ended_id" "$TEST_ROOT/ps.out" || { echo "ps default showed an ended call" >&2; cat "$TEST_ROOT/ps.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/ps.out"
  # ps -a includes the ended personal call.
  "$HOLD_BIN" ps -a >"$TEST_ROOT/ps-a.out" 2>&1 || return 1
  grep -Eq "^$ended_id[[:space:]].*Exited \\(2\\)" "$TEST_ROOT/ps-a.out" || { cat "$TEST_ROOT/ps-a.out" >&2; return 1; }
  # ps speaks only Docker: -l and -s are rejected as unknown flags.
  local badflag
  for badflag in -l -s -u; do
    set +e; "$HOLD_BIN" ps "$badflag" >/dev/null 2>"$TEST_ROOT/ps-bad.err"; rc=$?; set -e
    [ "$rc" -eq 1 ] || { echo "ps $badflag: rc=$rc (want 1)" >&2; return 1; }
    grep -q "unknown flag '$badflag'" "$TEST_ROOT/ps-bad.err" || { cat "$TEST_ROOT/ps-bad.err" >&2; return 1; }
  done
  "$HOLD_BIN" stop "$live_id" >/dev/null 2>&1 || true
}

# `hold purge -s` as a normal user re-execs through sudo (so sudo can prompt),
# passing exactly `<self> purge --system` (plus any state flags).
test_purge_system_reexecs_through_sudo() {
  make_fake_sudo || return 1
  export HOLD_FAKE_SUDO_RC=0
  # The re-exec replaces the process with (fake) sudo, which records its argv,
  # one entry per line: the self binary path, then purge, then --system.
  set +e
  "$HOLD_BIN" purge -s >/dev/null 2>&1
  set -e
  grep -qx 'purge' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing 'purge'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  grep -qx -- '--system' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing '--system'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  grep -q 'hold' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing the self binary path" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  ! grep -qx -- '--all' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv carried an unrequested --all" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
  # -a is forwarded as a state widener when requested.
  : > "$HOLD_FAKE_SUDO_ARGV"
  set +e
  "$HOLD_BIN" purge -s -a >/dev/null 2>&1
  set -e
  grep -qx -- '--all' "$HOLD_FAKE_SUDO_ARGV" || { echo "sudo argv missing forwarded '--all'" >&2; cat "$HOLD_FAKE_SUDO_ARGV" >&2; return 1; }
}

# As root, `list` is the global working view; `list -a` also walks every user's
# personal store and adds a USER column so ownership is clear.
test_root_list_all_labels_owner_and_walks_homes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  local homes="$TEST_ROOT/homes" uid owner
  uid=$(id -u); owner=$(id -un)
  # A fabricated personal store under a test home root, holding one call owned by
  # the invoking user (so its USER resolves via /etc/passwd). The record shape
  # mirrors the minimal valid record, with a live-looking pgid so it is a row.
  mkdir -p "$homes/somebody/.local/state/hold" || return 1
  cat > "$homes/somebody/.local/state/hold/aa11bb22cc330000000000000000000000000000000000000000000000000000.json" <<JSON
{"version":1,"id":"aa11bb22cc330000000000000000000000000000000000000000000000000000","pid":2,"pgid":2,"sid":2,"start_unix_ns":0,"argv":["peer-cmd"],"cmdline_display":"peer-cmd","uid":$uid,"gid":$uid,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0,"name":"peer_call"}
JSON
  chmod -R 755 "$homes" || return 1
  # A global call in the system store (root's own view).
  local out gid_call
  out=$(as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" -d --name global_root sleep 300 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  gid_call=$(printf '%s\n' "$out" | extract_id); [ -n "$gid_call" ] || { printf '%s\n' "$out" >&2; return 1; }
  # Plain root list: the global call only, no personal rows.
  as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" list >"$TEST_ROOT/root-list.out" 2>&1 || return 1
  grep -Eq "^$gid_call[[:space:]].*global_root$" "$TEST_ROOT/root-list.out" || { cat "$TEST_ROOT/root-list.out" >&2; return 1; }
  ! grep -q 'peer_call' "$TEST_ROOT/root-list.out" || { echo "plain root list walked personal stores" >&2; cat "$TEST_ROOT/root-list.out" >&2; return 1; }
  # Root list -a: USER column (second, after CALL ID); the peer call attributed
  # to its record owner, the global call to its recorded invoking user. Under
  # sudo that is the invoking account; run directly as root it would be "root".
  # In both cases the label equals `id -un` in this harness, so assert that.
  as_root env HOLD_TEST_HOME_ROOT="$homes" "$HOLD_REAL_BIN" list -a >"$TEST_ROOT/root-list-a.out" 2>&1 || return 1
  grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  grep -Eq "^aa11bb22cc33[[:space:]]+$owner[[:space:]].*peer_call$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  grep -Eq "^$gid_call[[:space:]]+$owner[[:space:]].*global_root$" "$TEST_ROOT/root-list-a.out" || { cat "$TEST_ROOT/root-list-a.out" >&2; return 1; }
  as_root "$HOLD_REAL_BIN" stop "system:$gid_call" >/dev/null 2>&1 || true
}

test_user_local_wins_over_public_root_collision() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  # A root-public entry that collides on the call's display prefix: the local
  # record must win so stop resolves it without a sudo re-exec.
  local pub_id="${id}0000000000000000000000000000000000000000000000000000"
  write_public_index_fixture "$pub_id" running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  [ ! -s "$HOLD_FAKE_SUDO_ARGV" ] || return 1
  pgid_terminated "$pgid" || return 1
  record_exists "$pub_id" "$HOLD_TEST_SYSTEM_STATE_DIR/public"
}

test_explicit_user_target() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$HOLD_BIN" stop "user:$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}



test_tail_ctrl_c_detaches_from_tail_and_keeps_run() {
  command -v setsid >/dev/null 2>&1 || skip "setsid not available"
  local out id pgid tail_pid rc
  out=$("$HOLD_BIN" -d bash -c 'while :; do echo tail-still-running; sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  setsid "$HOLD_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" &
  tail_pid=$!
  sleep 0.3
  kill -INT "-$tail_pid" 2>/dev/null || kill -INT "$tail_pid" 2>/dev/null || true
  set +e
  wait "$tail_pid"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  kill -0 "-$pgid" 2>/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_long_command_list_truncates_instead_of_skips() {
  local out id long_arg list_out
  long_arg=$(printf 'x%.0s' $(seq 1 140))
  out=$("$HOLD_BIN" -d /bin/echo "$long_arg" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]" || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*\.\.\."
}

test_normal_start_writes_user_local_state() {
  local out id mode
  out=$("$HOLD_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(record_path "$id") || return 1
  log=$(log_path "$id") || return 1
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  ! root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  ! record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  mode=$(file_mode "$json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$log") || return 1
  [ "$mode" = 600 ] || return 1
  python3 - "$json" <<'PY' || { cat "$json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
required_top = {'Id', 'Created', 'Path', 'Args', 'State', 'LogPath', 'LogIdx', 'Name', 'RestartCount', 'Config'}
missing = required_top - set(obj)
if missing:
    raise SystemExit(f'missing Docker-shaped run fields: {sorted(missing)}')
state_keys = {'Status', 'Running', 'Paused', 'Restarting', 'Dead', 'Pid', 'Pgid', 'Sid', 'ExitCode', 'Error', 'StartedAt', 'FinishedAt'}
missing = state_keys - set(obj.get('State') or {})
if missing:
    raise SystemExit(f'missing State keys: {sorted(missing)}')
config_keys = {'AttachStdin', 'AttachStdout', 'AttachStderr', 'Tty', 'OpenStdin', 'StdinOnce', 'Env', 'WorkingDir', 'CapAdd', 'CapDrop', 'Privileged', 'LogConfig'}
missing = config_keys - set(obj.get('Config') or {})
if missing:
    raise SystemExit(f'missing Config keys: {sorted(missing)}')
if obj['Path'] != (obj.get('argv') or [''])[0]:
    raise SystemExit(f'Path does not match recorded exec argv0: {obj["Path"]!r} vs {obj.get("argv")!r}')
if obj['Args'] != (obj.get('argv') or [])[1:]:
    raise SystemExit(f'Args should be argv after argv0: {obj["Args"]!r} vs {obj.get("argv")!r}')
working_dir = (obj.get('Config') or {}).get('WorkingDir')
observed_cwd = (obj.get('observed') or {}).get('cwd')
if observed_cwd and working_dir != observed_cwd:
    raise SystemExit(f'Config.WorkingDir should mirror observed cwd: {working_dir!r} vs {observed_cwd!r}')
if working_dir and (not working_dir.startswith('/') or (working_dir != '/' and working_dir.endswith('/'))):
    raise SystemExit(f'Config.WorkingDir is not a normalized absolute path: {working_dir!r}')
PY
}

test_docker_shaped_record_fallback_reader() {
  local id store record logdir logpath idxpath ps_out
  id=fe4a4b8fbc063e25cd4bcb6798791b1dd03caca62f6a0b4c1304f28940992c52
  store="$HOME/.local/state/hold"
  mkdir -p "$store" || return 1
  logdir="$HOME/.local/state/hold-logs/$id"
  mkdir -p "$logdir" || return 1
  logpath="$logdir/$id.log"
  idxpath="$logpath.idx"
  : >"$logpath" || return 1
  : >"$idxpath" || return 1
  record="$store/$id.json"
  cat >"$record" <<JSON
{
  "Id": "$id",
  "Created": "2026-06-28T17:30:47.520199158Z",
  "Path": "/usr/bin/python3",
  "Args": ["-m", "http.server", "8080"],
  "State": {
    "Status": "exited",
    "Running": false,
    "Paused": false,
    "Restarting": false,
    "Dead": false,
    "Pid": 999999,
    "Pgid": 999999,
    "Sid": 999999,
    "ExitCode": 7,
    "Error": "",
    "StartedAt": "2026-06-28T17:30:47.559521995Z",
    "FinishedAt": "2026-06-28T17:30:48.000000000Z"
  },
  "LogPath": "$logpath",
  "LogIdx": "$idxpath",
  "Name": "adjective_noun",
  "RestartCount": 0,
  "Config": {
    "User": "",
    "AttachStdin": true,
    "AttachStdout": true,
    "AttachStderr": true,
    "Tty": false,
    "OpenStdin": false,
    "StdinOnce": false,
    "Env": ["HOLD_DOCKER_SHAPED_READER=1"],
    "Origin": "web",
    "WorkingDir": "$TEST_ROOT",
    "CapAdd": ["NET_BIND_SERVICE"],
    "CapDrop": ["ALL"],
    "Privileged": false,
    "LogConfig": {"Type": "local", "Config": {}}
  },
  "version": 1,
  "id": "$id",
  "run_id": "$id",
  "start_unix_ns": 0,
  "created_unix_ns": 0,
  "uid": $(id -u),
  "gid": $(id -g),
  "proc_starttime_ticks": 0,
  "exe_dev": 0,
  "exe_ino": 0
}
JSON
  chmod 600 "$record" || return 1
  ps_out=$("$HOLD_BIN" ps -a 2>"$TEST_ROOT/docker-reader.err") || {
    cat "$TEST_ROOT/docker-reader.err" >&2
    return 1
  }
  ! grep -q 'skipping corrupt record' "$TEST_ROOT/docker-reader.err" || { cat "$TEST_ROOT/docker-reader.err" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q '^fe4a4b8fbc06' || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q '"/usr/bin/python3 -m http' || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -q 'adjective_noun' || { printf '%s\n' "$ps_out" >&2; return 1; }
}

test_root_start_writes_system_store_and_public_unknown() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id mode list_out
  out=$(as_root "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s
' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  log=$(root_log_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  public_json=$(record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  ! record_exists "$id" "$ROOT_HOME/.local/state/hold" || return 1
  mode=$(root_file_mode "$json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(root_file_mode "$log") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$public_json") || return 1
  [ "$mode" = 644 ] || return 1
  grep -Eq '"state_hint": "(running|exited)"' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"State": {' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"Pid": [1-9]' "$public_json" || { cat "$public_json" >&2; return 1; }
  grep -q '"StartedAt": "' "$public_json" || { cat "$public_json" >&2; return 1; }
  matched=0
  for _ in $(seq 1 30); do
    as_root cat "$json" >"$TEST_ROOT/root-private-state.json" || return 1
    if python3 - "$TEST_ROOT/root-private-state.json" "$public_json" >"$TEST_ROOT/root-public-state-compare.err" 2>&1 <<'PY'
import json, sys
private = json.load(open(sys.argv[1], encoding='utf-8'))
public = json.load(open(sys.argv[2], encoding='utf-8'))
projection_keys = {'Id', 'Created', 'Name', 'Config', 'State'}
missing_projection = projection_keys - set(public)
if missing_projection:
    raise SystemExit(f'public projection missing Docker-shaped keys: {sorted(missing_projection)}')
if public['Id'] != private['Id']:
    raise SystemExit(f'public/private Id drift: {public["Id"]!r} != {private["Id"]!r}')
if public['Created'] != private['Created']:
    raise SystemExit(f'public/private Created drift: {public["Created"]!r} != {private["Created"]!r}')
state_keys = {'Status', 'Running', 'Paused', 'Restarting', 'Dead', 'Pid', 'Pgid', 'Sid', 'ExitCode', 'Error', 'StartedAt', 'FinishedAt'}
for label, obj in [('private', private), ('public', public)]:
    state = obj.get('State') or {}
    missing = state_keys - set(state)
    if missing:
        raise SystemExit(f'{label} State missing keys: {sorted(missing)}')
for key in state_keys:
    if private['State'][key] != public['State'][key]:
        raise SystemExit(f'public/private State drift for {key}: {private["State"][key]!r} != {public["State"][key]!r}')
PY
    then
      matched=1
      break
    fi
    sleep 0.1
  done
  [ "$matched" = 1 ] || { cat "$TEST_ROOT/root-public-state-compare.err" "$TEST_ROOT/root-private-state.json" "$public_json" >&2; return 1; }
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s
' "$list_out" | grep -Eq "^$id[[:space:]].*(Up|Exited|running|exited)"
}

test_sudo_start_writes_system_store_with_invoking_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id json
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  ! record_exists "$id" "$ACTOR_HOME/.local/state/hold" || return 1
  root_grep '"invoked_by_uid": '"$TEST_UID" "$json" || return 1
  root_grep '"invoked_by_gid": '"$TEST_GID" "$json" || return 1
  root_grep '"invoked_by_user": "'"$TEST_USER"'"' "$json" || return 1
  root_grep '"invoked_via_sudo": true' "$json"
}

make_home_executable() {
  local app="$ACTOR_HOME/bin/home-tool"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  cat > "$app" <<'SH'
#!/bin/sh
printf 'home-tool\n'
SH
  chmod 755 "$app" || return 1
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  fi
  printf '%s\n' "$app"
}

make_sleeping_home_executable() {
  local app="$ACTOR_HOME/bin/home-tool"
  mkdir -p "$ACTOR_HOME/bin" || return 1
  cat > "$app" <<'SH'
#!/bin/sh
printf 'home-tool\n'
sleep 5
SH
  chmod 755 "$app" || return 1
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ]; then
    as_root chown "$TEST_UID:$TEST_GID" "$ACTOR_HOME/bin" "$app" || return 1
  fi
  printf '%s\n' "$app"
}

assert_home_system_start_is_user_local() {
  local out="$1" id json log owner
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json=$(root_record_path "$id" "$ACTOR_HOME/.local/state/hold") || return 1
  log=$(root_log_path "$id" "$ACTOR_HOME/.local/state/hold") || return 1
  root_file_exists "$json" || return 1
  root_file_exists "$log" || return 1
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  ! root_log_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs" || return 1
  ! record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  owner=$(root_file_owner "$json") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
}

test_sudo_system_start_of_home_executable_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out
  app=$(make_home_executable) || return 1
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d "$app" 2>&1) || return 1
  # Files materialize through a sudo child; poll briefly instead of asserting
  # one instant. A real placement bug fails every attempt in the window.
  local sudo_tries=0
  while :; do
    if assert_home_system_start_is_user_local "$out"; then
      return 0
    fi
    sudo_tries=$((sudo_tries + 1))
    [ "$sudo_tries" -lt 20 ] || break
    sleep 0.1
  done
  assert_home_system_start_is_user_local "$out"
}

test_sudo_system_start_of_home_argv_paths_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local cfg out id
  cfg="$ACTOR_HOME/app.toml"
  as_user sh -c 'printf "%s\n" "config" >"$1"' sh "$cfg" || return 1

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' "$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' --config="$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true

  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d /bin/sh -c 'sleep 5' --config "$cfg" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id); [ -n "$id" ] && as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true
}


test_sudo_context_can_stop_unique_user_local_run() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}


test_docker_shaped_cli_flags_and_rm() {
  local out id id2
  out=$("$HOLD_BIN" -d --name docker-web -e HOLD_DOCKER_ENV=ok -- /bin/sh -c 'echo "$HOLD_DOCKER_ENV"; sleep 0.1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  # Poll for the captured line: under load the logger can flush later than a
  # fixed 0.2s; a real capture regression still fails the whole window.
  local logs_tries=0
  while :; do
    "$HOLD_BIN" logs -n 1 --plain "$id" >"$TEST_ROOT/docker-logs-tail.out" 2>/dev/null || true
    grep -q '^ok$' "$TEST_ROOT/docker-logs-tail.out" && break
    logs_tries=$((logs_tries + 1))
    [ "$logs_tries" -lt 20 ] || { cat "$TEST_ROOT/docker-logs-tail.out" >&2; return 1; }
    sleep 0.1
  done
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/docker-inspect.json" || return 1
  python3 - "$TEST_ROOT/docker-inspect.json" "$id" <<'PY' || { cat "$TEST_ROOT/docker-inspect.json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
if not isinstance(obj, list) or len(obj) != 1:
    raise SystemExit('inspect must return a one-object JSON array')
record = obj[0]
if not str(record.get('id', '')).startswith(sys.argv[2]):
    raise SystemExit('inspect record id mismatch')
if record.get('name') != 'docker-web':
    raise SystemExit('inspect record did not include run name')
if 'ok\n' in json.dumps(record):
    raise SystemExit('inspect returned log text instead of structured record data')
PY
  # The 0.1s child may still be live when the log line lands; poll until
  # ps -a reports it Exited rather than sleeping a fixed beat.
  local ps_tries=0
  while :; do
    "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-ps.out" || return 1
    grep -Eq "^$id[[:space:]]+.*Exited.*docker-web$" "$TEST_ROOT/docker-ps.out" && break
    ps_tries=$((ps_tries + 1))
    [ "$ps_tries" -lt 20 ] || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }
    sleep 0.1
  done
  grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES" "$TEST_ROOT/docker-ps.out" || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }

  out=$("$HOLD_BIN" -d --name docker-direct /bin/sh -c 'echo direct; sleep 5' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" rm --force docker-direct >/dev/null || return 1
  set +e; "$HOLD_BIN" inspect "$id2" >"$TEST_ROOT/docker-rm-inspect.out" 2>&1; rc=$?; set -e
  [ "$rc" -ne 0 ] || { cat "$TEST_ROOT/docker-rm-inspect.out" >&2; return 1; }
}

# The ps table humanizes CREATED Docker-style, phrases STATUS as Up/Exited, and
# annotates a saved call inline on STATUS.
test_ps_table_humanized_status_and_saved() {
  local out id run_out exit_id ps_out
  out=$("$HOLD_BIN" -d --name ps-humanized -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  run_out=$("$HOLD_BIN" -d --name ps-exiter -- /bin/sh -c 'exit 5' 2>&1) || return 1
  exit_id=$(printf '%s\n' "$run_out" | extract_id)
  [ -n "$exit_id" ] || return 1
  record_ended_soon "$exit_id" || return 1
  # Age both calls past a second so CREATED reads a whole-unit Docker age
  # ("1 second ago") rather than the sub-second "Less than a second ago".
  sleep 1.2

  ps_out=$("$HOLD_BIN" ps -a) || return 1
  # Header is exact, in the CALL ID..NAMES order with no PROFILE column.
  printf '%s\n' "$ps_out" | grep -Eq "^CALL ID[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" ||
    { printf '%s\n' "$ps_out" >&2; return 1; }
  # CREATED is humanized: never a raw ISO stamp, always a Docker-style age.
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*(About a minute|[0-9]+ (seconds?|minutes?|hours?|days?|weeks?|months?)) ago" ||
    { printf '%s\n' "$ps_out" >&2; return 1; }
  ! printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*[0-9]{4}-[0-9]{2}-[0-9]{2}T" || return 1
  # STATUS phrasing: Up ... for the live call, Exited (5) ... ago for the ended one.
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*Up [0-9A-Za-z]" || { printf '%s\n' "$ps_out" >&2; return 1; }
  printf '%s\n' "$ps_out" | grep -Eq "^$exit_id[[:space:]].*Exited \(5\) .*ago" || { printf '%s\n' "$ps_out" >&2; return 1; }

  # save marks the live call; ps annotates protection inline on STATUS.
  "$HOLD_BIN" save ps-humanized >/dev/null 2>&1 || return 1
  ps_out=$("$HOLD_BIN" ps -a) || return 1
  printf '%s\n' "$ps_out" | grep -Eq "^$id[[:space:]].*Up .*\(saved\)" || { printf '%s\n' "$ps_out" >&2; return 1; }

  # list carries the same Up/Exited/(saved) STATUS phrasing as ps, but its own
  # header adds the USER column in Docker's IMAGE slot (second, after CALL ID).
  list_out=$("$HOLD_BIN" list -a) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^CALL ID[[:space:]]+USER[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES$" ||
    { printf '%s\n' "$list_out" >&2; return 1; }
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*Up .*\(saved\)" || { printf '%s\n' "$list_out" >&2; return 1; }
  printf '%s\n' "$list_out" | grep -Eq "^$exit_id[[:space:]].*Exited \(5\) .*ago" || { printf '%s\n' "$list_out" >&2; return 1; }

  "$HOLD_BIN" end ps-humanized >/dev/null 2>&1 || true
}

# Content-sized columns must not shear: every data cell begins exactly under its
# header, whether the command is short or long enough to be ellipsized.
test_ps_table_columns_do_not_shear() {
  local short_out long_out ps_out long_arg
  short_out=$("$HOLD_BIN" -d --name ps-short -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  long_arg=$(printf 'y%.0s' $(seq 1 120))
  long_out=$("$HOLD_BIN" -d --name ps-long -- /bin/sh -c "echo $long_arg; sleep 30" 2>&1) || return 1
  [ -n "$(printf '%s\n' "$short_out" | extract_id)" ] || return 1
  [ -n "$(printf '%s\n' "$long_out" | extract_id)" ] || return 1

  ps_out=$("$HOLD_BIN" ps -a) || return 1
  printf '%s\n' "$ps_out" | awk '
    NR == 1 {
      for (label = 1; label <= 4; label++) { }
      cols["COMMAND"] = index($0, "COMMAND")
      cols["CREATED"] = index($0, "CREATED")
      cols["STATUS"]  = index($0, "STATUS")
      next
    }
    {
      for (name in cols) {
        pos = cols[name]
        # The gutter before the column is a space and the cell itself is not,
        # so the column starts exactly where its header does.
        if (substr($0, pos - 1, 1) != " " || substr($0, pos, 1) == " ") {
          printf("shear at %s on: %s\n", name, $0) > "/dev/stderr"
          exit 1
        }
      }
    }
  ' || return 1
  # The long command is present and ellipsized rather than dropped.
  printf '%s\n' "$ps_out" | grep -Eq 'ps-long$' || return 1
  printf '%s\n' "$ps_out" | grep -Eq '\.\.\."' || return 1

  "$HOLD_BIN" end ps-short ps-long >/dev/null 2>&1 || true
}

# hold ports lists the bound listeners of a call's process group, one per line.
test_ports_lists_bound_listener() {
  local out id ports_out tries rc
  out=$("$HOLD_BIN" -d --name ports-smoke -- python3 -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",0)); s.listen(); print(s.getsockname()[1]); time.sleep(30)' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  ports_out=""
  for tries in $(seq 1 30); do
    ports_out=$("$HOLD_BIN" ports ports-smoke 2>/dev/null || true)
    printf '%s\n' "$ports_out" | grep -Eq '^127\.0\.0\.1:[0-9]+/tcp$' && break
    sleep 0.1
  done
  printf '%s\n' "$ports_out" | grep -Eq '^127\.0\.0\.1:[0-9]+/tcp$' || { printf '%s\n' "$ports_out" >&2; return 1; }

  # No target is a usage error.
  set +e; "$HOLD_BIN" ports >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 5 ] || return 1

  "$HOLD_BIN" end ports-smoke >/dev/null 2>&1 || true
}

# hold stats --no-stream prints one plain frame with the documented columns.
test_stats_single_shot_shape() {
  local out id stats_out
  out=$("$HOLD_BIN" -d --name stats-smoke -- /bin/sh -c 'sleep 30' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  stats_out=$("$HOLD_BIN" stats --no-stream stats-smoke) || return 1
  printf '%s\n' "$stats_out" | grep -Eq "^CALL ID[[:space:]]+NAME[[:space:]]+CPU %[[:space:]]+MEM \(RSS\)[[:space:]]+PIDS$" ||
    { printf '%s\n' "$stats_out" >&2; return 1; }
  printf '%s\n' "$stats_out" | grep -Eq "^$id[[:space:]]+stats-smoke[[:space:]]+[0-9.]+%[[:space:]]+[0-9.]+(B|KiB|MiB|GiB)[[:space:]]+[0-9]+$" ||
    { printf '%s\n' "$stats_out" >&2; return 1; }

  "$HOLD_BIN" end stats-smoke >/dev/null 2>&1 || true
}

# inspect reports where a live call's fds actually point. A child that redirects
# its own stdout to a file must show that file under Stdio.Stdout.
test_inspect_reports_stdio_targets() {
  local redir out id
  redir="$TEST_ROOT/inspect-stdio.out"
  out=$("$HOLD_BIN" -d --name stdio-smoke -- /bin/sh -c "exec >\"$redir\"; sleep 30" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  "$HOLD_BIN" inspect stdio-smoke >"$TEST_ROOT/inspect-stdio.json" || return 1
  python3 - "$TEST_ROOT/inspect-stdio.json" "$redir" <<'PY' || { cat "$TEST_ROOT/inspect-stdio.json" >&2; return 1; }
import json, sys
obj = json.load(open(sys.argv[1], encoding='utf-8'))
record = obj[0]
stdio = record.get('Stdio')
if not isinstance(stdio, dict):
    raise SystemExit('inspect did not report a Stdio object for a live call')
for key in ('Stdin', 'Stdout', 'Stderr'):
    if key not in stdio:
        raise SystemExit('Stdio missing key ' + key)
if stdio['Stdout'] != sys.argv[2]:
    raise SystemExit('Stdout fd target %r did not match the redirect %r' % (stdio['Stdout'], sys.argv[2]))
PY
  "$HOLD_BIN" end stdio-smoke >/dev/null 2>&1 || true
}

test_docker_bare_launch_foreground_follows_output_by_default() {
  local out id
  # Bare foreground streams the process output and prints no id of its own.
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo bare-foreground-ok' 2>"$TEST_ROOT/docker-bare-foreground.err") || {
    cat "$TEST_ROOT/docker-bare-foreground.err" >&2
    return 1
  }
  grep -q '^bare-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }
  if grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi

  # Detached prints exactly the bare 64-hex id and hides the process output.
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo bare-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-bare-detached.err") || {
    cat "$TEST_ROOT/docker-bare-detached.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^bare-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}


test_docker_run_foreground_follows_output_by_default() {
  local out id
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo docker-foreground-ok' 2>"$TEST_ROOT/docker-foreground.err") || {
    cat "$TEST_ROOT/docker-foreground.err" >&2
    return 1
  }
  grep -q '^docker-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }
  # Docker parity: foreground run prints only the process output, no id line.
  if grep -Eq '^[0-9a-f]{12}$|^[0-9a-f]{64}$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi

  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo docker-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-detached.err") || {
    cat "$TEST_ROOT/docker-detached.err" >&2
    return 1
  }
  # Docker parity: detach prints the full 64-hex id alone.
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^docker-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_unsupported_options_fail_loudly() {
  local rc
  "$HOLD_BIN" --detach-keys ctrl-p,ctrl-q /bin/true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-default.err" || {
    cat "$TEST_ROOT/docker-detach-keys-default.err" >&2
    return 1
  }

  "$HOLD_BIN" --detach-keys ctrl-a /bin/true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-custom.err" || {
    cat "$TEST_ROOT/docker-detach-keys-custom.err" >&2
    return 1
  }

  set +e
  "$HOLD_BIN" --detach-keys ctrl-not-a-key /bin/true >/dev/null 2>"$TEST_ROOT/docker-detach-keys-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "invalid --detach-keys: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --detach-keys sequence" "$TEST_ROOT/docker-detach-keys-invalid.err" || { cat "$TEST_ROOT/docker-detach-keys-invalid.err" >&2; return 1; }

  "$HOLD_BIN" prune all >/dev/null 2>&1 || true
  "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-unsupported-ps.out" || return 1
  ! grep -q '/bin/true' "$TEST_ROOT/docker-unsupported-ps.out" || { cat "$TEST_ROOT/docker-unsupported-ps.out" >&2; return 1; }
}


test_docker_restart_policy_restarts_failures() {
  local out id count got
  out=$("$HOLD_BIN" -d --restart on-failure:2 --restart-delay 0 -- /bin/sh -c 'mkdir -p "$HOME"; echo attempt >> "$HOME/restart-count"; c=$(wc -l < "$HOME/restart-count"); echo restart-attempt-$c; [ "$c" -lt 3 ] && exit 7 || sleep 1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  for _ in $(seq 1 30); do
    count=$(wc -l <"$HOME/restart-count" 2>/dev/null || echo 0)
    [ "$count" -ge 3 ] && break
    sleep 0.1
  done
  count=$(wc -l <"$HOME/restart-count" 2>/dev/null || echo 0)
  [ "$count" -eq 3 ] || { echo "restart attempts: $count" >&2; "$HOLD_BIN" logs "$id" --plain -n 50 >&2 || true; return 1; }
  got=$("$HOLD_BIN" logs "$id" --plain -n 50) || return 1
  printf '%s\n' "$got" | grep -q 'restart-attempt-1' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -q 'restart-attempt-2' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -q 'restart-attempt-3' || { printf '%s\n' "$got" >&2; return 1; }
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/restart-run.json" || return 1
  grep -q '"restart": "on-failure:2"' "$TEST_ROOT/restart-run.json" || { cat "$TEST_ROOT/restart-run.json" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
}

test_docker_foreground_propagates_exit_code() {
  local rc
  set +e
  "$HOLD_BIN" -- /bin/sh -c 'exit 7' >/dev/null 2>"$TEST_ROOT/fg-exit.err"
  rc=$?
  set -e
  [ "$rc" -eq 7 ] || { echo "foreground rc=$rc, want 7" >&2; cat "$TEST_ROOT/fg-exit.err" >&2; return 1; }
  set +e
  "$HOLD_BIN" --rm -- /bin/sh -c 'exit 5' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "--rm foreground rc=$rc, want 5" >&2; return 1; }
  "$HOLD_BIN" -- /bin/true >/dev/null 2>&1 || { echo "clean foreground exit not 0" >&2; return 1; }
}

test_restart_final_status_recorded() {
  local out id record
  out=$("$HOLD_BIN" -d --restart on-failure:1 --restart-delay 0 -- /bin/sh -c 'exit 3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record_ended_soon "$id" || { echo "restart call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"exit_code": 3' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" list -a | grep -F "Exited (3)" >/dev/null || { "$HOLD_BIN" list -a >&2; return 1; }
}

test_end_restart_call_records_term_status() {
  local out id record
  out=$("$HOLD_BIN" -d --restart always --restart-delay 0 -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.3
  "$HOLD_BIN" end "$id" >/dev/null 2>&1 || return 1
  record_ended_soon "$id" || { echo "ended restart call never marked exited" >&2; return 1; }
  record=$(record_path "$id") || return 1
  grep -q '"term_signal": ' "$record" || { cat "$record" >&2; return 1; }
}

test_unwitnessed_death_renders_exited_unknown() {
  local out id record pid ppid tries
  out=$("$HOLD_BIN" -d -- sleep 30 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id") || return 1
  pid=$(sed -n 's/.*"pid": \([0-9]*\).*/\1/p' "$record" | head -n1)
  [ -n "$pid" ] || { cat "$record" >&2; return 1; }
  ppid=$(awk '{print $4}' "/proc/$pid/stat" 2>/dev/null)
  [ -n "$ppid" ] || return 1
  # Kill the waiter first so no final frame is ever written, then the call.
  kill -9 "$ppid" 2>/dev/null || true
  kill -9 "-$pid" 2>/dev/null || true
  for tries in $(seq 1 40); do
    "$HOLD_BIN" list -a | grep -F "Exited (?)" >/dev/null && return 0
    sleep 0.05
  done
  "$HOLD_BIN" list -a >&2
  cat "$record" >&2
  return 1
}

test_docker_restart_validation_and_tty_gate() {
  local rc
  set +e
  "$HOLD_BIN" --restart nonsense /bin/true >/dev/null 2>"$TEST_ROOT/restart-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "invalid restart rc=$rc" >&2; return 1; }
  grep -q 'invalid --restart policy' "$TEST_ROOT/restart-invalid.err" || { cat "$TEST_ROOT/restart-invalid.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" --restart-delay 1 /bin/true >/dev/null 2>"$TEST_ROOT/restart-delay-alone.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "restart-delay without restart rc=$rc" >&2; return 1; }
  grep -q -- '--restart-delay requires --restart' "$TEST_ROOT/restart-delay-alone.err" || { cat "$TEST_ROOT/restart-delay-alone.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" -t --restart always /bin/true >/dev/null 2>"$TEST_ROOT/restart-tty.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "restart tty rc=$rc" >&2; return 1; }
  grep -q -- '--restart with --tty/-t is not supported yet' "$TEST_ROOT/restart-tty.err" || { cat "$TEST_ROOT/restart-tty.err" >&2; return 1; }
}




test_start_existing_run_appends_retained_log() {
  local out id log before_count after_count restart_out
  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo restart-append-marker' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  log=$(log_path "$id") || return 1
  for _ in $(seq 1 30); do
    before_count=$(grep -c '^restart-append-marker$' "$log" 2>/dev/null || true)
    [ "$before_count" -ge 1 ] && break
    sleep 0.1
  done
  [ "${before_count:-0}" -ge 1 ] || { cat "$log" >&2; return 1; }
  restart_out=$("$HOLD_BIN" -d "$id" 2>&1) || {
    printf '%s\n' "$restart_out" >&2
    return 1
  }
  printf '%s\n' "$restart_out" | grep -q "${id:0:12}" || { printf '%s\n' "$restart_out" >&2; return 1; }
  for _ in $(seq 1 30); do
    after_count=$(grep -c '^restart-append-marker$' "$log" 2>/dev/null || true)
    [ "$after_count" -ge 2 ] && break
    sleep 0.1
  done
  [ "${after_count:-0}" -ge 2 ] || { cat "$log" >&2; return 1; }
}


test_docker_publish_and_volume_rejected() {
  local rc
  set +e
  "$HOLD_BIN" -p 8080:3000 -- /bin/true >"$TEST_ROOT/docker-publish.out" 2>"$TEST_ROOT/docker-publish.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/docker-publish.out" "$TEST_ROOT/docker-publish.err" >&2; return 1; }
  grep -q 'does not publish or forward ports' "$TEST_ROOT/docker-publish.err" || { cat "$TEST_ROOT/docker-publish.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" -v "$TEST_ROOT:/work" -- /bin/true >"$TEST_ROOT/docker-volume.out" 2>"$TEST_ROOT/docker-volume.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/docker-volume.out" "$TEST_ROOT/docker-volume.err" >&2; return 1; }
  grep -q 'does not mount or remap volumes' "$TEST_ROOT/docker-volume.err" || { cat "$TEST_ROOT/docker-volume.err" >&2; return 1; }
}


test_docker_interactive_stdin_pipes_to_child() {
  local out id
  out=$(printf 'stdin-i-ok\n' | "$HOLD_BIN" -i -f -- /bin/cat 2>"$TEST_ROOT/docker-i.err") || {
    cat "$TEST_ROOT/docker-i.err" >&2
    return 1
  }
  grep -q '^stdin-i-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }

  out=$(printf 'stdin-no-i\n' | "$HOLD_BIN" -f -- /bin/cat 2>"$TEST_ROOT/docker-no-i.err") || {
    cat "$TEST_ROOT/docker-no-i.err" >&2
    return 1
  }
  if grep -q '^stdin-no-i$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_tty_foreground_attaches_and_detaches() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc
  cat >"$TEST_ROOT/docker-tty-child.sh" <<'EOF'
#!/bin/sh
IFS= read line
printf 'tty:%s\n' "$line"
sleep 30
EOF
  chmod +x "$TEST_ROOT/docker-tty-child.sh"
  set +e
  python3 - <<'PY' | script -qfec "$HOLD_BIN -it $TEST_ROOT/docker-tty-child.sh" /dev/null >"$TEST_ROOT/docker-tty.out" 2>"$TEST_ROOT/docker-tty.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"hello-tty\n")
out.flush()
time.sleep(0.5)
out.write(b"\x10\x11")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it rc=$rc" >&2; cat "$TEST_ROOT/docker-tty.out" "$TEST_ROOT/docker-tty.err" >&2; return 1; }
  grep -q 'tty:hello-tty' "$TEST_ROOT/docker-tty.out" || { cat "$TEST_ROOT/docker-tty.out" >&2; return 1; }
  # Docker parity: foreground -it prints no id; the detached run is found via listing.
  # The lone running call is the detached -it child; take the first table row's
  # CALL ID (the Docker-shaped COMMAND column ellipsizes the long temp path).
  id=$("$HOLD_BIN" list | awk 'NR==2 {print $1; exit}')
  [ -n "$id" ] || { "$HOLD_BIN" list >&2; return 1; }
  record=$(record_path "$id") || { find "$HOME/.local/state/hold" -maxdepth 1 -type f -print >&2 || true; return 1; }
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+.*Up " "$TEST_ROOT/docker-tty-ps.out" || { cat "$TEST_ROOT/docker-tty-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_docker_tty_custom_detach_keys() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc
  cat >"$TEST_ROOT/docker-tty-custom-child.sh" <<'EOF'
#!/bin/sh
printf 'custom-ready\n'
IFS= read line
printf 'custom-line:%s\n' "$line"
sleep 30
EOF
  chmod +x "$TEST_ROOT/docker-tty-custom-child.sh"
  set +e
  python3 - <<'PY' | script -qfec "$HOLD_BIN -it --detach-keys ctrl-a $TEST_ROOT/docker-tty-custom-child.sh" /dev/null >"$TEST_ROOT/docker-tty-custom.out" 2>"$TEST_ROOT/docker-tty-custom.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"\x01")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it custom detach rc=$rc" >&2; cat "$TEST_ROOT/docker-tty-custom.out" "$TEST_ROOT/docker-tty-custom.err" >&2; return 1; }
  grep -q 'custom-ready' "$TEST_ROOT/docker-tty-custom.out" || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  ! grep -q 'custom-line:' "$TEST_ROOT/docker-tty-custom.out" || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  # Docker parity: foreground -it prints no id; the detached run is found via listing.
  # The lone running call is the detached -it child; take the first table row's
  # CALL ID (the Docker-shaped COMMAND column ellipsizes the long temp path).
  id=$("$HOLD_BIN" list | awk 'NR==2 {print $1; exit}')
  [ -n "$id" ] || { "$HOLD_BIN" list >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-custom-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+.*Up " "$TEST_ROOT/docker-tty-custom-ps.out" || { cat "$TEST_ROOT/docker-tty-custom-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}




test_hold_shell_runs_real_shell_without_creating_runid_on_exit() {
  local before after rc
  "$HOLD_BIN" help shell >"$TEST_ROOT/hold-shell-help.out" || return 1
  grep -q "PTY/session wrapper" "$TEST_ROOT/hold-shell-help.out" || { cat "$TEST_ROOT/hold-shell-help.out" >&2; return 1; }
  grep -q 'Ctrl-P Ctrl-Q' "$TEST_ROOT/hold-shell-help.out" || { cat "$TEST_ROOT/hold-shell-help.out" >&2; return 1; }
  ! grep -q 'not implemented' "$TEST_ROOT/hold-shell-help.out" || { cat "$TEST_ROOT/hold-shell-help.out" >&2; return 1; }
  ! grep -q '/profiles' "$TEST_ROOT/hold-shell-help.out" || { cat "$TEST_ROOT/hold-shell-help.out" >&2; return 1; }

  before=$( { find "$HOME/.local/state/hold/runs" -name '*.json' 2>/dev/null || true; } | wc -l)
  set +e; printf 'echo hold-shell-ok\nexit\n' | SHELL=/bin/sh "$HOLD_BIN" shell >"$TEST_ROOT/hold-shell.out" 2>"$TEST_ROOT/hold-shell.err"; rc=$?; set -e
  [ "$rc" -eq 0 ] || { echo "hold shell: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/hold-shell.out" "$TEST_ROOT/hold-shell.err" >&2; return 1; }
  grep -q 'hold-shell-ok' "$TEST_ROOT/hold-shell.out" || { cat "$TEST_ROOT/hold-shell.out" "$TEST_ROOT/hold-shell.err" >&2; return 1; }
  ! grep -q 'not implemented' "$TEST_ROOT/hold-shell.err" || { cat "$TEST_ROOT/hold-shell.err" >&2; return 1; }
  after=$( { find "$HOME/.local/state/hold/runs" -name '*.json' 2>/dev/null || true; } | wc -l)
  [ "$before" = "$after" ] || { echo "hold shell created a runid on normal exit: before=$before after=$after" >&2; return 1; }
  ! grep -q 'hold>' "$TEST_ROOT/hold-shell.out" || { cat "$TEST_ROOT/hold-shell.out" >&2; return 1; }
  ! grep -q 'NAME' "$TEST_ROOT/hold-shell.out" || { cat "$TEST_ROOT/hold-shell.out" >&2; return 1; }
}


test_hold_shell_adopt_normalizes_relative_foreground_argv_paths() {
  [ "$(uname -s)" = "Linux" ] || skip "foreground PGID adoption currently implemented on Linux"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc record script
  script="$TEST_ROOT/shell-rel.sh"
  cat >"$script" <<'SH'
#!/bin/sh
echo shell-relative-started
sleep 30
SH
  chmod +x "$script"
  set +e
  python3 - "$TEST_ROOT" <<'PY' | SHELL=/bin/sh "$HOLD_BIN" shell >"$TEST_ROOT/hold-shell-rel-adopt.out" 2>"$TEST_ROOT/hold-shell-rel-adopt.err"
import sys, time
root = sys.argv[1]
out = sys.stdout.buffer
out.write(f"cd {root}\n/bin/sh ./shell-rel.sh\n".encode())
out.flush()
time.sleep(0.5)
out.write(b"\x10\x11")
out.flush()
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "hold shell relative detach: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/hold-shell-rel-adopt.out" "$TEST_ROOT/hold-shell-rel-adopt.err" >&2; return 1; }
  id=$(extract_id <"$TEST_ROOT/hold-shell-rel-adopt.out")
  [ -n "$id" ] || { cat "$TEST_ROOT/hold-shell-rel-adopt.out" "$TEST_ROOT/hold-shell-rel-adopt.err" >&2; return 1; }
  record=$(record_path "$id") || true
  [ -f "$record" ] || { find "$HOME/.local/state/hold" -maxdepth 1 -type f -print >&2 || true; return 1; }
  if ! python3 - "$record" "$script" <<'PY'
import json, sys
record = json.load(open(sys.argv[1], encoding='utf-8'))
argv = record.get('argv') or []
normalized = (record.get('normalized') or {}).get('argv') or []
observed = record.get('observed') or {}
observed_argv = observed.get('argv') or []
if len(argv) < 2 or argv[1] != sys.argv[2]:
    raise SystemExit(f'adopted shell record did not normalize relative foreground argv path: {argv!r}')
if len(normalized) < 2 or normalized[1] != sys.argv[2]:
    raise SystemExit(f'adopted shell normalized argv mismatch: {normalized!r}')
if len(observed_argv) < 2 or observed_argv[1] != './shell-rel.sh':
    raise SystemExit(f'adopted shell observed argv did not preserve captured relative path: {observed_argv!r}')
if observed.get('cwd') != __import__('os').path.dirname(sys.argv[2]):
    raise SystemExit(f'adopted shell observed cwd mismatch: {observed.get("cwd")!r}')
if not (observed.get('exe') or '').startswith('/'):
    raise SystemExit(f'adopted shell observed exe is not absolute: {observed.get("exe")!r}')
PY
  then
    cat "$record" >&2
    return 1
  fi
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_hold_shell_detach_adopts_foreground_process_group() {
  [ "$(uname -s)" = "Linux" ] || skip "foreground PGID adoption currently implemented on Linux"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local id rc record
  set +e
  python3 - <<'PY' | SHELL=/bin/sh "$HOLD_BIN" shell >"$TEST_ROOT/hold-shell-adopt.out" 2>"$TEST_ROOT/hold-shell-adopt.err"
import sys, time
out = sys.stdout.buffer
out.write(b"sleep 30\n")
out.flush()
time.sleep(1.2)
out.write(b"\x10\x11")
out.flush()
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "hold shell detach: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/hold-shell-adopt.out" "$TEST_ROOT/hold-shell-adopt.err" >&2; return 1; }
  id=$(sed -n \
    -e '/^[0-9a-f]\{12\}$/p' \
    -e 's/^hold: id=\([0-9a-f][0-9a-f]*\).*/\1/p' \
    -e 's/.*[^0-9a-f]\([0-9a-f]\{12\}\)[^0-9a-f]*$/\1/p' \
    "$TEST_ROOT/hold-shell-adopt.out" | head -n1)
  [ -n "$id" ] || { cat "$TEST_ROOT/hold-shell-adopt.out" "$TEST_ROOT/hold-shell-adopt.err" >&2; return 1; }
  record=$(record_path "$id") || true
  [ -f "$record" ] || { find "$HOME/.local/state/hold" -maxdepth 1 -type f -print >&2 || true; return 1; }
  grep -Fq '"argv": ["/usr/bin/sleep", "30"]' "$record" || { cat "$record" >&2; return 1; }
  grep -Fq 'hold  adopted' "$TEST_ROOT/hold-shell-adopt.out" ||
    grep -Fq 'hold  adopted' "$TEST_ROOT/hold-shell-adopt.err" ||
    { cat "$TEST_ROOT/hold-shell-adopt.out" "$TEST_ROOT/hold-shell-adopt.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}













test_build_artifact_coexistence() {
  make clean >/dev/null || return 1
  make hold hold STATIC_LDFLAGS= EXTRA_CPPFLAGS=-DHOLD_TESTING >/dev/null || return 1
  [ -x ./hold ] && [ -x ./hold ] || return 1
  make hold-dynamic EXTRA_CPPFLAGS=-DHOLD_TESTING >/dev/null || return 1
  [ -x ./hold ] && [ -x ./hold ] && [ -x ./hold-dynamic ] || return 1
  [ -e ./hold ] && [ -e ./hold ] && [ -e ./hold-dynamic ]
}

test_concurrent_unique_ids() {
  local i id ids uniq
  ids=""
  for i in $(seq 1 20); do
    "$HOLD_BIN" -d sleep 60 >"$TEST_ROOT/start.$i.out" 2>"$TEST_ROOT/start.$i.err" &
  done
  wait
  for i in $(seq 1 20); do
    id=$(extract_id <"$TEST_ROOT/start.$i.out")
    [ -n "$id" ] || return 1
    ids="$ids\n$id"
  done
  uniq=$(printf '%b\n' "$ids" | sed '/^$/d' | sort -u | wc -l)
  [ "$uniq" -eq 20 ] || return 1
  for id in $(printf '%b\n' "$ids" | sed '/^$/d'); do
    "$HOLD_BIN" kill "$id" >/dev/null 2>&1 || true
  done
  return 0
}

set -e
# ---------------------------------------------------------------------------
# Security-invariant enforcement tests (audit hardening). Each fails loudly if
# the corresponding privilege/privacy invariant regresses, rather than only
# exercising the happy path.
# ---------------------------------------------------------------------------

# Run the binary as the unprivileged user with spoofed sudo provenance injected
# AFTER any privilege drop, so even the root-runner can test the euid gate.
as_user_spoof_sudo() {
  local args
  args=(
    "HOME=$ACTOR_HOME"
    "HOLD_TEST_SYSTEM_STATE_DIR=$HOLD_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
    "SUDO_UID=0" "SUDO_GID=0" "SUDO_USER=root"
  )
  [ "${HOLD_BOOT_ID_PATH+x}" = x ] && args+=("HOLD_BOOT_ID_PATH=$HOLD_BOOT_ID_PATH")
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${args[@]}" "$@"
  else
    env "${args[@]}" "$@"
  fi
}

root_safe_hold_copy() {
  local name="$1" dir path
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 1
  ROOT_SAFE_HOLD_DIR="/usr/local/lib/hold-test-$$"
  dir="$ROOT_SAFE_HOLD_DIR/$(basename "$TEST_ROOT")"
  path="$dir/$name"
  as_root mkdir -p "$dir" || return 1
  as_root chown 0:0 "$ROOT_SAFE_HOLD_DIR" "$dir" || return 1
  as_root chmod 755 "$ROOT_SAFE_HOLD_DIR" "$dir" || return 1
  as_root cp "$HOLD_REAL_BIN" "$path" || return 1
  as_root chown 0:0 "$path" || return 1
  as_root chmod 755 "$path" || return 1
  printf '%s\n' "$path"
}

test_system_store_directory_modes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id d mode
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  for d in runs logs console; do
    mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/$d") || return 1
    [ "$mode" = 700 ] || { echo "$d/ mode=$mode (want 700 -- private state would be world-visible)" >&2; return 1; }
  done
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR") || return 1
  [ "$mode" = 755 ] || { echo "base mode=$mode (want 755)" >&2; return 1; }
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/public") || return 1
  [ "$mode" = 755 ]
}

test_system_store_tightens_preexisting_loose_dir() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id mode
  as_root mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  as_root chmod 0777 "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || return 1
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  mode=$(root_file_mode "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  [ "$mode" = 700 ] || { echo "pre-existing loose runs/ not tightened: mode=$mode" >&2; return 1; }
}

test_system_store_refuses_symlinked_critical_dirs() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local old_store name root state target rc mode
  old_store="$HOLD_TEST_SYSTEM_STATE_DIR"
  for name in base runs logs console public; do
    root="$TEST_ROOT/syslink-$name"
    state="$root/state"
    target="$root/target"
    as_root mkdir -p "$root" "$target" || return 1
    as_root chmod 0777 "$target" || return 1
    if [ "$name" = base ]; then
      as_root rm -rf "$state" || return 1
      as_root ln -s "$target" "$state" || return 1
    else
      as_root mkdir -p "$state" || return 1
      case "$name" in
        public) as_root mkdir -p "$state/runs" "$state/logs" "$state/console" || return 1 ;;
        console) as_root mkdir -p "$state/runs" "$state/logs" || return 1 ;;
        logs) as_root mkdir -p "$state/runs" || return 1 ;;
      esac
      as_root ln -s "$target" "$state/$name" || return 1
    fi
    HOLD_TEST_SYSTEM_STATE_DIR="$state"
    export HOLD_TEST_SYSTEM_STATE_DIR
    set +e
    as_root "$HOLD_REAL_BIN" -d true >/dev/null 2>"$TEST_ROOT/syslink-$name.err"
    rc=$?
    set -e
    HOLD_TEST_SYSTEM_STATE_DIR="$old_store"
    export HOLD_TEST_SYSTEM_STATE_DIR
    [ "$rc" -ne 0 ] || { echo "system store accepted symlinked $name directory" >&2; return 1; }
    mode=$(root_file_mode "$target") || return 1
    [ "$mode" = 777 ] || { echo "symlink target for $name was chmod-followed: mode=$mode" >&2; return 1; }
  done
}

test_system_store_artifacts_owned_by_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id owner record log
  id=$(as_root "$HOLD_REAL_BIN" -d true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  record=$(root_record_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  log=$(root_log_path "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  owner=$(root_file_owner "$record") || return 1
  [ "$owner" = "0:0" ] || { echo "record owner=$owner (want 0:0)" >&2; return 1; }
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "0:0" ] || { echo "log owner=$owner (want 0:0)" >&2; return 1; }
}

test_nonroot_ignores_spoofed_sudo_provenance() {
  local out id json
  out=$(as_user_spoof_sudo "$HOLD_REAL_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$ACTOR_HOME/.local/state/hold") || true
  [ -e "$json" ] || { echo "spoofed-SUDO run did not land in the user store" >&2; return 1; }
  ! root_record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs" || { echo "spoofed SUDO_* escalated into the system store" >&2; return 1; }
  if grep -q '"invoked_via_sudo": true' "$json" 2>/dev/null; then
    echo "spoofed SUDO_* was trusted by a non-root process" >&2
    return 1
  fi
}



test_signal_refuses_tampered_pgid() {
  local out id rec rc
  out=$("$HOLD_BIN" -d /bin/sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rec=$(record_path "$id") || return 1
  # A record whose pgid is tampered to <=1 must never be signaled -- kill(-pgid)
  # with pgid<=1 would hit pid 1 / the whole session. valid_record rejects it, so
  # stop refuses with exit 5 and the real process is left untouched.
  sed -i.bak 's/"pgid":[[:space:]]*[0-9][0-9]*/"pgid":1/' "$rec" || return 1
  set +e; "$HOLD_BIN" stop "$id" >/dev/null 2>&1; rc=$?; set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$rc" -eq 5 ] || { echo "stop on tampered pgid<=1: rc=$rc (want 5 refused)" >&2; return 1; }
}

test_public_index_write_rollback() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local rc pids leftover
  set +e
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE=1 as_root "$HOLD_REAL_BIN" -d bash -c 'exec -a hold_pubidx_test sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "public-index rollback start: rc=$rc (want 1)" >&2; return 1; }
  # The rollback SIGKILLs the group; poll for its disappearance instead of
  # sampling one instant. A genuine leak survives the whole window.
  local rb_tries=0
  while :; do
    pids=$(ps -eo pid=,args= | awk '/hold_pubidx_test/ && !/awk/ {print $1}')
    [ -z "$pids" ] && break
    rb_tries=$((rb_tries + 1))
    [ "$rb_tries" -lt 20 ] || { echo "leaked process after public-index rollback" >&2; return 1; }
    sleep 0.1
  done
  leftover=$(as_root sh -c 'ls "$1"/runs/*.json "$1"/public/*.json 2>/dev/null' sh "$HOLD_TEST_SYSTEM_STATE_DIR" 2>/dev/null || true)
  [ -z "$leftover" ] || { echo "leftover state after rollback: $leftover" >&2; return 1; }
}

test_quiet_suppresses_banner_keeps_id() {
  local out id
  out=$("$HOLD_BIN" --quiet -d /bin/sleep 300 2>"$TEST_ROOT/q.err") || return 1
  id=$(printf '%s\n' "$out" | sed -n '/^[0-9a-f]\{64\}$/p')
  [ -n "$id" ] || { echo "no id from --quiet start" >&2; return 1; }
  [ "$out" = "$id" ] || { echo "--quiet stdout is not the bare 64-hex id: [$out]" >&2; return 1; }
  [ ! -s "$TEST_ROOT/q.err" ] || { echo "--quiet start wrote to stderr:" >&2; cat "$TEST_ROOT/q.err" >&2; return 1; }
  "$HOLD_BIN" --quiet stop "$id" >/dev/null 2>"$TEST_ROOT/q2.err" || return 1
  [ ! -s "$TEST_ROOT/q2.err" ] || { echo "--quiet stop wrote to stderr" >&2; return 1; }
}

test_run_id_prefix_resolution() {
  local out id pgid pfx got
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  pfx=$(printf '%s' "$id" | cut -c1-4)
  got=$("$HOLD_BIN" stop --print "$pfx") || { echo "id-prefix did not resolve" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { echo "prefix resolved to wrong run: [$got]" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
}


test_misc_action_guards() {
  local rc
  set +e; "$HOLD_BIN" --console end deadbeef >/dev/null 2>"$TEST_ROOT/c.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--console end: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'only when launching a call' "$TEST_ROOT/c.err" || { cat "$TEST_ROOT/c.err" >&2; return 1; }
  set +e; "$HOLD_BIN" help bogustopic >/dev/null 2>"$TEST_ROOT/h.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "help bogustopic: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'unknown help topic' "$TEST_ROOT/h.err" || { cat "$TEST_ROOT/h.err" >&2; return 1; }
  # -l is now the --live filter, not the legacy --iso surface: list renders only
  # the Docker-shaped table, never the old STARTED_AT ISO header.
  "$HOLD_BIN" list -l >"$TEST_ROOT/l.out" 2>&1 || true
  ! grep -q 'STARTED_AT' "$TEST_ROOT/l.out" || { echo "list still emits the legacy ISO header" >&2; cat "$TEST_ROOT/l.out" >&2; return 1; }
}


test_owned_command_exact_arity() {
  local rc
  set +e; "$HOLD_BIN" tail deadbeef extra >/dev/null 2>"$TEST_ROOT/tail-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold tail <target>' "$TEST_ROOT/tail-extra.err" || { cat "$TEST_ROOT/tail-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" console deadbeef extra >/dev/null 2>"$TEST_ROOT/console-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "console extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold console <target>' "$TEST_ROOT/console-extra.err" || { cat "$TEST_ROOT/console-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" purge deadbeef extra >/dev/null 2>"$TEST_ROOT/prune-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "purge extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold purge' "$TEST_ROOT/prune-extra.err" || { cat "$TEST_ROOT/prune-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" tail --all deadbeef >/dev/null 2>"$TEST_ROOT/tail-all.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail --all: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold tail <target>' "$TEST_ROOT/tail-all.err" || { cat "$TEST_ROOT/tail-all.err" >&2; return 1; }

  "$HOLD_BIN" prune >/dev/null || { echo "prune with zero targets should remain valid" >&2; return 1; }
}



test_print_over_all_and_multiple() {
  local id1 id2 pgid1 pgid2 out
  id1=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  id2=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  pgid1=$(record_pgid "$id1"); pgid2=$(record_pgid "$id2")
  [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  out=$("$HOLD_BIN" stop --print "$id1" "$id2") || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" && printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "explicit multi --print missing a pgid" >&2; return 1; }
  "$HOLD_BIN" stop "$id1" "$id2" >/dev/null 2>&1 || true
}

run_test "--print spans --all and multiple explicit targets" test_print_over_all_and_multiple
run_test "stop refuses a record with a tampered pgid<=1 (exit 5)" test_signal_refuses_tampered_pgid
run_test "public-index write failure rolls back the start" test_public_index_write_rollback
run_test "--quiet prints bare id and silences stderr" test_quiet_suppresses_banner_keeps_id
run_test "run id prefix resolves to the full run" test_run_id_prefix_resolution
run_test "action/help/list argument guards" test_misc_action_guards
run_test "owned commands reject extra targets and unsupported --all" test_owned_command_exact_arity
run_test "system store directory modes are private (0700/0755)" test_system_store_directory_modes
run_test "system store tightens a pre-existing loose dir" test_system_store_tightens_preexisting_loose_dir
run_test "system store refuses symlinked critical dirs without chmod-following" test_system_store_refuses_symlinked_critical_dirs
run_test "system store artifacts are owned by root:root" test_system_store_artifacts_owned_by_root
run_test "non-root process ignores spoofed SUDO_* provenance" test_nonroot_ignores_spoofed_sudo_provenance
run_test "start/stop lifecycle" test_lifecycle
run_test "kill subcommand kills process group" test_kill_subcommand
run_test "stop kills full process group (children)" test_group_kill_children
run_test "exec failure creates no record" test_exec_failure_no_record
run_test "fast exit command is recorded as exited" test_fast_exit_record_exited
run_test "exec replacement remains controllable" test_exec_replacement_remains_controllable
run_test "corrupt record warning and prune cleanup" test_corrupt_record_handling
run_test "deeply nested/corrupt JSON record is rejected, not crashed" test_deep_json_record_rejected_not_crashed
run_test "symlinked record is rejected" test_symlinked_record_rejected
run_test "symlinked log is rejected" test_symlinked_log_rejected
run_test "invalid pgid=0 record is not listed as running" test_invalid_pgid_record
run_test "unreferenced logs are removed by prune" test_orphan_log_cleanup
run_test "ID input sanitization rejects invalid ids" test_id_sanitization
run_test "stop/kill --print emits group signal command" test_print_signal_output
run_test "signal refuses tampered live process-group identity" test_signal_refuses_tampered_live_group_identity
run_test "stop supports multiple IDs in one command" test_stop_multiple_ids
run_test "argument edge cases" test_argument_edges
run_test "Docker-shaped run/logs/ps/rm surface" test_docker_shaped_cli_flags_and_rm
run_test "ps table humanizes CREATED, phrases STATUS, and marks saved" test_ps_table_humanized_status_and_saved
run_test "ps table content-sized columns do not shear" test_ps_table_columns_do_not_shear
run_test "ports lists a call's bound listeners" test_ports_lists_bound_listener
run_test "stats single-shot prints the documented columns" test_stats_single_shot_shape
run_test "inspect reports live stdio fd targets" test_inspect_reports_stdio_targets
run_test "bare foreground streams output; -d detaches with a 64-hex id" test_docker_bare_launch_foreground_follows_output_by_default
run_test "Docker run foreground follows output by default" test_docker_run_foreground_follows_output_by_default
run_test "unsupported Docker-shaped options fail loudly" test_docker_unsupported_options_fail_loudly
run_test "Docker restart policy restarts failed processes" test_docker_restart_policy_restarts_failures
run_test "foreground launch propagates the exit code" test_docker_foreground_propagates_exit_code
run_test "restart call records its final exit status" test_restart_final_status_recorded
run_test "ending a restart call records the term status" test_end_restart_call_records_term_status
run_test "unwitnessed death renders Exited (?)" test_unwitnessed_death_renders_exited_unknown
run_test "Docker restart validates policies and tty gate" test_docker_restart_validation_and_tty_gate
run_test "start existing run appends retained log" test_start_existing_run_appends_retained_log
run_test "Docker publish and volume flags are rejected" test_docker_publish_and_volume_rejected
run_test "Docker -i keeps non-PTY stdin open" test_docker_interactive_stdin_pipes_to_child
run_test "Docker -it foreground attaches and detaches" test_docker_tty_foreground_attaches_and_detaches
run_test "Docker --detach-keys changes TTY detach sequence" test_docker_tty_custom_detach_keys
run_test "hold shell runs a real shell and normal exit creates no runid" test_hold_shell_runs_real_shell_without_creating_runid_on_exit
run_test "hold shell adopt normalizes relative foreground argv paths" test_hold_shell_adopt_normalizes_relative_foreground_argv_paths
run_test "hold shell Ctrl-P Ctrl-Q adopts the foreground process group" test_hold_shell_detach_adopts_foreground_process_group
run_test "special characters are preserved in argv JSON" test_special_chars_args
run_test "logging captures stdout+stderr" test_log_capture
run_test "internal viewer harness seeds literal and similarity filters" test_log_view_internal_seed_filters
run_test "internal viewer follows live logs through filter engine" test_log_view_follow_filters_live_output
run_test "logs follow opens dynamic TTY filter" test_hold_logs_follow_opens_dynamic_tty_filter
run_test "internal viewer follow filters dynamically from typed TTY input" test_log_view_follow_dynamic_tty_filter
run_test "log viewer renders polished header footer timestamp and source chrome" test_log_viewer_integrated_chrome_help_and_info
run_test "log viewer space excludes similar lines and Ctrl-R resets filters" test_log_viewer_space_excludes_and_ctrl_r_resets_filters
run_test "internal viewer selection movement uses cached filter rows" test_log_view_selection_uses_cached_rows
run_test "internal viewer follow pages older and newer filtered windows" test_log_view_follow_pages_filtered_windows
run_test "internal viewer follow page-up stays at the first log page" test_log_view_follow_page_up_stays_at_start
run_test "internal viewer follow oldest page does not wrap back to tail" test_log_view_follow_oldest_page_does_not_wrap_to_tail
run_test "internal viewer follow arrow-up to top does not wrap to tail" test_log_view_follow_arrow_up_to_top_does_not_wrap_to_tail
run_test "internal viewer top navigation is idempotent after reaching oldest page" test_log_view_follow_top_navigation_is_idempotent
run_test "internal viewer repeated top commands ignore live growth" test_log_view_follow_repeated_top_commands_ignore_live_growth
run_test "internal viewer Home key pins oldest page without tail loop" test_log_view_home_key_pins_oldest_page_without_tail_loop
run_test "internal viewer modified Home key pins oldest page without tail loop" test_log_view_modified_home_key_pins_oldest_page_without_tail_loop
run_test "internal viewer modified PageUp reaches oldest page without tail loop" test_log_view_modified_page_up_reaches_oldest_page_without_tail_loop
run_test "internal viewer follow page-down from top does not wrap to tail" test_log_view_follow_page_down_from_top_does_not_wrap_to_tail
run_test "internal viewer follow page-down stops on the last real page" test_log_view_follow_page_down_stops_on_last_real_page
run_test "internal viewer follow page-up returns to top after paging down" test_log_view_follow_page_up_after_top_page_down_returns_to_top
run_test "internal viewer filter changes preserve browsed-away page" test_log_view_follow_filter_change_preserves_browsed_page
run_test "internal viewer exclude changes preserve browsed-away page" test_log_view_follow_exclude_preserves_browsed_page
run_test "internal viewer exclude at live edge pins the current page" test_log_view_follow_exclude_at_live_edge_pins_current_page
run_test "internal viewer follow keeps top pinned while live log grows" test_log_view_follow_top_refills_as_live_log_grows
run_test "internal viewer cursor navigation disables live tail yank" test_log_view_follow_cursor_navigation_disables_tail_yank
run_test "internal viewer cursor browsing keeps a short top page pinned" test_log_view_follow_cursor_browse_keeps_short_top_page_pinned
run_test "internal viewer follow page-up stops at first filtered match" test_log_view_follow_page_up_stops_at_first_filtered_match
run_test "internal viewer printable f starts the dynamic filter" test_log_view_printable_f_starts_dynamic_filter
run_test "internal viewer follow page-up still works after the run exits" test_log_view_follow_exited_page_up_keeps_backward_navigation
run_test "internal viewer follow marks newer data while browsed away" test_log_view_follow_browsed_away_marks_newer_without_yank
run_test "internal viewer follow ignores nonmatching newer data while browsed away" test_log_view_follow_ignores_nonmatching_newer_data
run_test "internal viewer follow finds sparse newer match after large burst" test_log_view_follow_finds_sparse_newer_match_after_large_burst
run_test "-f starts and follows output" test_start_follow_short_form
run_test "tail <id> tails an existing run log" test_tail_verb_existing_id
run_test "persistent stale records remain visible and dumpable" test_persistent_stale_records
run_test "missing boot source does not force stale" test_boot_unavailable_does_not_force_stale
run_test "leader zombie with live group remains running" test_leader_zombie_group_still_running
run_test "tail <id> prints finished log output" test_tail_finished_log_prints_existing_output
run_test "--console works without an external attach tool" test_console_does_not_require_external_attach_tool
run_test "console reports a normal run has no console" test_console_reports_non_console_run
run_test "console attach round-trips and tees to the log" test_console_round_trip_and_log_tee
run_test "console exit code is recorded" test_console_exit_code_is_recorded
run_test "end on a console call signals the held group, not the broker" test_end_console_call_signals_target
run_test "end on a console call escalates TERM to KILL" test_end_console_call_escalates_to_kill
run_test "SIGTERM to the console broker forwards to the held group" test_console_broker_term_forwards_to_target
run_test "console rejects unrelated peer UID before replay" test_console_rejects_unrelated_peer_uid_before_replay
run_test "console can reattach after detach" test_console_can_reattach_after_detach
run_test "console socket lives in store dir, not /tmp, for long paths" test_console_socket_lives_in_store_dir
run_test "console target runs in caller cwd (relative-bind restores cwd)" test_console_target_runs_in_caller_cwd
run_test "prune <id> removes exactly one run record/output" test_prune_by_id
run_test "prune all removes prunable while preserving running" test_prune_all_keeps_running
run_test "save marks the record and is idempotent" test_save_marks_record_and_is_idempotent
run_test "sweeping purge skips saved calls" test_purge_sweep_skips_saved
run_test "targeted purge of a saved call refuses with exact wording" test_purge_targeted_refuses_saved_exact_wording
run_test "purge --force removes a saved ended call" test_purge_force_removes_saved_ended
run_test "purge --force removes a live saved call" test_purge_force_removes_live_saved
run_test "rename of a saved call keeps purge protection" test_rename_of_saved_call_keeps_protection
run_test "rename saves an unsaved call" test_rename_saves_an_unsaved_call
run_test "redial honors a recorded foreground recipe (and -d overrides)" test_redial_honors_recorded_foreground_mode
run_test "redial honors a recorded detached recipe" test_redial_honors_recorded_detached_mode
run_test "redial honors a recorded console recipe" test_redial_honors_recorded_console_mode
run_test "transactional launch rollback on record write failure" test_transactional_record_write_failure
run_test "raw start does not steal trailing --system" test_raw_start_does_not_steal_trailing_system
run_test "long command appears in list, truncated with ..." test_long_command_list_truncates_instead_of_skips
run_test "normal start writes user-local state" test_normal_start_writes_user_local_state
run_test "Docker-shaped run record is readable without duplicate legacy argv fields" test_docker_shaped_record_fallback_reader
run_test "root starts use system store and public state is projected" test_root_start_writes_system_store_and_public_unknown
run_test "sudo start writes system state with invoking-user metadata" test_sudo_start_writes_system_store_with_invoking_metadata
run_test "sudo --system home executable uses invoking-user store" test_sudo_system_start_of_home_executable_uses_user_store
run_test "sudo --system home argv paths use invoking-user store" test_sudo_system_start_of_home_argv_paths_uses_user_store
run_test "sudo context can stop unique invoking-user local run" test_sudo_context_can_stop_unique_user_local_run
run_test "public root index rows are redacted in normal list" test_public_root_index_list_is_redacted
run_test "public root index list reads projected State" test_public_root_index_list_reads_projected_state
run_test "list is the ledger with a USER column; -l/--live narrows to running" test_list_default_is_full_ledger_live_narrows
run_test "list <name> filters the ledger by the call's name" test_list_name_filter_matches_call_name
run_test "list -a renders the projected global ports" test_list_all_renders_global_ports_projection
run_test "list -s/--system shows the redacted global view only" test_system_list_is_redacted_global_only
run_test "ps is Docker's machine-wide running view" test_ps_is_docker_machine_wide_running_view
run_test "purge -s re-execs the global sweep through sudo" test_purge_system_reexecs_through_sudo
run_test "root list -a labels owners and walks user homes" test_root_list_all_labels_owner_and_walks_homes
run_test "normal run does not self-elevate on local/root ID conflict" test_user_local_wins_over_public_root_collision
run_test "explicit user:<id> targets user-local run" test_explicit_user_target
run_test "root capability drop-all then add applies requested cap" test_root_capability_drop_all_then_add_preserves_added_cap
run_test "direct capability metadata projects to inspect" test_direct_capability_metadata_projects_to_inspect
run_test "restart worker applies capability metadata" test_restart_worker_applies_capability_metadata
run_test "Docker --rm removes run artifacts after exit" test_docker_rm_removes_run_artifacts_after_exit
run_test "env flags are recipe data, not hold's own environment" test_env_flag_is_recipe_data_only
run_test "tail Ctrl-C detaches from tail and does not stop run" test_tail_ctrl_c_detaches_from_tail_and_keeps_run
run_test "build artifacts for static and dynamic coexist" test_build_artifact_coexistence
run_test "concurrent starts produce unique ids" test_concurrent_unique_ids

echo "----------------------------------------------------------------"
echo "summary: $PASSES passed, $FAILS failed, $SKIPS skipped"
if [ "$SKIPS" -ne 0 ]; then
  echo "  (some tests skipped; run as root and/or set HOLD_REQUIRE_ROOT_TESTS=1 for full coverage)"
fi
if [ "$FAILS" -ne 0 ]; then
  exit 1
fi
