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
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    HOLD_TEST_SUDOERS_DIR \
    HOLD_TEST_VISUDO_PROG \
    HOLD_TEST_SUDO_PROG; do
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
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    HOLD_TEST_SUDOERS_DIR \
    HOLD_TEST_VISUDO_PROG \
    HOLD_TEST_SUDO_PROG; do
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
    HOLD_TEST_FAIL_RECORD_WRITE \
    HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
    HOLD_FAKE_SUDO_ARGV \
    HOLD_FAKE_SUDO_RC \
    HOLD_TEST_SUDOERS_DIR \
    HOLD_TEST_VISUDO_PROG \
    HOLD_TEST_SUDO_PROG; do
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
  HOLD_TEST_FAIL_RECORD_WRITE \
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE \
  HOLD_FAKE_SUDO_ARGV \
  HOLD_FAKE_SUDO_RC \
  HOLD_TEST_SUDOERS_DIR \
  HOLD_TEST_VISUDO_PROG \
  HOLD_TEST_SUDO_PROG; do
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

grant_canonical_digest_file() {
  command -v python3 >/dev/null 2>&1 || return 127
  as_root cat "$1" | python3 -c '
import hashlib, json, sys
actions_order = ["start", "stop", "kill", "tail", "dump", "prune", "console"]
obj = json.load(sys.stdin)
if obj.get("schema") != "hold.subject-grant.v1":
    raise SystemExit(2)

def config_obj():
    cfg = obj.get("Config")
    return cfg if isinstance(cfg, dict) else {}

def mode_obj():
    mode = obj.get("mode")
    return mode if isinstance(mode, dict) else {}

def array_value(*keys):
    cfg = config_obj()
    for key in keys:
        if key in obj and obj[key] is not None:
            return obj[key]
        if key in cfg and cfg[key] is not None:
            return cfg[key]
    return []

def normalized_binary_path():
    return obj.get("binary_path") or obj.get("bin") or obj["Path"]

def normalized_argv():
    binary = normalized_binary_path()
    if "args" in obj and obj["args"] is not None:
        argv = list(obj["args"] or [])
        return argv if argv and argv[0] == binary else [binary] + argv
    if "argv" in obj and obj["argv"] is not None:
        argv = list(obj["argv"] or [])
        return argv if argv and argv[0] == binary else [binary] + argv
    return [obj["Path"]] + list(obj.get("Args") or [])

def normalized_bool(name, config_key=None):
    value = bool(obj.get(name, False))
    mode = mode_obj()
    if name in mode:
        value = bool(mode[name])
    if name == "allow_multi" and "multi" in mode:
        value = bool(mode["multi"])
    cfg = config_obj()
    if config_key and config_key in cfg:
        value = bool(cfg[config_key])
    return value

def normalized_log_destination():
    value = obj.get("log_destination") or ""
    cfg = config_obj()
    log_cfg = cfg.get("LogConfig") if isinstance(cfg.get("LogConfig"), dict) else {}
    if not value:
        value = log_cfg.get("Type") or ""
    return "" if value == "local" else value

h = hashlib.sha256()
def field(v):
    h.update(str(v).encode("utf-8"))
    h.update(b"\0")
field("hold-subject-grant-canonical-v1")
for key in ("schema", "subject", "profile"):
    field(key)
    field(obj[key])
field("binary_path")
field(normalized_binary_path())

def array_field(name):
    if name == "env":
        values = array_value("env", "Env")
    elif name == "cap_add":
        values = array_value("cap_add", "CapAdd")
    elif name == "cap_drop":
        values = array_value("cap_drop", "CapDrop")
    else:
        values = obj.get(name) or []
    if not values:
        return
    field(name)
    field(len(values))
    for i, value in enumerate(values):
        field(i)
        field(value)

argv = normalized_argv()
field("argv")
field(len(argv))
for i, arg in enumerate(argv):
    field(i)
    field(arg)
for key in ("env", "ports", "volumes", "cap_add", "cap_drop"):
    array_field(key)
if normalized_bool("interactive", "OpenStdin"):
    field("interactive")
    field("true")
if normalized_bool("tty", "Tty"):
    field("tty")
    field("true")
if normalized_bool("detach"):
    field("detach")
    field("true")
if normalized_bool("allow_multi"):
    field("allow_multi")
    field("true")
if obj.get("restart") and obj.get("restart") != "no":
    field("restart")
    field(obj["restart"])
    if "restart_delay_seconds" in obj:
        field("restart_delay_seconds")
        field(obj["restart_delay_seconds"])
log_destination = normalized_log_destination()
if log_destination:
    field("log_destination")
    field(log_destination)
action_set = set(obj["actions"])
field("actions")
for action in actions_order:
    if action in action_set:
        field(action)
print(h.hexdigest())
'
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
  sed -n -e '/^[0-9a-f]\{12\}$/p' -e 's/^hold: id=\([0-9a-f][0-9a-f]*\).*/\1/p' | head -n1
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
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$id" | grep -Eq '^[0-9a-f]{12}$' || return 1
  printf '%s\n' "$out" | grep -Fqx "hold  started  $id   $sleep_bin 300"
  printf '%s\n' "$out" | grep -Eq '^         log      .+/.+\.log$'
  printf '%s\n' "$out" | grep -Eq "^         stop     hold stop $id$"
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*running"
  "$HOLD_BIN" stop "$id" >/dev/null
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*exited"
  "$HOLD_BIN" prune >/dev/null
  lines=$("$HOLD_BIN" list | wc -l)
  [ "$lines" -eq 1 ]
}


test_start_output_stop_hint() {
  local out id
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf '%s\n' "$out" | grep -Eq "^         stop     hold stop $id$"
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
  "$HOLD_BIN" list | grep -Eq "^$id[[:space:]].*running" || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  pgid_terminated "$pgid"
}

test_corrupt_record_handling() {
  ensure_user_fixture_store || return 1
  printf 'garbage\n' > "$HOME/.local/state/hold/badbad00cafe.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^badbad00cafe' "$TEST_ROOT/list.out"
  ! grep -Eq '^0[[:space:]]' "$TEST_ROOT/list.out"
  grep -q 'warning: skipping corrupt record badbad00cafe.json' "$TEST_ROOT/list.err"
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/hold/badbad00cafe.json" ]
}

test_deep_json_record_rejected_not_crashed() {
  ensure_user_fixture_store || return 1
  local json i
  json='{"junk":'
  for i in $(seq 1 80); do json="${json}["; done
  json="${json}0"
  for i in $(seq 1 80); do json="${json}]"; done
  json="${json},\"version\":1,\"id\":\"abc12445cafe\",\"pid\":12345,\"pgid\":12345,\"sid\":12345,\"start_unix_ns\":0,\"argv\":[\"x\"],\"cmdline_display\":\"x\",\"uid\":0,\"gid\":0,\"proc_starttime_ticks\":0,\"exe_dev\":0,\"exe_ino\":0}"
  printf '%s\n' "$json" > "$HOME/.local/state/hold/abc12445cafe.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12445cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12445cafe.json' "$TEST_ROOT/list.err"
}

test_symlinked_record_rejected() {
  ensure_user_fixture_store || return 1
  cat > "$TEST_ROOT/record-target.json" <<'JSON'
{"version":1,"id":"abc12545cafe","pid":12345,"pgid":12345,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  ln -s "$TEST_ROOT/record-target.json" "$HOME/.local/state/hold/abc12545cafe.json" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12545cafe' "$TEST_ROOT/list.out" || return 1
  grep -q 'warning: skipping corrupt record abc12545cafe.json' "$TEST_ROOT/list.err"
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
  "$HOLD_BIN" dump "$id" >"$TEST_ROOT/dump.out" 2>"$TEST_ROOT/dump.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  ! grep -q 'symlink-secret' "$TEST_ROOT/dump.out" || return 1
  grep -q 'failed to open log for dump' "$TEST_ROOT/dump.err"
}

test_invalid_pgid_record() {
  ensure_user_fixture_store || return 1
  cat > "$HOME/.local/state/hold/abc12345cafe.json" <<'JSON'
{"version":1,"id":"abc12345cafe","pid":12345,"pgid":0,"sid":12345,"start_unix_ns":0,"argv":["x"],"cmdline_display":"x","uid":0,"gid":0,"proc_starttime_ticks":0,"exe_dev":0,"exe_ino":0}
JSON
  "$HOLD_BIN" list >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  ! grep -q '^abc12345cafe' "$TEST_ROOT/list.out"
}

test_orphan_log_cleanup() {
  ensure_user_fixture_store || return 1
  : > "$HOME/.local/state/hold/a1b2c3d4e5f6.log" || return 1
  : > "$HOME/.local/state/hold/deadbeefcafe.log" || return 1
  "$HOLD_BIN" prune >/dev/null || return 1
  [ ! -e "$HOME/.local/state/hold/a1b2c3d4e5f6.log" ] && [ ! -e "$HOME/.local/state/hold/deadbeefcafe.log" ]
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
  set +e
  "$HOLD_BIN" stop >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || return 1
  "$HOLD_BIN" --help >/dev/null || return 1
  "$HOLD_BIN" help >/dev/null || return 1
  "$HOLD_BIN" help --help >/dev/null || return 1
  "$HOLD_BIN" help profiles | grep -q 'hold profile <name> \[opts\] -- <cmd>' || return 1
  "$HOLD_BIN" help targets | grep -q 'run id, leading id prefix, or profile name' || return 1
  "$HOLD_BIN" help scripting | grep -q 'Exit codes:' || return 1
  "$HOLD_BIN" help run >"$TEST_ROOT/help-run.out" || return 1
  grep -q -- '-p, --publish SPEC' "$TEST_ROOT/help-run.out" || { cat "$TEST_ROOT/help-run.out" >&2; return 1; }
  grep -q -- '-v, --volume SPEC' "$TEST_ROOT/help-run.out" || { cat "$TEST_ROOT/help-run.out" >&2; return 1; }
  grep -q -- '--restart POLICY' "$TEST_ROOT/help-run.out" || { cat "$TEST_ROOT/help-run.out" >&2; return 1; }
  grep -q -- '--restart-delay N' "$TEST_ROOT/help-run.out" || { cat "$TEST_ROOT/help-run.out" >&2; return 1; }
  grep -q -- '--privileged' "$TEST_ROOT/help-run.out" || { cat "$TEST_ROOT/help-run.out" >&2; return 1; }
  "$HOLD_BIN" stop -h | grep -q 'usage: hold stop' || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'sleep 0.2; echo ignore-before; sleep 0.2; echo "live-needle one"; sleep 0.2; echo ignore-after; sleep 0.2; echo "live-needle two"' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'sleep 0.3; echo logs-ignore; sleep 0.3; echo "logs-live-hit"; sleep 0.2' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'sleep 0.3; echo boring-ignore; sleep 0.3; echo "needle-live"; sleep 0.2' 2>&1) || return 1
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
  local out id rc
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo "chrome one"; echo "chrome two"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; out=sys.stdout.buffer; out.write(b"\x08x\x09xq"); out.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 10 cols 90; $HOLD_BIN logs $id --interactive" /dev/null >"$TEST_ROOT/viewer-chrome.out" 2>"$TEST_ROOT/viewer-chrome.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/viewer-chrome.err" >&2; return 1; }
  grep -q 'HELP Ctrl-H' "$TEST_ROOT/viewer-chrome.out" || { cat "$TEST_ROOT/viewer-chrome.out" >&2; return 1; }
  grep -q 'Space exclude similar' "$TEST_ROOT/viewer-chrome.out" || { cat "$TEST_ROOT/viewer-chrome.out" >&2; return 1; }
  grep -q 'Process information' "$TEST_ROOT/viewer-chrome.out" || { cat "$TEST_ROOT/viewer-chrome.out" >&2; return 1; }
  grep -q "runid: $id" "$TEST_ROOT/viewer-chrome.out" || { cat "$TEST_ROOT/viewer-chrome.out" >&2; return 1; }
  grep -q 'PRESS ANY KEY TO EXIT' "$TEST_ROOT/viewer-chrome.out" || { cat "$TEST_ROOT/viewer-chrome.out" >&2; return 1; }
}

test_log_viewer_space_excludes_and_ctrl_r_resets_filters() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo "example database timeout"; echo "unrelated payment ok"; sleep 0.1' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.3
  set +e
  python3 -c 'import sys,time; sys.stdout.buffer.write(b" \x12q"); sys.stdout.flush(); time.sleep(0.1)' |
    script -qfec "stty rows 8 cols 90; $HOLD_BIN logs $id --interactive" /dev/null >"$TEST_ROOT/viewer-example-mark.out" 2>"$TEST_ROOT/viewer-example-mark.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/viewer-example-mark.err" >&2; return 1; }
  grep -q 'excluding similar' "$TEST_ROOT/viewer-example-mark.out" || { cat "$TEST_ROOT/viewer-example-mark.out" >&2; return 1; }
  grep -q 'Ctrl-R reset' "$TEST_ROOT/viewer-example-mark.out" || { cat "$TEST_ROOT/viewer-example-mark.out" >&2; return 1; }
  grep -q 'unrelated payment ok' "$TEST_ROOT/viewer-example-mark.out" || { cat "$TEST_ROOT/viewer-example-mark.out" >&2; return 1; }
}

test_log_view_selection_uses_cached_rows() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc max_gen
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'printf "needle 1\nneedle 2\nneedle 3\nneedle 4\n"; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in 1 2 3 4 5 6 7 8 9; do echo "page-hit-$i"; done; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 40); do echo "topjump-line-$i"; done; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 50); do echo "nowrap-line-$i"; done; sleep 0.5' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 45); do echo "arrowtop-line-$i"; done; sleep 0.3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 80); do echo "idempotent-top-line-$i"; done; sleep 0.8' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 40); do echo "sticktop-line-$i"; done; for i in $(seq 41 90); do sleep 0.03; echo "sticktop-line-$i"; done; sleep 0.2' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 80); do echo "modpgup-line-$i"; done; sleep 0.4' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 90); do echo "modhome-line-$i"; done; sleep 0.6; echo "modhome-line-91"; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 90); do echo "homepin-line-$i"; done; sleep 0.6; echo "homepin-line-91"; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 70); do echo "topdown-line-$i"; done; sleep 0.3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 12); do echo "loopend-line-$i"; done; sleep 0.3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 60); do echo "navloop-line-$i"; done; sleep 0.3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo "target-top-one"; echo "target-top-two"; for i in $(seq 1 60); do echo "noise-middle-$i"; done; echo "target-tail-one"; echo "target-tail-two"; sleep 0.3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo "exclude-alpha-root"; for i in $(seq 1 40); do echo "keep-near-top-$i"; done; for i in $(seq 1 20); do echo "tail-after-exclude-$i"; done; sleep 0.3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo "drop heartbeat alpha"; echo "keep window one"; echo "keep window two"; echo "keep window three"; echo "keep window four"; echo "keep window five"; sleep 0.5; for i in $(seq 1 40); do sleep 0.03; echo "late bottom $i"; done; sleep 0.2' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 12); do echo "growtop-line-$i"; sleep 0.02; done; sleep 0.5' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 12); do echo "cursor-yank-line-$i"; done; for i in $(seq 13 40); do sleep 0.04; echo "cursor-yank-line-$i"; done; sleep 0.2' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in 1 2 3 4; do echo "pinshort-line-$i"; done; sleep 0.8; for i in $(seq 5 40); do sleep 0.03; echo "pinshort-line-$i"; done; sleep 3' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 20); do echo "filtered-noise-$i"; done; for i in $(seq 1 40); do echo "filtered-target-line-$i"; done; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo "aaa unrelated"; echo "f-only-visible"; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in $(seq 1 40); do echo "exitpage-line-$i"; done; sleep 0.1' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in 1 2 3 4 5 6; do echo "away-hit-old-$i"; done; sleep 0.8; echo "away-hit-new-tail"; sleep 0.2' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in 1 2 3 4 5 6; do echo "nomatch-hit-old-$i"; done; sleep 0.8; echo "unrelated-new-tail"; sleep 0.2' 2>&1) || return 1
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
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'for i in 1 2 3 4 5 6; do echo "burst-hit-old-$i"; done; sleep 0.8; python3 -c '"'"'import sys; sys.stdout.write("unrelated\\n" * 40000); sys.stdout.write("burst-hit-new-tail\\n"); sys.stdout.flush()'"'"'; sleep 0.2' 2>&1) || return 1
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
  local out id
  out=$("$HOLD_BIN" -f bash -c 'echo follow-short' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
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
  list_out=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+-[[:space:]]+stale[[:space:]]"
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
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" dump "$id" >"$TEST_ROOT/dump.out" 2>&1 || return 1
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
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+-[[:space:]]+running[[:space:]]" || return 1
  ! printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+-[[:space:]]+stale[[:space:]]" || return 1
  set +e
  got=$(HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop --print "$id" 2>"$TEST_ROOT/missing-boot-print.err")
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "stop --print with missing boot source: rc=$rc (want 0)" >&2; cat "$TEST_ROOT/missing-boot-print.err" >&2; return 1; }
  [ "$got" = "kill -TERM -- -$pgid" ] || { printf 'got: %s\nwant: kill -TERM -- -%s\n' "$got" "$pgid" >&2; return 1; }
  HOLD_BOOT_ID_PATH="$bootfile" "$HOLD_BIN" stop "$id" >/dev/null
}

