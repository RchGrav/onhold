#!/usr/bin/env bash
set -Eeuo pipefail

SIGMUND_REAL_BIN="${SIGMUND_BIN:-./sigmund}"
case "$SIGMUND_REAL_BIN" in
  /*) ;;
  *) SIGMUND_REAL_BIN="$PWD/$SIGMUND_REAL_BIN" ;;
esac

FAILS=0
SUITE_ROOT="$(mktemp -d)"
chmod 755 "$SUITE_ROOT"
TEST_ROOT=""
HOME=""
SIGMUND_TEST_SYSTEM_STATE_DIR=""
ROOT_HOME=""
ACTOR_HOME=""
TEST_USER=""
TEST_UID=""
TEST_GID=""
USER_ACTOR_NEEDS_SUDO=0
ROOT_ACTOR_AVAILABLE=0
SUDO_BIN="$(command -v sudo || true)"
USER_CREATED=0
SIGMUND_TEST_TIMEOUT="${SIGMUND_TEST_TIMEOUT:-25}"

PASSES=0
SKIPS=0
pass() { echo "PASS: $1"; PASSES=$((PASSES + 1)); }
fail() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }
skip_note() { echo "SKIP: $1"; SKIPS=$((SKIPS + 1)); }
# Call `skip "<reason>"` from inside a test to report SKIP instead of a vacuous
# PASS. It returns sentinel 77, which run_test maps to SKIP (or to FAIL when
# SIGMUND_REQUIRE_ROOT_TESTS=1, so CI cannot silently no-op the root lane).
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
  case "$SIGMUND_TEST_TIMEOUT" in
    ''|*[!0-9]*|0)
      echo "FATAL: SIGMUND_TEST_TIMEOUT must be a positive integer number of seconds" >&2
      exit 1
      ;;
  esac
}

suite_cleanup() {
  set +e
  if [ "$USER_CREATED" -eq 1 ] && [ -n "$TEST_USER" ]; then
    userdel "$TEST_USER" >/dev/null 2>&1 || true
  fi
  remove_tree "$SUITE_ROOT"
}
trap suite_cleanup EXIT

remove_tree() {
  local path="$1"
  [ -n "$path" ] || return 0
  if [ "$(id -u)" -eq 0 ]; then
    rm -rf "$path"
  elif [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -n "$SUDO_BIN" ]; then
    "$SUDO_BIN" -n rm -rf "$path" || rm -rf "$path"
  else
    rm -rf "$path"
  fi
}

setup_suite_actors() {
  if [ "$(id -u)" -eq 0 ]; then
    USER_ACTOR_NEEDS_SUDO=1
    ROOT_ACTOR_AVAILABLE=1
    TEST_USER="sigmundtest_$$"
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
  export SIGMUND_REAL_BIN SUDO_BIN TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE
}

as_user() {
  local env_args
  env_args=(
    "HOME=$ACTOR_HOME"
    "SIGMUND_TEST_SYSTEM_STATE_DIR=$SIGMUND_TEST_SYSTEM_STATE_DIR"
    "SIGMUND_TEST_INVOKING_HOME=$ACTOR_HOME"
    "PATH=$PATH"
  )
  local name
  for name in \
    SIGMUND_BOOT_ID_PATH \
    SIGMUND_TEST_FAIL_RECORD_WRITE \
    SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE \
    SIGMUND_FAKE_SUDO_ARGV \
    SIGMUND_FAKE_SUDO_RC \
    SIGMUND_TEST_SUDOERS_DIR \
    SIGMUND_TEST_VISUDO_PROG \
    SIGMUND_TEST_SUDO_PROG; do
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
    "SIGMUND_TEST_SYSTEM_STATE_DIR=$SIGMUND_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
  )
  local name
  for name in \
    SIGMUND_BOOT_ID_PATH \
    SIGMUND_TEST_FAIL_RECORD_WRITE \
    SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE \
    SIGMUND_FAKE_SUDO_ARGV \
    SIGMUND_FAKE_SUDO_RC \
    SIGMUND_TEST_SUDOERS_DIR \
    SIGMUND_TEST_VISUDO_PROG \
    SIGMUND_TEST_SUDO_PROG; do
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
    "SIGMUND_TEST_SYSTEM_STATE_DIR=$SIGMUND_TEST_SYSTEM_STATE_DIR"
    "SIGMUND_TEST_INVOKING_HOME=$ACTOR_HOME"
    "SUDO_UID=$TEST_UID"
    "SUDO_GID=$TEST_GID"
    "SUDO_USER=$TEST_USER"
    "PATH=$PATH"
  )
  local name
  for name in \
    SIGMUND_BOOT_ID_PATH \
    SIGMUND_TEST_FAIL_RECORD_WRITE \
    SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE \
    SIGMUND_FAKE_SUDO_ARGV \
    SIGMUND_FAKE_SUDO_RC \
    SIGMUND_TEST_SUDOERS_DIR \
    SIGMUND_TEST_VISUDO_PROG \
    SIGMUND_TEST_SUDO_PROG; do
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
  cat > "$TEST_ROOT/user-sigmund" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
env_args=(
  "HOME=$SIGMUND_ACTOR_HOME"
  "SIGMUND_TEST_SYSTEM_STATE_DIR=$SIGMUND_TEST_SYSTEM_STATE_DIR"
  "SIGMUND_TEST_INVOKING_HOME=$SIGMUND_ACTOR_HOME"
  "PATH=$PATH"
)
for name in \
  SIGMUND_BOOT_ID_PATH \
  SIGMUND_TEST_FAIL_RECORD_WRITE \
  SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE \
  SIGMUND_FAKE_SUDO_ARGV \
  SIGMUND_FAKE_SUDO_RC \
  SIGMUND_TEST_SUDOERS_DIR \
  SIGMUND_TEST_VISUDO_PROG \
  SIGMUND_TEST_SUDO_PROG; do
  if [ "${!name+x}" = x ]; then
    env_args+=("$name=${!name}")
  fi
done
if [ "${SIGMUND_USER_ACTOR_NEEDS_SUDO:-0}" -eq 1 ]; then
  exec "$SIGMUND_ACTOR_SUDO_BIN" -n -u "$SIGMUND_ACTOR_USER" env "${env_args[@]}" "$SIGMUND_REAL_BIN" "$@"
fi
exec env "${env_args[@]}" "$SIGMUND_REAL_BIN" "$@"
SH
  chmod +x "$TEST_ROOT/user-sigmund" || return 1
  SIGMUND_BIN="$TEST_ROOT/user-sigmund"
  export SIGMUND_BIN
}

new_env() {
  TEST_ROOT="$(mktemp -d "$SUITE_ROOT/test.XXXXXX")" || return 1
  chmod 755 "$TEST_ROOT" || return 1
  SIGMUND_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
  SIGMUND_BOOT_ID_PATH="$TEST_ROOT/boot_id"
  printf 'boot-main\n' >"$SIGMUND_BOOT_ID_PATH" || return 1
  ROOT_HOME="$TEST_ROOT/root-home"
  mkdir -p "$SIGMUND_TEST_SYSTEM_STATE_DIR" "$ROOT_HOME" || return 1
  chmod 755 "$SIGMUND_TEST_SYSTEM_STATE_DIR" || return 1
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
  export HOME TEST_ROOT SIGMUND_TEST_SYSTEM_STATE_DIR SIGMUND_BOOT_ID_PATH ROOT_HOME ACTOR_HOME
  export SIGMUND_ACTOR_HOME="$ACTOR_HOME"
  export SIGMUND_ACTOR_USER="$TEST_USER"
  export SIGMUND_ACTOR_SUDO_BIN="$SUDO_BIN"
  export SIGMUND_USER_ACTOR_NEEDS_SUDO="$USER_ACTOR_NEEDS_SUDO"
  write_user_actor_wrapper
}

cleanup_env() {
  local store ids id sys_store
  store="$HOME/.local/state/sigmund"
  if [ -d "$store" ]; then
    ids=$(find "$store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      "$SIGMUND_BIN" kill "$id" >/dev/null 2>&1 || true
    done
  fi
  sys_store="$SIGMUND_TEST_SYSTEM_STATE_DIR/runs"
  if [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] && [ -d "$sys_store" ]; then
    ids=$(find "$sys_store" -maxdepth 1 -type f -name '*.json' -exec basename {} .json \; 2>/dev/null || true)
    for id in $ids; do
      as_root "$SIGMUND_REAL_BIN" kill "system:$id" >/dev/null 2>&1 || true
    done
  fi
  remove_tree "$TEST_ROOT"
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ] && [ -n "$ACTOR_HOME" ]; then
    find "$ACTOR_HOME" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
  fi
}

ensure_user_fixture_store() {
  mkdir -p "$HOME/.local/state/sigmund" || return 1
  chmod 700 "$HOME" "$HOME/.local" "$HOME/.local/state" "$HOME/.local/state/sigmund" 2>/dev/null || true
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

profile_bin_for_hash() {
  local path="$1" hash="$2"
  awk -v hash="$hash" '
    index($0, "\"" hash "\":") {
      if (match($0, /"bin": "[^"]*"/)) {
        print substr($0, RSTART + 8, RLENGTH - 9)
        found = 1
        exit
      }
    }
    END { if (!found) exit 1 }
  ' "$path"
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
    echo "TIMEOUT: $fn ($desc) exceeded ${SIGMUND_TEST_TIMEOUT}s"
    echo "TEST_ROOT: ${TEST_ROOT:-}"
    echo "--- relevant ps output ---"
    ps -eo pid=,ppid=,pgid=,stat=,etime=,args= 2>/dev/null |
      awk -v root="${TEST_ROOT:-}" -v suite="$SUITE_ROOT" '
        NR == 1 || $0 ~ /sigmund/ || (root != "" && index($0, root)) || index($0, suite) || $0 ~ /sleep|bash|sh/ {
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
  export HOME TEST_ROOT SIGMUND_BIN SIGMUND_REAL_BIN SIGMUND_TEST_SYSTEM_STATE_DIR ROOT_HOME ACTOR_HOME
  export TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE SUDO_BIN
  export SIGMUND_ACTOR_HOME SIGMUND_ACTOR_USER SIGMUND_ACTOR_SUDO_BIN SIGMUND_USER_ACTOR_NEEDS_SUDO
  set +e
  ( set -Eeuo pipefail; "$fn" ) &
  pid=$!
  deadline=$((SECONDS + SIGMUND_TEST_TIMEOUT))
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
    if [ "${SIGMUND_REQUIRE_ROOT_TESTS:-0}" -eq 1 ]; then
      fail "$desc (skipped, but SIGMUND_REQUIRE_ROOT_TESTS=1)"
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
  sed -n -e '/^[0-9a-f]\{8\}$/p' -e 's/^sigmund: id=\([0-9a-f][0-9a-f]*\).*/\1/p' | head -n1
}

record_pgid() {
  local id="$1" store="${2:-$HOME/.local/state/sigmund}"
  sed -n 's/.*"pgid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$store/$id.json" | head -n1
}

record_sid() {
  local id="$1" store="${2:-$HOME/.local/state/sigmund}"
  sed -n 's/.*"sid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$store/$id.json" | head -n1
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
  local out id lines sleep_bin
  sleep_bin="$(resolve_path "$(command -v sleep)")" || return 1
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$out" | grep -Fqx "sigmund  started  $id   $sleep_bin 300"
  printf '%s\n' "$out" | grep -Eq '^         log      .+/.+\.log$'
  printf '%s\n' "$out" | grep -Eq "^         stop     sigmund stop $id$"
  "$SIGMUND_BIN" list | grep -Eq "^$id[[:space:]].*running"
  "$SIGMUND_BIN" stop "$id" >/dev/null
  "$SIGMUND_BIN" list | grep -Eq "^$id[[:space:]].*exited"
  "$SIGMUND_BIN" prune >/dev/null
  lines=$("$SIGMUND_BIN" list | wc -l)
  [ "$lines" -eq 1 ]
}


