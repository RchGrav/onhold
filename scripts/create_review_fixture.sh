#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

hold_bin="${HOLD_BIN:-}"
if [[ -z "$hold_bin" ]]; then
  latest="$(find review-builds -maxdepth 2 -type f -name hold-dynamic 2>/dev/null | sort | tail -n 1 || true)"
  if [[ -n "$latest" ]]; then
    hold_bin="$latest"
  elif [[ -x ./hold ]]; then
    hold_bin="./hold"
  else
    echo "review fixture: no hold binary found; run make review-build first" >&2
    exit 1
  fi
fi
hold_bin="$(realpath "$hold_bin")"

fixture_root="${1:-review-fixtures/demo}"
fixture_root="$(mkdir -p "$fixture_root" && cd "$fixture_root" && pwd)"
demo_home="$fixture_root/home"
demo_bin="$fixture_root/bin"
ids_file="$fixture_root/RUNS.env"
readme="$fixture_root/README.txt"

# Stop any previous live demo runs in this fixture before replacing it.
if [[ -f "$ids_file" ]]; then
  # shellcheck disable=SC1090
  source "$ids_file" || true
  for id in ${WEB_RUN_ID:-} ${SLOW_RUN_ID:-}; do
    if [[ -n "$id" ]]; then
      HOME="$demo_home" "$hold_bin" end "$id" >/dev/null 2>&1 || true
    fi
  done
fi

rm -rf "$fixture_root"
mkdir -p "$demo_home" "$demo_bin"

cat > "$demo_bin/web-demo" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'web-demo boot: pid=%s\n' "$$"
printf 'listening on http://127.0.0.1:8080\n'
printf 'hint: in hold logs, type error, db, payment, or timeout\n'
i=0
while :; do
  i=$((i + 1))
  printf 'INFO request id=%04d path=/api/items status=200 latency=%dms\n' "$i" $((20 + (i % 40)))
  if (( i % 5 == 0 )); then
    printf 'WARN db retry id=%04d database connection timeout retry=%d\n' "$i" $((i / 5))
  fi
  if (( i % 9 == 0 )); then
    printf 'ERROR payment gateway declined order=%04d reason=card_expired\n' "$i"
  fi
  sleep 1
 done
SCRIPT
chmod +x "$demo_bin/web-demo"

cat > "$demo_bin/slow-demo" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'slow-demo boot: pid=%s\n' "$$"
printf 'type batch, reconcile, warning, or complete in the viewer\n'
i=0
while :; do
  i=$((i + 1))
  printf 'batch worker heartbeat batch=%03d queue=default\n' "$i"
  if (( i % 4 == 0 )); then
    printf 'warning reconcile drift batch=%03d expected=%d actual=%d\n' "$i" $((1000 + i)) $((997 + i))
  fi
  sleep 2
 done
SCRIPT
chmod +x "$demo_bin/slow-demo"

cat > "$demo_bin/finished-demo" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'finished-demo boot\n'
for i in $(seq 1 60); do
  case $((i % 10)) in
    0) printf 'ERROR import row=%03d failed validation field=email\n' "$i" ;;
    3) printf 'WARN import row=%03d duplicate customer ignored\n' "$i" ;;
    *) printf 'INFO import row=%03d accepted customer=demo-%03d\n' "$i" "$i" ;;
  esac
 done
printf 'finished-demo complete: accepted=48 warn=6 error=6\n'
SCRIPT
chmod +x "$demo_bin/finished-demo"

cat > "$demo_bin/burst-demo" <<'SCRIPT'
#!/usr/bin/env bash
set -euo pipefail
printf 'burst-demo start\n'
for i in $(seq 1 1200); do
  if (( i == 777 )); then
    printf 'RARE_MATCH incident id=%04d component=cache message="needle in large haystack"\n' "$i"
  elif (( i % 137 == 0 )); then
    printf 'WARN burst checkpoint id=%04d component=loader\n' "$i"
  else
    printf 'DEBUG burst filler id=%04d value=%08x\n' "$i" "$i"
  fi
 done
printf 'burst-demo complete\n'
SCRIPT
chmod +x "$demo_bin/burst-demo"

run_hold() {
  HOME="$demo_home" "$hold_bin" "$@"
}

# Place the calls; -d prints the bare 64-hex call id on stdout.
web_run="$(run_hold -d -- "$demo_bin/web-demo" 2>/dev/null)"
slow_run="$(run_hold -d -- "$demo_bin/slow-demo" 2>/dev/null)"
finished_run="$(run_hold -d -- "$demo_bin/finished-demo" 2>/dev/null)"
burst_run="$(run_hold -d -- "$demo_bin/burst-demo" 2>/dev/null)"

sleep 2
# Naming a call also saves it, so the fixture survives purge sweeps.
run_hold rename "$web_run" web-demo >/dev/null
run_hold rename "$slow_run" slow-demo >/dev/null
run_hold rename "$finished_run" finished-demo >/dev/null
run_hold rename "$burst_run" burst-demo >/dev/null

cat > "$ids_file" <<ENV
export HOLD_BIN='$hold_bin'
export HOLD_REVIEW_HOME='$demo_home'
export WEB_RUN_ID='$web_run'
export SLOW_RUN_ID='$slow_run'
export FINISHED_RUN_ID='$finished_run'
export BURST_RUN_ID='$burst_run'
ENV

cat > "$readme" <<README
Hold review fixture
===================

This fixture is isolated from your real user state. Every command below uses:

  HOME=$demo_home
  HOLD=$hold_bin

Load convenience variables:

  source $ids_file

Quick commands:

  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN list
  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN logs \$WEB_RUN_ID
  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN logs \$WEB_RUN_ID --follow
  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN logs \$FINISHED_RUN_ID
  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN logs \$BURST_RUN_ID

Viewer things to try:

  In the full-screen log viewer, just type words. There is no CLI prefilter.
  Try typing:

    error
    timeout
    db
    payment
    RARE_MATCH

  Backspace relaxes the filter.
  Space excludes lines like the highlighted line; Ctrl-R resets filters.
  PgUp/PgDn move through filtered windows.
  q quits.

Targets created:

  WEB_RUN_ID=$web_run        live noisy service log
  SLOW_RUN_ID=$slow_run       live slower worker log
  FINISHED_RUN_ID=$finished_run   completed import log
  BURST_RUN_ID=$burst_run      completed 1200-line sparse-match log

Cleanup live demo runs:

  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN end \$WEB_RUN_ID
  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN end \$SLOW_RUN_ID

The calls are named, so they are saved; a plain purge leaves them for
re-inspection. Remove one for good with:

  HOME=\$HOLD_REVIEW_HOME \$HOLD_BIN purge web-demo --force
README

printf 'Review fixture created:\n'
printf '  root:       %s\n' "$fixture_root"
printf '  env:        %s\n' "$ids_file"
printf '  readme:     %s\n' "$readme"
printf '  hold:       %s\n' "$hold_bin"
printf '  web run:    %s\n' "$web_run"
printf '  slow run:   %s\n' "$slow_run"
printf '  finished:   %s\n' "$finished_run"
printf '  burst:      %s\n' "$burst_run"
