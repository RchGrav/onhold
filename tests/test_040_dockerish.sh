#!/usr/bin/env bash
set -Eeuo pipefail

HOLD_REAL_BIN="${HOLD_BIN:-./hold-dynamic}"
case "$HOLD_REAL_BIN" in
  /*) ;;
  *) HOLD_REAL_BIN="$PWD/$HOLD_REAL_BIN" ;;
esac

TEST_ROOT="$(mktemp -d /tmp/hold-040.XXXXXX)"
cleanup() {
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" stop web --all >/dev/null 2>&1 || true
  HOME="$TEST_ROOT/home" HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system" "$HOLD_REAL_BIN" stop port-smoke >/dev/null 2>&1 || true
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

export HOME="$TEST_ROOT/home"
export HOLD_TEST_SYSTEM_STATE_DIR="$TEST_ROOT/system"
export HOLD_TEST_INVOKING_HOME="$TEST_ROOT/home"
mkdir -p "$HOME"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

hold() {
  "$HOLD_REAL_BIN" "$@"
}

extract_display() {
  sed -n '/^[0-9a-f]\{12\}$/p' | head -n1
}

extract_full_from_output() {
  sed -n 's#.*state/hold/\([0-9a-f]\{64\}\)\.log.*#\1#p' | head -n1
}

out=$(hold run -- /bin/sh -c 'echo smoke-out; echo smoke-err >&2' 2>&1)
printf '%s\n' "$out" | grep -qx 'smoke-out' || fail "foreground stdout missing"
printf '%s\n' "$out" | grep -qx 'smoke-err' || fail "foreground stderr missing"
display=$(printf '%s\n' "$out" | extract_display)
[ "${#display}" -eq 12 ] || fail "foreground run id display is not 12 hex"
full=$(printf '%s\n' "$out" | extract_full_from_output)
[ "${#full}" -eq 64 ] || fail "full 64-hex run id missing from log path"
log="$HOME/.local/state/hold/$full.log"
grep -Eq '^\{"log":"smoke-out\\n","stream":"stdout","time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z"\}$' "$log" ||
  fail "stdout JSON log entry missing"
hold logs --plain "$display" | grep -qx 'smoke-out' || fail "logs --plain did not decode JSON"

hold profile web -e HELLO=world --log-destination syslog -- /bin/sh -c 'echo $HELLO; sleep 2' >/dev/null
hold profile web export --json | grep -q '"log_destination": "syslog"' ||
  fail "profile log destination was not persisted"
if hold profile --name nope /bin/true >/tmp/hold-profile-name.out 2>/tmp/hold-profile-name.err; then
  fail "profile --name unexpectedly succeeded"
fi

runout=$(hold run web 2>&1)
printf '%s\n' "$runout" | grep -qx 'world' || fail "hold run <profile> did not foreground output"
runid=$(printf '%s\n' "$runout" | extract_display)
[ "${#runid}" -eq 12 ] || fail "profile run id display is not 12 hex"

bg1=$(hold run -d web 2>&1 | extract_display)
[ "${#bg1}" -eq 12 ] || fail "detached profile id missing"
if hold run -d web >/tmp/hold-dupe.out 2>/tmp/hold-dupe.err; then
  fail "active profile duplicate succeeded without --force"
fi
hold run -d --force web >/dev/null 2>&1 || fail "--force profile start failed"
hold stop web --all >/dev/null 2>&1 || true

bare=$(hold web 2>&1 | extract_display)
[ "${#bare}" -eq 12 ] || fail "bare profile launch did not print id"
sleep 0.3
hold logs --plain "$bare" | grep -qx 'world' || fail "bare profile log missing"

named=$(hold run -d --name named-smoke -- /bin/sh -c 'echo named; sleep 0.1' 2>&1 | extract_display)
[ "${#named}" -eq 12 ] || fail "named run id missing"
sleep 0.3
hold start named-smoke >/tmp/hold-restart.out 2>&1 || fail "restart by run name failed"
restart_id=$(extract_display < /tmp/hold-restart.out)
[ "$restart_id" = "$named" ] || fail "restart did not reuse existing run id"
sleep 0.3
[ "$(hold logs --plain named-smoke | grep -c '^named$')" -eq 2 ] ||
  fail "restart did not append to existing JSON log"

if hold run -p 8080:80 -- /bin/true >/tmp/hold-pub.out 2>/tmp/hold-pub.err; then
  fail "publish unexpectedly succeeded"
fi
grep -q 'does not publish or forward ports' /tmp/hold-pub.err ||
  fail "publish rejection message missing"
if hold run -v "$TEST_ROOT:/work" -- /bin/true >/tmp/hold-vol.out 2>/tmp/hold-vol.err; then
  fail "volume unexpectedly succeeded"
fi
grep -q 'does not mount or remap volumes' /tmp/hold-vol.err ||
  fail "volume rejection message missing"
if hold profile badpub -p 8080:80 -- /bin/true >/tmp/hold-profile-pub.out 2>/tmp/hold-profile-pub.err; then
  fail "profile publish unexpectedly succeeded"
fi
grep -q 'does not publish or forward ports' /tmp/hold-profile-pub.err ||
  fail "profile publish rejection message missing"
if hold profile badvol -v "$TEST_ROOT:/work" -- /bin/true >/tmp/hold-profile-vol.out 2>/tmp/hold-profile-vol.err; then
  fail "profile volume unexpectedly succeeded"
fi
grep -q 'does not mount or remap volumes' /tmp/hold-profile-vol.err ||
  fail "profile volume rejection message missing"

server=$(hold run -d --name port-smoke -- python3 -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); time.sleep(5)' 2>&1 | extract_display)
[ "${#server}" -eq 12 ] || fail "server id missing"
for _ in $(seq 1 30); do
  psout=$(hold ps 2>/dev/null || true)
  if printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp'; then
    break
  fi
  sleep 0.1
done
printf '%s\n' "$psout" | grep -q '^RUN ID[[:space:]]\+PROFILE[[:space:]]\+COMMAND[[:space:]]\+CREATED[[:space:]]\+STATUS[[:space:]]\+PORTS[[:space:]]\+NAMES' ||
  fail "ps header is not Docker-shaped"
printf '%s\n' "$psout" | grep -Eq '127\.0\.0\.1:[0-9]+/tcp' ||
  fail "observed listening port missing from ps"
hold stop port-smoke >/dev/null 2>&1 || true

rmout=$(hold run -d --rm -- /bin/sh -c 'echo rm; sleep 0.1' 2>&1)
rmfull=$(printf '%s\n' "$rmout" | extract_full_from_output)
[ "${#rmfull}" -eq 64 ] || fail "--rm full id missing"
for _ in $(seq 1 50); do
  [ ! -e "$HOME/.local/state/hold/$rmfull.json" ] && [ ! -e "$HOME/.local/state/hold/$rmfull.log" ] && break
  sleep 0.1
done
[ ! -e "$HOME/.local/state/hold/$rmfull.json" ] || fail "--rm record remained"
[ ! -e "$HOME/.local/state/hold/$rmfull.log" ] || fail "--rm log remained"

echo "PASS: hold 0.4 Docker-shaped smoke"
