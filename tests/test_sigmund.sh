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

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }

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
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 127
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
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 127
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
  export HOME TEST_ROOT SIGMUND_TEST_SYSTEM_STATE_DIR ROOT_HOME ACTOR_HOME
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

root_file_exists() {
  as_root test -f "$1"
}

root_path_absent() {
  as_root sh -c '[ ! -e "$1" ]' sh "$1"
}

root_file_mode() {
  as_root stat -c '%a' "$1" 2>/dev/null || as_root stat -f '%Lp' "$1"
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

run_test() {
  local desc="$1" fn="$2" rc
  new_env || { fail "$desc"; return; }
  export HOME TEST_ROOT SIGMUND_BIN SIGMUND_REAL_BIN SIGMUND_TEST_SYSTEM_STATE_DIR ROOT_HOME ACTOR_HOME
  export TEST_USER TEST_UID TEST_GID USER_ACTOR_NEEDS_SUDO ROOT_ACTOR_AVAILABLE SUDO_BIN
  export SIGMUND_ACTOR_HOME SIGMUND_ACTOR_USER SIGMUND_ACTOR_SUDO_BIN SIGMUND_USER_ACTOR_NEEDS_SUDO
  set +e
  ( set -Eeuo pipefail; "$fn" )
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    pass "$desc"
  else
    fail "$desc"
  fi
  cleanup_env
}

setup_suite_actors

extract_id() {
  sed -n -e '/^[0-9a-f]\{8\}$/p' -e 's/^sigmund: id=\([0-9a-f][0-9a-f]*\).*/\1/p' | head -n1
}

record_pgid() {
  local id="$1" store="${2:-$HOME/.local/state/sigmund}"
  sed -n 's/.*"pgid":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$store/$id.json" | head -n1
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

test_lifecycle() {
  local out id lines
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$out" | grep -Eq "^sigmund  started  $id[[:space:]]+sleep 300$"
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
  printf 'garbage\n' > "$HOME/.local/state/sigmund/badbad.json" || return 1
  "$SIGMUND_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^badbad' "$TEST_ROOT/list.out"
  ! grep -Eq '^0[[:space:]]' "$TEST_ROOT/list.out"
  grep -q 'warning: skipping corrupt record badbad.json' "$TEST_ROOT/list.err"
  "$SIGMUND_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/sigmund/badbad.json" ]
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
  local out id bootfile list_out
  bootfile="$TEST_ROOT/fake_boot_id"
  printf 'boot-a\n' >"$bootfile" || return 1
  out=$(SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  rm -f "$bootfile"
  list_out=$(SIGMUND_BOOT_ID_PATH="$bootfile" "$SIGMUND_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+running[[:space:]]" || return 1
  ! printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+stale[[:space:]]" || return 1
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

test_console_requires_socat_before_launch() {
  local fake_path rc count
  fake_path="$TEST_ROOT/no-socat"
  mkdir -p "$fake_path" || return 1
  set +e
  as_user env PATH="$fake_path" "$SIGMUND_REAL_BIN" --console /bin/true >"$TEST_ROOT/console-nosocat.out" 2>"$TEST_ROOT/console-nosocat.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { cat "$TEST_ROOT/console-nosocat.out" "$TEST_ROOT/console-nosocat.err" >&2; return 1; }
  grep -q -- '--console requires socat' "$TEST_ROOT/console-nosocat.err" || { cat "$TEST_ROOT/console-nosocat.err" >&2; return 1; }
  if [ -d "$HOME/.local/state/sigmund" ]; then
    count=$(find "$HOME/.local/state/sigmund" -maxdepth 1 -type f -name '*.json' | wc -l)
  else
    count=0
  fi
  [ "$count" -eq 0 ]
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
  command -v socat >/dev/null 2>&1 || return 0
  local out id store record
  out=$("$SIGMUND_BIN" --console /bin/sh -c 'read line; echo "got:$line"' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  store="$HOME/.local/state/sigmund"
  record="$store/$id.json"
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  "$SIGMUND_BIN" list | grep -Eq "^$id[[:space:]]+running[[:space:]]+.*[[:space:]]console[[:space:]]" || return 1
  printf 'ping\n' | "$SIGMUND_BIN" console "$id" >"$TEST_ROOT/console.out" 2>"$TEST_ROOT/console.err" || {
    cat "$TEST_ROOT/console.out" "$TEST_ROOT/console.err" >&2
    return 1
  }
  grep -q 'got:ping' "$TEST_ROOT/console.out" || { cat "$TEST_ROOT/console.out" >&2; return 1; }
  sleep 0.2
  grep -q 'got:ping' "$store/$id.log" || { cat "$store/$id.log" >&2; return 1; }
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1
  [ ! -e "$store/console/$id.sock" ]
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
  pids=$(pgrep -f 'sigmund_txn_test_sleep' || true)
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
  local out id out2 id2 pgid2 store
  store="$HOME/.local/state/sigmund"
  out=$("$SIGMUND_BIN" /bin/sh -c 'while :; do sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" web-test >"$TEST_ROOT/alias.out" 2>"$TEST_ROOT/alias.err" || return 1
  [ ! -s "$TEST_ROOT/alias.out" ] || return 1
  grep -qx "sigmund: pinned 'web-test' -> /bin/sh -c 'while :; do sleep 1; done'" "$TEST_ROOT/alias.err" || return 1
  [ -f "$store/aliases.json" ] || return 1
  [ ! -f "$store/profiles.json" ] || return 1
  [ ! -d "$store/profiles" ] || return 1
  grep -q '"web-test": {"bin": "' "$store/aliases.json" || return 1
  grep -q '"args": \["/bin/sh", "-c", "while :; do sleep 1; done"\]' "$store/aliases.json" || return 1
  "$SIGMUND_BIN" aliases | grep -Eq "^web-test[[:space:]]+user[[:space:]]+/bin/sh -c 'while :; do sleep 1; done'[[:space:]]+-$" || return 1
  "$SIGMUND_BIN" stop "$id" >/dev/null || return 1
  "$SIGMUND_BIN" prune "$id" >/dev/null || return 1

  out2=$("$SIGMUND_BIN" start web-test 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid2=$(record_pgid "$id2")
  [ -n "$id2" ] && [ -n "$pgid2" ] || return 1
  grep -q '"alias": "web-test"' "$store/$id2.json" || return 1
  ! grep -q '"profile_hash":' "$store/$id2.json" || return 1
  "$SIGMUND_BIN" stop web-test >/dev/null || return 1
  pgid_terminated "$pgid2"
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
  local out id
  out=$("$SIGMUND_BIN" /bin/sh -c ':' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$SIGMUND_BIN" alias "$id" db >"$TEST_ROOT/alias-db.out" 2>"$TEST_ROOT/alias-db.err" || return 1
  [ ! -s "$TEST_ROOT/alias-db.out" ] || return 1
  grep -qx "sigmund: pinned 'db' -> /bin/sh -c :" "$TEST_ROOT/alias-db.err" || return 1
  "$SIGMUND_BIN" aliases | grep -Eq "^db[[:space:]]+user[[:space:]]+/bin/sh -c :[[:space:]]+-$"
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
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 0
  local safe safe_real out id hash sudoers_dir sudoers_file rc visudo_ok
  safe="$TEST_ROOT/sigmund-safe"
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
  grep -qx "sigmund: pinned 'web-sys' -> /bin/sh -c :" "$TEST_ROOT/root-alias.err" || return 1
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
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 0
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
  command -v setsid >/dev/null 2>&1 || return 0
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
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 0
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
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 0
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

test_sudo_context_can_stop_unique_user_local_run() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || return 0
  local out id pgid
  out=$("$SIGMUND_BIN" sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  as_sudo_from_user "$SIGMUND_REAL_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_build_artifact_coexistence() {
  make clean >/dev/null || return 1
  make sigmund STATIC_LDFLAGS= EXTRA_CPPFLAGS=-DSIGMUND_TESTING >/dev/null || return 1
  [ -x ./sigmund ] || return 1
  make sigmund-dynamic EXTRA_CPPFLAGS=-DSIGMUND_TESTING >/dev/null || return 1
  [ -x ./sigmund ] && [ -x ./sigmund-dynamic ] || return 1
  [ -e ./sigmund ] && [ -e ./sigmund-dynamic ]
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
run_test "stop supports multiple IDs in one command" test_stop_multiple_ids
run_test "argument edge cases" test_argument_edges
run_test "special characters are preserved in argv JSON" test_special_chars_args
run_test "logging captures stdout+stderr" test_log_capture
run_test "-f starts and follows output" test_start_follow_short_form
run_test "tail <id> tails an existing run log" test_tail_verb_existing_id
run_test "persistent stale records remain visible and dumpable" test_persistent_stale_records
run_test "missing boot source does not force stale" test_boot_unavailable_does_not_force_stale
run_test "leader zombie with live group remains running" test_leader_zombie_group_still_running
run_test "tail <id> prints finished log output" test_tail_finished_log_prints_existing_output
run_test "--console refuses before launch when socat is missing" test_console_requires_socat_before_launch
run_test "console reports a normal run has no console" test_console_reports_non_console_run
run_test "console attach round-trips and tees to the log" test_console_round_trip_and_log_tee
run_test "prune <id> removes exactly one run record/output" test_prune_by_id
run_test "prune all removes prunable while preserving running" test_prune_all_keeps_running
run_test "transactional launch rollback on record write failure" test_transactional_record_write_failure
run_test "raw start does not steal trailing --system" test_raw_start_does_not_steal_trailing_system
run_test "long command appears in list, truncated with ..." test_long_command_list_truncates_instead_of_skips
run_test "normal start writes user-local state" test_normal_start_writes_user_local_state
run_test "root starts use system store and public state is unknown" test_root_start_writes_system_store_and_public_unknown
run_test "sudo start writes system state with invoking-user metadata" test_sudo_start_writes_system_store_with_invoking_metadata
run_test "sudo context can stop unique invoking-user local run" test_sudo_context_can_stop_unique_user_local_run
run_test "public root index rows are redacted in normal list" test_public_root_index_list_is_redacted
run_test "normal run does not self-elevate on local/root ID conflict" test_user_local_wins_over_public_root_collision
run_test "explicit user:<id> targets user-local run" test_explicit_user_target
run_test "user alias stores a direct recipe and starts/stops by alias" test_alias_profile_map_start_and_stop
run_test "alias start requires --multi when already running and --all stops all" test_alias_multi_gate_and_all_stop
run_test "profile start inherits current environment" test_profile_start_inherits_current_environment
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

if [ "$FAILS" -ne 0 ]; then
  exit 1
fi