test_start_output_stop_hint() {
  local out id
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$out" | grep -Eq "^         stop     sigmund stop $id$"
}
test_kill_subcommand() {
  local out id pgid
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  pgid=$(record_pgid "$id")
  [ -n "$pgid" ] || return 1
  "$SIGMUND_BIN" kill "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_group_kill_children() {
  local out id pgid children
  out=$("$SIGMUND_BIN" bash -c 'sleep 600 & sleep 601 & wait' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  sleep 0.2
  children=$(ps -eo pid=,pgid=,args= | awk -v g="$pgid" '$2==g && $1!=g && $3 ~ /^sleep$/ {print $1}')
  [ -n "$children" ] || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  sleep 0.2
  for p in $children; do
    pid_dead_enough "$p" || return 1
  done
  return 0
}

test_exec_failure_no_record() {
  local rc count
  set +e
  "$SIGMUND_BIN" nonexistent_binary_xyz >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  if [ -d "$HOME/.local/state/sigmund" ]; then
    count=$(find "$HOME/.local/state/sigmund" -maxdepth 1 -type f -name '*.json' | wc -l)
  else
    count=0
  fi
  [ "$count" -eq 0 ] || return 1
  count=$(find "$HOME/.local/state/sigmund" -maxdepth 1 -type f 2>/dev/null | wc -l)
  [ "$count" -eq 0 ]
}

test_fast_exit_record_exited() {
  local out id
  out=$("$SIGMUND_BIN" true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.1
  "$SIGMUND_BIN" list | grep -Eq "^$id[[:space:]].*exited"
}

test_exec_replacement_remains_controllable() {
  local out id pgid
  out=$("$SIGMUND_BIN" bash -c 'exec sleep 300' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$SIGMUND_BIN" list | grep -Eq "^$id[[:space:]].*running" || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_corrupt_record_handling() {
  ensure_user_fixture_store || return 1
  printf 'garbage\n' > "$HOME/.local/state/sigmund/badbad00.json" || return 1
  "$SIGMUND_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^badbad00' "$TEST_ROOT/list.out"
  ! grep -Eq '^0[[:space:]]' "$TEST_ROOT/list.out"
  grep -q 'warning: skipping corrupt record badbad00.json' "$TEST_ROOT/list.err"
  "$SIGMUND_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/sigmund/badbad00.json" ]
}

test_deep_json_record_rejected_not_crashed() {
  ensure_user_fixture_store || return 1
  local json i
  json='{"junk":'
  for i in $(seq 1 80); do json="${json}["; done
  json="${json}0"
  for i in $(seq 1 80); do json="${json}]"; done
  json="${json},\"version\":1,\"id\":\"abc12445\",\"pid\":12345,\"pgid\":12345,\"sid\":12345,\"start_unix_ns\":0,\"argv\":[\"x\"],\"cmdline_display\":\"x\",\"uid\":0,\"gid\":0,\"proc_starttime_ticks\":0,\"exe_dev\":0,\"exe_ino\":0}"
  printf '%s\n' "$json" > "$HOME/.local/state/sigmund/abc12445.json" || return 1
  "$SIGMUND_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12445' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12445.json' "$TEST_ROOT/list.err"
}

test_symlinked_record_rejected() {
  ensure_user_fixture_store || return 1
  cat > "$TEST_ROOT/record-target.json" <<'JSON'
{"version":1,"id":"abc12545","pid":12345,"pgid":12345,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  ln -s "$TEST_ROOT/record-target.json" "$HOME/.local/state/sigmund/abc12545.json" || return 1
  "$SIGMUND_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12545' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12545.json' "$TEST_ROOT/list.err"
}

test_symlinked_log_rejected() {
  local out id log rc
  out=$("$SIGMUND_BIN" /bin/echo original-log-line 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log="$HOME/.local/state/sigmund/$id.log"
  sleep 0.2
  [ -f "$log" ] || return 1
  printf 'symlink-secret\n' > "$TEST_ROOT/log-target" || return 1
  rm -f "$log" || return 1
  ln -s "$TEST_ROOT/log-target" "$log" || return 1
  set +e
  "$SIGMUND_BIN" dump "$id" >"$TEST_ROOT/dump.out" 2>"$TEST_ROOT/dump.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  ! grep -q 'symlink-secret' "$TEST_ROOT/dump.out" || return 1
  grep -q 'failed to open log for dump' "$TEST_ROOT/dump.err"
}

test_invalid_pgid_record() {
  ensure_user_fixture_store || return 1
  cat > "$HOME/.local/state/sigmund/abc12345.json" <<'JSON'
{"version":1,"id":"abc12345","pid":12345,"pgid":0,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  "$SIGMUND_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12345' "$TEST_ROOT/list.out"
}

test_orphan_log_cleanup() {
  ensure_user_fixture_store || return 1
  : > "$HOME/.local/state/sigmund/a1b2c3d4.log" || return 1
  : > "$HOME/.local/state/sigmund/deadbeef.log" || return 1
  "$SIGMUND_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/sigmund/a1b2c3d4.log" ] && [ ! -e "$HOME/.local/state/sigmund/deadbeef.log" ]
}

test_id_sanitization() {
  local rc
  for bad in '../../etc/passwd' 'AABBCC' 'hello!' ''; do
    set +e
    "$SIGMUND_BIN" stop "$bad" >/dev/null 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 5 ] || return 1
  done
}

test_print_signal_output() {
  local out id pgid got
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  got=$("$SIGMUND_BIN" stop --print "$id") || return 1
  [ "$got" = "kill -TERM -- -$pgid" ]
  got=$("$SIGMUND_BIN" kill --print "$id") || return 1
  [ "$got" = "kill -KILL -- -$pgid" ]
}

test_signal_refuses_tampered_live_group_identity() {
  local out id rec real_pgid real_sid shell_pgid shell_sid rc
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  real_pgid=$(record_pgid "$id")
  real_sid=$(record_sid "$id")
  [ -n "$id" ] && [ -n "$real_pgid" ] && [ -n "$real_sid" ] || return 1
  shell_pgid=$(ps -o pgid= -p $$ | tr -d ' ')
  shell_sid=$(ps -o sid= -p $$ | tr -d ' ')
  [ -n "$shell_pgid" ] && [ -n "$shell_sid" ] || return 1
  [ "$shell_pgid" != "$real_pgid" ] || { "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true; skip "shell and target unexpectedly share pgid"; }
  rec="$HOME/.local/state/sigmund/$id.json"
  cp "$rec" "$rec.bak" || return 1
  sed -i.tmp \
    -e "s/\"pgid\":[[:space:]]*[0-9][0-9]*/\"pgid\":$shell_pgid/" \
    -e "s/\"sid\":[[:space:]]*[0-9][0-9]*/\"sid\":$shell_sid/" \
    "$rec" || return 1
  rm -f "$rec.tmp"
  set +e
  "$SIGMUND_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/tampered-live-print.err"
  rc=$?
  set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$rc" -eq 2 ] || { echo "--print on tampered live pgid/sid: rc=$rc (want 2 refused)" >&2; return 1; }
  grep -Eq 'process group/session differs|cannot be signaled' "$TEST_ROOT/tampered-live-print.err" || {
    cat "$TEST_ROOT/tampered-live-print.err" >&2
    return 1
  }
}

test_stop_multiple_ids() {
  local out1 out2 id1 id2 pgid1 pgid2 rc
  out1=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  out2=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid1=$(record_pgid "$id1")
  pgid2=$(record_pgid "$id2")
  [ -n "$id1" ] && [ -n "$id2" ] && [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  set +e
  "$SIGMUND_BIN" stop "$id1" "$id2" >/dev/null
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_argument_edges() {
  local rc out
  set +e
  "$SIGMUND_BIN" >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  set +e
  "$SIGMUND_BIN" stop >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || return 1
  "$SIGMUND_BIN" --help >/dev/null || return 1
  "$SIGMUND_BIN" help >/dev/null || return 1
  "$SIGMUND_BIN" help --help >/dev/null || return 1
  "$SIGMUND_BIN" help profiles | grep -q 'sigmund alias <id> <name>' || return 1
  "$SIGMUND_BIN" help targets | grep -q 'run id, leading id prefix, or alias name' || return 1
  "$SIGMUND_BIN" help scripting | grep -q 'Exit codes:' || return 1
  "$SIGMUND_BIN" stop -h | grep -q 'usage: sigmund stop' || return 1
  out=$("$SIGMUND_BIN" --version) || return 1
  printf '%s\n' "$out" | grep -Eq '^(dev|[0-9a-f]{7,40}|v?[0-9]+\.[0-9]+\.[0-9]+.*)$'
  set +e
  "$SIGMUND_BIN" -l >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  set +e
  "$SIGMUND_BIN" --list >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  "$SIGMUND_BIN" -- sleep 1 >/dev/null
}

test_special_chars_args() {
  local out id json
  out=$("$SIGMUND_BIN" echo "hello world" "it's" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json="$HOME/.local/state/sigmund/$id.json"
  [ -f "$json" ] || return 1
  grep -Fq '"hello world"' "$json"
  grep -Fq '"it'"'"'s"' "$json"
}

test_log_capture() {
  local out id log
  out=$("$SIGMUND_BIN" bash -c 'echo out; echo err >&2; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log="$HOME/.local/state/sigmund/$id.log"
  sleep 0.4
  [ -f "$log" ] || return 1
  grep -q 'out' "$log" && grep -q 'err' "$log"
}

test_start_follow_short_form() {
  local out id
  out=$("$SIGMUND_BIN" -f bash -c 'echo follow-short' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$out" | grep -q 'follow-short'
}



test_tail_verb_existing_id() {
  local out id tailed
  out=$("$SIGMUND_BIN" bash -c 'sleep 0.3; echo from_tail_id; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  tailed=$("$SIGMUND_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'from_tail_id'
}

test_persistent_stale_records() {
  local out id store bootfile oldboot list_out stale_id stale_log
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" bash -c 'echo stale-line; sleep 0.2' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  store="$HOME/.local/state/sigmund"
  stale_log="$store/$id.log"
  [ -f "$stale_log" ] || return 1
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" list >/dev/null || return 1
  printf 'boot-b\n' >"$bootfile" || return 1
  list_out=$(SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+stale[[:space:]]"
  set +e
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" stop "$id" >/dev/null 2>"$TEST_ROOT/stop.err"
  [ "$?" -eq 2 ] || return 1
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" kill "$id" >/dev/null 2>"$TEST_ROOT/kill.err"
  [ "$?" -eq 2 ] || return 1
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" stop --print "$id" >/dev/null 2>"$TEST_ROOT/print.err"
  [ "$?" -eq 2 ] || return 1
  set -e
  grep -q 'stale' "$TEST_ROOT/stop.err"
  grep -q 'stale' "$TEST_ROOT/kill.err"
  grep -q 'stale' "$TEST_ROOT/print.err"
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/tail.out"
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" dump "$id" >"$TEST_ROOT/dump.out" 2>&1 || return 1
  grep -q 'stale-line' "$TEST_ROOT/dump.out"
  [ -f "$store/$id.json" ] && [ -f "$store/$id.log" ]
}


test_boot_unavailable_does_not_force_stale() {
  local out id bootfile list_out rc
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rm -f "$bootfile"
  list_out=$(SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+running[[:space:]]" || return 1
  ! printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+stale[[:space:]]" || return 1
  set +e
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" stop "$id" >/dev/null 2>"$TEST_ROOT/missing-boot-stop.err"
  rc=$?
  set -e
  [ "$rc" -eq 2 ] || { echo "stop with missing boot source: rc=$rc (want 2)" >&2; return 1; }
  grep -q 'missing boot_id' "$TEST_ROOT/missing-boot-stop.err" || { cat "$TEST_ROOT/missing-boot-stop.err" >&2; return 1; }
  printf 'boot-a\n' >"$bootfile" || return 1
  SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" stop "$id" >/dev/null
}

test_leader_zombie_group_still_running() {
  local i id list_out tail_pid
  "$SIGMUND_BIN" --tail bash -c 'sleep 60 & exit 0' >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" &
  tail_pid=$!
  id=""
  for i in $(seq 1 50); do
    id=$(extract_id <"$TEST_ROOT/tail.out" || true)
    [ -n "$id" ] && break
    sleep 0.05
  done
  [ -n "$id" ] || return 1
  sleep 0.3
  list_out=$("$SIGMUND_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+running[[:space:]]" || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  wait "$tail_pid" || true
}

test_tail_finished_log_prints_existing_output() {
  local out id tailed
  out=$("$SIGMUND_BIN" bash -c 'echo finished-tail-line' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  tailed=$("$SIGMUND_BIN" tail "$id" 2>&1) || return 1
  printf '%s\n' "$tailed" | grep -q 'finished-tail-line'
}

test_console_does_not_require_external_attach_tool() {
  local fake_path out id store record sock
  fake_path="$TEST_ROOT/no-tools"
  mkdir -p "$fake_path" || return 1
  out=$(as_user /usr/bin/env PATH="$fake_path" "$SIGMUND_REAL_BIN" --console /bin/sh -c 'read line; echo "native:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  printf 'ping\n' | as_user /usr/bin/env PATH="$fake_path" "$SIGMUND_REAL_BIN" console "$id" >"$TEST_ROOT/console-native.out" 2>"$TEST_ROOT/console-native.err" || {
    cat "$TEST_ROOT/console-native.out" "$TEST_ROOT/console-native.err" >&2
    return 1
  }
  grep -q 'native:ping' "$TEST_ROOT/console-native.out" || { cat "$TEST_ROOT/console-native.out" >&2; return 1; }
  store="$HOME/.local/state/sigmund"
  record="$store/$id.json"
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_reports_non_console_run() {
  local out id rc
  out=$("$SIGMUND_BIN" sleep 30 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" console "$id" >"$TEST_ROOT/console-none.out" 2>"$TEST_ROOT/console-none.err"
  rc=$?
  [ "$rc" -eq 0 ] || return 1
  grep -q "has no console" "$TEST_ROOT/console-none.err" || { cat "$TEST_ROOT/console-none.err" >&2; return 1; }
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
}

test_console_round_trip_and_log_tee() {
  local out id store record sock
  out=$("$SIGMUND_BIN" --console /bin/sh -c 'read line; echo "got:$line"' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/sigmund"
  record="$store/$id.json"
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }
  "$SIGMUND_BIN" list >"$TEST_ROOT/console-list.out" || return 1
  grep -Eq "^$id[[:space:]]+running[[:space:]]+.*[[:space:]]console[[:space:]]" "$TEST_ROOT/console-list.out" || {
    cat "$TEST_ROOT/console-list.out" >&2
    return 1
  }
  printf 'ping\n' | "$SIGMUND_BIN" console "$id" >"$TEST_ROOT/console.out" 2>"$TEST_ROOT/console.err" || {
    cat "$TEST_ROOT/console.out" "$TEST_ROOT/console.err" >&2
    return 1
  }
  grep -q 'got:ping' "$TEST_ROOT/console.out" || { cat "$TEST_ROOT/console.out" >&2; return 1; }
  sleep 0.2
  grep -q 'got:ping' "$store/$id.log" || { cat "$store/$id.log" >&2; return 1; }
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_console_socket_lives_in_store_dir() {
  # The console socket must live inside the store's console directory, never in
  # /tmp, even though the harness's store path is longer than the AF_UNIX
  # sun_path limit (~104). This exercises the relative-bind path.
  local out id store record sock
  out=$("$SIGMUND_BIN" --console /bin/sh -c 'while read line; do [ "$line" = noop ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/sigmund"
  record="$store/$id.json"
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  if [ "$sock" != "$store/console/$id.sock" ]; then
    echo "console socket not in store console dir (got: $sock)" >&2
    "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  if [ ! -S "$sock" ]; then
    echo "no socket present at $sock" >&2
    "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The socket must be owner-only (0600): it is an exec channel into the run.
  sock_mode=$(file_mode "$sock") || { "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  if [ "$sock_mode" != 600 ]; then
    echo "console socket mode=$sock_mode (want 600)" >&2
    "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  fi
  # The relative path must also be connectable when the store path is long.
  printf 'noop\n' | "$SIGMUND_BIN" console "$id" >/dev/null 2>"$TEST_ROOT/console-store.err" || {
    cat "$TEST_ROOT/console-store.err" >&2
    "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  }
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
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

  out=$( cd "$workdir" && "$SIGMUND_BIN" --console /bin/sh -c \
    'if [ -e sentinel-file ]; then echo CWD_OK; else echo CWD_BAD; fi; sleep 30' 2>&1 ) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/sigmund"
  logf="$store/$id.log"
  ok=0
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if grep -q CWD_OK "$logf" 2>/dev/null; then ok=1; break; fi
    if grep -q CWD_BAD "$logf" 2>/dev/null; then ok=0; break; fi
    sleep 0.2
  done
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
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
   * sigmund's own client, so a long store path stays within sun_path. */
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
  if (write_all(fd, "SIGMUND1", 8) != 0) return 5;
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
  out=$("$SIGMUND_BIN" --console /bin/sh -c 'echo replay-secret; while read line; do echo "got:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record="$HOME/.local/state/sigmund/$id.json"
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -S "$sock" ] || return 1
  log="$HOME/.local/state/sigmund/$id.log"

  # Deliberately relax filesystem traversal and socket mode in this temp store
  # so the kernel accepts the unrelated connection attempt; the broker's peer
  # credential check must still reject it before replaying output or forwarding
  # input.
  chmod 0711 "$ACTOR_HOME" "$ACTOR_HOME/.local" "$ACTOR_HOME/.local/state" \
    "$HOME/.local/state/sigmund" "$HOME/.local/state/sigmund/console" || return 1
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

  printf 'done\n' | "$SIGMUND_BIN" console "$id" >"$TEST_ROOT/authorized-console.out" 2>"$TEST_ROOT/authorized-console.err" || {
    cat "$TEST_ROOT/authorized-console.out" "$TEST_ROOT/authorized-console.err" >&2
    return 1
  }
  grep -q 'got:done' "$TEST_ROOT/authorized-console.out" || { cat "$TEST_ROOT/authorized-console.out" >&2; return 1; }
}

test_console_can_reattach_after_detach() {
  local out id store record sock helper
  out=$("$SIGMUND_BIN" --console /bin/sh -c 'while read line; do echo "seen:$line"; [ "$line" = done ] && exit 0; done' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  store="$HOME/.local/state/sigmund"
  record="$store/$id.json"
  sock=$(sed -n 's/.*"console_sock":[[:space:]]*"\([^"]*\)".*/\1/p' "$record" | head -n1)
  [ -n "$sock" ] || { cat "$record" >&2; return 1; }

  helper=$(build_console_protocol_helper) || return 1
  as_user "$helper" "$sock" 'first
' 'seen:first' >"$TEST_ROOT/console-first.out" 2>"$TEST_ROOT/console-first.err" || {
    cat "$TEST_ROOT/console-first.out" "$TEST_ROOT/console-first.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-first.out" || { cat "$TEST_ROOT/console-first.out" >&2; return 1; }

  printf 'done\n' | "$SIGMUND_BIN" console "$id" >"$TEST_ROOT/console-second.out" 2>"$TEST_ROOT/console-second.err" || {
    cat "$TEST_ROOT/console-second.out" "$TEST_ROOT/console-second.err" >&2
    return 1
  }
  grep -q 'seen:first' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  grep -q 'seen:done' "$TEST_ROOT/console-second.out" || { cat "$TEST_ROOT/console-second.out" >&2; return 1; }
  sleep 0.2
  grep -q 'seen:first' "$store/$id.log" || { cat "$store/$id.log" >&2; return 1; }
  grep -q 'seen:done' "$store/$id.log" || { cat "$store/$id.log" >&2; return 1; }
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1
  path_absent_soon "$sock" || {
    ls -la "$store" "$store/console" "$(dirname "$sock")" >&2 || true
    return 1
  }
}

test_prune_by_id() {
  local out1 out2 id1 id2 store
  out1=$("$SIGMUND_BIN" true 2>&1) || return 1
  out2=$("$SIGMUND_BIN" true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/sigmund"
  "$SIGMUND_BIN" prune "$id1" >/dev/null || return 1
  [ ! -e "$store/$id1.json" ] && [ ! -e "$store/$id1.log" ] || return 1
  [ -e "$store/$id2.json" ] && [ -e "$store/$id2.log" ]
}

test_prune_all_keeps_running() {
  local out1 out2 id1 id2 store
  out1=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  out2=$("$SIGMUND_BIN" true 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id1" ] && [ -n "$id2" ] || return 1
  store="$HOME/.local/state/sigmund"
  "$SIGMUND_BIN" prune all >/dev/null || return 1
  [ -e "$store/$id1.json" ] || return 1
  [ ! -e "$store/$id2.json" ] || return 1
  [ ! -e "$store/$id2.log" ] || return 1
}

test_transactional_record_write_failure() {
  local rc pids
  set +e
  SIGMUND_TEST_FAIL_RECORD_WRITE=1 "$SIGMUND_BIN" bash -c 'exec -a sigmund_txn_test_sleep sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  # Detect a leaked process via ps (a required tool) rather than pgrep with
  # `|| true`, which would pass vacuously if pgrep were absent.
  pids=$(ps -eo pid=,args= | awk '/sigmund_txn_test_sleep/ && !/awk/ {print $1}')
  [ -z "$pids" ]
}


make_fake_sudo() {
  mkdir -p "$TEST_ROOT/fakebin" || return 1
  cat > "$TEST_ROOT/fakebin/sudo" <<'SH'
#!/usr/bin/env bash
: "${SIGMUND_FAKE_SUDO_ARGV:?}"
printf '%s\n' "$@" > "$SIGMUND_FAKE_SUDO_ARGV"
exit "${SIGMUND_FAKE_SUDO_RC:-77}"
SH
  chmod 755 "$TEST_ROOT/fakebin" "$TEST_ROOT/fakebin/sudo" || return 1
  export SIGMUND_FAKE_SUDO_ARGV="$TEST_ROOT/sudo.argv"
  : > "$SIGMUND_FAKE_SUDO_ARGV" || return 1
  chmod 0666 "$SIGMUND_FAKE_SUDO_ARGV" || return 1
  export SIGMUND_FAKE_SUDO_RC=77
  export PATH="$TEST_ROOT/fakebin:$PATH"
}

write_public_index_fixture() {
  local id="$1" state="${2:-running}" started="${3:-2026-06-15T18:42:11Z}" alias="${4:-}"
  mkdir -p "$SIGMUND_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$SIGMUND_TEST_SYSTEM_STATE_DIR" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public" || return 1
  local alias_json=""
  if [ -n "$alias" ]; then
    alias_json="\"alias\":\"$alias\","
  fi
  cat > "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" <<JSON
{"id":"$id","root_managed":true,"requires_elevation":true,${alias_json}"state_hint":"$state","started_at":"$started","argv":["secret"],"cmdline_display":"secret command"}
JSON
  chmod 0644 "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
}

write_public_alias_fixture() {
  local name="$1" hash="$2"
  mkdir -p "$SIGMUND_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$SIGMUND_TEST_SYSTEM_STATE_DIR" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/aliases.json" <<JSON
{
  "$name": "$hash"
}
JSON
  chmod 0644 "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/aliases.json" || return 1
}

system_alias_hash() {
  local name="$1"
  sed -n "s/.*\"$name\": \"\\([0-9a-f]\\{64\\}\\)\".*/\\1/p" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/aliases.json"
}

test_alias_profile_map_start_and_stop() {
  local out id out2 id2 pgid2 store sh_bin
  store="$HOME/.local/state/sigmund"
  sh_bin="$(resolve_path /bin/sh)" || return 1
  out=$("$SIGMUND_BIN" /bin/sh -c 'while :; do sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" web-test >"$TEST_ROOT/alias.out" 2>"$TEST_ROOT/alias.err" || return 1
  [ ! -s "$TEST_ROOT/alias.out" ] || return 1
  grep -Fqx "sigmund: pinned 'web-test' -> $sh_bin -c 'while :; do sleep 1; done'" "$TEST_ROOT/alias.err" || return 1
  [ -f "$store/aliases.json" ] || return 1
  [ ! -f "$store/profiles.json" ] || return 1
  [ ! -d "$store/profiles" ] || return 1
  grep -q '"web-test": {"bin": "' "$store/aliases.json" || return 1
  grep -Fq "\"args\": [\"$sh_bin\", \"-c\", \"while :; do sleep 1; done\"]" "$store/aliases.json" || return 1
  "$SIGMUND_BIN" aliases >"$TEST_ROOT/aliases-list.out" || return 1
  grep -Eq "^web-test[[:space:]]+user[[:space:]]+.*[[:space:]]+-$" "$TEST_ROOT/aliases-list.out" || return 1
  "$SIGMUND_BIN" list >"$TEST_ROOT/list-after-alias.out" 2>"$TEST_ROOT/list-after-alias.err" || return 1
  ! grep -q 'aliases.json' "$TEST_ROOT/list-after-alias.err" || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1

  "$SIGMUND_BIN" prune >"$TEST_ROOT/prune-after-alias.out" 2>"$TEST_ROOT/prune-after-alias.err" || return 1
  [ -f "$store/aliases.json" ] || return 1
  ! grep -q 'aliases.json' "$TEST_ROOT/prune-after-alias.err" || return 1

  out2=$("$SIGMUND_BIN" start web-test 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid2=$(record_pgid "$id2")
  [ -n "$id2" ] && [ -n "$pgid2" ] || return 1
  grep -q '"alias": "web-test"' "$store/$id2.json" || return 1
  ! grep -q '"profile_hash":' "$store/$id2.json" || return 1
  "$SIGMUND_BIN" stop web-test >/dev/null || return 1
  pgid_terminated "$pgid2"
}

test_alias_from_relative_executable_uses_recorded_absolute_argv0() {
  local app work other store expected out id
  app="$TEST_ROOT/app"
  work="$app/work"
  other="$TEST_ROOT/other"
  store="$HOME/.local/state/sigmund"
  mkdir -p "$app/bin" "$work" "$other" || return 1
  printf '#!/bin/sh\nsleep 300\n' >"$app/bin/daemon" || return 1
  chmod +x "$app/bin/daemon" || return 1
  expected="$(resolve_path "$app/bin/daemon")" || return 1

  out=$(cd "$work" && "$SIGMUND_BIN" ../bin/daemon 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  grep -Fq "\"argv\": [\"$expected\"]" "$store/$id.json" || return 1

  (cd "$other" && "$SIGMUND_BIN" alias "$id" rel-daemon >"$TEST_ROOT/alias-rel.out" 2>"$TEST_ROOT/alias-rel.err") || return 1
  grep -Fq "\"rel-daemon\": {\"bin\": \"$expected\", \"args\": [\"$expected\"]}" "$store/aliases.json" || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
}

test_alias_multi_gate_and_all_stop() {
  local out id id1 id2 out1 out2 rc pgid1 pgid2
  out=$("$SIGMUND_BIN" sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" web-multi >/dev/null || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1

  out1=$("$SIGMUND_BIN" start web-multi 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  pgid1=$(record_pgid "$id1")
  [ -n "$pgid1" ] || return 1
  set +e
  "$SIGMUND_BIN" start web-multi >"$TEST_ROOT/multi-refuse.out" 2>"$TEST_ROOT/multi-refuse.err"
  rc=$?
  set -e
  [ "$rc" -eq 6 ] || return 1
  grep -q -- '--multi' "$TEST_ROOT/multi-refuse.err" || return 1

  out2=$("$SIGMUND_BIN" start web-multi --multi 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid2=$(record_pgid "$id2")
  [ -n "$pgid2" ] || return 1
  set +e
  "$SIGMUND_BIN" stop web-multi >"$TEST_ROOT/alias-ambig.out" 2>"$TEST_ROOT/alias-ambig.err"
  rc=$?
  set -e
  [ "$rc" -eq 6 ] || return 1
  grep -q 'matches more than one' "$TEST_ROOT/alias-ambig.err" || return 1
  "$SIGMUND_BIN" stop web-multi --all >/dev/null || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_profile_start_inherits_current_environment() {
  local out id out2 id2 got
  out=$(as_user env SIGMUND_PROFILE_ENV=seed "$SIGMUND_REAL_BIN" /bin/sh -c 'printf "%s\n" "$SIGMUND_PROFILE_ENV"' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_user "$SIGMUND_REAL_BIN" alias "$id" env-test >"$TEST_ROOT/alias-env.out" 2>"$TEST_ROOT/alias-env.err" || return 1

  out2=$(as_user env SIGMUND_PROFILE_ENV=profile-value "$SIGMUND_REAL_BIN" start env-test 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id2" ] || return 1
  sleep 0.2
  got=$(as_user "$SIGMUND_REAL_BIN" dump "$id2") || return 1
  printf '%s\n' "$got" | grep -qx 'profile-value' || return 1
  ! printf '%s\n' "$got" | grep -qx 'seed'
}

test_profile_transcript_import_export_roundtrip() {
  local transcript export out id got store
  store="$HOME/.local/state/sigmund"
  transcript="$TEST_ROOT/import.profile"
  cat >"$transcript" <<'EOF' || return 1
profile cli-prof
set command -- /bin/echo 'hello import'
save
EOF
  "$SIGMUND_BIN" profile import "$transcript" >"$TEST_ROOT/import.out" 2>"$TEST_ROOT/import.err" || return 1
  [ ! -s "$TEST_ROOT/import.out" ] || return 1
  [ ! -s "$TEST_ROOT/import.err" ] || return 1
  grep -Fq '"cli-prof": {"bin": "/usr/bin/echo"' "$store/aliases.json" ||
    grep -Fq '"cli-prof": {"bin": "/bin/echo"' "$store/aliases.json" || return 1
  grep -Fq '"args": ["/bin/echo", "hello import"]' "$store/aliases.json" || return 1

  "$SIGMUND_BIN" profile export cli-prof >"$TEST_ROOT/export.profile" || return 1
  grep -Fxq "profile cli-prof" "$TEST_ROOT/export.profile" || return 1
  grep -Eq "^set command -- (/usr)?/bin/echo 'hello import'$" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "save" "$TEST_ROOT/export.profile" || return 1

  out=$("$SIGMUND_BIN" start cli-prof 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.1
  got=$("$SIGMUND_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'hello import'
}

test_profile_json_export_and_import() {
  local json out id got store
  store="$HOME/.local/state/sigmund"
  json="$TEST_ROOT/json.profile"
  cat >"$json" <<'JSON' || return 1
{"version":1,"name":"json-prof","bin":"/bin/echo","args":["/bin/echo","hello json"]}
JSON
  "$SIGMUND_BIN" profile import "$json" >/dev/null || return 1
  "$SIGMUND_BIN" profile export json-prof --json >"$TEST_ROOT/export.json" || return 1
  grep -Fq '"name": "json-prof"' "$TEST_ROOT/export.json" || return 1
  grep -Fq '"args": ["/bin/echo", "hello json"]' "$TEST_ROOT/export.json" || return 1
  grep -Fq '"json-prof": {"bin": "/bin/echo"' "$store/aliases.json" || return 1

  out=$("$SIGMUND_BIN" start json-prof 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.1
  got=$("$SIGMUND_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'hello json'
}

test_invalid_alias_names_rejected() {
  local out id rc
  out=$("$SIGMUND_BIN" true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  "$SIGMUND_BIN" alias "$id" bad.name >"$TEST_ROOT/alias-bad.out" 2>"$TEST_ROOT/alias-bad.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || return 1
  grep -q 'invalid alias' "$TEST_ROOT/alias-bad.err"
}

test_short_hex_alias_name_allowed() {
  local out id sh_bin
  sh_bin="$(resolve_path /bin/sh)" || return 1
  out=$("$SIGMUND_BIN" /bin/sh -c ':' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" db >"$TEST_ROOT/alias-db.out" 2>"$TEST_ROOT/alias-db.err" || return 1
  [ ! -s "$TEST_ROOT/alias-db.out" ] || return 1
  grep -Fqx "sigmund: pinned 'db' -> $sh_bin -c :" "$TEST_ROOT/alias-db.err" || return 1
  "$SIGMUND_BIN" aliases >"$TEST_ROOT/aliases-db.out" || return 1
  grep -Eq "^db[[:space:]]+user[[:space:]]+" "$TEST_ROOT/aliases-db.out" || return 1
  grep -Fq "$sh_bin -c :" "$TEST_ROOT/aliases-db.out"
}

test_system_alias_action_self_elevates_alias() {
  local hash rc
  hash=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
  write_public_alias_fixture web-test "$hash" || return 1
  write_public_index_fixture abc12345 running 2026-06-15T18:42:11Z web-test || return 1
  make_fake_sudo || return 1
  set +e
  "$SIGMUND_BIN" stop web-test >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$SIGMUND_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "stop" ] || return 1
  [ "${args[5]}" = "abc12345" ] || return 1
  [ "${args[6]}" = "web-test" ] || return 1
  [ "${args[7]}" = "$hash" ] || return 1
}

test_system_alias_start_self_elevates_alias() {
  local hash rc
  hash=abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd
  write_public_alias_fixture web-test "$hash" || return 1
  make_fake_sudo || return 1
  set +e
  "$SIGMUND_BIN" --system start web-test >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$SIGMUND_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "start" ] || return 1
  [ "${args[5]}" = "00000000" ] || return 1
  [ "${args[6]}" = "web-test" ] || return 1
  [ "${args[7]}" = "$hash" ] || return 1
}

test_grant_revoke_writes_hash_scoped_sudoers() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe safe_real out id hash sudoers_dir sudoers_file rc visudo_ok sh_bin
  safe="$TEST_ROOT/sigmund-safe"
  sh_bin="$(resolve_path /bin/sh)" || return 1
  cp "$SIGMUND_REAL_BIN" "$safe" || return 1
  as_root chown 0:0 "$safe" || return 1
  as_root chmod 755 "$safe" || return 1
  safe_real="$(cd "$(dirname "$safe")" && pwd -P)/$(basename "$safe")" || return 1
  sudoers_dir="$TEST_ROOT/sudoers.d"
  mkdir -p "$sudoers_dir" || return 1
  chmod 755 "$sudoers_dir" || return 1
  export SIGMUND_TEST_SUDOERS_DIR="$sudoers_dir"
  visudo_ok="$TEST_ROOT/visudo-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$visudo_ok" || return 1
  chmod 755 "$visudo_ok" || return 1
  export SIGMUND_TEST_VISUDO_PROG="$visudo_ok"

  out=$(as_root "$safe" /bin/sh -c ':' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_root "$safe" alias "$id" web-sys >"$TEST_ROOT/root-alias.out" 2>"$TEST_ROOT/root-alias.err" || return 1
  [ ! -s "$TEST_ROOT/root-alias.out" ] || return 1
  grep -Fqx "sigmund: pinned 'web-sys' -> $sh_bin -c :" "$TEST_ROOT/root-alias.err" || return 1
  hash=$(system_alias_hash web-sys)
  [ -n "$hash" ] || return 1
  set +e
  as_root "$safe" grant "$hash" "$TEST_USER" start >"$TEST_ROOT/grant-hash.out" 2>"$TEST_ROOT/grant-hash.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/grant-hash.out" "$TEST_ROOT/grant-hash.err" >&2; return 1; }
  grep -q 'existing system alias' "$TEST_ROOT/grant-hash.err" || { cat "$TEST_ROOT/grant-hash.err" >&2; return 1; }
  as_root "$safe" grant web-sys "$TEST_USER" start,stop >"$TEST_ROOT/grant.out" 2>"$TEST_ROOT/grant.err" || { cat "$TEST_ROOT/grant.out" "$TEST_ROOT/grant.err" >&2; return 1; }
  sudoers_file="$sudoers_dir/sigmund_web-sys_$TEST_USER"
  root_file_exists "$sudoers_file" || { echo "missing $sudoers_file" >&2; cat "$TEST_ROOT/grant.err" >&2; return 1; }
  root_grep '# actions-list: start,stop' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$TEST_USER ALL=(root) NOPASSWD: $safe_real ^--system --elevated (start|stop) [0-9a-f]{8} web-sys $hash$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }

  as_root "$safe" revoke web-sys "$TEST_USER" start >"$TEST_ROOT/revoke.out" 2>"$TEST_ROOT/revoke.err" || { cat "$TEST_ROOT/revoke.out" "$TEST_ROOT/revoke.err" >&2; return 1; }
  root_grep '# actions-list: stop' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$TEST_USER ALL=(root) NOPASSWD: $safe_real ^--system --elevated (stop) [0-9a-f]{8} web-sys $hash$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }

  as_root "$safe" grant web-sys "$TEST_USER" >"$TEST_ROOT/grant-all.out" 2>"$TEST_ROOT/grant-all.err" || { cat "$TEST_ROOT/grant-all.out" "$TEST_ROOT/grant-all.err" >&2; return 1; }
  root_grep '# actions: ALL' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep '# actions-list: start,stop,kill,tail,dump,prune,console' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$TEST_USER ALL=(root) NOPASSWD: $safe_real ^--system --elevated (start|stop|kill|tail|dump|prune|console) [0-9a-f]{8} web-sys $hash$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  as_root "$safe" revoke web-sys "$TEST_USER" >"$TEST_ROOT/revoke-all.out" 2>"$TEST_ROOT/revoke-all.err" || { cat "$TEST_ROOT/revoke-all.out" "$TEST_ROOT/revoke-all.err" >&2; return 1; }
  root_path_absent "$sudoers_file"
}

test_elevated_capability_start_and_stop_validate_alias_hash() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe out id hash start_out cap_id rc
  safe="$TEST_ROOT/sigmund-cap"
  cp "$SIGMUND_REAL_BIN" "$safe" || return 1
  as_root chown 0:0 "$safe" || return 1
  as_root chmod 755 "$safe" || return 1

  out=$(as_root "$safe" /bin/sh -c 'while :; do sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_root "$safe" alias "$id" web-cap >"$TEST_ROOT/cap-alias.out" 2>"$TEST_ROOT/cap-alias.err" || return 1
  [ ! -s "$TEST_ROOT/cap-alias.out" ] || return 1
  hash=$(system_alias_hash web-cap)
  [ -n "$hash" ] || return 1
  as_root "$safe" stop "$id" >/dev/null || return 1
  as_root "$safe" prune "$id" >/dev/null || return 1

  start_out=$(as_root "$safe" --system --elevated start 00000000 web-cap "$hash" 2>&1) || return 1
  cap_id=$(printf '%s\n' "$start_out" | extract_id)
  [ -n "$cap_id" ] || return 1
  root_grep '"alias": "web-cap"' "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$cap_id.json" || return 1
  ! root_grep '"profile_hash":' "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$cap_id.json" || return 1

  set +e
  as_root "$safe" --system --elevated stop "$cap_id" web-cap 0000000000000000000000000000000000000000000000000000000000000000 >/dev/null 2>"$TEST_ROOT/cap-bad.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  grep -q 'capability' "$TEST_ROOT/cap-bad.err" || return 1

  as_root "$safe" --system --elevated stop "$cap_id" web-cap "$hash" >/dev/null || return 1
}

test_raw_start_does_not_steal_trailing_system() {
  local out id log
  out=$("$SIGMUND_BIN" sh -c 'printf "arg:%s\n" "$1"' sh --system 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  log="$HOME/.local/state/sigmund/$id.log"
  sleep 0.2
  grep -q '^arg:--system$' "$log"
}

test_public_root_index_list_is_redacted() {
  write_public_index_fixture abc12345 running 2026-06-15T18:42:11Z || return 1
  "$SIGMUND_BIN" list --iso >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  grep -Eq '^abc12345[[:space:]]+unknown[[:space:]]+2026-06-15T18:42:11Z[[:space:]]+-[[:space:]]+<root-managed>$' "$TEST_ROOT/list.out" || return 1
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

test_user_local_wins_over_public_root_collision() {
  local out id pgid
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  write_public_index_fixture "$id" running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  [ ! -s "$SIGMUND_FAKE_SUDO_ARGV" ] || return 1
  pgid_terminated "$pgid" || return 1
  [ -f "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" ]
}

test_explicit_user_target() {
  local out id pgid
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  "$SIGMUND_BIN" stop "user:$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}



test_action_self_elevation_uses_argv_fork_wait() {
  local rc
  write_public_index_fixture abc12345 running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  set +e
  "$SIGMUND_BIN" stop abc12345 >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$SIGMUND_FAKE_SUDO_ARGV"
  [ "${#args[@]}" -eq 6 ] || return 1
  [ "${args[0]}" = "--" ] || return 1
  [ -x "${args[1]}" ] || return 1
  [ "${args[2]}" = "--system" ] || return 1
  [ "${args[3]}" = "--elevated" ] || return 1
  [ "${args[4]}" = "stop" ] || return 1
  [ "${args[5]}" = "system:abc12345" ] || return 1
  ! grep -qx -- 'sh\|-c' "$SIGMUND_FAKE_SUDO_ARGV"
}

test_elevated_action_returns_child_status() {
  local rc
  write_public_index_fixture abc12345 running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  export SIGMUND_FAKE_SUDO_RC=42
  set +e
  "$SIGMUND_BIN" kill abc12345 >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 42 ]
}

test_sudo_exec_failure_returns_clean_error() {
  local rc
  write_public_index_fixture abc12345 running 2026-06-15T18:42:11Z || return 1
  export SIGMUND_TEST_SUDO_PROG="$TEST_ROOT/missing-sudo"
  set +e
  "$SIGMUND_BIN" stop abc12345 >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 127 ] || return 1
  grep -q 'failed to exec sudo' "$TEST_ROOT/stderr"
}

test_tail_ctrl_c_detaches_from_tail_and_keeps_run() {
  command -v setsid >/dev/null 2>&1 || skip "setsid not available"
  local out id pgid tail_pid rc
  out=$("$SIGMUND_BIN" bash -c 'while :; do echo tail-still-running; sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  setsid "$SIGMUND_BIN" tail "$id" >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" &
  tail_pid=$!
  sleep 0.3
  kill -INT "-$tail_pid" 2>/dev/null || kill -INT "$tail_pid" 2>/dev/null || true
  set +e
  wait "$tail_pid"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || return 1
  kill -0 "-$pgid" 2>/dev/null || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_system_switch_canonicalizes_owned_command() {
  local rc
  make_fake_sudo || return 1
  set +e
  "$SIGMUND_BIN" --system stop abc12345 >/dev/null 2>"$TEST_ROOT/one.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  cp "$SIGMUND_FAKE_SUDO_ARGV" "$TEST_ROOT/one.argv" || return 1
  set +e
  "$SIGMUND_BIN" stop abc12345 --system >/dev/null 2>"$TEST_ROOT/two.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  cmp -s "$TEST_ROOT/one.argv" "$SIGMUND_FAKE_SUDO_ARGV" || return 1
  grep -qx -- '--system' "$TEST_ROOT/one.argv" || return 1
  grep -qx -- '--elevated' "$TEST_ROOT/one.argv" || return 1
  tail -n 2 "$TEST_ROOT/one.argv" | grep -qx 'abc12345'
}

test_system_raw_self_elevation_preserves_child_switches_and_delimiter() {
  local rc
  make_fake_sudo || return 1
  set +e
  "$SIGMUND_BIN" --system child-command --system >/dev/null 2>"$TEST_ROOT/raw.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$SIGMUND_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "child-command" ] || return 1
  [ "${args[5]}" = "--system" ] || return 1

  set +e
  "$SIGMUND_BIN" --system -- list --system >/dev/null 2>"$TEST_ROOT/delim.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$SIGMUND_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "--" ] || return 1
  [ "${args[5]}" = "list" ] || return 1
  [ "${args[6]}" = "--system" ] || return 1
}

test_elevated_requires_root() {
  local rc
  set +e
  "$SIGMUND_BIN" --elevated stop abc12345 >/dev/null 2>"$TEST_ROOT/elevated.err"
  rc=$?
  set -e
  [ "$rc" -eq 3 ] || return 1
  grep -q -- '--elevated without root authority' "$TEST_ROOT/elevated.err"
}


test_long_command_list_truncates_instead_of_skips() {
  local out id long_arg list_out
  long_arg=$(printf 'x%.0s' $(seq 1 140))
  out=$("$SIGMUND_BIN" /bin/echo "$long_arg" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  list_out=$("$SIGMUND_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]" || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]].*\.\.\."
}

test_normal_start_writes_user_local_state() {
  local out id mode
  out=$("$SIGMUND_BIN" true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  [ -f "$HOME/.local/state/sigmund/$id.json" ] || return 1
  [ -f "$HOME/.local/state/sigmund/$id.log" ] || return 1
  [ ! -e "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json" ] || return 1
  [ ! -e "$SIGMUND_TEST_SYSTEM_STATE_DIR/logs/$id.log" ] || return 1
  [ ! -e "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" ] || return 1
  mode=$(file_mode "$HOME/.local/state/sigmund/$id.json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$HOME/.local/state/sigmund/$id.log") || return 1
  [ "$mode" = 600 ]
}

test_root_start_writes_system_store_and_public_unknown() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id mode list_out
  out=$(as_root "$SIGMUND_REAL_BIN" true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  root_file_exists "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json" || return 1
  root_file_exists "$SIGMUND_TEST_SYSTEM_STATE_DIR/logs/$id.log" || return 1
  [ -f "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" ] || return 1
  root_path_absent "$ROOT_HOME/.local/state/sigmund/$id.json" || return 1
  mode=$(root_file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(root_file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR/logs/$id.log") || return 1
  [ "$mode" = 600 ] || return 1
  mode=$(file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json") || return 1
  [ "$mode" = 644 ] || return 1
  grep -q '"state_hint": "unknown"' "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  list_out=$("$SIGMUND_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+unknown[[:space:]]"
}

test_sudo_start_writes_system_store_with_invoking_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id json
  out=$(as_sudo_from_user "$SIGMUND_REAL_BIN" true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json="$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json"
  root_file_exists "$json" || return 1
  root_file_exists "$SIGMUND_TEST_SYSTEM_STATE_DIR/logs/$id.log" || return 1
  [ -f "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" ] || return 1
  [ ! -e "$ACTOR_HOME/.local/state/sigmund/$id.json" ] || return 1
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

assert_home_system_start_is_user_local() {
  local out="$1" id json log owner
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  json="$ACTOR_HOME/.local/state/sigmund/$id.json"
  log="$ACTOR_HOME/.local/state/sigmund/$id.log"
  root_file_exists "$json" || return 1
  root_file_exists "$log" || return 1
  root_path_absent "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json" || return 1
  root_path_absent "$SIGMUND_TEST_SYSTEM_STATE_DIR/logs/$id.log" || return 1
  root_path_absent "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/$id.json" || return 1
  owner=$(root_file_owner "$json") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
  owner=$(root_file_owner "$log") || return 1
  [ "$owner" = "$TEST_UID:$TEST_GID" ] || return 1
}

test_sudo_system_start_of_home_executable_uses_user_store() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out
  app=$(make_home_executable) || return 1
  out=$(as_sudo_from_user "$SIGMUND_REAL_BIN" --system "$app" 2>&1) || return 1
  assert_home_system_start_is_user_local "$out"
}

test_home_elevated_run_alias_stays_user_local() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out id aliases
  app=$(make_home_executable) || return 1
  out=$(as_sudo_from_user "$SIGMUND_REAL_BIN" --system "$app" 2>&1) || return 1
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  as_user "$SIGMUND_REAL_BIN" alias "$id" home-elevated >/dev/null || return 1
  aliases="$ACTOR_HOME/.local/state/sigmund/aliases.json"
  root_file_exists "$aliases" || return 1
  root_grep '"home-elevated"' "$aliases" || return 1
  root_path_absent "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/aliases.json" || return 1
}

test_sudo_context_can_stop_unique_user_local_run() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id pgid
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  as_sudo_from_user "$SIGMUND_REAL_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_mund_unified_cli_surface() {
  local out id id2
  out=$("$SIGMUND_BIN" run -- /bin/sh -c 'echo mund-run-line' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$SIGMUND_BIN" logs "$id" >"$TEST_ROOT/mund-logs.out" 2>"$TEST_ROOT/mund-logs.err" || {
    cat "$TEST_ROOT/mund-logs.out" "$TEST_ROOT/mund-logs.err" >&2
    return 1
  }
  grep -q 'mund-run-line' "$TEST_ROOT/mund-logs.out" || { cat "$TEST_ROOT/mund-logs.out" >&2; return 1; }
  "$SIGMUND_BIN" inspect "$id" >"$TEST_ROOT/mund-inspect.out" || return 1
  grep -q 'mund-run-line' "$TEST_ROOT/mund-inspect.out" || return 1
  "$SIGMUND_BIN" status >"$TEST_ROOT/mund-status.out" || return 1
  grep -q "$id" "$TEST_ROOT/mund-status.out" || return 1
  "$SIGMUND_BIN" show runs >"$TEST_ROOT/mund-show-runs.out" || return 1
  grep -q 'ID' "$TEST_ROOT/mund-show-runs.out" || return 1
  "$SIGMUND_BIN" doctor >"$TEST_ROOT/mund-doctor.out" || return 1
  grep -q 'version:' "$TEST_ROOT/mund-doctor.out" || return 1

  out=$("$SIGMUND_BIN" run -- /usr/bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" mund-web >/dev/null || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1
  "$SIGMUND_BIN" profiles >"$TEST_ROOT/mund-profiles.out" || return 1
  grep -q 'mund-web' "$TEST_ROOT/mund-profiles.out" || return 1
  "$SIGMUND_BIN" profile list >"$TEST_ROOT/mund-profile-list.out" || return 1
  grep -q 'mund-web' "$TEST_ROOT/mund-profile-list.out" || return 1
  "$SIGMUND_BIN" show profiles >"$TEST_ROOT/mund-show-profiles.out" || return 1
  grep -q 'mund-web' "$TEST_ROOT/mund-show-profiles.out" || return 1
  out=$("$SIGMUND_BIN" profile run mund-web 2>&1) || return 1
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$SIGMUND_BIN" show running mund-web >"$TEST_ROOT/mund-show-running.out" || return 1
  grep -q "$id2" "$TEST_ROOT/mund-show-running.out" || { cat "$TEST_ROOT/mund-show-running.out" >&2; return 1; }
  "$SIGMUND_BIN" stop "$id2" >/dev/null || return 1
  "$SIGMUND_BIN" clean mund-web >/dev/null || return 1
}

test_build_artifact_coexistence() {
  make clean >/dev/null || return 1
  make sigmund mund STATIC_LDFLAGS= EXTRA_CPPFLAGS=-DSIGMUND_TESTING >/dev/null || return 1
  [ -x ./sigmund ] && [ -x ./mund ] || return 1
  make sigmund-dynamic EXTRA_CPPFLAGS=-DSIGMUND_TESTING >/dev/null || return 1
  [ -x ./sigmund ] && [ -x ./mund ] && [ -x ./sigmund-dynamic ] || return 1
  [ -e ./sigmund ] && [ -e ./mund ] && [ -e ./sigmund-dynamic ]
}

test_concurrent_unique_ids() {
  local i id ids uniq
  ids=""
  for i in $(seq 1 20); do
    "$SIGMUND_BIN" sleep 60 >"$TEST_ROOT/start.$i.out" 2>"$TEST_ROOT/start.$i.err" &
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
    "$SIGMUND_BIN" kill "$id" >/dev/null 2>&1 || true
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
    "SIGMUND_TEST_SYSTEM_STATE_DIR=$SIGMUND_TEST_SYSTEM_STATE_DIR"
    "PATH=$PATH"
    "SUDO_UID=0" "SUDO_GID=0" "SUDO_USER=root"
  )
  [ "${SIGMUND_BOOT_ID_PATH+x}" = x ] && args+=("SIGMUND_BOOT_ID_PATH=$SIGMUND_BOOT_ID_PATH")
  if [ "$USER_ACTOR_NEEDS_SUDO" -eq 1 ]; then
    "$SUDO_BIN" -n -u "$TEST_USER" env "${args[@]}" "$@"
  else
    env "${args[@]}" "$@"
  fi
}

# Pin a root system alias 'web-sys' with a safe (root-owned, 0755, no-space-path)
# sigmund copy and a passing visudo stub. Call directly (NOT in $(...)) so the
# exported SIGMUND_TEST_SUDOERS_DIR/VISUDO_PROG reach the caller; sets globals
# GRANT_SAFE and GRANT_SUDOERS_DIR.
grant_fixture() {
  local id
  GRANT_SAFE="$TEST_ROOT/sigmund-safe"
  cp "$SIGMUND_REAL_BIN" "$GRANT_SAFE" || return 1
  as_root chown 0:0 "$GRANT_SAFE" || return 1
  as_root chmod 755 "$GRANT_SAFE" || return 1
  GRANT_SUDOERS_DIR="$TEST_ROOT/sudoers.d"
  mkdir -p "$GRANT_SUDOERS_DIR" || return 1
  chmod 755 "$GRANT_SUDOERS_DIR" || return 1
  export SIGMUND_TEST_SUDOERS_DIR="$GRANT_SUDOERS_DIR"
  GRANT_VISUDO_OK="$TEST_ROOT/visudo-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$GRANT_VISUDO_OK" || return 1
  chmod 755 "$GRANT_VISUDO_OK" || return 1
  export SIGMUND_TEST_VISUDO_PROG="$GRANT_VISUDO_OK"
  id=$(as_root "$GRANT_SAFE" /bin/sh -c ':' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  as_root "$GRANT_SAFE" alias "$id" web-sys >/dev/null 2>&1 || return 1
}

test_system_store_directory_modes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id d mode
  id=$(as_root "$SIGMUND_REAL_BIN" true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  for d in runs logs console; do
    mode=$(root_file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR/$d") || return 1
    [ "$mode" = 700 ] || { echo "$d/ mode=$mode (want 700 -- private state would be world-visible)" >&2; return 1; }
  done
  mode=$(root_file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR") || return 1
  [ "$mode" = 755 ] || { echo "base mode=$mode (want 755)" >&2; return 1; }
  mode=$(root_file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR/public") || return 1
  [ "$mode" = 755 ]
}

test_system_store_tightens_preexisting_loose_dir() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id mode
  as_root mkdir -p "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs" || return 1
  as_root chmod 0777 "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs" || return 1
  id=$(as_root "$SIGMUND_REAL_BIN" true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  mode=$(root_file_mode "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs") || return 1
  [ "$mode" = 700 ] || { echo "pre-existing loose runs/ not tightened: mode=$mode" >&2; return 1; }
}

test_system_store_refuses_symlinked_critical_dirs() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local old_store name root state target rc mode
  old_store="$SIGMUND_TEST_SYSTEM_STATE_DIR"
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
    SIGMUND_TEST_SYSTEM_STATE_DIR="$state"
    export SIGMUND_TEST_SYSTEM_STATE_DIR
    set +e
    as_root "$SIGMUND_REAL_BIN" true >/dev/null 2>"$TEST_ROOT/syslink-$name.err"
    rc=$?
    set -e
    SIGMUND_TEST_SYSTEM_STATE_DIR="$old_store"
    export SIGMUND_TEST_SYSTEM_STATE_DIR
    [ "$rc" -ne 0 ] || { echo "system store accepted symlinked $name directory" >&2; return 1; }
    mode=$(root_file_mode "$target") || return 1
    [ "$mode" = 777 ] || { echo "symlink target for $name was chmod-followed: mode=$mode" >&2; return 1; }
  done
}

test_system_store_artifacts_owned_by_root() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id owner
  id=$(as_root "$SIGMUND_REAL_BIN" true 2>&1 | extract_id)
  [ -n "$id" ] || return 1
  owner=$(root_file_owner "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json") || return 1
  [ "$owner" = "0:0" ] || { echo "record owner=$owner (want 0:0)" >&2; return 1; }
  owner=$(root_file_owner "$SIGMUND_TEST_SYSTEM_STATE_DIR/logs/$id.log") || return 1
  [ "$owner" = "0:0" ] || { echo "log owner=$owner (want 0:0)" >&2; return 1; }
}

test_nonroot_ignores_spoofed_sudo_provenance() {
  local out id json
  out=$(as_user_spoof_sudo "$SIGMUND_REAL_BIN" true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json="$ACTOR_HOME/.local/state/sigmund/$id.json"
  [ -e "$json" ] || { echo "spoofed-SUDO run did not land in the user store" >&2; return 1; }
  [ ! -e "$SIGMUND_TEST_SYSTEM_STATE_DIR/runs/$id.json" ] || { echo "spoofed SUDO_* escalated into the system store" >&2; return 1; }
  if grep -q '"invoked_via_sudo": true' "$json" 2>/dev/null; then
    echo "spoofed SUDO_* was trusted by a non-root process" >&2
    return 1
  fi
}

test_aliases_json_symlink_not_followed() {
  local out id aliases attacker
  out=$("$SIGMUND_BIN" /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" myweb >/dev/null 2>&1 || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
  aliases="$HOME/.local/state/sigmund/aliases.json"
  [ -f "$aliases" ] || return 1
  # sanity: the alias is visible when the file is a real regular file
  "$SIGMUND_BIN" aliases 2>/dev/null | grep -q myweb || { echo "alias not visible before symlink (test setup)" >&2; return 1; }
  # replace it with a symlink to an identical attacker-controlled file
  attacker="$TEST_ROOT/attacker-aliases.json"
  cp "$aliases" "$attacker" || return 1
  rm -f "$aliases"
  ln -s "$attacker" "$aliases" || return 1
  # O_NOFOLLOW must reject the symlinked alias dictionary: myweb must not load
  if "$SIGMUND_BIN" aliases 2>/dev/null | grep -q myweb; then
    echo "symlinked aliases.json was followed (myweb loaded through the symlink)" >&2
    return 1
  fi
}

test_alias_profile_atomic_writers_ignore_fixed_temp_attacks() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id store fixed attacker before leftovers

  ensure_user_fixture_store || return 1
  store="$HOME/.local/state/sigmund"
  attacker="$TEST_ROOT/user-alias-attacker"
  printf 'do-not-touch-alias-symlink\n' >"$attacker" || return 1
  fixed="$store/.aliases.tmp"
  ln -s "$attacker" "$fixed" || return 1
  out=$("$SIGMUND_BIN" /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" fixedlink >/dev/null 2>&1 || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$(cat "$attacker")" = "do-not-touch-alias-symlink" ] || { echo "alias writer followed fixed symlink temp" >&2; return 1; }
  [ -L "$fixed" ] || { echo "alias writer replaced fixed symlink temp" >&2; return 1; }
  rm -f "$fixed" || return 1
  printf 'do-not-touch-alias-file\n' >"$fixed" || return 1
  out=$("$SIGMUND_BIN" /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" fixedfile >/dev/null 2>&1 || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$(cat "$fixed")" = "do-not-touch-alias-file" ] || { echo "alias writer truncated fixed temp file" >&2; return 1; }
  leftovers=$(find "$store" -maxdepth 1 -name '.aliases.*.tmp' -print 2>/dev/null || true)
  [ -z "$leftovers" ] || { echo "alias writer left temp files: $leftovers" >&2; return 1; }

  # System aliases write both profiles.json in the private base and aliases.json
  # in public; cover fixed symlink and fixed regular temp names for both writers.
  out=$(as_root "$SIGMUND_REAL_BIN" /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf 'do-not-touch-profile-symlink\n' >"$TEST_ROOT/profile-attacker" || return 1
  printf 'do-not-touch-public-alias-symlink\n' >"$TEST_ROOT/public-alias-attacker" || return 1
  as_root ln -s "$TEST_ROOT/profile-attacker" "$SIGMUND_TEST_SYSTEM_STATE_DIR/.profiles.tmp" || return 1
  as_root ln -s "$TEST_ROOT/public-alias-attacker" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp" || return 1
  as_root "$SIGMUND_REAL_BIN" alias "$id" syslink >/dev/null 2>&1 || return 1
  [ "$(cat "$TEST_ROOT/profile-attacker")" = "do-not-touch-profile-symlink" ] || { echo "profile writer followed fixed symlink temp" >&2; return 1; }
  [ "$(cat "$TEST_ROOT/public-alias-attacker")" = "do-not-touch-public-alias-symlink" ] || { echo "system alias writer followed fixed symlink temp" >&2; return 1; }
  root_file_exists "$SIGMUND_TEST_SYSTEM_STATE_DIR/profiles.json" || return 1
  root_file_exists "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/aliases.json" || return 1
  as_root rm -f "$SIGMUND_TEST_SYSTEM_STATE_DIR/.profiles.tmp" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp" || return 1

  out=$(as_root "$SIGMUND_REAL_BIN" /bin/sleep 61 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_root sh -c 'printf "%s\n" "do-not-touch-profile-file" >"$1"; printf "%s\n" "do-not-touch-public-alias-file" >"$2"' sh \
    "$SIGMUND_TEST_SYSTEM_STATE_DIR/.profiles.tmp" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp" || return 1
  as_root "$SIGMUND_REAL_BIN" alias "$id" sysfile >/dev/null 2>&1 || return 1
  before=$(as_root cat "$SIGMUND_TEST_SYSTEM_STATE_DIR/.profiles.tmp") || return 1
  [ "$before" = "do-not-touch-profile-file" ] || { echo "profile writer truncated fixed temp file" >&2; return 1; }
  before=$(as_root cat "$SIGMUND_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp") || return 1
  [ "$before" = "do-not-touch-public-alias-file" ] || { echo "system alias writer truncated fixed temp file" >&2; return 1; }
  leftovers=$(as_root find "$SIGMUND_TEST_SYSTEM_STATE_DIR" "$SIGMUND_TEST_SYSTEM_STATE_DIR/public" -maxdepth 1 \
    \( -name '.profiles.*.tmp' -o -name '.aliases.*.tmp' \) -print 2>/dev/null || true)
  [ -z "$leftovers" ] || { echo "system alias/profile writer left temp files: $leftovers" >&2; return 1; }
}

test_grant_sudoers_file_is_root_only() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe sudoers_file mode owner
  grant_fixture || return 1
  as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/grant.err" || { cat "$TEST_ROOT/grant.err" >&2; return 1; }
  sudoers_file="$SIGMUND_TEST_SUDOERS_DIR/sigmund_web-sys_$TEST_USER"
  root_file_exists "$sudoers_file" || return 1
  mode=$(root_file_mode "$sudoers_file") || return 1
  [ "$mode" = 440 ] || { echo "managed sudoers mode=$mode (want 440)" >&2; return 1; }
  owner=$(root_file_owner "$sudoers_file") || return 1
  [ "$owner" = "0:0" ] || { echo "managed sudoers owner=$owner (want 0:0)" >&2; return 1; }
}

test_grant_aborts_when_visudo_rejects() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe visudo_bad sudoers_file rc
  grant_fixture || return 1
  visudo_bad="$TEST_ROOT/visudo-bad"
  printf '#!/usr/bin/env sh\nexit 1\n' >"$visudo_bad" || return 1
  chmod 755 "$visudo_bad" || return 1
  export SIGMUND_TEST_VISUDO_PROG="$visudo_bad"
  sudoers_file="$SIGMUND_TEST_SUDOERS_DIR/sigmund_web-sys_$TEST_USER"
  set +e
  as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/visudo.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "grant succeeded despite visudo rejecting the candidate" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "unvalidated sudoers file was installed" >&2; return 1; }
  root_path_absent "$sudoers_file.tmp" 2>/dev/null || true
}

test_grant_refuses_unsafe_self_binary() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe bad sudoers_file rc
  grant_fixture || return 1
  sudoers_file="$SIGMUND_TEST_SUDOERS_DIR/sigmund_web-sys_$TEST_USER"
  # (a) non-root-owned sigmund binary
  bad="$TEST_ROOT/sigmund-userowned"
  cp "$SIGMUND_REAL_BIN" "$bad" || return 1
  as_root chown "$TEST_UID:$TEST_GID" "$bad" || return 1
  as_root chmod 755 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/u.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a non-root-owned binary" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via non-root-owned binary" >&2; return 1; }
  # (b) group/world-writable sigmund binary
  bad="$TEST_ROOT/sigmund-writable"
  cp "$SIGMUND_REAL_BIN" "$bad" || return 1
  as_root chown 0:0 "$bad" || return 1
  as_root chmod 0777 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/w.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a world-writable binary" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via world-writable binary" >&2; return 1; }
  # (c) whitespace in the binary path
  mkdir -p "$TEST_ROOT/bad dir" || return 1
  bad="$TEST_ROOT/bad dir/sigmund"
  cp "$SIGMUND_REAL_BIN" "$bad" || return 1
  as_root chown 0:0 "$bad" || return 1
  as_root chmod 755 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/s.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a whitespace-in-path binary" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via whitespace-path binary" >&2; return 1; }
}

test_signal_refuses_tampered_pgid() {
  local out id rec rc
  out=$("$SIGMUND_BIN" /bin/sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rec="$HOME/.local/state/sigmund/$id.json"
  # A record whose pgid is tampered to <=1 must never be signaled -- kill(-pgid)
  # with pgid<=1 would hit pid 1 / the whole session. valid_record rejects it, so
  # stop refuses with exit 5 and the real process is left untouched.
  sed -i.bak 's/"pgid":[[:space:]]*[0-9][0-9]*/"pgid":1/' "$rec" || return 1
  set +e; "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1; rc=$?; set -e
  mv "$rec.bak" "$rec" 2>/dev/null || true
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$rc" -eq 5 ] || { echo "stop on tampered pgid<=1: rc=$rc (want 5 refused)" >&2; return 1; }
}

test_elevated_child_signal_maps_to_128_plus_sig() {
  local rc fakesudo
  write_public_index_fixture abc12345 running 2026-06-15T18:42:11Z || return 1
  fakesudo="$TEST_ROOT/fakebin-sig/sudo"
  mkdir -p "$TEST_ROOT/fakebin-sig" || return 1
  printf '#!/usr/bin/env bash\nkill -TERM $$\nsleep 5\n' >"$fakesudo" || return 1
  chmod 755 "$TEST_ROOT/fakebin-sig" "$fakesudo" || return 1
  export SIGMUND_TEST_SUDO_PROG="$fakesudo"
  set +e; "$SIGMUND_BIN" kill abc12345 >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 143 ] || { echo "signal-killed sudo child: rc=$rc (want 143 = 128+SIGTERM)" >&2; return 1; }
}

test_public_index_write_rollback() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local rc pids leftover
  set +e
  SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE=1 as_root "$SIGMUND_REAL_BIN" bash -c 'exec -a sigmund_pubidx_test sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "public-index rollback start: rc=$rc (want 1)" >&2; return 1; }
  pids=$(ps -eo pid=,args= | awk '/sigmund_pubidx_test/ && !/awk/ {print $1}')
  [ -z "$pids" ] || { echo "leaked process after public-index rollback" >&2; return 1; }
  leftover=$(as_root sh -c 'ls "$1"/runs/*.json "$1"/public/*.json 2>/dev/null' sh "$SIGMUND_TEST_SYSTEM_STATE_DIR" 2>/dev/null || true)
  [ -z "$leftover" ] || { echo "leftover state after rollback: $leftover" >&2; return 1; }
}

test_quiet_suppresses_banner_keeps_id() {
  local out id
  out=$("$SIGMUND_BIN" --quiet /bin/sleep 300 2>"$TEST_ROOT/q.err") || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { echo "no id from --quiet start" >&2; return 1; }
  [ "$out" = "$id" ] || { echo "--quiet stdout is not the bare id: [$out]" >&2; return 1; }
  [ ! -s "$TEST_ROOT/q.err" ] || { echo "--quiet start wrote to stderr:" >&2; cat "$TEST_ROOT/q.err" >&2; return 1; }
  "$SIGMUND_BIN" --quiet stop "$id" >/dev/null 2>"$TEST_ROOT/q2.err" || return 1
  [ ! -s "$TEST_ROOT/q2.err" ] || { echo "--quiet stop wrote to stderr" >&2; return 1; }
}

test_run_id_prefix_resolution() {
  local out id pgid pfx got
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  pfx=$(printf '%s' "$id" | cut -c1-4)
  got=$("$SIGMUND_BIN" stop --print "$pfx") || { echo "id-prefix did not resolve" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { echo "prefix resolved to wrong run: [$got]" >&2; return 1; }
  "$SIGMUND_BIN" stop "$id" >/dev/null 2>&1 || true
}

test_ambiguous_tail_resolvable_by_run_id() {
  local id1 id2 rc tpid
  # An alias whose runs emit output, so tailing one by id has something to follow.
  id1=$("$SIGMUND_BIN" sh -c 'while :; do echo tick; sleep 0.2; done' 2>&1 | extract_id) || return 1
  "$SIGMUND_BIN" alias "$id1" web-amb >/dev/null || return 1
  "$SIGMUND_BIN" stop "$id1" >/dev/null; "$SIGMUND_BIN" prune "$id1" >/dev/null
  id1=$("$SIGMUND_BIN" start web-amb 2>&1 | extract_id); [ -n "$id1" ] || return 1
  id2=$("$SIGMUND_BIN" start web-amb --multi 2>&1 | extract_id); [ -n "$id2" ] || return 1
  # tail by ALIAS with several running is ambiguous: exit 6, the candidate run ids
  # are listed, and (since tail is not an --all command) --all must NOT be suggested.
  set +e; "$SIGMUND_BIN" tail web-amb >/dev/null 2>"$TEST_ROOT/amb.err"; rc=$?; set -e
  [ "$rc" -eq 6 ] || { echo "ambiguous tail: rc=$rc (want 6)" >&2; return 1; }
  grep -q 'matches more than one' "$TEST_ROOT/amb.err" || { cat "$TEST_ROOT/amb.err" >&2; return 1; }
  ! grep -q -- '--all' "$TEST_ROOT/amb.err" || { echo "tail ambiguity wrongly suggested --all" >&2; return 1; }
  grep -q "$id1" "$TEST_ROOT/amb.err" && grep -q "$id2" "$TEST_ROOT/amb.err" || { echo "ambiguity did not list the candidate run ids" >&2; return 1; }
  # but you can always tail one specific instance BY RUN ID -- that is never ambiguous.
  "$SIGMUND_BIN" tail "$id1" >"$TEST_ROOT/byid.out" 2>"$TEST_ROOT/byid.err" &
  tpid=$!
  sleep 0.6
  kill "$tpid" 2>/dev/null
  wait "$tpid" 2>/dev/null || true
  ! grep -q 'matches more than one' "$TEST_ROOT/byid.err" || { echo "tail by run id was wrongly treated as ambiguous" >&2; return 1; }
  grep -q tick "$TEST_ROOT/byid.out" || { echo "tail by run id followed nothing" >&2; cat "$TEST_ROOT/byid.err" >&2; return 1; }
  "$SIGMUND_BIN" stop web-amb --all >/dev/null 2>&1 || true
}

test_misc_action_guards() {
  local rc
  set +e; "$SIGMUND_BIN" --console stop deadbeef >/dev/null 2>"$TEST_ROOT/c.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--console stop: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'only to starts' "$TEST_ROOT/c.err" || { cat "$TEST_ROOT/c.err" >&2; return 1; }
  set +e; "$SIGMUND_BIN" help bogustopic >/dev/null 2>"$TEST_ROOT/h.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "help bogustopic: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'unknown help topic' "$TEST_ROOT/h.err" || { cat "$TEST_ROOT/h.err" >&2; return 1; }
  "$SIGMUND_BIN" list -l >"$TEST_ROOT/l.out" 2>&1 || { echo "list -l failed" >&2; return 1; }
  grep -q 'STARTED_AT' "$TEST_ROOT/l.out" || { echo "list -l missing ISO header" >&2; return 1; }
}

test_owned_command_exact_arity() {
  local rc
  set +e; "$SIGMUND_BIN" tail deadbeef extra >/dev/null 2>"$TEST_ROOT/tail-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: sigmund tail <target>' "$TEST_ROOT/tail-extra.err" || { cat "$TEST_ROOT/tail-extra.err" >&2; return 1; }

  set +e; "$SIGMUND_BIN" dump deadbeef extra >/dev/null 2>"$TEST_ROOT/dump-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "dump extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: sigmund dump <target>' "$TEST_ROOT/dump-extra.err" || { cat "$TEST_ROOT/dump-extra.err" >&2; return 1; }

  set +e; "$SIGMUND_BIN" console deadbeef extra >/dev/null 2>"$TEST_ROOT/console-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "console extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: sigmund console <target>' "$TEST_ROOT/console-extra.err" || { cat "$TEST_ROOT/console-extra.err" >&2; return 1; }

  set +e; "$SIGMUND_BIN" prune deadbeef extra >/dev/null 2>"$TEST_ROOT/prune-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "prune extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: sigmund prune \[target|all\] \[--all\]' "$TEST_ROOT/prune-extra.err" || { cat "$TEST_ROOT/prune-extra.err" >&2; return 1; }

  set +e; "$SIGMUND_BIN" tail --all deadbeef >/dev/null 2>"$TEST_ROOT/tail-all.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail --all: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: sigmund tail <target>' "$TEST_ROOT/tail-all.err" || { cat "$TEST_ROOT/tail-all.err" >&2; return 1; }

  "$SIGMUND_BIN" prune >/dev/null || { echo "prune with zero targets should remain valid" >&2; return 1; }
}

test_grant_revoke_argument_refusals() {
  local rc
  # non-root cannot grant (privilege refusal)
  set +e; "$SIGMUND_BIN" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/ng.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "non-root grant unexpectedly succeeded" >&2; return 1; }
  # root: invalid action and invalid subject are rejected
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  grant_fixture || return 1
  set +e; as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" bogusaction >/dev/null 2>"$TEST_ROOT/ba.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "bad action: rc=$rc (want 5)" >&2; cat "$TEST_ROOT/ba.err" >&2; return 1; }
  set +e; as_root "$GRANT_SAFE" grant web-sys 'bad..subject' start >/dev/null 2>"$TEST_ROOT/bs.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "bad subject: rc=$rc (want 5)" >&2; cat "$TEST_ROOT/bs.err" >&2; return 1; }
}

test_multi_n_exact_count_and_invalid() {
  local id ids running rc
  id=$("$SIGMUND_BIN" sleep 60 2>&1 | extract_id) || return 1
  "$SIGMUND_BIN" alias "$id" web-n >/dev/null || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null; "$SIGMUND_BIN" prune "$id" >/dev/null
  ids=$("$SIGMUND_BIN" start web-n --multi 3 2>/dev/null | sed -n '/^[0-9a-f]\{8\}$/p')
  [ "$(printf '%s\n' "$ids" | grep -c .)" -eq 3 ] || { echo "--multi 3 did not print 3 ids" >&2; return 1; }
  running=$("$SIGMUND_BIN" list 2>/dev/null | grep -c running || true)
  [ "$running" -ge 3 ] || { echo "expected >=3 running, got $running" >&2; return 1; }
  set +e; "$SIGMUND_BIN" start web-n --multi=abc >/dev/null 2>"$TEST_ROOT/m.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi=abc: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --multi count" "$TEST_ROOT/m.err" || { cat "$TEST_ROOT/m.err" >&2; return 1; }
  set +e; "$SIGMUND_BIN" start web-n --multi abc >/dev/null 2>"$TEST_ROOT/ms.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi abc: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --multi count 'abc'" "$TEST_ROOT/ms.err" || { cat "$TEST_ROOT/ms.err" >&2; return 1; }
  set +e; "$SIGMUND_BIN" start web-n --multi 0 >/dev/null 2>"$TEST_ROOT/mz.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi 0: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --multi count '0'" "$TEST_ROOT/mz.err" || { cat "$TEST_ROOT/mz.err" >&2; return 1; }
  "$SIGMUND_BIN" stop web-n --all >/dev/null 2>&1 || true
}

test_tail_cannot_follow_multiple_starts() {
  local id rc running
  id=$("$SIGMUND_BIN" sleep 60 2>&1 | extract_id) || return 1
  "$SIGMUND_BIN" alias "$id" web-t >/dev/null || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null; "$SIGMUND_BIN" prune "$id" >/dev/null
  set +e; "$SIGMUND_BIN" start web-t --multi 2 --tail >/dev/null 2>"$TEST_ROOT/t.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi 2 --tail: rc=$rc (want 5)" >&2; return 1; }
  grep -q "cannot follow multiple starts" "$TEST_ROOT/t.err" || { cat "$TEST_ROOT/t.err" >&2; return 1; }
  running=$("$SIGMUND_BIN" list 2>/dev/null | grep -c running || true)
  [ "$running" -eq 0 ] || { echo "runs created despite refusal ($running)" >&2; "$SIGMUND_BIN" stop web-t --all >/dev/null 2>&1; return 1; }
}

test_print_over_all_and_multiple() {
  local id1 id2 pgid1 pgid2 out
  id1=$("$SIGMUND_BIN" sleep 60 2>&1 | extract_id) || return 1
  "$SIGMUND_BIN" alias "$id1" web-pr >/dev/null || return 1
  "$SIGMUND_BIN" stop "$id1" >/dev/null; "$SIGMUND_BIN" prune "$id1" >/dev/null
  id1=$("$SIGMUND_BIN" start web-pr 2>&1 | extract_id); pgid1=$(record_pgid "$id1")
  id2=$("$SIGMUND_BIN" start web-pr --multi 2>&1 | extract_id); pgid2=$(record_pgid "$id2")
  [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  out=$("$SIGMUND_BIN" stop --print --all web-pr) || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" || { echo "--print --all missing pgid1" >&2; return 1; }
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "--print --all missing pgid2" >&2; return 1; }
  out=$("$SIGMUND_BIN" stop --print "$id1" "$id2") || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" && printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "explicit multi --print missing a pgid" >&2; return 1; }
  "$SIGMUND_BIN" stop web-pr --all >/dev/null 2>&1 || true
}

run_test "--multi N starts exactly N and rejects a bad count" test_multi_n_exact_count_and_invalid
run_test "--tail cannot follow a multi start (exit 5, nothing started)" test_tail_cannot_follow_multiple_starts
run_test "--print spans --all and multiple explicit targets" test_print_over_all_and_multiple
run_test "stop refuses a record with a tampered pgid<=1 (exit 5)" test_signal_refuses_tampered_pgid
run_test "signal-killed elevation child maps to 128+sig" test_elevated_child_signal_maps_to_128_plus_sig
run_test "public-index write failure rolls back the start" test_public_index_write_rollback
run_test "--quiet prints bare id and silences stderr" test_quiet_suppresses_banner_keeps_id
run_test "run id prefix resolves to the full run" test_run_id_prefix_resolution
run_test "ambiguous alias tail lists ids; tail by run id still resolves" test_ambiguous_tail_resolvable_by_run_id
run_test "action/help/list argument guards" test_misc_action_guards
run_test "owned commands reject extra targets and unsupported --all" test_owned_command_exact_arity
run_test "grant/revoke argument and privilege refusals" test_grant_revoke_argument_refusals
run_test "system store directory modes are private (0700/0755)" test_system_store_directory_modes
run_test "system store tightens a pre-existing loose dir" test_system_store_tightens_preexisting_loose_dir
run_test "system store refuses symlinked critical dirs without chmod-following" test_system_store_refuses_symlinked_critical_dirs
run_test "system store artifacts are owned by root:root" test_system_store_artifacts_owned_by_root
run_test "non-root process ignores spoofed SUDO_* provenance" test_nonroot_ignores_spoofed_sudo_provenance
run_test "symlinked aliases.json is not followed (O_NOFOLLOW)" test_aliases_json_symlink_not_followed
run_test "alias/profile atomic writers ignore fixed temp file and symlink attacks" test_alias_profile_atomic_writers_ignore_fixed_temp_attacks
run_test "managed sudoers file is mode 0440 root:root" test_grant_sudoers_file_is_root_only
run_test "grant aborts and writes nothing when visudo rejects" test_grant_aborts_when_visudo_rejects
run_test "grant refuses an unsafe sigmund self-binary" test_grant_refuses_unsafe_self_binary
run_test "start/stop lifecycle" test_lifecycle
run_test "kill subcommand kills process group" test_kill_subcommand
run_test "start output includes stop helper" test_start_output_stop_hint
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
run_test "mund unified CLI surface" test_mund_unified_cli_surface
run_test "special characters are preserved in argv JSON" test_special_chars_args
run_test "logging captures stdout+stderr" test_log_capture
run_test "-f starts and follows output" test_start_follow_short_form
run_test "tail <id> tails an existing run log" test_tail_verb_existing_id
run_test "persistent stale records remain visible and dumpable" test_persistent_stale_records
run_test "missing boot source does not force stale" test_boot_unavailable_does_not_force_stale
run_test "leader zombie with live group remains running" test_leader_zombie_group_still_running
run_test "tail <id> prints finished log output" test_tail_finished_log_prints_existing_output
run_test "--console works without an external attach tool" test_console_does_not_require_external_attach_tool
run_test "console reports a normal run has no console" test_console_reports_non_console_run
run_test "console attach round-trips and tees to the log" test_console_round_trip_and_log_tee
run_test "console rejects unrelated peer UID before replay" test_console_rejects_unrelated_peer_uid_before_replay
run_test "console can reattach after detach" test_console_can_reattach_after_detach
run_test "console socket lives in store dir, not /tmp, for long paths" test_console_socket_lives_in_store_dir
run_test "console target runs in caller cwd (relative-bind restores cwd)" test_console_target_runs_in_caller_cwd
run_test "prune <id> removes exactly one run record/output" test_prune_by_id
run_test "prune all removes prunable while preserving running" test_prune_all_keeps_running
run_test "transactional launch rollback on record write failure" test_transactional_record_write_failure
run_test "raw start does not steal trailing --system" test_raw_start_does_not_steal_trailing_system
run_test "long command appears in list, truncated with ..." test_long_command_list_truncates_instead_of_skips
run_test "normal start writes user-local state" test_normal_start_writes_user_local_state
run_test "root starts use system store and public state is unknown" test_root_start_writes_system_store_and_public_unknown
run_test "sudo start writes system state with invoking-user metadata" test_sudo_start_writes_system_store_with_invoking_metadata
run_test "sudo --system home executable uses invoking-user store" test_sudo_system_start_of_home_executable_uses_user_store
run_test "alias for elevated home executable stays user-local" test_home_elevated_run_alias_stays_user_local
run_test "sudo context can stop unique invoking-user local run" test_sudo_context_can_stop_unique_user_local_run
run_test "public root index rows are redacted in normal list" test_public_root_index_list_is_redacted
run_test "normal run does not self-elevate on local/root ID conflict" test_user_local_wins_over_public_root_collision
run_test "explicit user:<id> targets user-local run" test_explicit_user_target
run_test "user alias stores a direct recipe and starts/stops by alias" test_alias_profile_map_start_and_stop
run_test "alias from relative executable keeps absolute recorded argv0" test_alias_from_relative_executable_uses_recorded_absolute_argv0
run_test "alias start requires --multi when already running and --all stops all" test_alias_multi_gate_and_all_stop
run_test "profile start inherits current environment" test_profile_start_inherits_current_environment
run_test "profile transcript import/export round-trips a user-local recipe" test_profile_transcript_import_export_roundtrip
run_test "profile JSON export/import round-trips a user-local recipe" test_profile_json_export_and_import
run_test "invalid alias names are rejected" test_invalid_alias_names_rejected
run_test "short hex-looking alias names are allowed" test_short_hex_alias_name_allowed
run_test "system alias action self-elevates alias token" test_system_alias_action_self_elevates_alias
run_test "system alias start self-elevates alias token" test_system_alias_start_self_elevates_alias
run_test "grant/revoke writes hash-scoped sudoers entries" test_grant_revoke_writes_hash_scoped_sudoers
run_test "elevated capability start/stop validates alias and hash" test_elevated_capability_start_and_stop_validate_alias_hash
run_test "action self-elevation uses argv-preserving sudo fork+wait" test_action_self_elevation_uses_argv_fork_wait
run_test "elevated action returns child/root-sigmund status" test_elevated_action_returns_child_status
run_test "sudo exec failure returns clean error" test_sudo_exec_failure_returns_clean_error
run_test "tail Ctrl-C detaches from tail and does not stop run" test_tail_ctrl_c_detaches_from_tail_and_keeps_run
run_test "--system owned commands canonicalize sudo argv" test_system_switch_canonicalizes_owned_command
run_test "--system raw self-elevation preserves child args and delimiter" test_system_raw_self_elevation_preserves_child_switches_and_delimiter
run_test "--elevated requires root authority" test_elevated_requires_root
run_test "build artifacts for static and dynamic coexist" test_build_artifact_coexistence
run_test "concurrent starts produce unique ids" test_concurrent_unique_ids

echo "----------------------------------------------------------------"
echo "summary: $PASSES passed, $FAILS failed, $SKIPS skipped"
if [ "$SKIPS" -ne 0 ]; then
  echo "  (some tests skipped; run as root and/or set SIGMUND_REQUIRE_ROOT_TESTS=1 for full coverage)"
fi
if [ "$FAILS" -ne 0 ]; then
  exit 1
fi