test_leader_zombie_group_still_running() {
  local i id list_out tail_pid
  "$HOLD_BIN" --tail bash -c 'sleep 60 & exit 0' >"$TEST_ROOT/tail.out" 2>"$TEST_ROOT/tail.err" &
  tail_pid=$!
  id=""
  for i in $(seq 1 50); do
    id=$(extract_id <"$TEST_ROOT/tail.out" || true)
    [ -n "$id" ] && break
    sleep 0.05
  done
  [ -n "$id" ] || return 1
  sleep 0.3
  list_out=$("$HOLD_BIN" list) || return 1
  printf '%s\n' "$list_out" | grep -Eq "^$id[[:space:]]+-[[:space:]]+running[[:space:]]" || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  wait "$tail_pid" || true
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
  grep -Eq "^$id[[:space:]]+-[[:space:]]+running[[:space:]]" "$TEST_ROOT/console-list.out" || {
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

write_public_alias_fixture() {
  local name="$1" hash="$2"
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/aliases.json" <<JSON
{
  "$name": "$hash"
}
JSON
  chmod 0644 "$HOLD_TEST_SYSTEM_STATE_DIR/public/aliases.json" || return 1
}

system_alias_hash() {
  local name="$1"
  sed -n "s/.*\"$name\": \"\\([0-9a-f]\\{64\\}\\)\".*/\\1/p" "$HOLD_TEST_SYSTEM_STATE_DIR/public/aliases.json"
}

test_alias_profile_map_start_and_stop() {
  local out id out2 id2 pgid2 store sh_bin
  store="$HOME/.local/state/hold"
  sh_bin="$(resolve_path /bin/sh)" || return 1
  out=$("$HOLD_BIN" -d /bin/sh -c 'while :; do sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as web-test >"$TEST_ROOT/alias.out" 2>"$TEST_ROOT/alias.err" || return 1
  [ ! -s "$TEST_ROOT/alias.out" ] || return 1
  grep -Fqx "hold: pinned 'web-test' -> $sh_bin -c 'while :; do sleep 1; done'" "$TEST_ROOT/alias.err" || return 1
  [ -f "$store/aliases.json" ] || return 1
  [ ! -f "$store/profiles.json" ] || return 1
  [ ! -d "$store/profiles" ] || return 1
  grep -q '"web-test": {"bin": "' "$store/aliases.json" || return 1
  grep -Fq "\"args\": [\"-c\", \"while :; do sleep 1; done\"]" "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  "$HOLD_BIN" profiles >"$TEST_ROOT/aliases-list.out" || return 1
  grep -Eq "^web-test[[:space:]]+user[[:space:]]+.*[[:space:]]+-$" "$TEST_ROOT/aliases-list.out" || return 1
  "$HOLD_BIN" list >"$TEST_ROOT/list-after-alias.out" 2>"$TEST_ROOT/list-after-alias.err" || return 1
  ! grep -q 'aliases.json' "$TEST_ROOT/list-after-alias.err" || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  "$HOLD_BIN" prune "$id" >/dev/null || return 1

  "$HOLD_BIN" prune >"$TEST_ROOT/prune-after-alias.out" 2>"$TEST_ROOT/prune-after-alias.err" || return 1
  [ -f "$store/aliases.json" ] || return 1
  ! grep -q 'aliases.json' "$TEST_ROOT/prune-after-alias.err" || return 1

  out2=$("$HOLD_BIN" start web-test 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid2=$(record_pgid "$id2")
  [ -n "$id2" ] && [ -n "$pgid2" ] || return 1
  record=$(record_path "$id2" "$store") || return 1
  grep -q '"alias": "web-test"' "$record" || return 1
  ! grep -q '"profile_hash":' "$record" || return 1
  "$HOLD_BIN" stop web-test >/dev/null || return 1
  pgid_terminated "$pgid2"
}

test_alias_from_relative_executable_uses_recorded_absolute_argv0() {
  local app work other store expected out id
  app="$TEST_ROOT/app"
  work="$app/work"
  other="$TEST_ROOT/other"
  store="$HOME/.local/state/hold"
  mkdir -p "$app/bin" "$work" "$other" || return 1
  printf '#!/bin/sh\nsleep 300\n' >"$app/bin/daemon" || return 1
  chmod +x "$app/bin/daemon" || return 1
  expected="$(resolve_path "$app/bin/daemon")" || return 1

  out=$(cd "$work" && "$HOLD_BIN" -d ../bin/daemon 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  record=$(record_path "$id" "$store") || return 1
  grep -Fq "\"argv\": [\"$expected\"]" "$record" || return 1

  (cd "$other" && "$HOLD_BIN" profile save "$id" as rel-daemon >"$TEST_ROOT/alias-rel.out" 2>"$TEST_ROOT/alias-rel.err") || return 1
  grep -Fq "\"rel-daemon\": {\"bin\": \"$expected\", \"args\": []" "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  grep -Fq '"detach": true' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_alias_multi_gate_and_all_stop() {
  local out id id1 id2 out1 out2 rc pgid1 pgid2
  out=$("$HOLD_BIN" -d sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as web-multi >/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  "$HOLD_BIN" prune "$id" >/dev/null || return 1

  out1=$("$HOLD_BIN" start web-multi 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  pgid1=$(record_pgid "$id1")
  [ -n "$pgid1" ] || return 1
  set +e
  "$HOLD_BIN" start web-multi >"$TEST_ROOT/multi-refuse.out" 2>"$TEST_ROOT/multi-refuse.err"
  rc=$?
  set -e
  [ "$rc" -eq 6 ] || return 1
  grep -q -- '--force' "$TEST_ROOT/multi-refuse.err" || return 1

  out2=$("$HOLD_BIN" start web-multi --force 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid2=$(record_pgid "$id2")
  [ -n "$pgid2" ] || return 1
  set +e
  "$HOLD_BIN" stop web-multi >"$TEST_ROOT/alias-ambig.out" 2>"$TEST_ROOT/alias-ambig.err"
  rc=$?
  set -e
  [ "$rc" -eq 6 ] || return 1
  grep -q 'matches more than one' "$TEST_ROOT/alias-ambig.err" || return 1
  "$HOLD_BIN" stop web-multi --all >/dev/null || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_profile_multi_mode_allows_repeated_starts() {
  local out1 out2 id1 id2 pgid1 pgid2
  cat >"$TEST_ROOT/profile-multi.hold" <<'EOF'
enable
configure terminal
profile web-allow-multi
binary /usr/bin/sleep
argv 60
multi
commit
end
write
EOF
  "$HOLD_BIN" profile import "$TEST_ROOT/profile-multi.hold" >/dev/null || return 1
  "$HOLD_BIN" profile export web-allow-multi --json >"$TEST_ROOT/profile-multi.json" || return 1
  python3 - "$TEST_ROOT/profile-multi.json" <<'PY' || { cat "$TEST_ROOT/profile-multi.json" >&2; return 1; }
import json, sys
mode = (json.load(open(sys.argv[1], encoding='utf-8')).get('mode') or {})
if mode.get('multi') is not True:
    raise SystemExit(f'multi mode was not persisted: {mode!r}')
PY
  out1=$("$HOLD_BIN" start web-allow-multi 2>&1) || return 1
  id1=$(printf '%s\n' "$out1" | extract_id)
  pgid1=$(record_pgid "$id1")
  [ -n "$pgid1" ] || return 1
  out2=$("$HOLD_BIN" start web-allow-multi 2>&1) || {
    printf '%s\n' "$out2" >&2
    return 1
  }
  id2=$(printf '%s\n' "$out2" | extract_id)
  pgid2=$(record_pgid "$id2")
  [ -n "$pgid2" ] || return 1
  [ "$id1" != "$id2" ] || return 1
  "$HOLD_BIN" stop web-allow-multi --all >/dev/null || return 1
  pgid_terminated "$pgid1" || return 1
  pgid_terminated "$pgid2"
}

test_profile_start_inherits_current_environment() {
  local out id out2 id2 got
  out=$(as_user env HOLD_PROFILE_ENV=seed "$HOLD_REAL_BIN" /bin/sh -c 'printf "%s\n" "$HOLD_PROFILE_ENV"' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_user "$HOLD_REAL_BIN" profile save "$id" as env-test >"$TEST_ROOT/alias-env.out" 2>"$TEST_ROOT/alias-env.err" || return 1

  out2=$(as_user env HOLD_PROFILE_ENV=profile-value "$HOLD_REAL_BIN" start env-test 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id2" ] || return 1
  sleep 0.2
  got=$(as_user "$HOLD_REAL_BIN" dump "$id2") || return 1
  printf '%s\n' "$got" | grep -qx 'profile-value' || return 1
  ! printf '%s\n' "$got" | grep -qx 'seed'
}

test_docker_env_persists_with_named_profile() {
  local out id out2 id2 got
  "$HOLD_BIN" profile envpersist -e HOLD_DOCKER_ENV=fromdocker -- /bin/sh -c 'printf "%s\n" "$HOLD_DOCKER_ENV"' >"$TEST_ROOT/envpersist-create.out" || return 1
  "$HOLD_BIN" profile export envpersist --json >"$TEST_ROOT/envpersist-profile.json" || return 1
  grep -Fq '"env": ["HOLD_DOCKER_ENV=fromdocker"]' "$TEST_ROOT/envpersist-profile.json" || { cat "$TEST_ROOT/envpersist-profile.json" >&2; return 1; }
  out=$("$HOLD_BIN" run envpersist 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  got=$("$HOLD_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'fromdocker' || return 1

  out2=$("$HOLD_BIN" run envpersist 2>&1) || return 1
  id2=$(printf '%s\n' "$out2" | extract_id)
  [ -n "$id2" ] || return 1
  sleep 0.2
  got=$("$HOLD_BIN" dump "$id2") || return 1
  printf '%s\n' "$got" | grep -qx 'fromdocker'
}

test_profile_save_preserves_capability_metadata() {
  local out id id2 store aliases json
  store="$HOME/.local/state/hold"
  out=$("$HOLD_BIN" run -d --cap-drop ALL -- /bin/sleep 60 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" profile save "$id" as cap-prof >"$TEST_ROOT/cap-prof-save.out" 2>"$TEST_ROOT/cap-prof-save.err" || {
    cat "$TEST_ROOT/cap-prof-save.out" "$TEST_ROOT/cap-prof-save.err" >&2
    return 1
  }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  aliases="$store/aliases.json"
  grep -Fq '"cap_drop": ["ALL"]' "$aliases" || { cat "$aliases" >&2; return 1; }

  out=$("$HOLD_BIN" run -d cap-prof 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id2" "$store") || return 1
  grep -Fq '"cap_drop": ["ALL"]' "$json" || { cat "$json" >&2; return 1; }
  "$HOLD_BIN" stop "$id2" >/dev/null || return 1
}


test_profile_create_export_import_preserves_capability_metadata() {
  local out id store json1 json2 transcript record
  store="$HOME/.local/state/hold"

  "$HOLD_BIN" profile cap-meta --cap-add NET_BIND_SERVICE --cap-drop ALL -- /bin/sleep 60 \
    >"$TEST_ROOT/cap-meta-create.out" 2>"$TEST_ROOT/cap-meta-create.err" || {
      cat "$TEST_ROOT/cap-meta-create.out" "$TEST_ROOT/cap-meta-create.err" >&2
      return 1
    }

  json1="$TEST_ROOT/cap-meta.json"
  "$HOLD_BIN" profile export cap-meta --json >"$json1" || return 1
  python3 - "$json1" <<'PY' || { cat "$json1" >&2; return 1; }
import json, sys
p = json.load(open(sys.argv[1], encoding='utf-8'))
if p.get('cap_add') != ['NET_BIND_SERVICE']:
    raise SystemExit(f"cap_add not exported: {p!r}")
if p.get('cap_drop') != ['ALL']:
    raise SystemExit(f"cap_drop not exported: {p!r}")
PY

  transcript="$TEST_ROOT/cap-meta.profile"
  "$HOLD_BIN" profile export cap-meta >"$transcript" || return 1
  grep -Fxq 'cap-add NET_BIND_SERVICE' "$transcript" || { cat "$transcript" >&2; return 1; }
  grep -Fxq 'cap-drop ALL' "$transcript" || { cat "$transcript" >&2; return 1; }

  "$HOLD_BIN" profile import "$transcript" as cap-meta-import \
    >"$TEST_ROOT/cap-meta-import.out" 2>"$TEST_ROOT/cap-meta-import.err" || {
      cat "$TEST_ROOT/cap-meta-import.out" "$TEST_ROOT/cap-meta-import.err" >&2
      return 1
    }
  json2="$TEST_ROOT/cap-meta-import.json"
  "$HOLD_BIN" profile export cap-meta-import --json >"$json2" || return 1
  python3 - "$json2" <<'PY' || { cat "$json2" >&2; return 1; }
import json, sys
p = json.load(open(sys.argv[1], encoding='utf-8'))
if p.get('cap_add') != ['NET_BIND_SERVICE']:
    raise SystemExit(f"cap_add did not round-trip: {p!r}")
if p.get('cap_drop') != ['ALL']:
    raise SystemExit(f"cap_drop did not round-trip: {p!r}")
PY

  "$HOLD_BIN" profile cap-drop-run --cap-drop ALL -- /bin/sleep 60 \
    >"$TEST_ROOT/cap-drop-run-create.out" 2>"$TEST_ROOT/cap-drop-run-create.err" || {
      cat "$TEST_ROOT/cap-drop-run-create.out" "$TEST_ROOT/cap-drop-run-create.err" >&2
      return 1
    }
  out=$("$HOLD_BIN" run -d cap-drop-run 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  record=$(record_path "$id" "$store") || return 1
  grep -Fq '"cap_drop": ["ALL"]' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}


test_root_capability_drop_all_then_add_preserves_added_cap() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "root actor unavailable"
  [ -r /proc/self/status ] || skip "/proc status unavailable"
  local out cap_hex
  out=$(as_root "$HOLD_REAL_BIN" run --rm --cap-drop ALL --cap-add NET_BIND_SERVICE -- /bin/sh -c 'grep "^CapEff:" /proc/self/status' 2>&1) || {
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
  out=$(as_root "$HOLD_REAL_BIN" run -d --cap-add NET_BIND_SERVICE --cap-drop ALL -- /bin/sleep 60 2>&1) || {
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
  out=$(as_root "$HOLD_REAL_BIN" run -d --restart on-failure:1 --restart-delay 0 \
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


test_docker_env_file_persists_with_named_profile() {
  local out id got envfile
  envfile="$TEST_ROOT/docker.env"
  cat >"$envfile" <<'EOF'
# hold env file
HOLD_ENV_FILE_ONE=alpha
HOLD_ENV_FILE_TWO=value with spaces
EOF
  "$HOLD_BIN" profile envfile -e HOLD_INLINE=inline --env-file "$envfile" -- /usr/bin/env >"$TEST_ROOT/envfile-create.out" || return 1
  "$HOLD_BIN" profile export envfile --json >"$TEST_ROOT/envfile-profile.json" || return 1
  grep -Fq '"env": ["HOLD_INLINE=inline", "HOLD_ENV_FILE_ONE=alpha", "HOLD_ENV_FILE_TWO=value with spaces"]' "$TEST_ROOT/envfile-profile.json" || { cat "$TEST_ROOT/envfile-profile.json" >&2; return 1; }
  out=$("$HOLD_BIN" run envfile 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.2
  got=$("$HOLD_BIN" logs --plain -n 500 "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'HOLD_INLINE=inline' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -qx 'HOLD_ENV_FILE_ONE=alpha' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -qx 'HOLD_ENV_FILE_TWO=value with spaces' || { printf '%s\n' "$got" >&2; return 1; }
}


test_docker_rm_removes_run_artifacts_after_exit() {
  local out id store json log
  store="$HOME/.local/state/hold"
  out=$("$HOLD_BIN" run -d --rm -- /bin/sh -c 'echo rm-gone; sleep 0.3' 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
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


test_docker_rm_removes_profile_run_artifacts_after_exit() {
  local out id store json log
  store="$HOME/.local/state/hold"
  "$HOLD_BIN" profile rmprofile -- /bin/sh -c 'sleep 0.3' >"$TEST_ROOT/rmprofile-create.out" || return 1
  out=$("$HOLD_BIN" run -d --rm rmprofile 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  json=$(record_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  log=$(log_path "$id" "$store") || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  [ -f "$json" ] || { find "$store" -maxdepth 1 -type f -print >&2 || true; return 1; }
  path_absent_soon "$json" || { echo "--rm profile run did not remove record $json" >&2; return 1; }
  path_absent_soon "$log" || { echo "--rm profile run did not remove log $log" >&2; return 1; }
}

test_captive_profile_env_persists_and_runs() {
  local out id got store
  store="$HOME/.local/state/hold"
  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-env.out" 2>"$TEST_ROOT/captive-env.err" || { cat "$TEST_ROOT/captive-env.out" "$TEST_ROOT/captive-env.err" >&2; return 1; }
enable
configure terminal
profile envcap
binary /usr/bin/env
env HOLD_CISCO_ENV cisco-value
info
commit
end
run envcap
exit
EOF
  grep -q 'HOLD_CISCO_ENV=cisco-value' "$TEST_ROOT/captive-env.out" || { cat "$TEST_ROOT/captive-env.out" >&2; return 1; }
  grep -Fq '"env": ["HOLD_CISCO_ENV=cisco-value"]' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  id=$(extract_id <"$TEST_ROOT/captive-env.out")
  [ -n "$id" ] || { cat "$TEST_ROOT/captive-env.out" "$TEST_ROOT/captive-env.err" >&2; return 1; }
  sleep 0.2
  got=$("$HOLD_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'HOLD_CISCO_ENV=cisco-value'
}

test_captive_profile_loads_existing_recipe_for_edit() {
  local out id edit_id got store
  store="$HOME/.local/state/hold"
  "$HOLD_BIN" profile iosedit -e HOLD_OLD_ENV=old -- /usr/bin/env >"$TEST_ROOT/iosedit-create.out" || return 1
  out=$("$HOLD_BIN" run iosedit 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.2

  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-edit-existing.out" 2>"$TEST_ROOT/captive-edit-existing.err" || { cat "$TEST_ROOT/captive-edit-existing.out" "$TEST_ROOT/captive-edit-existing.err" >&2; return 1; }
enable
configure terminal
profile iosedit
info
no env HOLD_OLD_ENV
env HOLD_NEW_ENV new
commit
end
run iosedit
exit
EOF
  grep -q 'binary    : /usr/bin/env' "$TEST_ROOT/captive-edit-existing.out" || { cat "$TEST_ROOT/captive-edit-existing.out" >&2; return 1; }
  grep -q 'HOLD_OLD_ENV=old' "$TEST_ROOT/captive-edit-existing.out" || { cat "$TEST_ROOT/captive-edit-existing.out" >&2; return 1; }
  grep -q 'Profile committed' "$TEST_ROOT/captive-edit-existing.out" || { cat "$TEST_ROOT/captive-edit-existing.out" >&2; return 1; }
  grep -Fq '"env": ["HOLD_NEW_ENV=new"]' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  ! grep -q 'HOLD_OLD_ENV=old' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  edit_id=$(extract_id <"$TEST_ROOT/captive-edit-existing.out" | tail -n1)
  [ -n "$edit_id" ] || { cat "$TEST_ROOT/captive-edit-existing.out" "$TEST_ROOT/captive-edit-existing.err" >&2; return 1; }
  sleep 0.2
  got=$("$HOLD_BIN" logs --plain -n 500 "$edit_id") || return 1
  printf '%s\n' "$got" | grep -q 'HOLD_NEW_ENV=new' || { printf '%s\n' "$got" >&2; return 1; }
  ! printf '%s\n' "$got" | grep -q 'HOLD_OLD_ENV=old' || { printf '%s\n' "$got" >&2; return 1; }
}


test_captive_profile_publish_volume_rejected() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local rc

  set +e
  cat <<EOF | "$HOLD_BIN" >"$TEST_ROOT/captive-meta-reject.out" 2>"$TEST_ROOT/captive-meta-reject.err"
enable
configure terminal
profile iosmeta
binary /bin/true
publish 8080:3000
volume $TEST_ROOT:/work
commit
end
exit
EOF
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "publish/volume commands should make the captive session report failure" >&2; return 1; }
  cat "$TEST_ROOT/captive-meta-reject.out" "$TEST_ROOT/captive-meta-reject.err" >"$TEST_ROOT/captive-meta-reject.all"
  grep -q 'does not publish or forward ports' "$TEST_ROOT/captive-meta-reject.all" || { cat "$TEST_ROOT/captive-meta-reject.all" >&2; return 1; }
  grep -q 'does not mount or remap volumes' "$TEST_ROOT/captive-meta-reject.all" || { cat "$TEST_ROOT/captive-meta-reject.all" >&2; return 1; }
  grep -q 'Profile committed' "$TEST_ROOT/captive-meta-reject.out" || { cat "$TEST_ROOT/captive-meta-reject.out" >&2; return 1; }

  "$HOLD_BIN" profile export iosmeta --json >"$TEST_ROOT/iosmeta-reject.json" || return 1
  python3 - "$TEST_ROOT/iosmeta-reject.json" <<'PY' || { cat "$TEST_ROOT/iosmeta-reject.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if 'ports' in profile or 'volumes' in profile:
    raise SystemExit(f"publish/volume metadata was recorded: {profile!r}")
PY
}


test_captive_profile_capability_metadata_preserves_edits_and_clears() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local rc

  set +e
  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-cap-set.out" 2>"$TEST_ROOT/captive-cap-set.err"
enable
configure terminal
profile ioscap
binary /bin/sleep
argv 60
cap-add NET_BIND_SERVICE
cap-drop ALL
info
commit
end
exit
EOF
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-cap-set.out" "$TEST_ROOT/captive-cap-set.err" >&2; return 1; }
  grep -q 'cap-add   : NET_BIND_SERVICE' "$TEST_ROOT/captive-cap-set.out" || { cat "$TEST_ROOT/captive-cap-set.out" >&2; return 1; }
  grep -q 'cap-drop  : ALL' "$TEST_ROOT/captive-cap-set.out" || { cat "$TEST_ROOT/captive-cap-set.out" >&2; return 1; }

  "$HOLD_BIN" profile export ioscap --json >"$TEST_ROOT/ioscap-set.json" || return 1
  python3 - "$TEST_ROOT/ioscap-set.json" <<'PY' || { cat "$TEST_ROOT/ioscap-set.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if profile.get('cap_add') != ['NET_BIND_SERVICE']:
    raise SystemExit(f"cap_add not committed: {profile!r}")
if profile.get('cap_drop') != ['ALL']:
    raise SystemExit(f"cap_drop not committed: {profile!r}")
PY

  set +e
  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-cap-clear.out" 2>"$TEST_ROOT/captive-cap-clear.err"
enable
configure terminal
profile ioscap
info
no cap-add
no cap-drop
info
commit
end
exit
EOF
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-cap-clear.out" "$TEST_ROOT/captive-cap-clear.err" >&2; return 1; }
  grep -q 'cap-add   : NET_BIND_SERVICE' "$TEST_ROOT/captive-cap-clear.out" || { cat "$TEST_ROOT/captive-cap-clear.out" >&2; return 1; }
  grep -q 'cap-drop  : ALL' "$TEST_ROOT/captive-cap-clear.out" || { cat "$TEST_ROOT/captive-cap-clear.out" >&2; return 1; }
  grep -q 'cap-add   : -' "$TEST_ROOT/captive-cap-clear.out" || { cat "$TEST_ROOT/captive-cap-clear.out" >&2; return 1; }
  grep -q 'cap-drop  : -' "$TEST_ROOT/captive-cap-clear.out" || { cat "$TEST_ROOT/captive-cap-clear.out" >&2; return 1; }

  "$HOLD_BIN" profile export ioscap --json >"$TEST_ROOT/ioscap-clear.json" || return 1
  python3 - "$TEST_ROOT/ioscap-clear.json" <<'PY' || { cat "$TEST_ROOT/ioscap-clear.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if 'cap_add' in profile or 'cap_drop' in profile:
    raise SystemExit(f"capability fields not cleared: {profile!r}")
PY
}


test_captive_profile_restart_metadata_preserves_edits_and_clears() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc
  "$HOLD_BIN" profile iosrestart -d --restart on-failure:2 --restart-delay 3 -- /bin/sh -c 'sleep 1' >"$TEST_ROOT/iosrestart-create.out" || return 1
  out=$("$HOLD_BIN" run iosrestart 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true

  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-restart-preserve.out" 2>"$TEST_ROOT/captive-restart-preserve.err" || { cat "$TEST_ROOT/captive-restart-preserve.out" "$TEST_ROOT/captive-restart-preserve.err" >&2; return 1; }
enable
configure terminal
profile iosrestart
info
env HOLD_CISCO_TOUCHED yes
commit
end
exit
EOF
  grep -q 'restart   : on-failure:2 delay=3' "$TEST_ROOT/captive-restart-preserve.out" || {
    cat "$TEST_ROOT/captive-restart-preserve.out" >&2
    return 1
  }
  "$HOLD_BIN" profile export iosrestart --json >"$TEST_ROOT/iosrestart-preserve.json" || return 1
  python3 - "$TEST_ROOT/iosrestart-preserve.json" <<'PY' || { cat "$TEST_ROOT/iosrestart-preserve.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if profile.get('restart') != 'on-failure:2':
    raise SystemExit(f"restart not preserved: {profile.get('restart')!r}")
if profile.get('restart_delay_seconds') != 3:
    raise SystemExit(f"restart delay not preserved: {profile.get('restart_delay_seconds')!r}")
if 'HOLD_CISCO_TOUCHED=yes' not in profile.get('env', []):
    raise SystemExit(f"env edit missing after commit: {profile.get('env')!r}")
PY

  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-restart-edit.out" 2>"$TEST_ROOT/captive-restart-edit.err" || { cat "$TEST_ROOT/captive-restart-edit.out" "$TEST_ROOT/captive-restart-edit.err" >&2; return 1; }
enable
configure terminal
profile iosrestart
restart always
restart-delay 4
commit
end
exit
EOF
  "$HOLD_BIN" profile export iosrestart --json >"$TEST_ROOT/iosrestart-edit.json" || return 1
  python3 - "$TEST_ROOT/iosrestart-edit.json" <<'PY' || { cat "$TEST_ROOT/iosrestart-edit.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if profile.get('restart') != 'always':
    raise SystemExit(f"restart not edited: {profile.get('restart')!r}")
if profile.get('restart_delay_seconds') != 4:
    raise SystemExit(f"restart delay not edited: {profile.get('restart_delay_seconds')!r}")
PY

  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-restart-clear.out" 2>"$TEST_ROOT/captive-restart-clear.err"
enable
configure terminal
profile iosrestart
no restart
commit
end
exit
EOF
  rc=$?
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-restart-clear.out" "$TEST_ROOT/captive-restart-clear.err" >&2; return 1; }
  "$HOLD_BIN" profile export iosrestart --json >"$TEST_ROOT/iosrestart-clear.json" || return 1
  python3 - "$TEST_ROOT/iosrestart-clear.json" <<'PY' || { cat "$TEST_ROOT/iosrestart-clear.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if 'restart' in profile or 'restart_delay_seconds' in profile:
    raise SystemExit(f"restart fields not cleared: {profile!r}")
PY
}


test_captive_profile_quoted_argv_and_env_round_trip() {
  local rc id got store
  store="$HOME/.local/state/hold"
  set +e
  cat <<'EOF' | "$HOLD_BIN" >"$TEST_ROOT/captive-quoted.out" 2>"$TEST_ROOT/captive-quoted.err"
enable
configure terminal
profile quoted
binary /bin/sh
argv -c 'printf "%s\n" "$HOLD_QUOTED_ENV"; printf "%s\n" "arg with spaces"'
env HOLD_QUOTED_ENV "value with spaces"
commit
end
run quoted
exit
EOF
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-quoted.out" "$TEST_ROOT/captive-quoted.err" >&2; return 1; }
  grep -q 'Profile committed.' "$TEST_ROOT/captive-quoted.out" || { cat "$TEST_ROOT/captive-quoted.out" >&2; return 1; }
  grep -Fq '"env": ["HOLD_QUOTED_ENV=value with spaces"]' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  grep -Fq 'HOLD_QUOTED_ENV' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  grep -Fq 'arg with spaces' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  id=$(extract_id <"$TEST_ROOT/captive-quoted.out")
  [ -n "$id" ] || { cat "$TEST_ROOT/captive-quoted.out" "$TEST_ROOT/captive-quoted.err" >&2; return 1; }
  sleep 0.2
  got=$("$HOLD_BIN" logs --plain -n 50 "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'value with spaces' || { printf '%s\n' "$got" >&2; return 1; }
  printf '%s\n' "$got" | grep -qx 'arg with spaces' || { printf '%s\n' "$got" >&2; return 1; }
}

test_profile_transcript_import_export_roundtrip() {
  local transcript export out id got store
  store="$HOME/.local/state/hold"
  transcript="$TEST_ROOT/import.profile"
  cat >"$transcript" <<'EOF' || return 1
enable
configure terminal
profile cli-prof
binary /bin/echo
argv 'hello import'
commit
end
write
EOF
  "$HOLD_BIN" profile import "$transcript" >"$TEST_ROOT/import.out" 2>"$TEST_ROOT/import.err" || return 1
  [ ! -s "$TEST_ROOT/import.out" ] || return 1
  [ ! -s "$TEST_ROOT/import.err" ] || return 1
  grep -Fq '"cli-prof": {"bin": "/usr/bin/echo"' "$store/aliases.json" ||
    grep -Fq '"cli-prof": {"bin": "/bin/echo"' "$store/aliases.json" || return 1
  grep -Fq '"args": ["hello import"]' "$store/aliases.json" || return 1

  "$HOLD_BIN" import "$transcript" as top-prof --dry-run >"$TEST_ROOT/top-import-dry.out" 2>"$TEST_ROOT/top-import-dry.err" || return 1
  grep -Fxq "profile top-prof import dry-run ok" "$TEST_ROOT/top-import-dry.out" || {
    cat "$TEST_ROOT/top-import-dry.out" "$TEST_ROOT/top-import-dry.err" >&2
    return 1
  }
  ! grep -Fq '"top-prof":' "$store/aliases.json" || return 1

  "$HOLD_BIN" import "$transcript" as top-prof --yes >"$TEST_ROOT/top-import.out" 2>"$TEST_ROOT/top-import.err" || return 1
  [ ! -s "$TEST_ROOT/top-import.out" ] || return 1
  [ ! -s "$TEST_ROOT/top-import.err" ] || return 1
  grep -Fq '"top-prof": {"bin": "/usr/bin/echo"' "$store/aliases.json" ||
    grep -Fq '"top-prof": {"bin": "/bin/echo"' "$store/aliases.json" || return 1

  "$HOLD_BIN" profile export cli-prof >"$TEST_ROOT/export.profile" || return 1
  grep -Fxq "enable" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "configure terminal" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "profile cli-prof" "$TEST_ROOT/export.profile" || return 1
  grep -Eq "^binary (/usr)?/bin/echo$" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "argv 'hello import'" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "commit" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "end" "$TEST_ROOT/export.profile" || return 1
  grep -Fxq "write" "$TEST_ROOT/export.profile" || return 1

  "$HOLD_BIN" export top-prof as "$TEST_ROOT/top-export.profile" || return 1
  grep -Fxq "profile top-prof" "$TEST_ROOT/top-export.profile" || return 1
  grep -Fxq "argv 'hello import'" "$TEST_ROOT/top-export.profile" || return 1
  "$HOLD_BIN" export top-prof >"$TEST_ROOT/top-export.stdout" || return 1
  grep -Fxq "profile top-prof" "$TEST_ROOT/top-export.stdout" || return 1
  "$HOLD_BIN" export top-prof --format json >"$TEST_ROOT/top-export.json" || return 1
  grep -Fq '"name": "top-prof"' "$TEST_ROOT/top-export.json" || return 1
  ! grep -Fq '"ports":' "$TEST_ROOT/top-export.json" || return 1
  ! grep -Fq '"volumes":' "$TEST_ROOT/top-export.json" || return 1

  out=$("$HOLD_BIN" start cli-prof 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.1
  got=$("$HOLD_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'hello import'
}

test_profile_name_first_set_command() {
  local out id got
  "$HOLD_BIN" profile edit-prof set command -- /bin/echo 'name first edit' \
    >"$TEST_ROOT/profile-set.out" 2>"$TEST_ROOT/profile-set.err" || {
      cat "$TEST_ROOT/profile-set.out" "$TEST_ROOT/profile-set.err" >&2
      return 1
    }
  [ ! -s "$TEST_ROOT/profile-set.out" ] || return 1
  [ ! -s "$TEST_ROOT/profile-set.err" ] || return 1

  "$HOLD_BIN" profile edit-prof export --format cli >"$TEST_ROOT/profile-set.export" || return 1
  grep -Fxq "profile edit-prof" "$TEST_ROOT/profile-set.export" || return 1
  grep -Eq "^binary (/usr)?/bin/echo$" "$TEST_ROOT/profile-set.export" || {
    cat "$TEST_ROOT/profile-set.export" >&2
    return 1
  }
  grep -Fxq "argv 'name first edit'" "$TEST_ROOT/profile-set.export" || {
    cat "$TEST_ROOT/profile-set.export" >&2
    return 1
  }

  "$HOLD_BIN" profile edit-prof show >"$TEST_ROOT/profile-set.show" || return 1
  grep -Fxq "profile edit-prof" "$TEST_ROOT/profile-set.show" || return 1
  grep -Eq "^binary (/usr)?/bin/echo$" "$TEST_ROOT/profile-set.show" || {
    cat "$TEST_ROOT/profile-set.show" >&2
    return 1
  }
  grep -Fxq "argv 'name first edit'" "$TEST_ROOT/profile-set.show" || {
    cat "$TEST_ROOT/profile-set.show" >&2
    return 1
  }

  "$HOLD_BIN" profile edit-prof export --format json >"$TEST_ROOT/profile-set.json" || return 1
  grep -Fq '"name": "edit-prof"' "$TEST_ROOT/profile-set.json" || return 1
  grep -Fq '"args": ["name first edit"]' "$TEST_ROOT/profile-set.json" || return 1

  out=$("$HOLD_BIN" profile edit-prof start 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.1
  got=$("$HOLD_BIN" logs "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'name first edit'
}

test_profile_name_first_crud() {
  "$HOLD_BIN" profile crud-prof create -- /bin/echo 'crud profile' \
    >"$TEST_ROOT/profile-crud.create.out" 2>"$TEST_ROOT/profile-crud.create.err" || {
      cat "$TEST_ROOT/profile-crud.create.out" "$TEST_ROOT/profile-crud.create.err" >&2
      return 1
    }
  [ ! -s "$TEST_ROOT/profile-crud.create.out" ] || return 1
  [ ! -s "$TEST_ROOT/profile-crud.create.err" ] || return 1

  if "$HOLD_BIN" profile crud-prof create -- /bin/echo duplicate \
      >"$TEST_ROOT/profile-crud.dup.out" 2>"$TEST_ROOT/profile-crud.dup.err"; then
    cat "$TEST_ROOT/profile-crud.dup.out" "$TEST_ROOT/profile-crud.dup.err" >&2
    return 1
  fi
  grep -Fq "profile 'crud-prof' already exists" "$TEST_ROOT/profile-crud.dup.err" || {
    cat "$TEST_ROOT/profile-crud.dup.err" >&2
    return 1
  }

  "$HOLD_BIN" profile crud-prof rename renamed-prof \
    >"$TEST_ROOT/profile-crud.rename.out" 2>"$TEST_ROOT/profile-crud.rename.err" || {
      cat "$TEST_ROOT/profile-crud.rename.out" "$TEST_ROOT/profile-crud.rename.err" >&2
      return 1
    }
  [ ! -s "$TEST_ROOT/profile-crud.rename.out" ] || return 1
  [ ! -s "$TEST_ROOT/profile-crud.rename.err" ] || return 1

  if "$HOLD_BIN" profile crud-prof show >"$TEST_ROOT/profile-crud.old.out" 2>"$TEST_ROOT/profile-crud.old.err"; then
    cat "$TEST_ROOT/profile-crud.old.out" "$TEST_ROOT/profile-crud.old.err" >&2
    return 1
  fi
  grep -Fq "profile 'crud-prof' not found" "$TEST_ROOT/profile-crud.old.err" || return 1

  "$HOLD_BIN" profile renamed-prof show >"$TEST_ROOT/profile-crud.show" || return 1
  grep -Fxq "profile renamed-prof" "$TEST_ROOT/profile-crud.show" || return 1
  grep -Eq "^binary (/usr)?/bin/echo$" "$TEST_ROOT/profile-crud.show" || {
    cat "$TEST_ROOT/profile-crud.show" >&2
    return 1
  }
  grep -Fxq "argv 'crud profile'" "$TEST_ROOT/profile-crud.show" || {
    cat "$TEST_ROOT/profile-crud.show" >&2
    return 1
  }

  "$HOLD_BIN" profile renamed-prof delete \
    >"$TEST_ROOT/profile-crud.delete.out" 2>"$TEST_ROOT/profile-crud.delete.err" || {
      cat "$TEST_ROOT/profile-crud.delete.out" "$TEST_ROOT/profile-crud.delete.err" >&2
      return 1
    }
  [ ! -s "$TEST_ROOT/profile-crud.delete.out" ] || return 1
  [ ! -s "$TEST_ROOT/profile-crud.delete.err" ] || return 1

  if "$HOLD_BIN" profile renamed-prof show >"$TEST_ROOT/profile-crud.deleted.out" 2>"$TEST_ROOT/profile-crud.deleted.err"; then
    cat "$TEST_ROOT/profile-crud.deleted.out" "$TEST_ROOT/profile-crud.deleted.err" >&2
    return 1
  fi
  grep -Fq "profile 'renamed-prof' not found" "$TEST_ROOT/profile-crud.deleted.err" || return 1
}

test_profile_json_export_and_import() {
  local json docker_json out id got store
  store="$HOME/.local/state/hold"
  json="$TEST_ROOT/json.profile"
  cat >"$json" <<'JSON' || return 1
{"version":1,"name":"json-prof","bin":"/bin/echo","args":["/bin/echo","hello json"]}
JSON
  "$HOLD_BIN" profile import "$json" >/dev/null || return 1
  "$HOLD_BIN" profile export json-prof --json >"$TEST_ROOT/export.json" || return 1
  grep -Fq '"name": "json-prof"' "$TEST_ROOT/export.json" || return 1
  grep -Fq '"args": ["hello json"]' "$TEST_ROOT/export.json" || return 1
  grep -Fq '"json-prof": {"bin": "/bin/echo"' "$store/aliases.json" || return 1

  out=$("$HOLD_BIN" start json-prof 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.1
  got=$("$HOLD_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'hello json' || return 1

  docker_json="$TEST_ROOT/docker-shaped.profile.json"
  cat >"$docker_json" <<'JSON' || return 1
{
  "version": 1,
  "name": "docker-json-prof",
  "Path": "/bin/sh",
  "Args": ["-c", "printf '%s\\n' \"$DOCKER_PROFILE_TEST\""],
  "Config": {
    "Env": ["DOCKER_PROFILE_TEST=from-config"],
    "Tty": false,
    "OpenStdin": false,
    "CapAdd": null,
    "CapDrop": null,
    "LogConfig": {"Type": "local", "Config": {}}
  }
}
JSON
  "$HOLD_BIN" profile import "$docker_json" >/dev/null || return 1
  grep -Fq '"docker-json-prof": {"bin": "/bin/sh"' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  out=$("$HOLD_BIN" start docker-json-prof 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.1
  got=$("$HOLD_BIN" dump "$id") || return 1
  printf '%s\n' "$got" | grep -qx 'from-config'
}

test_invalid_alias_names_rejected() {
  local out id rc
  out=$("$HOLD_BIN" -d true 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  set +e
  "$HOLD_BIN" profile save "$id" as bad.name >"$TEST_ROOT/alias-bad.out" 2>"$TEST_ROOT/alias-bad.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || return 1
  grep -q 'invalid profile' "$TEST_ROOT/alias-bad.err"
}

test_short_hex_alias_name_allowed() {
  local out id sh_bin
  sh_bin="$(resolve_path /bin/sh)" || return 1
  out=$("$HOLD_BIN" -d /bin/sh -c ':' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as db >"$TEST_ROOT/alias-db.out" 2>"$TEST_ROOT/alias-db.err" || return 1
  [ ! -s "$TEST_ROOT/alias-db.out" ] || return 1
  grep -Fqx "hold: pinned 'db' -> $sh_bin -c :" "$TEST_ROOT/alias-db.err" || return 1
  "$HOLD_BIN" profiles >"$TEST_ROOT/aliases-db.out" || return 1
  grep -Eq "^db[[:space:]]+user[[:space:]]+" "$TEST_ROOT/aliases-db.out" || return 1
  grep -Fq "$sh_bin -c :" "$TEST_ROOT/aliases-db.out"
}

test_system_alias_action_self_elevates_alias() {
  local hash rc
  hash=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
  write_public_alias_fixture web-test "$hash" || return 1
  write_public_index_fixture abc12345cafe running 2026-06-15T18:42:11Z web-test || return 1
  make_fake_sudo || return 1
  set +e
  "$HOLD_BIN" stop web-test >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "stop" ] || return 1
  [ "${args[5]}" = "abc12345cafe" ] || return 1
  [ "${args[6]}" = "web-test" ] || return 1
  [ "${args[7]}" = "$hash" ] || return 1
}

test_system_alias_start_self_elevates_alias() {
  local hash rc
  hash=abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd
  write_public_alias_fixture web-test "$hash" || return 1
  make_fake_sudo || return 1
  set +e
  "$HOLD_BIN" --system start web-test >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "start" ] || return 1
  [ "${args[5]}" = "000000000000" ] || return 1
  [ "${args[6]}" = "web-test" ] || return 1
  [ "${args[7]}" = "$hash" ] || return 1
}

test_system_profile_save_preserves_capability_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id hash started cap_id cap_record profiles safe sudoers_dir visudo_ok grant_hash grant_private grant_public cap_token cap_out rc

  safe=$(root_safe_hold_copy hold-grant-cap) || return 1

  out=$(as_root "$safe" run -d --cap-drop ALL -- /bin/sleep 60 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }

  as_root "$safe" profile save "$id" as sys-cap-meta \
    >"$TEST_ROOT/sys-cap-save.out" 2>"$TEST_ROOT/sys-cap-save.err" || {
      cat "$TEST_ROOT/sys-cap-save.out" "$TEST_ROOT/sys-cap-save.err" >&2
      return 1
    }
  profiles="$HOLD_TEST_SYSTEM_STATE_DIR/profiles.json"
  root_grep '"cap_drop": ["ALL"]' "$profiles" || { as_root cat "$profiles" >&2; return 1; }
  hash=$(system_alias_hash sys-cap-meta)
  [ -n "$hash" ] || return 1

  as_root "$safe" stop "$id" >/dev/null || return 1
  as_root "$safe" prune "$id" >/dev/null || return 1

  started=$(as_root "$safe" --system --elevated start 000000000000 sys-cap-meta "$hash" 2>&1) || {
    printf '%s\n' "$started" >&2
    return 1
  }
  cap_id=$(printf '%s\n' "$started" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$started" >&2; return 1; }
  cap_record=$(root_record_path "$cap_id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  root_grep '"cap_drop": ["ALL"]' "$cap_record" || { as_root cat "$cap_record" >&2; return 1; }
  root_grep '"CapDrop": ["ALL"]' "$cap_record" || { as_root cat "$cap_record" >&2; return 1; }
  as_root "$safe" stop "$cap_id" >/dev/null || return 1

  sudoers_dir="$TEST_ROOT/sudoers-cap.d"
  mkdir -p "$sudoers_dir" || return 1
  chmod 755 "$sudoers_dir" || return 1
  export HOLD_TEST_SUDOERS_DIR="$sudoers_dir"
  visudo_ok="$TEST_ROOT/visudo-cap-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$visudo_ok" || return 1
  chmod 755 "$visudo_ok" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_ok"

  as_root "$safe" grant sys-cap-meta "$TEST_USER" start >"$TEST_ROOT/grant-cap.out" 2>"$TEST_ROOT/grant-cap.err" || {
    cat "$TEST_ROOT/grant-cap.out" "$TEST_ROOT/grant-cap.err" >&2
    return 1
  }
  grant_private="$HOLD_TEST_SYSTEM_STATE_DIR/grants/users/$TEST_USER/sys-cap-meta.json"
  grant_public="$HOLD_TEST_SYSTEM_STATE_DIR/public/grants/users/$TEST_USER/sys-cap-meta.json"
  root_grep '"cap_drop": ["ALL"]' "$grant_private" || { as_root cat "$grant_private" >&2; return 1; }
  root_grep '"detach": true' "$grant_private" || { as_root cat "$grant_private" >&2; return 1; }
  grant_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ -n "$grant_hash" ] || { as_root cat "$grant_public" >&2; return 1; }
  grant_digest=$(grant_canonical_digest_file "$grant_private") || return 1
  [ "$grant_hash" = "$grant_digest" ] || { echo "grant hash $grant_hash != canonical private digest $grant_digest" >&2; as_root cat "$grant_private" >&2; return 1; }

  cap_token="eyJ2IjoxLCJvcCI6InN0YXJ0IiwiZm9yY2UiOmZhbHNlLCJkZXRhY2giOnRydWV9"
  cap_out=$(as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run sys-cap-meta --cap "$grant_hash" "$cap_token" 2>&1) || { printf '%s\n' "$cap_out" >&2; return 1; }
  cap_id=$(printf '%s\n' "$cap_out" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$cap_out" >&2; return 1; }
  cap_record=$(root_record_path "$cap_id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  root_grep '"cap_drop": ["ALL"]' "$cap_record" || { as_root cat "$cap_record" >&2; return 1; }
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run sys-cap-meta --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/grant-cap-second.out" 2>"$TEST_ROOT/grant-cap-second.err"
  rc=$?
  set -e
  [ "$rc" -eq 6 ] || { echo "second singular grant start rc=$rc (want 6)" >&2; cat "$TEST_ROOT/grant-cap-second.out" "$TEST_ROOT/grant-cap-second.err" >&2; return 1; }
  grep -q 'already has a running process' "$TEST_ROOT/grant-cap-second.err" || { cat "$TEST_ROOT/grant-cap-second.err" >&2; return 1; }
  as_root "$safe" stop "$cap_id" >/dev/null || return 1

  as_root env GRANT_PRIVATE="$grant_private" python3 - <<'PY' || return 1
import json, os
path = os.environ["GRANT_PRIVATE"]
with open(path, "r", encoding="utf-8") as f:
    obj = json.load(f)
obj.pop("cap_drop", None)
with open(path, "w", encoding="utf-8") as f:
    json.dump(obj, f, sort_keys=True)
    f.write("\n")
PY
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run sys-cap-meta --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/grant-cap-tamper.out" 2>"$TEST_ROOT/grant-cap-tamper.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "grant capability survived cap_drop removal" >&2; return 1; }
  grep -q 'capability' "$TEST_ROOT/grant-cap-tamper.err" || { cat "$TEST_ROOT/grant-cap-tamper.err" >&2; return 1; }
}


test_grant_refuses_user_writable_profile_paths() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe out id script sudoers_dir visudo_ok rc grant_private grant_public grant_hash cap_token

  safe=$(root_safe_hold_copy hold-grant-paths) || return 1

  script="$TEST_ROOT/user-writable-target.sh"
  printf '#!/bin/sh\nsleep 60\n' >"$script" || return 1
  chmod 755 "$script" || return 1

  out=$(as_root "$safe" run -d -- /bin/sh "$script" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  as_root "$safe" profile save "$id" as unsafe-path >/dev/null 2>"$TEST_ROOT/unsafe-save.err" || {
    cat "$TEST_ROOT/unsafe-save.err" >&2
    return 1
  }

  sudoers_dir="$TEST_ROOT/sudoers-paths.d"
  mkdir -p "$sudoers_dir" || return 1
  chmod 755 "$sudoers_dir" || return 1
  export HOLD_TEST_SUDOERS_DIR="$sudoers_dir"
  visudo_ok="$TEST_ROOT/visudo-paths-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$visudo_ok" || return 1
  chmod 755 "$visudo_ok" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_ok"

  set +e
  as_root "$safe" grant unsafe-path "$TEST_USER" start >"$TEST_ROOT/grant-paths.out" 2>"$TEST_ROOT/grant-paths.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/grant-paths.out" "$TEST_ROOT/grant-paths.err" >&2; return 1; }
  grep -q 'refusing sudoers grant' "$TEST_ROOT/grant-paths.err" || { cat "$TEST_ROOT/grant-paths.err" >&2; return 1; }
  grep -q -- '--secure unsafe-path' "$TEST_ROOT/grant-paths.err" || { cat "$TEST_ROOT/grant-paths.err" >&2; return 1; }
  grep -q -- '--force unsafe-path' "$TEST_ROOT/grant-paths.err" || { cat "$TEST_ROOT/grant-paths.err" >&2; return 1; }
  grant_private="$HOLD_TEST_SYSTEM_STATE_DIR/grants/users/$TEST_USER/unsafe-path.json"
  grant_public="$HOLD_TEST_SYSTEM_STATE_DIR/public/grants/users/$TEST_USER/unsafe-path.json"
  root_path_absent "$grant_private" || return 1
  root_path_absent "$grant_public" || return 1

  as_root "$safe" grant --force unsafe-path "$TEST_USER" start >"$TEST_ROOT/grant-paths-force.out" 2>"$TEST_ROOT/grant-paths-force.err" || {
    cat "$TEST_ROOT/grant-paths-force.out" "$TEST_ROOT/grant-paths-force.err" >&2
    return 1
  }
  grep -q -- '--force bypasses grant path ownership checks' "$TEST_ROOT/grant-paths-force.err" || {
    cat "$TEST_ROOT/grant-paths-force.err" >&2
    return 1
  }
  root_file_exists "$grant_private" || return 1
  root_file_exists "$grant_public" || return 1
  grant_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ -n "$grant_hash" ] || { as_root cat "$grant_public" >&2; return 1; }
  cap_token="eyJ2IjoxLCJvcCI6InN0YXJ0IiwiZm9yY2UiOmZhbHNlfQ"
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run unsafe-path --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/grant-path-runtime.out" 2>"$TEST_ROOT/grant-path-runtime.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "forced unsafe grant was accepted at runtime" >&2; cat "$TEST_ROOT/grant-path-runtime.out" "$TEST_ROOT/grant-path-runtime.err" >&2; return 1; }
  grep -Eq 'unsafe path|not grant-safe|capability' "$TEST_ROOT/grant-path-runtime.err" || { cat "$TEST_ROOT/grant-path-runtime.err" >&2; return 1; }
  as_root "$safe" stop "$id" >/dev/null || return 1
}


test_granted_start_pins_privileged_cwd() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe out id hash sudoers_dir visudo_ok grant_public grant_hash cap_token hostile cap_out cap_id log

  safe=$(root_safe_hold_copy hold-grant-cwd) || return 1

  out=$(as_root "$safe" run -d -- /bin/pwd 2>&1) || { printf '%s\n' "$out" >&2; return 1; }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  as_root "$safe" profile save "$id" as cwd-pin >/dev/null 2>"$TEST_ROOT/cwd-pin-save.err" || {
    cat "$TEST_ROOT/cwd-pin-save.err" >&2
    return 1
  }
  hash=$(system_alias_hash cwd-pin)
  [ -n "$hash" ] || return 1

  sudoers_dir="$TEST_ROOT/sudoers-cwd.d"
  mkdir -p "$sudoers_dir" || return 1
  chmod 755 "$sudoers_dir" || return 1
  export HOLD_TEST_SUDOERS_DIR="$sudoers_dir"
  visudo_ok="$TEST_ROOT/visudo-cwd-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$visudo_ok" || return 1
  chmod 755 "$visudo_ok" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_ok"

  as_root "$safe" grant cwd-pin "$TEST_USER" start >"$TEST_ROOT/grant-cwd.out" 2>"$TEST_ROOT/grant-cwd.err" || {
    cat "$TEST_ROOT/grant-cwd.out" "$TEST_ROOT/grant-cwd.err" >&2
    return 1
  }
  grant_public="$HOLD_TEST_SYSTEM_STATE_DIR/public/grants/users/$TEST_USER/cwd-pin.json"
  grant_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ -n "$grant_hash" ] || { as_root cat "$grant_public" >&2; return 1; }
  cap_token="eyJ2IjoxLCJvcCI6InN0YXJ0IiwiZm9yY2UiOmZhbHNlLCJkZXRhY2giOnRydWV9"
  hostile="$ACTOR_HOME/hostile-cwd"
  mkdir -p "$hostile" || return 1
  cap_out=$(cd "$hostile" && as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run cwd-pin --cap "$grant_hash" "$cap_token" 2>&1) || { printf '%s\n' "$cap_out" >&2; return 1; }
  cap_id=$(printf '%s\n' "$cap_out" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$cap_out" >&2; return 1; }
  log=$(root_log_path "$cap_id" "$HOLD_TEST_SYSTEM_STATE_DIR/logs") || return 1
  for _ in $(seq 1 30); do
    as_root cat "$log" >"$TEST_ROOT/cwd-pin.log" 2>/dev/null || true
    if grep -qx '/' "$TEST_ROOT/cwd-pin.log"; then
      break
    fi
    sleep 0.1
  done
  grep -qx '/' "$TEST_ROOT/cwd-pin.log" || { as_root cat "$log" >&2; return 1; }
  ! grep -Fxq "$hostile" "$TEST_ROOT/cwd-pin.log" || { cat "$TEST_ROOT/cwd-pin.log" >&2; return 1; }
}

test_grant_revoke_writes_hash_scoped_sudoers() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local safe safe_real out id hash grant_hash old_grant_hash grant_digest raw_digest cap_token cap_out cap_id sudoers_dir sudoers_file grant_private grant_public grant_private_saved grant_public_saved public_bad_hash semantic_digest rc visudo_ok sh_bin source_profiles source_profiles_before vector_json vector_digest docker_vector_digest docker_grant_hash
  vector_json="$TEST_ROOT/grant-vector.json"
  cat >"$vector_json" <<'EOF' || return 1
{
  "actions": ["stop", "start"],
  "argv": ["/usr/bin/python3", "/srv/app/server.py", "--port", "3000"],
  "binary_path": "/usr/bin/python3",
  "profile": "web",
  "subject": "alice",
  "schema": "hold.subject-grant.v1"
}
EOF
  vector_digest=$(grant_canonical_digest_file "$vector_json") || return 1
  [ "$vector_digest" = "9b1a3df24cf16eedc7604919ede6e5f972e9a86bf9c5e37e95339298b4481e72" ] || {
    echo "canonical grant digest vector mismatch: $vector_digest" >&2
    return 1
  }
  cat >"$vector_json.docker" <<'EOF' || return 1
{
  "actions": ["stop", "start"],
  "Path": "/usr/bin/python3",
  "Args": ["/srv/app/server.py", "--port", "3000"],
  "profile": "web",
  "subject": "alice",
  "schema": "hold.subject-grant.v1"
}
EOF
  docker_vector_digest=$(grant_canonical_digest_file "$vector_json.docker") || return 1
  [ "$docker_vector_digest" = "$vector_digest" ] || {
    echo "Docker-shaped grant digest vector mismatch: $docker_vector_digest != $vector_digest" >&2
    return 1
  }
  safe=$(root_safe_hold_copy hold-safe) || return 1
  sh_bin="$(resolve_path /bin/sh)" || return 1
  safe_real="$(cd "$(dirname "$safe")" && pwd -P)/$(basename "$safe")" || return 1
  sudoers_dir="$TEST_ROOT/sudoers.d"
  mkdir -p "$sudoers_dir" || return 1
  chmod 755 "$sudoers_dir" || return 1
  export HOLD_TEST_SUDOERS_DIR="$sudoers_dir"
  visudo_ok="$TEST_ROOT/visudo-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$visudo_ok" || return 1
  chmod 755 "$visudo_ok" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_ok"

  out=$(as_root "$safe" -d /bin/sh -c ':' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_root "$safe" profile save "$id" as web-sys >"$TEST_ROOT/root-alias.out" 2>"$TEST_ROOT/root-alias.err" || return 1
  [ ! -s "$TEST_ROOT/root-alias.out" ] || return 1
  grep -Fqx "hold: pinned 'web-sys' -> $sh_bin -c :" "$TEST_ROOT/root-alias.err" || return 1
  hash=$(system_alias_hash web-sys)
  [ -n "$hash" ] || return 1
  source_profiles="$HOLD_TEST_SYSTEM_STATE_DIR/profiles.json"
  source_profiles_before="$TEST_ROOT/source-profiles.before"
  as_root cp "$source_profiles" "$source_profiles_before" || return 1
  ! root_grep '"source_hash"' "$source_profiles" || { echo "source profile store persisted source_hash" >&2; as_root cat "$source_profiles" >&2; return 1; }
  ! root_grep '"hash":' "$source_profiles" || { echo "source profile store persisted object-local hash field" >&2; as_root cat "$source_profiles" >&2; return 1; }
  set +e
  as_root "$safe" grant "$hash" "$TEST_USER" start >"$TEST_ROOT/grant-hash.out" 2>"$TEST_ROOT/grant-hash.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/grant-hash.out" "$TEST_ROOT/grant-hash.err" >&2; return 1; }
  grep -q 'existing system profile' "$TEST_ROOT/grant-hash.err" || { cat "$TEST_ROOT/grant-hash.err" >&2; return 1; }
  as_root "$safe" grant web-sys "$TEST_USER" start,stop >"$TEST_ROOT/grant.out" 2>"$TEST_ROOT/grant.err" || { cat "$TEST_ROOT/grant.out" "$TEST_ROOT/grant.err" >&2; return 1; }
  sudoers_file="$sudoers_dir/hold_web-sys_$TEST_USER"
  root_file_exists "$sudoers_file" || { echo "missing $sudoers_file" >&2; cat "$TEST_ROOT/grant.err" >&2; return 1; }
  grant_private="$HOLD_TEST_SYSTEM_STATE_DIR/grants/users/$TEST_USER/web-sys.json"
  grant_public="$HOLD_TEST_SYSTEM_STATE_DIR/public/grants/users/$TEST_USER/web-sys.json"
  root_file_exists "$grant_private" || { echo "missing $grant_private" >&2; return 1; }
  root_file_exists "$grant_public" || { echo "missing $grant_public" >&2; return 1; }
  ! root_grep '"source_hash"' "$grant_private" || { echo "private grant persisted source_hash" >&2; as_root cat "$grant_private" >&2; return 1; }
  ! root_grep '"hash"' "$grant_private" || { echo "private grant persisted hash" >&2; as_root cat "$grant_private" >&2; return 1; }
  grant_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ -n "$grant_hash" ] || { as_root cat "$grant_public" >&2; return 1; }
  grant_digest=$(grant_canonical_digest_file "$grant_private") || return 1
  [ "$grant_hash" = "$grant_digest" ] || { echo "grant hash $grant_hash != canonical private digest $grant_digest" >&2; return 1; }
  raw_digest=$(as_root cat "$grant_private" | sha256_stdin) || return 1
  root_grep '# actions-list: start,stop' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$safe_real ^run web-sys --cap $grant_hash [A-Za-z0-9_-]{1,768}$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$safe_real ^--system --elevated stop ([A-Fa-f0-9]{12,64}|ffffffffffff) web-sys $grant_hash$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }

  make_fake_sudo || return 1
  set +e
  as_user "$safe" --system start web-sys >"$TEST_ROOT/granted-start.out" 2>"$TEST_ROOT/granted-start.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || { cat "$TEST_ROOT/granted-start.out" "$TEST_ROOT/granted-start.err" >&2; return 1; }
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[0]}" = "--" ] || return 1
  [ "${args[1]}" = "$safe_real" ] || return 1
  [ "${args[2]}" = "run" ] || return 1
  [ "${args[3]}" = "web-sys" ] || return 1
  [ "${args[4]}" = "--cap" ] || return 1
  [ "${args[5]}" = "$grant_hash" ] || return 1
  [ "${args[6]}" = "eyJ2IjoxLCJvcCI6InN0YXJ0IiwiZm9yY2UiOmZhbHNlLCJkZXRhY2giOnRydWV9" ] || return 1
  ! grep -qx -- '--system' "$HOLD_FAKE_SUDO_ARGV" || return 1
  ! grep -qx -- '--elevated' "$HOLD_FAKE_SUDO_ARGV" || return 1

  cap_token="eyJ2IjoxLCJvcCI6InN0YXJ0IiwiZm9yY2UiOmZhbHNlfQ"
  cap_out=$(as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" 2>&1) || { printf '%s\n' "$cap_out" >&2; return 1; }
  cap_id=$(printf '%s\n' "$cap_out" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$cap_out" >&2; return 1; }
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" --system --elevated kill "$cap_id" web-sys "$grant_hash" >"$TEST_ROOT/grant-kill-denied.out" 2>"$TEST_ROOT/grant-kill-denied.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "kill action unexpectedly accepted by start,stop grant" >&2; return 1; }
  grep -q 'capability' "$TEST_ROOT/grant-kill-denied.err" || { cat "$TEST_ROOT/grant-kill-denied.err" >&2; return 1; }
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" --system --elevated stop "$cap_id" web-sys "$grant_hash" >/dev/null 2>"$TEST_ROOT/grant-stop.err" || {
      cat "$TEST_ROOT/grant-stop.err" >&2
      return 1
    }
  grant_private_saved="$grant_private.saved"
  as_root cp "$grant_private" "$grant_private_saved" || return 1
  grant_public_saved="$grant_public.saved"
  as_root cp "$grant_public" "$grant_public_saved" || return 1
  as_root env GRANT_PUBLIC="$grant_public" python3 - <<'PY' || return 1
import json, os
path = os.environ["GRANT_PUBLIC"]
with open(path, "r", encoding="utf-8") as f:
    obj = json.load(f)
obj["hash"] = "0" * 64
with open(path, "w", encoding="utf-8") as f:
    json.dump(obj, f, sort_keys=True)
    f.write("\n")
PY
  public_bad_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ "$public_bad_hash" = "0000000000000000000000000000000000000000000000000000000000000000" ] || return 1
  cap_out=$(as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" 2>&1) || { printf '%s\n' "$cap_out" >&2; return 1; }
  cap_id=$(printf '%s\n' "$cap_out" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$cap_out" >&2; return 1; }
  as_root "$safe" stop "$cap_id" >/dev/null 2>&1 || true
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$public_bad_hash" "$cap_token" >"$TEST_ROOT/cap-public-cache-tamper.out" 2>"$TEST_ROOT/cap-public-cache-tamper.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "corrupted public digest cache was accepted as authority" >&2; return 1; }
  grep -q 'capability' "$TEST_ROOT/cap-public-cache-tamper.err" || { cat "$TEST_ROOT/cap-public-cache-tamper.err" >&2; return 1; }
  as_root cp "$grant_public_saved" "$grant_public" || return 1
  as_root env GRANT_PRIVATE="$grant_private" python3 - <<'PY' || return 1
import json, os
path = os.environ["GRANT_PRIVATE"]
with open(path, "r", encoding="utf-8") as f:
    obj = json.load(f)
with open(path, "w", encoding="utf-8") as f:
    f.write("{\n")
    f.write('  "actions": ' + json.dumps(obj["actions"]) + ",\n")
    f.write('  "argv": ' + json.dumps(obj["argv"]) + ",\n")
    f.write('  "binary_path": ' + json.dumps(obj["binary_path"]) + ",\n")
    for key in ("env", "ports", "volumes", "cap_add", "cap_drop", "interactive", "tty", "detach", "allow_multi", "restart", "restart_delay_seconds", "log_destination"):
        if key in obj:
            f.write('  ' + json.dumps(key) + ': ' + json.dumps(obj[key]) + ",\n")
    f.write('  "profile": ' + json.dumps(obj["profile"]) + ",\n")
    f.write('  "subject": ' + json.dumps(obj["subject"]) + ",\n")
    f.write('  "schema": ' + json.dumps(obj["schema"]) + "\n")
    f.write("}\n")
PY
  [ "$(grant_canonical_digest_file "$grant_private")" = "$grant_hash" ] || return 1
  [ "$(as_root cat "$grant_private" | sha256_stdin)" != "$raw_digest" ] || { echo "reformat did not change raw file digest; test setup invalid" >&2; return 1; }
  cap_out=$(as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" 2>&1) || { printf '%s\n' "$cap_out" >&2; return 1; }
  cap_id=$(printf '%s\n' "$cap_out" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$cap_out" >&2; return 1; }
  as_root "$safe" stop "$cap_id" >/dev/null 2>&1 || true

  as_root cp "$grant_private_saved" "$grant_private" || return 1
  as_root env GRANT_PRIVATE="$grant_private" python3 - <<'PY' || return 1
import json, os
path = os.environ["GRANT_PRIVATE"]
with open(path, "r", encoding="utf-8") as f:
    obj = json.load(f)
argv = list(obj["argv"])
out = {
    "schema": obj["schema"],
    "subject": obj["subject"],
    "profile": obj["profile"],
    "actions": obj["actions"],
    "Path": argv[0],
    "Args": argv[1:],
    "Config": {
        "OpenStdin": bool(obj.get("interactive", False)),
        "Tty": bool(obj.get("tty", False)),
        "Env": obj.get("env", []),
        "CapAdd": obj.get("cap_add"),
        "CapDrop": obj.get("cap_drop"),
        "LogConfig": {"Type": obj.get("log_destination", "local")},
    },
}
for key in ("ports", "volumes", "detach", "allow_multi", "restart", "restart_delay_seconds"):
    if key in obj:
        out[key] = obj[key]
with open(path, "w", encoding="utf-8") as f:
    json.dump(out, f, sort_keys=True)
    f.write("\n")
PY
  docker_grant_hash=$(grant_canonical_digest_file "$grant_private") || return 1
  [ "$docker_grant_hash" = "$grant_hash" ] || { echo "Docker-shaped private grant digest mismatch: $docker_grant_hash != $grant_hash" >&2; as_root cat "$grant_private" >&2; return 1; }
  cap_out=$(as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$docker_grant_hash" "$cap_token" 2>&1) || { printf '%s\n' "$cap_out" >&2; as_root cat "$grant_private" >&2; return 1; }
  cap_id=$(printf '%s\n' "$cap_out" | extract_id)
  [ -n "$cap_id" ] || { printf '%s\n' "$cap_out" >&2; as_root cat "$grant_private" >&2; return 1; }
  as_root "$safe" stop "$cap_id" >/dev/null 2>&1 || true
  as_root cp "$grant_private_saved" "$grant_private" || return 1

  as_root env GRANT_PRIVATE="$grant_private" python3 - <<'PY' || return 1
import json, os
path = os.environ["GRANT_PRIVATE"]
with open(path, "r", encoding="utf-8") as f:
    obj = json.load(f)
obj["argv"] = list(obj["argv"])
obj["argv"][-1] = obj["argv"][-1] + "-semantic-drift"
with open(path, "w", encoding="utf-8") as f:
    json.dump(obj, f, sort_keys=True)
    f.write("\n")
PY
  semantic_digest=$(grant_canonical_digest_file "$grant_private") || return 1
  [ "$semantic_digest" != "$grant_hash" ] || { echo "argv semantic drift did not change canonical digest" >&2; return 1; }
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/cap-argv-tamper.out" 2>"$TEST_ROOT/cap-argv-tamper.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "old capability survived argv semantic drift" >&2; return 1; }
  grep -q 'capability' "$TEST_ROOT/cap-argv-tamper.err" || { cat "$TEST_ROOT/cap-argv-tamper.err" >&2; return 1; }
  as_root cp "$grant_private_saved" "$grant_private" || return 1
  as_root sh -c '
    tmp="$1.tmp"
    while IFS= read -r line; do
      case "$line" in
        *\"actions\"*) printf "%s\n" "  \"actions\": [\"stop\"]" ;;
        *) printf "%s\n" "$line" ;;
      esac
    done < "$1" > "$tmp" &&
    cat "$tmp" > "$1" &&
    rm -f "$tmp"
  ' sh "$grant_private" || return 1
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/cap-action-tamper.out" 2>"$TEST_ROOT/cap-action-tamper.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  grep -q 'capability' "$TEST_ROOT/cap-action-tamper.err" || { cat "$TEST_ROOT/cap-action-tamper.err" >&2; return 1; }
  as_root cp "$grant_private_saved" "$grant_private" || return 1
  as_root sh -c 'printf "\n" >> "$1"' sh "$grant_private" || return 1
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/cap-tamper.out" 2>"$TEST_ROOT/cap-tamper.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/cap-tamper.err" >&2; return 1; }
  cap_id=$(cat "$TEST_ROOT/cap-tamper.out" | extract_id)
  [ -n "$cap_id" ] || { cat "$TEST_ROOT/cap-tamper.out" >&2; return 1; }
  as_root "$safe" stop "$cap_id" >/dev/null 2>&1 || true

  old_grant_hash="$grant_hash"
  as_root "$safe" revoke web-sys "$TEST_USER" start >"$TEST_ROOT/revoke.out" 2>"$TEST_ROOT/revoke.err" || { cat "$TEST_ROOT/revoke.out" "$TEST_ROOT/revoke.err" >&2; return 1; }
  grant_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ -n "$grant_hash" ] || { as_root cat "$grant_public" >&2; return 1; }
  [ "$grant_hash" != "$old_grant_hash" ] || { echo "grant digest did not change after action narrowing" >&2; return 1; }
  [ "$grant_hash" = "$(grant_canonical_digest_file "$grant_private")" ] || return 1
  root_grep '# actions-list: stop' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  ! root_grep '^run web-sys --cap' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$safe_real ^--system --elevated stop ([A-Fa-f0-9]{12,64}|ffffffffffff) web-sys $grant_hash$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  set +e
  as_root env SUDO_UID="$TEST_UID" SUDO_GID="$TEST_GID" SUDO_USER="$TEST_USER" HOLD_TEST_INVOKING_HOME="$ACTOR_HOME" \
    "$safe" run web-sys --cap "$grant_hash" "$cap_token" >"$TEST_ROOT/cap-start-with-stop-only.out" 2>"$TEST_ROOT/cap-start-with-stop-only.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || return 1
  grep -q 'capability' "$TEST_ROOT/cap-start-with-stop-only.err" || { cat "$TEST_ROOT/cap-start-with-stop-only.err" >&2; return 1; }

  as_root "$safe" grant web-sys "$TEST_USER" >"$TEST_ROOT/grant-all.out" 2>"$TEST_ROOT/grant-all.err" || { cat "$TEST_ROOT/grant-all.out" "$TEST_ROOT/grant-all.err" >&2; return 1; }
  grant_hash=$(as_root sed -n 's/.*"hash": "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$grant_public")
  [ -n "$grant_hash" ] || { as_root cat "$grant_public" >&2; return 1; }
  root_grep '# actions: ALL' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep '# actions-list: start,stop,kill,tail,dump,prune,console' "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$safe_real ^run web-sys --cap $grant_hash [A-Za-z0-9_-]{1,768}$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  root_grep "$safe_real ^--system --elevated console [A-Fa-f0-9]{12,64} web-sys $grant_hash$" "$sudoers_file" || { as_root cat "$sudoers_file" >&2; return 1; }
  as_root cmp "$source_profiles_before" "$source_profiles" || { echo "source profile store changed during grant-specific edits" >&2; return 1; }
  as_root "$safe" revoke web-sys "$TEST_USER" >"$TEST_ROOT/revoke-all.out" 2>"$TEST_ROOT/revoke-all.err" || { cat "$TEST_ROOT/revoke-all.out" "$TEST_ROOT/revoke-all.err" >&2; return 1; }
  root_path_absent "$sudoers_file"
  root_path_absent "$grant_private"
  root_path_absent "$grant_public"
}

test_elevated_capability_start_and_stop_validate_alias_hash() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe out id hash start_out cap_id rc
  safe=$(root_safe_hold_copy hold-cap) || return 1

  out=$(as_root "$safe" -d /bin/sh -c 'while :; do sleep 1; done' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_root "$safe" profile save "$id" as web-cap >"$TEST_ROOT/cap-alias.out" 2>"$TEST_ROOT/cap-alias.err" || return 1
  [ ! -s "$TEST_ROOT/cap-alias.out" ] || return 1
  hash=$(system_alias_hash web-cap)
  [ -n "$hash" ] || return 1
  as_root "$safe" stop "$id" >/dev/null || return 1
  as_root "$safe" prune "$id" >/dev/null || return 1

  start_out=$(as_root "$safe" --system --elevated start 000000000000 web-cap "$hash" 2>&1) || return 1
  cap_id=$(printf '%s\n' "$start_out" | extract_id)
  [ -n "$cap_id" ] || return 1
  cap_record=$(root_record_path "$cap_id" "$HOLD_TEST_SYSTEM_STATE_DIR/runs") || return 1
  root_grep '"alias": "web-cap"' "$cap_record" || return 1
  ! root_grep '"profile_hash":' "$cap_record" || return 1

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
  out=$("$HOLD_BIN" -d sh -c 'printf "arg:%s\n" "$1"' sh --system 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  "$HOLD_BIN" logs --plain "$id" >"$TEST_ROOT/raw-start.log" || return 1
  grep -q '^arg:--system$' "$TEST_ROOT/raw-start.log"
}

test_public_root_index_list_is_redacted() {
  write_public_index_fixture abc12345cafe running 2026-06-15T18:42:11Z || return 1
  "$HOLD_BIN" list --iso >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  grep -Eq '^abc12345cafe[[:space:]]+-[[:space:]]+running[[:space:]]+2026-06-15T18:42:11Z[[:space:]]+<root-managed>$' "$TEST_ROOT/list.out" || return 1
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

test_public_root_index_list_reads_projected_state() {
  local id=deadbeefcafe
  mkdir -p "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  chmod 755 "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" || return 1
  cat > "$HOLD_TEST_SYSTEM_STATE_DIR/public/$id.json" <<'JSON'
{
  "id": "deadbeefcafe",
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
  "$HOLD_BIN" list --iso >"$TEST_ROOT/list.out" 2>"$TEST_ROOT/list.err" || return 1
  grep -Eq '^deadbeefcafe[[:space:]]+-[[:space:]]+exited[[:space:]]+2026-06-15T18:42:11Z[[:space:]]+<root-managed>$' "$TEST_ROOT/list.out" || { cat "$TEST_ROOT/list.out" >&2; return 1; }
  ! grep -q 'secret' "$TEST_ROOT/list.out"
}

test_user_local_wins_over_public_root_collision() {
  local out id pgid
  out=$("$HOLD_BIN" -d sleep 300 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  pgid=$(record_pgid "$id")
  [ -n "$id" ] && [ -n "$pgid" ] || return 1
  write_public_index_fixture "$id" running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  [ ! -s "$HOLD_FAKE_SUDO_ARGV" ] || return 1
  pgid_terminated "$pgid" || return 1
  record_exists "$id" "$HOLD_TEST_SYSTEM_STATE_DIR/public"
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



test_action_self_elevation_uses_argv_fork_wait() {
  local rc
  write_public_index_fixture abc12345cafe running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  set +e
  "$HOLD_BIN" stop abc12345cafe >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${#args[@]}" -eq 6 ] || return 1
  [ "${args[0]}" = "--" ] || return 1
  [ -x "${args[1]}" ] || return 1
  [ "${args[2]}" = "--system" ] || return 1
  [ "${args[3]}" = "--elevated" ] || return 1
  [ "${args[4]}" = "stop" ] || return 1
  [ "${args[5]}" = "system:abc12345cafe" ] || return 1
  ! grep -qx -- 'sh\|-c' "$HOLD_FAKE_SUDO_ARGV"
}

test_elevated_action_returns_child_status() {
  local rc
  write_public_index_fixture abc12345cafe running 2026-06-15T18:42:11Z || return 1
  make_fake_sudo || return 1
  export HOLD_FAKE_SUDO_RC=42
  set +e
  "$HOLD_BIN" kill abc12345cafe >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 42 ]
}

test_sudo_exec_failure_returns_clean_error() {
  local rc
  write_public_index_fixture abc12345cafe running 2026-06-15T18:42:11Z || return 1
  export HOLD_TEST_SUDO_PROG="$TEST_ROOT/missing-sudo"
  set +e
  "$HOLD_BIN" stop abc12345cafe >"$TEST_ROOT/stdout" 2>"$TEST_ROOT/stderr"
  rc=$?
  set -e
  [ "$rc" -eq 127 ] || return 1
  grep -q 'failed to exec sudo' "$TEST_ROOT/stderr"
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

test_system_switch_canonicalizes_owned_command() {
  local rc
  make_fake_sudo || return 1
  set +e
  "$HOLD_BIN" --system stop abc12345cafe >/dev/null 2>"$TEST_ROOT/one.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  cp "$HOLD_FAKE_SUDO_ARGV" "$TEST_ROOT/one.argv" || return 1
  set +e
  "$HOLD_BIN" stop abc12345cafe --system >/dev/null 2>"$TEST_ROOT/two.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  cmp -s "$TEST_ROOT/one.argv" "$HOLD_FAKE_SUDO_ARGV" || return 1
  grep -qx -- '--system' "$TEST_ROOT/one.argv" || return 1
  grep -qx -- '--elevated' "$TEST_ROOT/one.argv" || return 1
  tail -n 2 "$TEST_ROOT/one.argv" | grep -qx 'abc12345cafe'

  set +e
  "$HOLD_BIN" --system run -d -- /bin/true --child-flag >/dev/null 2>"$TEST_ROOT/run-system.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "run" ] || return 1
  [ "${args[5]}" = "--" ] || return 1
  [ "${args[6]}" = "/bin/true" ] || return 1
  [ "${args[7]}" = "--child-flag" ] || return 1

  set +e
  "$HOLD_BIN" --system run --tail --console -- /bin/true >/dev/null 2>"$TEST_ROOT/run-system-flags.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "run" ] || return 1
  [ "${args[5]}" = "--tail" ] || return 1
  [ "${args[6]}" = "--console" ] || return 1
  [ "${args[7]}" = "--" ] || return 1
  [ "${args[8]}" = "/bin/true" ] || return 1

  set +e
  "$HOLD_BIN" --system start web-prof --force >/dev/null 2>"$TEST_ROOT/start-force.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "start" ] || return 1
  [ "${args[5]}" = "--force" ] || return 1
  [ "${args[6]}" = "web-prof" ] || return 1

  set +e
  "$HOLD_BIN" --system start web-prof --multi 2 >/dev/null 2>"$TEST_ROOT/start-multi.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "start" ] || return 1
  [ "${args[5]}" = "--multi" ] || return 1
  [ "${args[6]}" = "2" ] || return 1
  [ "${args[7]}" = "web-prof" ] || return 1
}

test_system_raw_self_elevation_preserves_child_switches_and_delimiter() {
  local rc
  make_fake_sudo || return 1
  set +e
  "$HOLD_BIN" --system -d child-command --system >/dev/null 2>"$TEST_ROOT/raw.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "child-command" ] || return 1
  [ "${args[5]}" = "--system" ] || return 1

  set +e
  "$HOLD_BIN" --system -d -- list --system >/dev/null 2>"$TEST_ROOT/delim.err"
  rc=$?
  set -e
  [ "$rc" -eq 77 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[4]}" = "--" ] || return 1
  [ "${args[5]}" = "list" ] || return 1
  [ "${args[6]}" = "--system" ] || return 1
}

test_elevated_requires_root() {
  local rc
  make_fake_sudo || return 1
  export HOLD_FAKE_SUDO_RC=1
  set +e
  "$HOLD_BIN" --elevated stop abc12345cafe >/dev/null 2>"$TEST_ROOT/elevated.err"
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || return 1
  args=()
  while IFS= read -r line; do args+=("$line"); done < "$HOLD_FAKE_SUDO_ARGV"
  [ "${args[0]}" = "--" ] || return 1
  [ "${args[2]}" = "--system" ] || return 1
  [ "${args[3]}" = "--elevated" ] || return 1
  [ "${args[4]}" = "stop" ] || return 1
  [ "${args[5]}" = "abc12345cafe" ] || return 1
  ! grep -q -- '--elevated without root authority' "$TEST_ROOT/elevated.err"
}


test_long_command_list_truncates_instead_of_skips() {
  local out id long_arg list_out
  long_arg=$(printf 'x%.0s' $(seq 1 140))
  out=$("$HOLD_BIN" -d /bin/echo "$long_arg" 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  sleep 0.2
  list_out=$("$HOLD_BIN" list) || return 1
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
  "$HOLD_BIN" profile save fe4a4b8fbc06 as docker-derived \
    >"$TEST_ROOT/docker-derived.out" 2>"$TEST_ROOT/docker-derived.err" || {
      cat "$TEST_ROOT/docker-derived.out" "$TEST_ROOT/docker-derived.err" >&2
      return 1
    }
  grep -Fq '"docker-derived": {"bin": ' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
  grep -Fq 'http.server' "$store/aliases.json" || { cat "$store/aliases.json" >&2; return 1; }
}

test_root_start_writes_system_store_and_public_unknown() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id mode list_out
  out=$(as_root "$HOLD_REAL_BIN" true 2>&1) || return 1
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
  list_out=$("$HOLD_BIN" list) || return 1
  printf '%s
' "$list_out" | grep -Eq "^$id[[:space:]]+-[[:space:]]+(running|exited|Exited|Up)[[:space:]]"
}

test_sudo_start_writes_system_store_with_invoking_metadata() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id json
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" true 2>&1) || return 1
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

test_home_elevated_run_alias_stays_user_local() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local app out id aliases
  app=$(make_sleeping_home_executable) || return 1
  out=$(as_sudo_from_user "$HOLD_REAL_BIN" --system -d "$app" 2>&1) || return 1
  assert_home_system_start_is_user_local "$out" || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  as_user "$HOLD_REAL_BIN" profile save "$id" as home-elevated >/dev/null || {
    as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true
    return 1
  }
  as_sudo_from_user "$HOLD_REAL_BIN" stop "$id" >/dev/null 2>&1 || true
  aliases="$ACTOR_HOME/.local/state/hold/aliases.json"
  root_file_exists "$aliases" || return 1
  root_grep '"home-elevated"' "$aliases" || return 1
  root_path_absent "$HOLD_TEST_SYSTEM_STATE_DIR/public/aliases.json" || return 1
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

test_hold_unified_cli_surface() {
  local out id id2 id3
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo hold-run-line' 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" logs "$id" >"$TEST_ROOT/hold-logs.out" 2>"$TEST_ROOT/hold-logs.err" || {
    cat "$TEST_ROOT/hold-logs.out" "$TEST_ROOT/hold-logs.err" >&2
    return 1
  }
  grep -q 'hold-run-line' "$TEST_ROOT/hold-logs.out" || { cat "$TEST_ROOT/hold-logs.out" >&2; return 1; }
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/hold-inspect.out" || return 1
  grep -q 'hold-run-line' "$TEST_ROOT/hold-inspect.out" || return 1
  "$HOLD_BIN" status >"$TEST_ROOT/hold-status.out" || return 1
  grep -q "$id" "$TEST_ROOT/hold-status.out" || return 1
  "$HOLD_BIN" show runs >"$TEST_ROOT/hold-show-runs.out" || return 1
  grep -q 'ID' "$TEST_ROOT/hold-show-runs.out" || return 1
  "$HOLD_BIN" doctor >"$TEST_ROOT/hold-doctor.out" || return 1
  grep -q 'version:' "$TEST_ROOT/hold-doctor.out" || return 1

  out=$("$HOLD_BIN" run -d -- /usr/bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as hold-web >/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
  "$HOLD_BIN" prune "$id" >/dev/null || return 1
  "$HOLD_BIN" profiles >"$TEST_ROOT/hold-profiles.out" || return 1
  grep -q 'hold-web' "$TEST_ROOT/hold-profiles.out" || return 1
  set +e; "$HOLD_BIN" profile list >"$TEST_ROOT/hold-profile-list.out" 2>"$TEST_ROOT/hold-profile-list.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold profile list: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/hold-profile-list.err" || { cat "$TEST_ROOT/hold-profile-list.err" >&2; return 1; }
  "$HOLD_BIN" show profiles >"$TEST_ROOT/hold-show-profiles.out" || return 1
  grep -q 'hold-web' "$TEST_ROOT/hold-show-profiles.out" || return 1
  out=$("$HOLD_BIN" profile run hold-web 2>&1) || return 1
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" show running hold-web >"$TEST_ROOT/hold-show-running.out" || return 1
  grep -q "$id2" "$TEST_ROOT/hold-show-running.out" || { cat "$TEST_ROOT/hold-show-running.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id2" >/dev/null || return 1
  "$HOLD_BIN" clean hold-web >/dev/null || return 1
}

test_docker_shaped_cli_flags_and_rm() {
  local out id id2
  out=$("$HOLD_BIN" run -d --name docker-web -e HOLD_DOCKER_ENV=ok -- /bin/sh -c 'echo "$HOLD_DOCKER_ENV"; sleep 0.1' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.2
  "$HOLD_BIN" logs -n 1 --plain "$id" >"$TEST_ROOT/docker-logs-tail.out" || return 1
  grep -q '^ok$' "$TEST_ROOT/docker-logs-tail.out" || { cat "$TEST_ROOT/docker-logs-tail.out" >&2; return 1; }
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
  "$HOLD_BIN" ps -a >"$TEST_ROOT/docker-ps.out" || return 1
  grep -Eq "^RUN ID[[:space:]]+PROFILE[[:space:]]+COMMAND[[:space:]]+CREATED[[:space:]]+STATUS[[:space:]]+PORTS[[:space:]]+NAMES" "$TEST_ROOT/docker-ps.out" || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }
  grep -Eq "^$id[[:space:]]+-[[:space:]].*Exited.*docker-web$" "$TEST_ROOT/docker-ps.out" || { cat "$TEST_ROOT/docker-ps.out" >&2; return 1; }
  "$HOLD_BIN" profiles >"$TEST_ROOT/docker-profiles.out" || return 1
  ! grep -q 'docker-web' "$TEST_ROOT/docker-profiles.out" || { cat "$TEST_ROOT/docker-profiles.out" >&2; return 1; }

  out=$("$HOLD_BIN" -d --name docker-direct /bin/sh -c 'echo direct; sleep 5' 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id2=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id2" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" rm --force docker-direct >/dev/null || return 1
  set +e; "$HOLD_BIN" inspect "$id2" >"$TEST_ROOT/docker-rm-inspect.out" 2>&1; rc=$?; set -e
  [ "$rc" -ne 0 ] || { cat "$TEST_ROOT/docker-rm-inspect.out" >&2; return 1; }
  "$HOLD_BIN" profiles >"$TEST_ROOT/docker-profiles-after-rm.out" || return 1
  ! grep -q 'docker-direct' "$TEST_ROOT/docker-profiles-after-rm.out" || { cat "$TEST_ROOT/docker-profiles-after-rm.out" >&2; return 1; }
}

test_docker_bare_launch_foreground_follows_output_by_default() {
  local out id
  out=$("$HOLD_BIN" -- /bin/sh -c 'echo bare-foreground-ok' 2>"$TEST_ROOT/docker-bare-foreground.err") || {
    cat "$TEST_ROOT/docker-bare-foreground.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  ! grep -q '^bare-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }

  out=$("$HOLD_BIN" -d -- /bin/sh -c 'echo bare-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-bare-detached.err") || {
    cat "$TEST_ROOT/docker-bare-detached.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^bare-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}


test_docker_run_foreground_follows_output_by_default() {
  local out id
  out=$("$HOLD_BIN" run -- /bin/sh -c 'echo docker-foreground-ok' 2>"$TEST_ROOT/docker-foreground.err") || {
    cat "$TEST_ROOT/docker-foreground.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  grep -q '^docker-foreground-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }

  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo docker-detached-hidden; sleep 0.1' 2>"$TEST_ROOT/docker-detached.err") || {
    cat "$TEST_ROOT/docker-detached.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  if grep -q '^docker-detached-hidden$' <<<"$out"; then
    printf '%s\n' "$out" >&2
    return 1
  fi
}

test_docker_unsupported_options_fail_loudly() {
  local rc
  "$HOLD_BIN" start /bin/true --detach-keys ctrl-p,ctrl-q >/dev/null 2>"$TEST_ROOT/docker-detach-keys-default.err" || {
    cat "$TEST_ROOT/docker-detach-keys-default.err" >&2
    return 1
  }

  "$HOLD_BIN" start /bin/true --detach-keys ctrl-a >/dev/null 2>"$TEST_ROOT/docker-detach-keys-custom.err" || {
    cat "$TEST_ROOT/docker-detach-keys-custom.err" >&2
    return 1
  }

  set +e
  "$HOLD_BIN" start /bin/true --detach-keys ctrl-not-a-key >/dev/null 2>"$TEST_ROOT/docker-detach-keys-invalid.err"
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
  out=$("$HOLD_BIN" run -d --restart on-failure:2 --restart-delay 0 -- /bin/sh -c 'mkdir -p "$HOME"; echo attempt >> "$HOME/restart-count"; c=$(wc -l < "$HOME/restart-count"); echo restart-attempt-$c; [ "$c" -lt 3 ] && exit 7 || sleep 1' 2>&1) || {
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


test_docker_restart_validation_and_tty_gate() {
  local rc
  set +e
  "$HOLD_BIN" run --restart nonsense /bin/true >/dev/null 2>"$TEST_ROOT/restart-invalid.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "invalid restart rc=$rc" >&2; return 1; }
  grep -q 'invalid --restart policy' "$TEST_ROOT/restart-invalid.err" || { cat "$TEST_ROOT/restart-invalid.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" run --restart-delay 1 /bin/true >/dev/null 2>"$TEST_ROOT/restart-delay-alone.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "restart-delay without restart rc=$rc" >&2; return 1; }
  grep -q -- '--restart-delay requires --restart' "$TEST_ROOT/restart-delay-alone.err" || { cat "$TEST_ROOT/restart-delay-alone.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" run -t --restart always /bin/true >/dev/null 2>"$TEST_ROOT/restart-tty.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { echo "restart tty rc=$rc" >&2; return 1; }
  grep -q -- '--restart with --tty/-t is not supported yet' "$TEST_ROOT/restart-tty.err" || { cat "$TEST_ROOT/restart-tty.err" >&2; return 1; }
}




test_start_existing_run_appends_retained_log() {
  local out id log before_count after_count restart_out
  out=$("$HOLD_BIN" run -d -- /bin/sh -c 'echo restart-append-marker' 2>&1) || {
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
  restart_out=$("$HOLD_BIN" start "$id" 2>&1) || {
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

test_docker_restart_persists_with_named_profile() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id replay_id
  "$HOLD_BIN" profile restartprof -d --restart on-failure:1 --restart-delay 1 -- /bin/sh -c 'echo restart-profile; sleep 0.1' >"$TEST_ROOT/restartprof-create.out" || return 1
  out=$("$HOLD_BIN" run -d restartprof 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.2
  "$HOLD_BIN" profile export restartprof --json >"$TEST_ROOT/restartprof.json" || return 1
  "$HOLD_BIN" profile export restartprof >"$TEST_ROOT/restartprof.txt" || return 1
  out=$("$HOLD_BIN" run -d restartprof 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  replay_id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$replay_id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.2
  "$HOLD_BIN" inspect "$replay_id" >"$TEST_ROOT/restartprof-run.json" || return 1
  "$HOLD_BIN" stop "$id" "$replay_id" >/dev/null 2>&1 || true
  grep -q '^restart on-failure:1$' "$TEST_ROOT/restartprof.txt" || { cat "$TEST_ROOT/restartprof.txt" >&2; return 1; }
  grep -q '^restart-delay 1$' "$TEST_ROOT/restartprof.txt" || { cat "$TEST_ROOT/restartprof.txt" >&2; return 1; }
  python3 - "$TEST_ROOT/restartprof.json" "$TEST_ROOT/restartprof-run.json" <<'PY'
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
run = json.load(open(sys.argv[2], encoding='utf-8'))[0]
for label, obj in (("profile", profile), ("run", run)):
    if obj.get("restart") != "on-failure:1":
        raise SystemExit(f"{label} restart mismatch: {obj.get('restart')!r}")
    if obj.get("restart_delay_seconds") != 1:
        raise SystemExit(f"{label} restart delay mismatch: {obj.get('restart_delay_seconds')!r}")
PY
}

test_docker_publish_and_volume_rejected() {
  local rc
  set +e
  "$HOLD_BIN" run -p 8080:3000 -- /bin/true >"$TEST_ROOT/docker-publish.out" 2>"$TEST_ROOT/docker-publish.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/docker-publish.out" "$TEST_ROOT/docker-publish.err" >&2; return 1; }
  grep -q 'does not publish or forward ports' "$TEST_ROOT/docker-publish.err" || { cat "$TEST_ROOT/docker-publish.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" run -v "$TEST_ROOT:/work" -- /bin/true >"$TEST_ROOT/docker-volume.out" 2>"$TEST_ROOT/docker-volume.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/docker-volume.out" "$TEST_ROOT/docker-volume.err" >&2; return 1; }
  grep -q 'does not mount or remap volumes' "$TEST_ROOT/docker-volume.err" || { cat "$TEST_ROOT/docker-volume.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" profile bad-publish -p 8080:3000 -- /bin/true >"$TEST_ROOT/profile-publish.out" 2>"$TEST_ROOT/profile-publish.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/profile-publish.out" "$TEST_ROOT/profile-publish.err" >&2; return 1; }
  grep -q 'does not publish or forward ports' "$TEST_ROOT/profile-publish.err" || { cat "$TEST_ROOT/profile-publish.err" >&2; return 1; }

  set +e
  "$HOLD_BIN" profile bad-volume -v "$TEST_ROOT:/work" -- /bin/true >"$TEST_ROOT/profile-volume.out" 2>"$TEST_ROOT/profile-volume.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/profile-volume.out" "$TEST_ROOT/profile-volume.err" >&2; return 1; }
  grep -q 'does not mount or remap volumes' "$TEST_ROOT/profile-volume.err" || { cat "$TEST_ROOT/profile-volume.err" >&2; return 1; }
}

test_docker_named_profile_persists_mode_flags() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id replay_id
  "$HOLD_BIN" profile modeprof -d -i -t -- /bin/sleep 5 >"$TEST_ROOT/modeprof-create.out" || return 1
  out=$("$HOLD_BIN" run modeprof 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" profile export modeprof --json >"$TEST_ROOT/modeprof.json" || return 1
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/modeprof-run.json" || return 1
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  "$HOLD_BIN" prune "$id" >/dev/null 2>&1 || true

  out=$("$HOLD_BIN" run modeprof 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  replay_id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$replay_id" ] || { printf '%s\n' "$out" >&2; return 1; }
  "$HOLD_BIN" inspect "$replay_id" >"$TEST_ROOT/modeprof-replay.json" || return 1
  "$HOLD_BIN" stop "$replay_id" >/dev/null 2>&1 || true
  python3 - "$TEST_ROOT/modeprof.json" "$TEST_ROOT/modeprof-run.json" "$TEST_ROOT/modeprof-replay.json" <<'PY' || {
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
first = json.load(open(sys.argv[2], encoding='utf-8'))[0]
replay = json.load(open(sys.argv[3], encoding='utf-8'))[0]
mode = profile.get('mode') or {}
for key in ('interactive', 'tty', 'detach'):
    if mode.get(key) is not True:
        raise SystemExit(f'profile mode {key} was not persisted: {mode!r}')
if 'console_sock' not in first:
    raise SystemExit('initial -it run did not allocate a console socket')
if 'console_sock' not in replay:
    raise SystemExit('profile replay did not preserve tty/console behavior')
if replay.get('alias') != 'modeprof':
    raise SystemExit(f'profile replay lost alias: {replay.get("alias")!r}')
PY
    cat "$TEST_ROOT/modeprof.json" "$TEST_ROOT/modeprof-run.json" "$TEST_ROOT/modeprof-replay.json" >&2
    return 1
  }
}

test_docker_interactive_stdin_pipes_to_child() {
  local out id
  out=$(printf 'stdin-i-ok\n' | "$HOLD_BIN" run -i -f -- /bin/cat 2>"$TEST_ROOT/docker-i.err") || {
    cat "$TEST_ROOT/docker-i.err" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  grep -q '^stdin-i-ok$' <<<"$out" || { printf '%s\n' "$out" >&2; return 1; }

  out=$(printf 'stdin-no-i\n' | "$HOLD_BIN" run -f -- /bin/cat 2>"$TEST_ROOT/docker-no-i.err") || {
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
  python3 - <<'PY' | script -qfec "$HOLD_BIN run -it $TEST_ROOT/docker-tty-child.sh" /dev/null >"$TEST_ROOT/docker-tty.out" 2>"$TEST_ROOT/docker-tty.err"
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
  id=$(tr -d '\r' <"$TEST_ROOT/docker-tty.out" | extract_id)
  [ -n "$id" ] || { cat "$TEST_ROOT/docker-tty.out" >&2; return 1; }
  record=$(record_path "$id") || { find "$HOME/.local/state/hold" -maxdepth 1 -type f -print >&2 || true; return 1; }
  grep -q '"console_sock": "' "$record" || { cat "$record" >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+-[[:space:]].*Up " "$TEST_ROOT/docker-tty-ps.out" || { cat "$TEST_ROOT/docker-tty-ps.out" >&2; return 1; }
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
  python3 - <<'PY' | script -qfec "$HOLD_BIN run -it --detach-keys ctrl-a $TEST_ROOT/docker-tty-custom-child.sh" /dev/null >"$TEST_ROOT/docker-tty-custom.out" 2>"$TEST_ROOT/docker-tty-custom.err"
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
  id=$(tr -d '\r' <"$TEST_ROOT/docker-tty-custom.out" | extract_id)
  [ -n "$id" ] || { cat "$TEST_ROOT/docker-tty-custom.out" >&2; return 1; }
  "$HOLD_BIN" ps >"$TEST_ROOT/docker-tty-custom-ps.out" || return 1
  grep -Eq "^$id[[:space:]]+-[[:space:]].*Up " "$TEST_ROOT/docker-tty-custom-ps.out" || { cat "$TEST_ROOT/docker-tty-custom-ps.out" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}


test_docker_profile_it_reconnects_singular_tty_run() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id rc count
  cat >"$TEST_ROOT/docker-tty-reconnect-child.sh" <<'EOF'
#!/bin/sh
printf 'ready-reconnect\n'
while IFS= read -r line; do
  printf 'reconnect:%s\n' "$line"
done
EOF
  chmod +x "$TEST_ROOT/docker-tty-reconnect-child.sh"
  "$HOLD_BIN" profile ttyreconnect -d -i -t -- "$TEST_ROOT/docker-tty-reconnect-child.sh" >"$TEST_ROOT/docker-tty-reconnect-create.out" || return 1
  out=$("$HOLD_BIN" run ttyreconnect 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.3
  set +e
  python3 - <<'PY' | script -qfec "$HOLD_BIN run -it ttyreconnect" /dev/null >"$TEST_ROOT/docker-tty-reconnect.out" 2>"$TEST_ROOT/docker-tty-reconnect.err"
import sys, time
out = sys.stdout.buffer
time.sleep(0.5)
out.write(b"hello-reconnect\n")
out.flush()
time.sleep(0.5)
out.write(b"\x10\x11")
out.flush()
time.sleep(0.1)
PY
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { echo "run -it ttyreconnect rc=$rc" >&2; cat "$TEST_ROOT/docker-tty-reconnect.out" "$TEST_ROOT/docker-tty-reconnect.err" >&2; "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  grep -q 'reconnect:hello-reconnect' "$TEST_ROOT/docker-tty-reconnect.out" || { cat "$TEST_ROOT/docker-tty-reconnect.out" >&2; "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  count=$(python3 - "$HOME/.local/state/hold" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
n = 0
for path in root.glob('*.json'):
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except Exception:
        continue
    if data.get('alias') == 'ttyreconnect':
        n += 1
print(n)
PY
)
  [ "$count" = "1" ] || { echo "expected one ttyreconnect runid, got $count" >&2; find "$HOME/.local/state/hold" -maxdepth 1 -name '*.json' -print >&2; "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_runid_and_named_profile_normalize_relative_file_args() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local out id script profile_json
  script="$TEST_ROOT/rel-normalize.sh"
  cat >"$script" <<'SH'
#!/bin/sh
echo normalize-relative-run
SH
  chmod +x "$script"

  (cd "$TEST_ROOT" && "$HOLD_BIN" profile relnorm -d -- /bin/sh ./rel-normalize.sh >"$TEST_ROOT/rel-normalize-create.out") || return 1
  out=$("$HOLD_BIN" run relnorm 2>&1) || {
    printf '%s\n' "$out" >&2
    return 1
  }
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.2
  "$HOLD_BIN" inspect "$id" >"$TEST_ROOT/rel-normalize-inspect.json" || return 1
  "$HOLD_BIN" profile export relnorm --json >"$TEST_ROOT/rel-normalize-profile.json" || return 1
  if ! python3 - "$TEST_ROOT/rel-normalize-inspect.json" "$TEST_ROOT/rel-normalize-profile.json" "$script" <<'PY'
import json, sys
record = json.load(open(sys.argv[1], encoding='utf-8'))[0]
profile = json.load(open(sys.argv[2], encoding='utf-8'))
want = sys.argv[3]
argv = record.get('argv') or []
normalized = (record.get('normalized') or {}).get('argv') or []
observed = record.get('observed') or {}
observed_argv = observed.get('argv') or []
args = profile.get('args') or []
if len(argv) < 2 or argv[1] != want:
    raise SystemExit(f'run record did not normalize relative script arg: {argv!r}')
if len(normalized) < 2 or normalized[1] != want:
    raise SystemExit(f'normalized argv did not preserve absolute script arg: {normalized!r}')
if len(observed_argv) < 2 or observed_argv[1] != want:
    raise SystemExit(f'observed argv did not use the normalized profile script arg: {observed_argv!r}')
if not (observed.get('cwd') or '').startswith('/'):
    raise SystemExit(f'observed cwd is not absolute: {observed.get("cwd")!r}')
if not (observed.get('exe') or '').startswith('/'):
    raise SystemExit(f'observed exe is not absolute: {observed.get("exe")!r}')
if len(args) < 1 or args[0] != want:
    raise SystemExit(f'named profile did not normalize relative script arg: {args!r}')
PY
  then
    cat "$TEST_ROOT/rel-normalize-inspect.json" "$TEST_ROOT/rel-normalize-profile.json" >&2
    return 1
  fi
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


test_captive_cli_cisco_prompts_and_profile_commit() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local rc id
  set +e
  python3 -c 'import sys,time; sys.stdout.write("?\nenable\nconfigure ?\nconfigure terminal\nprofile web\nbinary /usr/bin/sleep\nargv 30\ninfo\ncommit\nend\nshow profiles\nexit\n"); sys.stdout.flush(); time.sleep(0.1)' |
    script -qfec "$HOLD_BIN" /dev/null >"$TEST_ROOT/captive-cli.out" 2>"$TEST_ROOT/captive-cli.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-cli.out" "$TEST_ROOT/captive-cli.err" >&2; return 1; }
  grep -q 'hold> ' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -q 'hold# ' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -q 'hold(config)# ' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -q 'hold(config-profile:web)# ' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -q 'Exec commands:' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -q 'terminal   Configure from the terminal' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -q 'Profile committed.' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
  grep -Eq '^web[[:space:]]+user[[:space:]]+/usr/bin/sleep 30' "$TEST_ROOT/captive-cli.out" || { cat "$TEST_ROOT/captive-cli.out" >&2; return 1; }
}

test_captive_cli_noninteractive_transcript_is_script_safe() {
  local rc id
  set +e
  printf 'enable\nconfigure terminal\nprofile batch\nbinary /usr/bin/sleep\nargv 30\ncommit\nend\nrun batch\nexit\n' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-batch.out" 2>"$TEST_ROOT/captive-batch.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-batch.out" "$TEST_ROOT/captive-batch.err" >&2; return 1; }
  ! grep -q 'hold> ' "$TEST_ROOT/captive-batch.out" || { cat "$TEST_ROOT/captive-batch.out" >&2; return 1; }
  grep -q 'Profile committed.' "$TEST_ROOT/captive-batch.out" || { cat "$TEST_ROOT/captive-batch.out" >&2; return 1; }
  id=$(extract_id <"$TEST_ROOT/captive-batch.out")
  [ -n "$id" ] || { cat "$TEST_ROOT/captive-batch.out" "$TEST_ROOT/captive-batch.err" >&2; return 1; }
  "$HOLD_BIN" stop "$id" >/dev/null || return 1
}

test_captive_cli_profile_mode_flags_commit_and_clear() {
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local rc
  set +e
  printf 'enable\nconfigure terminal\nprofile iosmode\nbinary /usr/bin/sleep\nargv 30\ninteractive\ntty\ndetach\nmulti\ninfo\ncommit\nend\nexit\n' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-mode-set.out" 2>"$TEST_ROOT/captive-mode-set.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-mode-set.out" "$TEST_ROOT/captive-mode-set.err" >&2; return 1; }
  grep -q 'modes     : interactive tty detach multi' "$TEST_ROOT/captive-mode-set.out" || {
    cat "$TEST_ROOT/captive-mode-set.out" >&2
    return 1
  }
  "$HOLD_BIN" profile export iosmode --json >"$TEST_ROOT/iosmode-set.json" || return 1
  "$HOLD_BIN" profile export iosmode >"$TEST_ROOT/iosmode-set.profile" || return 1
  grep -Fxq "interactive" "$TEST_ROOT/iosmode-set.profile" || { cat "$TEST_ROOT/iosmode-set.profile" >&2; return 1; }
  grep -Fxq "tty" "$TEST_ROOT/iosmode-set.profile" || { cat "$TEST_ROOT/iosmode-set.profile" >&2; return 1; }
  grep -Fxq "detach" "$TEST_ROOT/iosmode-set.profile" || { cat "$TEST_ROOT/iosmode-set.profile" >&2; return 1; }
  grep -Fxq "multi" "$TEST_ROOT/iosmode-set.profile" || { cat "$TEST_ROOT/iosmode-set.profile" >&2; return 1; }
  python3 - "$TEST_ROOT/iosmode-set.json" <<'PY' || { cat "$TEST_ROOT/iosmode-set.json" >&2; return 1; }
import json, sys
mode = (json.load(open(sys.argv[1], encoding='utf-8')).get('mode') or {})
for key in ('interactive', 'tty', 'detach', 'multi'):
    if mode.get(key) is not True:
        raise SystemExit(f'missing committed mode {key}: {mode!r}')
PY
  "$HOLD_BIN" profile iosmode delete >/dev/null || return 1
  "$HOLD_BIN" profile import "$TEST_ROOT/iosmode-set.profile" >/dev/null || return 1
  "$HOLD_BIN" profile export iosmode --json >"$TEST_ROOT/iosmode-imported.json" || return 1
  python3 - "$TEST_ROOT/iosmode-imported.json" <<'PY' || { cat "$TEST_ROOT/iosmode-imported.json" >&2; return 1; }
import json, sys
mode = (json.load(open(sys.argv[1], encoding='utf-8')).get('mode') or {})
for key in ('interactive', 'tty', 'detach', 'multi'):
    if mode.get(key) is not True:
        raise SystemExit(f'imported transcript missing mode {key}: {mode!r}')
PY

  set +e
  printf 'enable\nconfigure terminal\nprofile iosmode\nno interactive\nno console\nno detach\nno multi\ninfo\ncommit\nend\nexit\n' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-mode-clear.out" 2>"$TEST_ROOT/captive-mode-clear.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-mode-clear.out" "$TEST_ROOT/captive-mode-clear.err" >&2; return 1; }
  grep -q 'modes     : -' "$TEST_ROOT/captive-mode-clear.out" || {
    cat "$TEST_ROOT/captive-mode-clear.out" >&2
    return 1
  }
  "$HOLD_BIN" profile export iosmode --json >"$TEST_ROOT/iosmode-clear.json" || return 1
  python3 - "$TEST_ROOT/iosmode-clear.json" <<'PY' || { cat "$TEST_ROOT/iosmode-clear.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if 'mode' in profile:
    raise SystemExit(f'mode object should be omitted after no interactive/no console/no detach/no multi: {profile!r}')
PY
}


test_captive_profile_log_destination_persists_and_clears() {
  local rc
  set +e
  printf 'enable
configure terminal
profile iolog
binary /bin/echo
argv hello
log-destination syslog
info
commit
end
exit
' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-logdest-set.out" 2>"$TEST_ROOT/captive-logdest-set.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-logdest-set.out" "$TEST_ROOT/captive-logdest-set.err" >&2; return 1; }
  grep -q 'log-dest  : syslog' "$TEST_ROOT/captive-logdest-set.out" || {
    cat "$TEST_ROOT/captive-logdest-set.out" >&2
    return 1
  }
  "$HOLD_BIN" profile export iolog --json >"$TEST_ROOT/iolog-set.json" || return 1
  "$HOLD_BIN" profile export iolog >"$TEST_ROOT/iolog-set.profile" || return 1
  grep -Fxq "log-destination syslog" "$TEST_ROOT/iolog-set.profile" || { cat "$TEST_ROOT/iolog-set.profile" >&2; return 1; }
  python3 - "$TEST_ROOT/iolog-set.json" <<'PY' || { cat "$TEST_ROOT/iolog-set.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if profile.get('log_destination') != 'syslog':
    raise SystemExit(f'missing syslog destination: {profile!r}')
PY

  set +e
  printf 'enable
configure terminal
profile iolog
info
default log-destination
info
commit
end
exit
' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-logdest-clear.out" 2>"$TEST_ROOT/captive-logdest-clear.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-logdest-clear.out" "$TEST_ROOT/captive-logdest-clear.err" >&2; return 1; }
  grep -q 'log-dest  : syslog' "$TEST_ROOT/captive-logdest-clear.out" || {
    cat "$TEST_ROOT/captive-logdest-clear.out" >&2
    return 1
  }
  grep -q 'log-dest  : -' "$TEST_ROOT/captive-logdest-clear.out" || {
    cat "$TEST_ROOT/captive-logdest-clear.out" >&2
    return 1
  }
  "$HOLD_BIN" profile export iolog --json >"$TEST_ROOT/iolog-clear.json" || return 1
  python3 - "$TEST_ROOT/iolog-clear.json" <<'PY' || { cat "$TEST_ROOT/iolog-clear.json" >&2; return 1; }
import json, sys
profile = json.load(open(sys.argv[1], encoding='utf-8'))
if 'log_destination' in profile:
    raise SystemExit(f'log_destination should be omitted after default log-destination: {profile!r}')
PY
}


test_captive_cli_ios_contextual_help_and_reserved_words() {
  local rc
  set +e
  printf '?
show ?
enable
?
configure ?
configure terminal
?
profile ?
profile ioshelp
?
binary ?
argv ?
env ?
param ?
no ?
default ?
default multi
end
write
disable
exit
' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-ios-help.out" 2>"$TEST_ROOT/captive-ios-help.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-ios-help.out" "$TEST_ROOT/captive-ios-help.err" >&2; return 1; }
  grep -q 'attach      Attach to a compatible running instance' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'console     Attach with a PTY' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'grant       Grant a user capability' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  ! grep -q '  ping' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'revoke      Revoke a user capability' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'write       Write running config to storage' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'aliases    Show public alias table' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'pattern     Manage the pattern dictionary' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'alias         Define a profile-local alias (reserved)' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'param         Expose a validated optional parameter (reserved)' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'multi         Allow multiple concurrent instances' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'log-destination Mirror logs to syslog' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'pty-shim      Start under the PTY shim (reserved)' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'WORD       Absolute path to the executable' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'WORD       Long flag to expose' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q 'Building configuration...' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  grep -q '\[OK\]' "$TEST_ROOT/captive-ios-help.out" || { cat "$TEST_ROOT/captive-ios-help.out" >&2; return 1; }
  ! grep -q 'Unknown' "$TEST_ROOT/captive-ios-help.err" || { cat "$TEST_ROOT/captive-ios-help.err" >&2; return 1; }
}

test_captive_cli_ios_operational_commands() {
  local rc
  set +e
  printf 'enable
configure terminal
profile ops
binary /bin/sh
argv -c
argv "echo captive-op"
commit
end
run ops
logs ops --plain
inspect ops
prune ops
exit
' |
    "$HOLD_BIN" >"$TEST_ROOT/captive-ios-ops.out" 2>"$TEST_ROOT/captive-ios-ops.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-ios-ops.out" "$TEST_ROOT/captive-ios-ops.err" >&2; return 1; }
  grep -q 'Profile committed.' "$TEST_ROOT/captive-ios-ops.out" || { cat "$TEST_ROOT/captive-ios-ops.out" >&2; return 1; }
  grep -q 'captive-op' "$TEST_ROOT/captive-ios-ops.out" || { cat "$TEST_ROOT/captive-ios-ops.out" >&2; return 1; }
  grep -q '"alias": "ops"' "$TEST_ROOT/captive-ios-ops.out" || { cat "$TEST_ROOT/captive-ios-ops.out" >&2; return 1; }
  grep -q 'hold: pruned 1 past run for' "$TEST_ROOT/captive-ios-ops.err" || { cat "$TEST_ROOT/captive-ios-ops.err" >&2; return 1; }
}

test_captive_cli_rejects_ping_pseudo_command() {
  local rc
  set +e
  printf 'ping target\nexit\n' | "$HOLD_BIN" >"$TEST_ROOT/captive-ping.out" 2>"$TEST_ROOT/captive-ping.err"
  rc=$?
  set -e
  [ "$rc" -eq 5 ] || { cat "$TEST_ROOT/captive-ping.out" "$TEST_ROOT/captive-ping.err" >&2; return 1; }
  grep -q "% Unknown command 'ping'" "$TEST_ROOT/captive-ping.err" || { cat "$TEST_ROOT/captive-ping.err" >&2; return 1; }
}


test_captive_cli_interactive_input_handles_arrows_and_mouse_reports() {
  command -v script >/dev/null 2>&1 || skip "script not available"
  command -v python3 >/dev/null 2>&1 || skip "python3 not available"
  local rc
  set +e
  python3 - <<'PYINPUT' |
import sys, time
out = sys.stdout.buffer
# Exercise the PTY/interactive input path: edit argv "ac" into "abc" with
# Left/Right style escape input, and inject an SGR mouse event that should be
# swallowed rather than becoming a literal command.
out.write(b"enable\n")
out.write(b"\x1b[A\x1b[B\x1b[C\x1b[<0;10;5M\n")
out.write(b"configure terminal\nprofile inputedit\nbinary /bin/echo\n")
out.write(b"argv ac\x1b[Db\n")
out.write(b"commit\nend\nrun inputedit\nlogs inputedit --plain\nprune inputedit\nexit\n")
out.flush()
time.sleep(0.2)
PYINPUT
    script -qfec "stty -echo rows 24 cols 120; $HOLD_BIN" /dev/null >"$TEST_ROOT/captive-input-edit.out" 2>"$TEST_ROOT/captive-input-edit.err"
  rc=$?
  set -e
  [ "$rc" -eq 0 ] || { cat "$TEST_ROOT/captive-input-edit.out" "$TEST_ROOT/captive-input-edit.err" >&2; return 1; }
  python3 - "$TEST_ROOT/captive-input-edit.out" <<'PYCHECK' || { cat "$TEST_ROOT/captive-input-edit.out" >&2; return 1; }
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'ignore')
plain = re.sub(r'\x1b\[[0-9;?]*[A-Za-z~]', '', raw).replace('\r', '\n')
if not re.search(r'(?m)^abc$', plain):
    raise SystemExit('edited argv did not produce abc log output')
# Do not reject raw PTY input echo here. Some script(1) implementations
# include bytes typed by the driving process in the transcript even when the
# application correctly swallows terminal escape and mouse sequences. The
# product contract is covered by the edited output above plus the stderr
# Unknown-command check below.
PYCHECK
  ! grep -q "Unknown command" "$TEST_ROOT/captive-input-edit.err" || { cat "$TEST_ROOT/captive-input-edit.err" >&2; return 1; }
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

# Pin a root system alias 'web-sys' with a safe (root-owned, 0755, no-space-path)
# hold copy and a passing visudo stub. Call directly (NOT in $(...)) so the
# exported HOLD_TEST_SUDOERS_DIR/VISUDO_PROG reach the caller; sets globals
# GRANT_SAFE and GRANT_SUDOERS_DIR.
grant_fixture() {
  local id
  GRANT_SAFE=$(root_safe_hold_copy hold-safe) || return 1
  GRANT_SUDOERS_DIR="$TEST_ROOT/sudoers.d"
  mkdir -p "$GRANT_SUDOERS_DIR" || return 1
  chmod 755 "$GRANT_SUDOERS_DIR" || return 1
  export HOLD_TEST_SUDOERS_DIR="$GRANT_SUDOERS_DIR"
  GRANT_VISUDO_OK="$TEST_ROOT/visudo-ok"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$GRANT_VISUDO_OK" || return 1
  chmod 755 "$GRANT_VISUDO_OK" || return 1
  export HOLD_TEST_VISUDO_PROG="$GRANT_VISUDO_OK"
  id=$(as_root "$GRANT_SAFE" /bin/sh -c ':' 2>&1 | extract_id) || return 1
  [ -n "$id" ] || return 1
  as_root "$GRANT_SAFE" profile save "$id" as web-sys >/dev/null 2>&1 || return 1
}

test_system_store_directory_modes() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local id d mode
  id=$(as_root "$HOLD_REAL_BIN" true 2>&1 | extract_id)
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
  id=$(as_root "$HOLD_REAL_BIN" true 2>&1 | extract_id)
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
    as_root "$HOLD_REAL_BIN" true >/dev/null 2>"$TEST_ROOT/syslink-$name.err"
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
  id=$(as_root "$HOLD_REAL_BIN" true 2>&1 | extract_id)
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
  out=$(as_user_spoof_sudo "$HOLD_REAL_BIN" true 2>&1) || return 1
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

test_aliases_json_symlink_not_followed() {
  local out id aliases attacker
  out=$("$HOLD_BIN" -d /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as myweb >/dev/null 2>&1 || return 1
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  aliases="$HOME/.local/state/hold/aliases.json"
  [ -f "$aliases" ] || return 1
  # sanity: the alias is visible when the file is a real regular file
  "$HOLD_BIN" profiles 2>/dev/null | grep -q myweb || { echo "alias not visible before symlink (test setup)" >&2; return 1; }
  # replace it with a symlink to an identical attacker-controlled file
  attacker="$TEST_ROOT/attacker-aliases.json"
  cp "$aliases" "$attacker" || return 1
  rm -f "$aliases"
  ln -s "$attacker" "$aliases" || return 1
  # O_NOFOLLOW must reject the symlinked alias dictionary: myweb must not load
  if "$HOLD_BIN" profiles 2>/dev/null | grep -q myweb; then
    echo "symlinked aliases.json was followed (myweb loaded through the symlink)" >&2
    return 1
  fi
}

test_alias_profile_atomic_writers_ignore_fixed_temp_attacks() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local out id store fixed attacker before leftovers

  ensure_user_fixture_store || return 1
  store="$HOME/.local/state/hold"
  attacker="$TEST_ROOT/user-alias-attacker"
  printf 'do-not-touch-alias-symlink\n' >"$attacker" || return 1
  fixed="$store/.aliases.tmp"
  ln -s "$attacker" "$fixed" || return 1
  out=$("$HOLD_BIN" -d /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as fixedlink >/dev/null 2>&1 || return 1
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$(cat "$attacker")" = "do-not-touch-alias-symlink" ] || { echo "alias writer followed fixed symlink temp" >&2; return 1; }
  [ -L "$fixed" ] || { echo "alias writer replaced fixed symlink temp" >&2; return 1; }
  rm -f "$fixed" || return 1
  printf 'do-not-touch-alias-file\n' >"$fixed" || return 1
  out=$("$HOLD_BIN" -d /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  "$HOLD_BIN" profile save "$id" as fixedfile >/dev/null 2>&1 || return 1
  "$HOLD_BIN" stop "$id" >/dev/null 2>&1 || true
  [ "$(cat "$fixed")" = "do-not-touch-alias-file" ] || { echo "alias writer truncated fixed temp file" >&2; return 1; }
  leftovers=$(find "$store" -maxdepth 1 -name '.aliases.*.tmp' -print 2>/dev/null || true)
  [ -z "$leftovers" ] || { echo "alias writer left temp files: $leftovers" >&2; return 1; }

  # System aliases write both profiles.json in the private base and aliases.json
  # in public; cover fixed symlink and fixed regular temp names for both writers.
  out=$(as_root "$HOLD_REAL_BIN" -d /bin/sleep 60 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  printf 'do-not-touch-profile-symlink\n' >"$TEST_ROOT/profile-attacker" || return 1
  printf 'do-not-touch-public-alias-symlink\n' >"$TEST_ROOT/public-alias-attacker" || return 1
  as_root ln -s "$TEST_ROOT/profile-attacker" "$HOLD_TEST_SYSTEM_STATE_DIR/.profiles.tmp" || return 1
  as_root ln -s "$TEST_ROOT/public-alias-attacker" "$HOLD_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp" || return 1
  as_root "$HOLD_REAL_BIN" profile save "$id" as syslink >/dev/null 2>&1 || return 1
  [ "$(cat "$TEST_ROOT/profile-attacker")" = "do-not-touch-profile-symlink" ] || { echo "profile writer followed fixed symlink temp" >&2; return 1; }
  [ "$(cat "$TEST_ROOT/public-alias-attacker")" = "do-not-touch-public-alias-symlink" ] || { echo "system alias writer followed fixed symlink temp" >&2; return 1; }
  root_file_exists "$HOLD_TEST_SYSTEM_STATE_DIR/profiles.json" || return 1
  root_file_exists "$HOLD_TEST_SYSTEM_STATE_DIR/public/aliases.json" || return 1
  as_root rm -f "$HOLD_TEST_SYSTEM_STATE_DIR/.profiles.tmp" "$HOLD_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp" || return 1

  out=$(as_root "$HOLD_REAL_BIN" -d /bin/sleep 61 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || return 1
  as_root sh -c 'printf "%s\n" "do-not-touch-profile-file" >"$1"; printf "%s\n" "do-not-touch-public-alias-file" >"$2"' sh \
    "$HOLD_TEST_SYSTEM_STATE_DIR/.profiles.tmp" "$HOLD_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp" || return 1
  as_root "$HOLD_REAL_BIN" profile save "$id" as sysfile >/dev/null 2>&1 || return 1
  before=$(as_root cat "$HOLD_TEST_SYSTEM_STATE_DIR/.profiles.tmp") || return 1
  [ "$before" = "do-not-touch-profile-file" ] || { echo "profile writer truncated fixed temp file" >&2; return 1; }
  before=$(as_root cat "$HOLD_TEST_SYSTEM_STATE_DIR/public/.aliases.tmp") || return 1
  [ "$before" = "do-not-touch-public-alias-file" ] || { echo "system alias writer truncated fixed temp file" >&2; return 1; }
  leftovers=$(as_root find "$HOLD_TEST_SYSTEM_STATE_DIR" "$HOLD_TEST_SYSTEM_STATE_DIR/public" -maxdepth 1 \
    \( -name '.profiles.*.tmp' -o -name '.aliases.*.tmp' \) -print 2>/dev/null || true)
  [ -z "$leftovers" ] || { echo "system alias/profile writer left temp files: $leftovers" >&2; return 1; }
}

test_grant_sudoers_file_is_root_only() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe sudoers_file mode owner
  grant_fixture || return 1
  as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/grant.err" || { cat "$TEST_ROOT/grant.err" >&2; return 1; }
  sudoers_file="$HOLD_TEST_SUDOERS_DIR/hold_web-sys_$TEST_USER"
  root_file_exists "$sudoers_file" || return 1
  mode=$(root_file_mode "$sudoers_file") || return 1
  [ "$mode" = 440 ] || { echo "managed sudoers mode=$mode (want 440)" >&2; return 1; }
  owner=$(root_file_owner "$sudoers_file") || return 1
  [ "$owner" = "0:0" ] || { echo "managed sudoers owner=$owner (want 0:0)" >&2; return 1; }
}

test_grant_aborts_when_visudo_rejects() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe visudo_bad visudo_ok sudoers_file grant_private grant_public before_private before_public before_sudoers after_private after_public after_sudoers rc
  grant_fixture || return 1
  visudo_bad="$TEST_ROOT/visudo-bad"
  printf '#!/usr/bin/env sh\nexit 1\n' >"$visudo_bad" || return 1
  chmod 755 "$visudo_bad" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_bad"
  sudoers_file="$HOLD_TEST_SUDOERS_DIR/hold_web-sys_$TEST_USER"
  set +e
  as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/visudo.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "grant succeeded despite visudo rejecting the candidate" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "unvalidated sudoers file was installed" >&2; return 1; }
  root_path_absent "$sudoers_file.tmp" 2>/dev/null || true
  grant_private="$HOLD_TEST_SYSTEM_STATE_DIR/grants/users/$TEST_USER/web-sys.json"
  grant_public="$HOLD_TEST_SYSTEM_STATE_DIR/public/grants/users/$TEST_USER/web-sys.json"
  root_path_absent "$grant_private" || { echo "private grant copy left behind after visudo rejection" >&2; return 1; }
  root_path_absent "$grant_public" || { echo "public grant cache left behind after visudo rejection" >&2; return 1; }

  visudo_ok="$TEST_ROOT/visudo-ok-again"
  printf '#!/usr/bin/env sh\nexit 0\n' >"$visudo_ok" || return 1
  chmod 755 "$visudo_ok" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_ok"
  as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" start >"$TEST_ROOT/grant-ok.out" 2>"$TEST_ROOT/grant-ok.err" || {
    cat "$TEST_ROOT/grant-ok.out" "$TEST_ROOT/grant-ok.err" >&2
    return 1
  }
  before_private="$TEST_ROOT/grant-private.before"
  before_public="$TEST_ROOT/grant-public.before"
  before_sudoers="$TEST_ROOT/sudoers.before"
  after_private="$TEST_ROOT/grant-private.after"
  after_public="$TEST_ROOT/grant-public.after"
  after_sudoers="$TEST_ROOT/sudoers.after"
  as_root cp "$grant_private" "$before_private" || return 1
  as_root cp "$grant_public" "$before_public" || return 1
  as_root cp "$sudoers_file" "$before_sudoers" || return 1
  export HOLD_TEST_VISUDO_PROG="$visudo_bad"
  set +e
  as_root "$GRANT_SAFE" grant web-sys "$TEST_USER" stop >"$TEST_ROOT/grant-refresh-bad.out" 2>"$TEST_ROOT/grant-refresh-bad.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || { echo "existing grant refresh succeeded despite visudo rejection" >&2; return 1; }
  as_root cp "$grant_private" "$after_private" || return 1
  as_root cp "$grant_public" "$after_public" || return 1
  as_root cp "$sudoers_file" "$after_sudoers" || return 1
  as_root cmp "$before_private" "$after_private" || { echo "private grant was not restored after failed refresh" >&2; return 1; }
  as_root cmp "$before_public" "$after_public" || { echo "public grant cache was not restored after failed refresh" >&2; return 1; }
  as_root cmp "$before_sudoers" "$after_sudoers" || { echo "sudoers changed after failed refresh" >&2; return 1; }
}

test_grant_refuses_unsafe_self_binary() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local safe bad sudoers_file rc
  grant_fixture || return 1
  sudoers_file="$HOLD_TEST_SUDOERS_DIR/hold_web-sys_$TEST_USER"
  # (a) non-root-owned hold binary
  bad="$TEST_ROOT/hold-userowned"
  cp "$HOLD_REAL_BIN" "$bad" || return 1
  as_root chown "$TEST_UID:$TEST_GID" "$bad" || return 1
  as_root chmod 755 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/u.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a non-root-owned binary" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via non-root-owned binary" >&2; return 1; }
  # (b) group/world-writable hold binary
  bad="$TEST_ROOT/hold-writable"
  cp "$HOLD_REAL_BIN" "$bad" || return 1
  as_root chown 0:0 "$bad" || return 1
  as_root chmod 0777 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/w.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a world-writable binary" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via world-writable binary" >&2; return 1; }
  # (c) root-owned hold binary beneath an unsafe parent directory
  mkdir -p "$TEST_ROOT/unsafe-parent" || return 1
  chmod 0777 "$TEST_ROOT/unsafe-parent" || return 1
  bad="$TEST_ROOT/unsafe-parent/hold"
  cp "$HOLD_REAL_BIN" "$bad" || return 1
  as_root chown 0:0 "$bad" || return 1
  as_root chmod 755 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/p.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a binary under an unsafe parent directory" >&2; return 1; }
  grep -Eq 'unsafe parent directory chain|not grant-safe' "$TEST_ROOT/p.err" || { cat "$TEST_ROOT/p.err" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via unsafe-parent binary" >&2; return 1; }
  # (d) whitespace in the binary path
  mkdir -p "$TEST_ROOT/bad dir" || return 1
  bad="$TEST_ROOT/bad dir/hold"
  cp "$HOLD_REAL_BIN" "$bad" || return 1
  as_root chown 0:0 "$bad" || return 1
  as_root chmod 755 "$bad" || return 1
  set +e; as_root "$bad" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/s.err"; rc=$?; set -e
  [ "$rc" -ne 0 ] || { echo "granted via a whitespace-in-path binary" >&2; return 1; }
  root_path_absent "$sudoers_file" || { echo "sudoers written via whitespace-path binary" >&2; return 1; }
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

test_elevated_child_signal_maps_to_128_plus_sig() {
  local rc fakesudo
  write_public_index_fixture abc12345cafe running 2026-06-15T18:42:11Z || return 1
  fakesudo="$TEST_ROOT/fakebin-sig/sudo"
  mkdir -p "$TEST_ROOT/fakebin-sig" || return 1
  printf '#!/usr/bin/env bash\nkill -TERM $$\nsleep 5\n' >"$fakesudo" || return 1
  chmod 755 "$TEST_ROOT/fakebin-sig" "$fakesudo" || return 1
  export HOLD_TEST_SUDO_PROG="$fakesudo"
  set +e; "$HOLD_BIN" kill abc12345cafe >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 143 ] || { echo "signal-killed sudo child: rc=$rc (want 143 = 128+SIGTERM)" >&2; return 1; }
}

test_public_index_write_rollback() {
  [ "$ROOT_ACTOR_AVAILABLE" -eq 1 ] || skip "no root actor"
  local rc pids leftover
  set +e
  HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE=1 as_root "$HOLD_REAL_BIN" -d bash -c 'exec -a hold_pubidx_test sleep 60' >/dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 1 ] || { echo "public-index rollback start: rc=$rc (want 1)" >&2; return 1; }
  pids=$(ps -eo pid=,args= | awk '/hold_pubidx_test/ && !/awk/ {print $1}')
  [ -z "$pids" ] || { echo "leaked process after public-index rollback" >&2; return 1; }
  leftover=$(as_root sh -c 'ls "$1"/runs/*.json "$1"/public/*.json 2>/dev/null' sh "$HOLD_TEST_SYSTEM_STATE_DIR" 2>/dev/null || true)
  [ -z "$leftover" ] || { echo "leftover state after rollback: $leftover" >&2; return 1; }
}

test_quiet_suppresses_banner_keeps_id() {
  local out id
  out=$("$HOLD_BIN" --quiet -d /bin/sleep 300 2>"$TEST_ROOT/q.err") || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { echo "no id from --quiet start" >&2; return 1; }
  [ "$out" = "$id" ] || { echo "--quiet stdout is not the bare id: [$out]" >&2; return 1; }
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

test_ambiguous_tail_resolvable_by_run_id() {
  local id1 id2 rc tpid
  # An alias whose runs emit output, so tailing one by id has something to follow.
  id1=$("$HOLD_BIN" -d sh -c 'while :; do echo tick; sleep 0.2; done' 2>&1 | extract_id) || return 1
  "$HOLD_BIN" profile save "$id1" as web-amb >/dev/null || return 1
  "$HOLD_BIN" stop "$id1" >/dev/null; "$HOLD_BIN" prune "$id1" >/dev/null
  id1=$("$HOLD_BIN" start web-amb 2>&1 | extract_id); [ -n "$id1" ] || return 1
  id2=$("$HOLD_BIN" start web-amb --force 2>&1 | extract_id); [ -n "$id2" ] || return 1
  # tail by ALIAS with several running is ambiguous: exit 6, the candidate run ids
  # are listed, and (since tail is not an --all command) --all must NOT be suggested.
  set +e; "$HOLD_BIN" tail web-amb >/dev/null 2>"$TEST_ROOT/amb.err"; rc=$?; set -e
  [ "$rc" -eq 6 ] || { echo "ambiguous tail: rc=$rc (want 6)" >&2; return 1; }
  grep -q 'matches more than one' "$TEST_ROOT/amb.err" || { cat "$TEST_ROOT/amb.err" >&2; return 1; }
  ! grep -q -- '--all' "$TEST_ROOT/amb.err" || { echo "tail ambiguity wrongly suggested --all" >&2; return 1; }
  grep -q "$id1" "$TEST_ROOT/amb.err" && grep -q "$id2" "$TEST_ROOT/amb.err" || { echo "ambiguity did not list the candidate run ids" >&2; return 1; }
  # but you can always tail one specific instance BY RUN ID -- that is never ambiguous.
  "$HOLD_BIN" tail "$id1" >"$TEST_ROOT/byid.out" 2>"$TEST_ROOT/byid.err" &
  tpid=$!
  sleep 0.6
  kill "$tpid" 2>/dev/null
  wait "$tpid" 2>/dev/null || true
  ! grep -q 'matches more than one' "$TEST_ROOT/byid.err" || { echo "tail by run id was wrongly treated as ambiguous" >&2; return 1; }
  grep -q tick "$TEST_ROOT/byid.out" || { echo "tail by run id followed nothing" >&2; cat "$TEST_ROOT/byid.err" >&2; return 1; }
  "$HOLD_BIN" stop web-amb --all >/dev/null 2>&1 || true
}

test_misc_action_guards() {
  local rc
  set +e; "$HOLD_BIN" --console stop deadbeef >/dev/null 2>"$TEST_ROOT/c.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--console stop: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'only to starts' "$TEST_ROOT/c.err" || { cat "$TEST_ROOT/c.err" >&2; return 1; }
  set +e; "$HOLD_BIN" help bogustopic >/dev/null 2>"$TEST_ROOT/h.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "help bogustopic: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'unknown help topic' "$TEST_ROOT/h.err" || { cat "$TEST_ROOT/h.err" >&2; return 1; }
  "$HOLD_BIN" list -l >"$TEST_ROOT/l.out" 2>&1 || { echo "list -l failed" >&2; return 1; }
  grep -q 'STARTED_AT' "$TEST_ROOT/l.out" || { echo "list -l missing ISO header" >&2; return 1; }
}

test_public_cli_contract_guards() {
  local rc out id
  "$HOLD_BIN" --help >"$TEST_ROOT/help-dash.out" || return 1
  grep -q 'hold run \[run-options\] <cmd|profile>' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  grep -q 'hold logs <target> --plain' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  grep -q 'hold inspect <target>' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  grep -q 'hold profile save <id> as <name>' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  grep -q 'hold export <name> \[as <file>\]' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  grep -q 'hold import <file> as <name>' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  ! grep -q 'hold dump' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  ! grep -q 'hold --console' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }
  ! grep -Eq 'hold aliases?([[:space:]]|$)' "$TEST_ROOT/help-dash.out" || { cat "$TEST_ROOT/help-dash.out" >&2; return 1; }

  "$HOLD_BIN" help >"$TEST_ROOT/help.out" || return 1
  grep -q 'hold run \[run-options\] <cmd|profile>' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  grep -q 'hold logs <target> --plain' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  grep -q 'hold inspect <target>' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  grep -q 'hold profile save <id> as <name>' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  grep -q 'hold export <name> \[as <file>\]' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  grep -q 'hold import <file> as <name>' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  ! grep -q 'hold dump' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  ! grep -q 'hold --console' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }
  ! grep -Eq 'hold aliases?([[:space:]]|$)' "$TEST_ROOT/help.out" || { cat "$TEST_ROOT/help.out" >&2; return 1; }

  "$HOLD_BIN" help profiles >"$TEST_ROOT/help-profiles.out" || return 1
  grep -q 'hold profile save <id> as <name>' "$TEST_ROOT/help-profiles.out" || { cat "$TEST_ROOT/help-profiles.out" >&2; return 1; }
  grep -q 'hold export <name>' "$TEST_ROOT/help-profiles.out" || { cat "$TEST_ROOT/help-profiles.out" >&2; return 1; }
  grep -q 'hold import FILE as <name>' "$TEST_ROOT/help-profiles.out" || { cat "$TEST_ROOT/help-profiles.out" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/help-profiles.out" || { cat "$TEST_ROOT/help-profiles.out" >&2; return 1; }
  ! grep -Eq 'hold aliases?([[:space:]]|$)' "$TEST_ROOT/help-profiles.out" || { cat "$TEST_ROOT/help-profiles.out" >&2; return 1; }

  "$HOLD_BIN" help profile >"$TEST_ROOT/help-profile.out" || return 1
  grep -q 'hold profile <name> <show|start|run|create|set|export|rename|delete>' "$TEST_ROOT/help-profile.out" || {
    cat "$TEST_ROOT/help-profile.out" >&2; return 1;
  }
  grep -q 'hold profile save <runid> as web' "$TEST_ROOT/help-profile.out" || { cat "$TEST_ROOT/help-profile.out" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/help-profile.out" || { cat "$TEST_ROOT/help-profile.out" >&2; return 1; }
  ! grep -q 'hold profile <list|' "$TEST_ROOT/help-profile.out" || { cat "$TEST_ROOT/help-profile.out" >&2; return 1; }
  ! grep -Eq 'hold aliases?([[:space:]]|$)' "$TEST_ROOT/help-profile.out" || { cat "$TEST_ROOT/help-profile.out" >&2; return 1; }

  "$HOLD_BIN" profile -h >"$TEST_ROOT/profile-dash-help.out" || return 1
  grep -q 'hold profile save <runid> as web' "$TEST_ROOT/profile-dash-help.out" || { cat "$TEST_ROOT/profile-dash-help.out" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/profile-dash-help.out" || { cat "$TEST_ROOT/profile-dash-help.out" >&2; return 1; }
  ! grep -q 'hold profile <list|' "$TEST_ROOT/profile-dash-help.out" || { cat "$TEST_ROOT/profile-dash-help.out" >&2; return 1; }
  ! grep -Eq 'hold aliases?([[:space:]]|$)' "$TEST_ROOT/profile-dash-help.out" || { cat "$TEST_ROOT/profile-dash-help.out" >&2; return 1; }

  set +e; "$HOLD_BIN" profile list >/dev/null 2>"$TEST_ROOT/profile-list-removed.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold profile list: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/profile-list-removed.err" || { cat "$TEST_ROOT/profile-list-removed.err" >&2; return 1; }

  set +e; "$HOLD_BIN" profile ls -v >/dev/null 2>"$TEST_ROOT/profile-ls-removed.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold profile ls -v: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'hold profiles -v' "$TEST_ROOT/profile-ls-removed.err" || { cat "$TEST_ROOT/profile-ls-removed.err" >&2; return 1; }

  set +e; "$HOLD_BIN" profile web >/dev/null 2>"$TEST_ROOT/profile-name-usage.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold profile web: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'hold profile <name> <show|start|run|create|set|export|rename|delete>' "$TEST_ROOT/profile-name-usage.err" || {
    cat "$TEST_ROOT/profile-name-usage.err" >&2; return 1;
  }

  set +e; "$HOLD_BIN" profile web nope >/dev/null 2>"$TEST_ROOT/profile-bad-op-usage.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold profile web nope: rc=$rc (want 5)" >&2; return 1; }
  grep -q "failed to resolve profile command 'nope'" "$TEST_ROOT/profile-bad-op-usage.err" || {
    cat "$TEST_ROOT/profile-bad-op-usage.err" >&2; return 1;
  }

  set +e; "$HOLD_BIN" alias deadbeef web >/dev/null 2>"$TEST_ROOT/alias-removed.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold alias: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'alias command was removed' "$TEST_ROOT/alias-removed.err" || { cat "$TEST_ROOT/alias-removed.err" >&2; return 1; }
  grep -q 'hold profile save <id> as <name>' "$TEST_ROOT/alias-removed.err" || { cat "$TEST_ROOT/alias-removed.err" >&2; return 1; }

  set +e; "$HOLD_BIN" alias -h >/dev/null 2>"$TEST_ROOT/alias-help-removed.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold alias -h: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'alias command was removed' "$TEST_ROOT/alias-help-removed.err" || { cat "$TEST_ROOT/alias-help-removed.err" >&2; return 1; }

  set +e; "$HOLD_BIN" aliases >/dev/null 2>"$TEST_ROOT/aliases-removed.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold aliases: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'aliases command was removed' "$TEST_ROOT/aliases-removed.err" || { cat "$TEST_ROOT/aliases-removed.err" >&2; return 1; }
  grep -q 'hold profiles' "$TEST_ROOT/aliases-removed.err" || { cat "$TEST_ROOT/aliases-removed.err" >&2; return 1; }

  set +e; "$HOLD_BIN" aliases -h >/dev/null 2>"$TEST_ROOT/aliases-help-removed.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold aliases -h: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'aliases command was removed' "$TEST_ROOT/aliases-help-removed.err" || { cat "$TEST_ROOT/aliases-help-removed.err" >&2; return 1; }

  set +e; "$HOLD_BIN" help alias >/dev/null 2>"$TEST_ROOT/help-alias.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold help alias: rc=$rc (want 5)" >&2; return 1; }
  grep -q "unknown help topic 'alias'" "$TEST_ROOT/help-alias.err" || { cat "$TEST_ROOT/help-alias.err" >&2; return 1; }

  set +e; "$HOLD_BIN" run web stop >/dev/null 2>"$TEST_ROOT/run-namespace-target-first.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold run web stop: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold run .* -- <cmd>' "$TEST_ROOT/run-namespace-target-first.err" || { cat "$TEST_ROOT/run-namespace-target-first.err" >&2; return 1; }

  set +e; "$HOLD_BIN" run stop web >/dev/null 2>"$TEST_ROOT/run-namespace-verb-first.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold run stop web: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold run .* -- <cmd>' "$TEST_ROOT/run-namespace-verb-first.err" || { cat "$TEST_ROOT/run-namespace-verb-first.err" >&2; return 1; }

  set +e; "$HOLD_BIN" run -e CHECK=1 logs web >/dev/null 2>"$TEST_ROOT/run-namespace-after-option.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "hold run -e CHECK=1 logs web: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold run .* -- <cmd>' "$TEST_ROOT/run-namespace-after-option.err" || { cat "$TEST_ROOT/run-namespace-after-option.err" >&2; return 1; }

  mkdir -p "$TEST_ROOT/literal-help-bin" || return 1
  cat >"$TEST_ROOT/literal-help-bin/--help" <<'SH'
#!/usr/bin/env sh
echo literal-help-command
SH
  cat >"$TEST_ROOT/literal-help-bin/-h" <<'SH'
#!/usr/bin/env sh
echo literal-h-command
SH
  chmod +x "$TEST_ROOT/literal-help-bin/--help" "$TEST_ROOT/literal-help-bin/-h" || return 1
  out=$(PATH="$TEST_ROOT/literal-help-bin:$PATH" "$HOLD_BIN" run -- --help 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.3
  PATH="$TEST_ROOT/literal-help-bin:$PATH" "$HOLD_BIN" dump "$id" >"$TEST_ROOT/literal-help-command.out" || return 1
  grep -q 'literal-help-command' "$TEST_ROOT/literal-help-command.out" || { cat "$TEST_ROOT/literal-help-command.out" >&2; return 1; }

  out=$(PATH="$TEST_ROOT/literal-help-bin:$PATH" "$HOLD_BIN" run -- -h 2>&1) || return 1
  id=$(printf '%s\n' "$out" | extract_id)
  [ -n "$id" ] || { printf '%s\n' "$out" >&2; return 1; }
  sleep 0.3
  PATH="$TEST_ROOT/literal-help-bin:$PATH" "$HOLD_BIN" dump "$id" >"$TEST_ROOT/literal-h-command.out" || return 1
  grep -q 'literal-h-command' "$TEST_ROOT/literal-h-command.out" || { cat "$TEST_ROOT/literal-h-command.out" >&2; return 1; }

  public_files=$(
    { git ls-files docs examples install.sh scripts Makefile 2>/dev/null || find docs examples install.sh scripts Makefile -type f 2>/dev/null; } |
      grep -Ev '^docs/(archive/.*|0\.4-repair-ledger\.md|0\.4\.0-direction-.*|security-review-.*)$' || true
  )
  # The live GitHub repository is still RchGrav/sigmund during the 0.4.0
  # rename window, so the installer needs that repository default. Treat that
  # exact source-location line as an allowed repo-coordinate exception while
  # still rejecting leaked legacy branding or alias/run-namespace docs.
  if printf '%s\n' "$public_files" | xargs grep -InEi '(^|[^[:alnum:]_])(sigmund|mund)([^[:alnum:]_]|$)|(^|[^[:alnum:]_])hold[[:space:]]+aliases?([^[:alnum:]_]|$)|["'\'']?[$]HOLD_BIN["'\'']?[[:space:]]+aliases?([^[:alnum:]_]|$)|alias aliases|hold[[:space:]]+(start|grant|revoke)[[:space:]]+<alias>|start[[:space:]]+<alias>|known alias|alias selection|alias-labeled|alias exists|create an alias|aliases turn|system alias|alias starts|alias ambiguity|alias filtering|alias creation|run id or alias|command as an alias|system:alias|grant alias|root alias|public alias/hash|root-managed alias capabilities|supplied alias|alias/hash capability|run-alias verification|behind an alias|alias was called|alias was created from|for this alias/profile|across aliases' 2>/dev/null |
      grep -Fv 'REPO_NAME="${HOLD_REPO_NAME:-sigmund}"' \
      >"$TEST_ROOT/forbidden-public-names.out"; then
    cat "$TEST_ROOT/forbidden-public-names.out" >&2
    return 1
  fi
  if grep -RInE "grant target must be an existing system alias|alias '%s|--multi applies only to alias|recorded under alias|failed to read .* aliases|usage: hold %s <alias>" \
      src >"$TEST_ROOT/forbidden-public-runtime-strings.out" 2>/dev/null; then
    cat "$TEST_ROOT/forbidden-public-runtime-strings.out" >&2
    return 1
  fi
  if awk '
    /Known commands include:/ { in_known = 1; next }
    in_known && /^```/ { fences++; if (fences == 2) in_known = fences = 0; next }
    in_known && /^[[:space:]]*aliases?[[:space:]]*$/ { print FILENAME ":" FNR ":" $0; bad = 1 }
    END { exit bad ? 1 : 0 }
  ' docs/SPEC.md >"$TEST_ROOT/forbidden-command-list.out"; then
    :
  else
    cat "$TEST_ROOT/forbidden-command-list.out" >&2
    return 1
  fi
}

test_owned_command_exact_arity() {
  local rc
  set +e; "$HOLD_BIN" tail deadbeef extra >/dev/null 2>"$TEST_ROOT/tail-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold tail <target>' "$TEST_ROOT/tail-extra.err" || { cat "$TEST_ROOT/tail-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" dump deadbeef extra >/dev/null 2>"$TEST_ROOT/dump-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "dump extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold dump <target>' "$TEST_ROOT/dump-extra.err" || { cat "$TEST_ROOT/dump-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" console deadbeef extra >/dev/null 2>"$TEST_ROOT/console-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "console extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold console <target>' "$TEST_ROOT/console-extra.err" || { cat "$TEST_ROOT/console-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" prune deadbeef extra >/dev/null 2>"$TEST_ROOT/prune-extra.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "prune extra: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold prune \[target|all\] \[--all\]' "$TEST_ROOT/prune-extra.err" || { cat "$TEST_ROOT/prune-extra.err" >&2; return 1; }

  set +e; "$HOLD_BIN" tail --all deadbeef >/dev/null 2>"$TEST_ROOT/tail-all.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "tail --all: rc=$rc (want 5)" >&2; return 1; }
  grep -q 'usage: hold tail <target>' "$TEST_ROOT/tail-all.err" || { cat "$TEST_ROOT/tail-all.err" >&2; return 1; }

  "$HOLD_BIN" prune >/dev/null || { echo "prune with zero targets should remain valid" >&2; return 1; }
}

test_grant_revoke_argument_refusals() {
  local rc
  # non-root cannot grant (privilege refusal)
  set +e; "$HOLD_BIN" grant web-sys "$TEST_USER" start >/dev/null 2>"$TEST_ROOT/ng.err"; rc=$?; set -e
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
  local id id2 ids running rc
  id=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  "$HOLD_BIN" profile save "$id" as web-n >/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null; "$HOLD_BIN" prune "$id" >/dev/null
  ids=$("$HOLD_BIN" start web-n --multi 3 2>/dev/null | sed -n '/^[0-9a-f]\{12\}$/p')
  [ "$(printf '%s\n' "$ids" | grep -c .)" -eq 3 ] || { echo "--multi 3 did not print 3 ids" >&2; return 1; }
  running=$("$HOLD_BIN" list 2>/dev/null | grep -c running || true)
  [ "$running" -ge 3 ] || { echo "expected >=3 running, got $running" >&2; return 1; }
  "$HOLD_BIN" stop web-n --all >/dev/null 2>&1 || true
  id=$("$HOLD_BIN" run -d web-n 2>&1 | extract_id); [ -n "$id" ] || return 1
  set +e; "$HOLD_BIN" run -d web-n >/dev/null 2>"$TEST_ROOT/run-force.err"; rc=$?; set -e
  [ "$rc" -eq 6 ] || { echo "run duplicate without --force: rc=$rc (want 6)" >&2; return 1; }
  grep -q -- '--force' "$TEST_ROOT/run-force.err" || { cat "$TEST_ROOT/run-force.err" >&2; return 1; }
  id2=$("$HOLD_BIN" run -d --force web-n 2>&1 | extract_id); [ -n "$id2" ] || return 1
  [ "$id2" != "$id" ] || { echo "run --force reused the original id" >&2; return 1; }
  "$HOLD_BIN" stop web-n --all >/dev/null 2>&1 || true
  ids=$("$HOLD_BIN" run -d --multi 2 web-n 2>/dev/null | sed -n '/^[0-9a-f]\{12\}$/p')
  [ "$(printf '%s\n' "$ids" | grep -c .)" -eq 2 ] || { echo "run --multi 2 did not print 2 ids" >&2; return 1; }
  "$HOLD_BIN" stop web-n --all >/dev/null 2>&1 || true
  set +e; "$HOLD_BIN" start web-n --multi >/dev/null 2>"$TEST_ROOT/mm.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi missing: rc=$rc (want 5)" >&2; return 1; }
  grep -q -- "--multi requires a positive count" "$TEST_ROOT/mm.err" || { cat "$TEST_ROOT/mm.err" >&2; return 1; }
  set +e; "$HOLD_BIN" start web-n --multi=abc >/dev/null 2>"$TEST_ROOT/m.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi=abc: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --multi count" "$TEST_ROOT/m.err" || { cat "$TEST_ROOT/m.err" >&2; return 1; }
  set +e; "$HOLD_BIN" start web-n --multi abc >/dev/null 2>"$TEST_ROOT/ms.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi abc: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --multi count 'abc'" "$TEST_ROOT/ms.err" || { cat "$TEST_ROOT/ms.err" >&2; return 1; }
  set +e; "$HOLD_BIN" start web-n --multi 0 >/dev/null 2>"$TEST_ROOT/mz.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi 0: rc=$rc (want 5)" >&2; return 1; }
  grep -q "invalid --multi count '0'" "$TEST_ROOT/mz.err" || { cat "$TEST_ROOT/mz.err" >&2; return 1; }
  "$HOLD_BIN" stop web-n --all >/dev/null 2>&1 || true
}

test_tail_cannot_follow_multiple_starts() {
  local id rc running
  id=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  "$HOLD_BIN" profile save "$id" as web-t >/dev/null || return 1
  "$HOLD_BIN" stop "$id" >/dev/null; "$HOLD_BIN" prune "$id" >/dev/null
  set +e; "$HOLD_BIN" start web-t --multi 2 --tail >/dev/null 2>"$TEST_ROOT/t.err"; rc=$?; set -e
  [ "$rc" -eq 5 ] || { echo "--multi 2 --tail: rc=$rc (want 5)" >&2; return 1; }
  grep -q "cannot follow multiple starts" "$TEST_ROOT/t.err" || { cat "$TEST_ROOT/t.err" >&2; return 1; }
  running=$("$HOLD_BIN" list 2>/dev/null | grep -c running || true)
  [ "$running" -eq 0 ] || { echo "runs created despite refusal ($running)" >&2; "$HOLD_BIN" stop web-t --all >/dev/null 2>&1; return 1; }
}

test_print_over_all_and_multiple() {
  local id1 id2 pgid1 pgid2 out
  id1=$("$HOLD_BIN" -d sleep 60 2>&1 | extract_id) || return 1
  "$HOLD_BIN" profile save "$id1" as web-pr >/dev/null || return 1
  "$HOLD_BIN" stop "$id1" >/dev/null; "$HOLD_BIN" prune "$id1" >/dev/null
  id1=$("$HOLD_BIN" start web-pr 2>&1 | extract_id); pgid1=$(record_pgid "$id1")
  id2=$("$HOLD_BIN" start web-pr --force 2>&1 | extract_id); pgid2=$(record_pgid "$id2")
  [ -n "$pgid1" ] && [ -n "$pgid2" ] || return 1
  out=$("$HOLD_BIN" stop --print --all web-pr) || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" || { echo "--print --all missing pgid1" >&2; return 1; }
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "--print --all missing pgid2" >&2; return 1; }
  out=$("$HOLD_BIN" stop --print "$id1" "$id2") || return 1
  printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid1" && printf '%s\n' "$out" | grep -qF "kill -TERM -- -$pgid2" || { echo "explicit multi --print missing a pgid" >&2; return 1; }
  "$HOLD_BIN" stop web-pr --all >/dev/null 2>&1 || true
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
run_test "public CLI contract rejects legacy aliases and run namespaces" test_public_cli_contract_guards
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
run_test "grant refuses an unsafe hold self-binary" test_grant_refuses_unsafe_self_binary
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
run_test "hold unified CLI surface" test_hold_unified_cli_surface
run_test "Docker-shaped run/logs/ps/rm surface" test_docker_shaped_cli_flags_and_rm
run_test "Docker bare convenience launch defaults to background" test_docker_bare_launch_foreground_follows_output_by_default
run_test "Docker run foreground follows output by default" test_docker_run_foreground_follows_output_by_default
run_test "unsupported Docker-shaped options fail loudly" test_docker_unsupported_options_fail_loudly
run_test "Docker restart policy restarts failed processes" test_docker_restart_policy_restarts_failures
run_test "Docker restart validates policies and tty gate" test_docker_restart_validation_and_tty_gate
run_test "Docker restart persists with named profiles" test_docker_restart_persists_with_named_profile
run_test "start existing run appends retained log" test_start_existing_run_appends_retained_log
run_test "Docker publish and volume flags are rejected" test_docker_publish_and_volume_rejected
run_test "Docker named profiles persist -d -i -t mode flags" test_docker_named_profile_persists_mode_flags
run_test "Docker -i keeps non-PTY stdin open" test_docker_interactive_stdin_pipes_to_child
run_test "Docker -it foreground attaches and detaches" test_docker_tty_foreground_attaches_and_detaches
run_test "Docker --detach-keys changes TTY detach sequence" test_docker_tty_custom_detach_keys
run_test "Docker run -it profile reconnects a singular TTY run" test_docker_profile_it_reconnects_singular_tty_run
run_test "run IDs and named profiles normalize relative file args" test_runid_and_named_profile_normalize_relative_file_args
run_test "hold shell runs a real shell and normal exit creates no runid" test_hold_shell_runs_real_shell_without_creating_runid_on_exit
run_test "hold shell adopt normalizes relative foreground argv paths" test_hold_shell_adopt_normalizes_relative_foreground_argv_paths
run_test "hold shell Ctrl-P Ctrl-Q adopts the foreground process group" test_hold_shell_detach_adopts_foreground_process_group
run_test "captive CLI shows Cisco prompts and commits a profile" test_captive_cli_cisco_prompts_and_profile_commit
run_test "captive CLI noninteractive transcript is script-safe" test_captive_cli_noninteractive_transcript_is_script_safe
run_test "captive CLI profile mode flags commit and clear" test_captive_cli_profile_mode_flags_commit_and_clear
run_test "captive CLI edits profile log destination" test_captive_profile_log_destination_persists_and_clears
run_test "captive CLI IOS contextual help and reserved words" test_captive_cli_ios_contextual_help_and_reserved_words
run_test "captive CLI IOS operational commands" test_captive_cli_ios_operational_commands
run_test "captive CLI rejects ping pseudo-command" test_captive_cli_rejects_ping_pseudo_command
run_test "captive CLI interactive input handles arrows and mouse reports" test_captive_cli_interactive_input_handles_arrows_and_mouse_reports
run_test "special characters are preserved in argv JSON" test_special_chars_args
run_test "logging captures stdout+stderr" test_log_capture
run_test "internal viewer harness seeds literal and similarity filters" test_log_view_internal_seed_filters
run_test "internal viewer follows live logs through filter engine" test_log_view_follow_filters_live_output
run_test "logs follow opens dynamic TTY filter" test_hold_logs_follow_opens_dynamic_tty_filter
run_test "internal viewer follow filters dynamically from typed TTY input" test_log_view_follow_dynamic_tty_filter
run_test "log viewer shows integrated chrome help and info overlays" test_log_viewer_integrated_chrome_help_and_info
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
run_test "Docker-shaped run record is readable without duplicate legacy argv fields" test_docker_shaped_record_fallback_reader
run_test "root starts use system store and public state is projected" test_root_start_writes_system_store_and_public_unknown
run_test "sudo start writes system state with invoking-user metadata" test_sudo_start_writes_system_store_with_invoking_metadata
run_test "sudo --system home executable uses invoking-user store" test_sudo_system_start_of_home_executable_uses_user_store
run_test "sudo --system home argv paths use invoking-user store" test_sudo_system_start_of_home_argv_paths_uses_user_store
run_test "alias for elevated home executable stays user-local" test_home_elevated_run_alias_stays_user_local
run_test "sudo context can stop unique invoking-user local run" test_sudo_context_can_stop_unique_user_local_run
run_test "public root index rows are redacted in normal list" test_public_root_index_list_is_redacted
run_test "public root index list reads projected State" test_public_root_index_list_reads_projected_state
run_test "normal run does not self-elevate on local/root ID conflict" test_user_local_wins_over_public_root_collision
run_test "explicit user:<id> targets user-local run" test_explicit_user_target
run_test "user alias stores a direct recipe and starts/stops by alias" test_alias_profile_map_start_and_stop
run_test "alias from relative executable keeps absolute recorded argv0" test_alias_from_relative_executable_uses_recorded_absolute_argv0
run_test "profile start requires --force when already running and --all stops all" test_alias_multi_gate_and_all_stop
run_test "profile multi mode allows repeated starts without --force" test_profile_multi_mode_allows_repeated_starts
run_test "profile start inherits current environment" test_profile_start_inherits_current_environment
run_test "Docker --env persists with a named profile" test_docker_env_persists_with_named_profile
run_test "profile save preserves capability metadata" test_profile_save_preserves_capability_metadata
run_test "profile create/export/import preserves capability metadata" test_profile_create_export_import_preserves_capability_metadata
run_test "root capability drop-all then add applies requested cap" test_root_capability_drop_all_then_add_preserves_added_cap
run_test "direct capability metadata projects to inspect" test_direct_capability_metadata_projects_to_inspect
run_test "restart worker applies capability metadata" test_restart_worker_applies_capability_metadata
run_test "Docker --env-file persists with a named profile" test_docker_env_file_persists_with_named_profile
run_test "Docker --rm removes run artifacts after exit" test_docker_rm_removes_run_artifacts_after_exit
run_test "Docker --rm removes profile run artifacts after exit" test_docker_rm_removes_profile_run_artifacts_after_exit
run_test "captive profile env persists and runs" test_captive_profile_env_persists_and_runs
run_test "captive profile mode loads and edits an existing recipe" test_captive_profile_loads_existing_recipe_for_edit
run_test "captive profile publish/volume commands are rejected" test_captive_profile_publish_volume_rejected
run_test "captive profile capability metadata preserves edits and clears" test_captive_profile_capability_metadata_preserves_edits_and_clears
run_test "captive profile restart metadata preserves edits and clears" test_captive_profile_restart_metadata_preserves_edits_and_clears
run_test "captive profile mode preserves quoted argv and env values" test_captive_profile_quoted_argv_and_env_round_trip
run_test "profile transcript import/export round-trips a user-local recipe" test_profile_transcript_import_export_roundtrip
run_test "profile name-first set command edits a user-local recipe" test_profile_name_first_set_command
run_test "profile name-first create rename and delete manage a user-local recipe" test_profile_name_first_crud
run_test "profile JSON export/import round-trips a user-local recipe" test_profile_json_export_and_import
run_test "invalid alias names are rejected" test_invalid_alias_names_rejected
run_test "short hex-looking alias names are allowed" test_short_hex_alias_name_allowed
run_test "system alias action self-elevates alias token" test_system_alias_action_self_elevates_alias
run_test "system alias start self-elevates alias token" test_system_alias_start_self_elevates_alias
run_test "system profile save preserves capability metadata" test_system_profile_save_preserves_capability_metadata
run_test "grant refuses user-writable profile paths unless forced" test_grant_refuses_user_writable_profile_paths
run_test "granted start pins privileged cwd" test_granted_start_pins_privileged_cwd
run_test "grant/revoke writes hash-scoped sudoers entries" test_grant_revoke_writes_hash_scoped_sudoers
run_test "elevated capability start/stop validates alias and hash" test_elevated_capability_start_and_stop_validate_alias_hash
run_test "action self-elevation uses argv-preserving sudo fork+wait" test_action_self_elevation_uses_argv_fork_wait
run_test "elevated action returns child/root-hold status" test_elevated_action_returns_child_status
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
  echo "  (some tests skipped; run as root and/or set HOLD_REQUIRE_ROOT_TESTS=1 for full coverage)"
fi
if [ "$FAILS" -ne 0 ]; then
  exit 1
fi
