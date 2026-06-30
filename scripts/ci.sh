#!/usr/bin/env bash
# One entrypoint, same rigor everywhere. Runs the full local mirror of the
# GitHub CI matrix against the current tree:
#   static -Werror build -> dynamic -Werror build -> regression suite +
#   profile-hash vector + 0.4 Docker-shaped smoke -> ASan/UBSan build+test ->
#   cppcheck -> layer-dependency lint.
#
# Runs identically on macOS and Linux. Each check that the local toolchain
# genuinely lacks (e.g. cppcheck not installed) is reported as SKIPPED with a
# visible notice, never silently. For full Linux parity from a Mac, run
# scripts/linux.sh (which executes this script inside the committed CI image).
#
# Env:
#   CC                     compiler to use (default: cc)
#   HOLD_REQUIRE_ROOT_TESTS=1   turn test skips into failures (set when root)
set -uo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

WERROR="-std=c11 -Wall -Wextra -Wpedantic -Werror -O2"
SAN_FLAGS="-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined"
CC_BIN="${CC:-cc}"
export HOLD_TEST_TIMEOUT="${HOLD_TEST_TIMEOUT:-60}"

fails=0
ran=""
skipped=""
step()    { printf '\n=== %s ===\n' "$1"; }
mark_ok() { ran="$ran $1"; }
mark_no() { skipped="$skipped|$1 ($2)"; printf '  -- SKIPPED: %s (%s)\n' "$1" "$2"; }
must()    { if ! "$@"; then printf '  FAILED: %s\n' "$*" >&2; fails=$((fails + 1)); fi; }

step "version sync"
must bash scripts/check_version_sync.sh
mark_ok "version-sync"

step "static, -Werror (CC=$CC_BIN)"
must make clean
must make CC="$CC_BIN" CFLAGS="$WERROR"
mark_ok "static-build"

step "dynamic, -Werror"
must make clean
must make hold-dynamic CC="$CC_BIN" CFLAGS="$WERROR"
mark_ok "dynamic-build"

step "regression suite + profile-hash vector"
must make clean
must make test CC="$CC_BIN" CFLAGS="$WERROR"
must make test-040 CC="$CC_BIN" CFLAGS="$WERROR"
mark_ok "suite"

step "ASan/UBSan"
if printf 'int main(void){return 0;}\n' | "$CC_BIN" -fsanitize=address,undefined -x c - -o /tmp/.hold_sanprobe 2>/dev/null; then
  rm -f /tmp/.hold_sanprobe
  must make clean
  must make test CC="$CC_BIN" CFLAGS="$SAN_FLAGS" STATIC_LDFLAGS='' LDFLAGS='-fsanitize=address,undefined' TEST_LDFLAGS='-fsanitize=address,undefined'
  mark_ok "asan-ubsan"
else
  mark_no "asan-ubsan" "$CC_BIN has no -fsanitize support"
fi

step "cppcheck static analysis"
if command -v cppcheck >/dev/null 2>&1; then
  must cppcheck --enable=warning,performance,portability --error-exitcode=1 \
    --suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches \
    --std=c11 -Iinclude src/
  mark_ok "cppcheck"
else
  mark_no "cppcheck" "not installed; run scripts/linux.sh for parity"
fi

step "layer dependency lint"
must bash scripts/lint_layers.sh
mark_ok "layer-lint"

make clean >/dev/null 2>&1 || true

printf '\n================ ci.sh summary ================\n'
printf 'ran:%s\n' "$ran"
[ -n "$skipped" ] && printf 'skipped:%s\n' "$(printf '%s' "$skipped" | tr '|' '\n  ')"
if [ "$fails" -ne 0 ]; then
  printf 'RESULT: FAIL (%d step(s) failed)\n' "$fails"
  exit 1
fi
printf 'RESULT: PASS\n'
