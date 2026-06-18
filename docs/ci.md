# Using Sigmund in CI

[Docs index](index.md) | [Previous: CLI contract](cli-contract.md) | [Loop: Index](index.md) | Related: [Launcher](launcher.md), [Identity](identity.md)

Sigmund is useful in CI when one step needs to start a long-running helper and later steps need to inspect or stop it. Examples include databases, web servers, emulators, local object stores, test daemons, and language-specific dev servers. It gives CI scripts a durable run ID, a log, and safe process-group teardown without bringing in `tmux`, `systemd`, a custom supervisor, or hand-managed PID files.

The important CI contract is simple:

- `sigmund <cmd...>` starts the helper detached from the shell's process group.
- stdout is only the run ID, so scripts can capture it directly.
- stderr contains human status such as the log path and stop command.
- `sigmund dump <id>` prints the saved log and exits.
- `sigmund stop <id>` validates the recorded process group, sends `SIGTERM`, then escalates to `SIGKILL` if needed.
- `sigmund prune <id>` removes past run state after teardown.

## Recipes

### Start a helper in one step and use it later

This is the basic CI problem: a shell step exits, but the helper should keep running for later steps.

```yaml
- name: Start API
  shell: bash
  run: |
    set -Eeuo pipefail
    api_id="$(./sigmund npm run start:api)"
    echo "API_RUN_ID=$api_id" >> "$GITHUB_ENV"

- name: Test against API
  shell: bash
  run: |
    set -Eeuo pipefail
    curl -fsS http://127.0.0.1:3000/health
    npm run test:integration
```

Benefit: the later step gets a stable Sigmund run ID instead of relying on `$!`, which died with the shell that launched it.

### Show logs only when tests fail

```yaml
- name: Dump API log on failure
  if: failure()
  shell: bash
  run: |
    ./sigmund dump "$API_RUN_ID" || true
```

Benefit: logs are captured from the file Sigmund created at launch, so you do not need to redirect output by hand or keep a background `tail` alive.

### Always stop the whole process group

```yaml
- name: Stop API
  if: always()
  shell: bash
  run: |
    set +e
    ./sigmund stop "$API_RUN_ID"
    stop_rc=$?
    ./sigmund prune "$API_RUN_ID" || true
    exit "$stop_rc"
```

Benefit: Sigmund signals the recorded process group after validation. This is safer than `kill "$pid"` and catches child processes that a shell wrapper may have spawned.

### Start multiple helpers and tear them down together

```bash
api_id="$(sigmund npm run start:api)"
worker_id="$(sigmund npm run start:worker)"
redis_id="$(sigmund redis-server --port 6379)"

sigmund stop "$api_id" "$worker_id" "$redis_id"
sigmund prune "$api_id" || true
sigmund prune "$worker_id" || true
sigmund prune "$redis_id" || true
```

Benefit: each helper has its own run ID and log, while `stop` can still tear down several recorded process groups in one command.

### Keep a live log open for a long smoke test

Use `tail` when a CI step is intentionally a live monitor:

```bash
run_id="$(sigmund ./long-running-smoke-server)"
sigmund tail "$run_id" &
tail_pid=$!

./run-smoke-tests
test_rc=$?

kill "$tail_pid" 2>/dev/null || true
sigmund stop "$run_id" || true
sigmund prune "$run_id" || true
exit "$test_rc"
```

Benefit: `tail` follows the recorded log without becoming the owner of the helper process. Stopping the tail does not stop the run.

### Print the underlying signal command for debugging

```bash
sigmund stop --print "$API_RUN_ID"
```

Benefit: this validates the record and prints the process-group signal command Sigmund would use, which is useful when debugging CI cleanup behavior.

### Use a root-managed helper only when needed

```yaml
- name: Start root-managed helper
  shell: bash
  run: |
    run_id="$(./sigmund --system ./needs-root-managed-state --flag)"
    echo "ROOT_HELPER_RUN_ID=$run_id" >> "$GITHUB_ENV"

- name: Stop root-managed helper
  if: always()
  shell: bash
  run: |
    ./sigmund stop "system:$ROOT_HELPER_RUN_ID" || true
```

Benefit: root-managed runs are discoverable through the public redacted index while private command details and logs remain root-only. Use this only when the helper really belongs in system state.

## Basic Pattern

```mermaid
flowchart LR
    Start["Start helper"] --> Save["Save run ID"]
    Save --> Wait["Wait for readiness"]
    Wait --> Tests["Run tests"]
    Tests --> Logs["Dump logs on failure"]
    Tests --> Stop["Stop helper always"]
    Logs --> Stop
    Stop --> Prune["Prune state"]

    classDef script fill:#ccfbf1,stroke:#0f766e,color:#134e4a
    classDef launch fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef safety fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    classDef cleanup fill:#fef3c7,stroke:#b45309,color:#78350f
    class Start,Save launch
    class Wait,Tests script
    class Logs safety
    class Stop,Prune cleanup
```

The key is to save the run ID somewhere the later CI steps can read. In GitHub Actions, use `$GITHUB_ENV` or `$GITHUB_OUTPUT`. In other CI systems, use their environment-file, artifact, workspace file, or step-output mechanism.

## Full GitHub Actions Example

This workflow starts a Python HTTP server, waits until it is reachable, runs checks, dumps logs on failure, and always stops the process group.

```yaml
name: integration

on:
  push:
  pull_request:

jobs:
  integration:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Build Sigmund
        run: make

      - name: Start web helper
        shell: bash
        run: |
          set -Eeuo pipefail
          run_id="$(./sigmund python3 -m http.server 8765)"
          echo "WEB_RUN_ID=$run_id" >> "$GITHUB_ENV"

      - name: Wait for web helper
        shell: bash
        run: |
          set -Eeuo pipefail
          for i in $(seq 1 30); do
            if curl -fsS http://127.0.0.1:8765/ >/dev/null; then
              exit 0
            fi
            sleep 1
          done
          ./sigmund dump "$WEB_RUN_ID" || true
          exit 1

      - name: Run integration tests
        shell: bash
        run: |
          set -Eeuo pipefail
          curl -fsS http://127.0.0.1:8765/ >/dev/null
          # Replace this with your real test command.
          # npm run test:integration

      - name: Show helper log on failure
        if: failure()
        shell: bash
        run: |
          ./sigmund dump "$WEB_RUN_ID" || true

      - name: Stop helper
        if: always()
        shell: bash
        run: |
          set +e
          ./sigmund stop "$WEB_RUN_ID"
          stop_rc=$?
          ./sigmund prune "$WEB_RUN_ID" || true
          exit "$stop_rc"
```

This example does not use `tail` because CI jobs usually need finite steps. `dump` is the better failure path because it prints the current log and exits. Use `tail` in CI only when you intentionally want a live log-following step.

## Generic Shell Template

Use this shape in any CI system that runs POSIX shell:

```bash
set -Eeuo pipefail

run_id="$(sigmund ./your-server --port 9000)"
printf '%s\n' "$run_id" > .sigmund-your-server-id

cleanup() {
  rc=$?
  if [ "$rc" -ne 0 ]; then
    sigmund dump "$run_id" || true
  fi
  sigmund stop "$run_id" || true
  sigmund prune "$run_id" || true
  exit "$rc"
}
trap cleanup EXIT

for i in $(seq 1 60); do
  if curl -fsS http://127.0.0.1:9000/health >/dev/null; then
    break
  fi
  sleep 1
done

./run-your-tests
```

This keeps the run ID in a shell variable for same-step tests and in a file if a later step needs it. If your CI runs each step in a fresh shell, write the ID to the CI system's supported output mechanism too.

## Multiple Helpers

Start each service separately and stop each ID during teardown:

```bash
api_id="$(sigmund npm run start:api)"
worker_id="$(sigmund npm run start:worker)"
redis_id="$(sigmund redis-server --port 6379)"

cleanup() {
  rc=$?
  [ "$rc" -eq 0 ] || {
    sigmund dump "$api_id" || true
    sigmund dump "$worker_id" || true
    sigmund dump "$redis_id" || true
  }
  sigmund stop "$api_id" "$worker_id" "$redis_id" || true
  sigmund prune "$api_id" || true
  sigmund prune "$worker_id" || true
  sigmund prune "$redis_id" || true
  exit "$rc"
}
trap cleanup EXIT
```

`stop` accepts multiple targets. `prune` is shown one ID at a time because that makes cleanup diagnostics easier to read in CI logs.

## Exit-Code Handling

For strict teardown, handle important `stop` statuses explicitly:

```bash
sigmund stop "$run_id"
stop_rc=$?
case "$stop_rc" in
  0)
    ;;
  2)
    echo "Sigmund refused to signal because the run could not be validated" >&2
    ;;
  5)
    echo "No recorded run matched $run_id" >&2
    ;;
  *)
    echo "sigmund stop failed with exit code $stop_rc" >&2
    ;;
esac
exit "$stop_rc"
```

Exit code 2 matters in CI. It means Sigmund protected you from signaling a process group it could not prove was still the one it started. That is a test infrastructure problem worth surfacing, not a cleanup detail to hide.

## Root-Managed Helpers

Use `--system` only when the helper really needs root-managed state:

```bash
run_id="$(sigmund --system ./root-helper --flag)"
echo "ROOT_HELPER_RUN_ID=$run_id" >> "$GITHUB_ENV"
```

Later commands such as `sigmund stop system:<id>` may self-elevate through sudo when the public root index can identify the target. Normal user-local runs do not need `--system`, and user-local matches win over root-public collisions.

## What to Avoid

- Do not parse the human start banner. Capture stdout for the run ID.
- Do not use `tail` where a finite `dump` is enough.
- Do not ignore exit code 2 from `stop`; it is Sigmund's safety refusal.
- Do not use `--system` just to make a run visible. Use it only for root-managed helpers.
- Do not assume a public root row is authoritative liveness. Root/private validation happens when the action runs.

## Source anchors

Primary functions: `perform_start`, `cmd_dump_action`, `cmd_signal_action`, `do_signal_action`, `cmd_prune_action`, `help_scripting`, and `main`.

## Continue

[Back to docs index](index.md) | [Top](#using-sigmund-in-ci) | [Loop to start](index.md) | Branch to: [CLI contract](cli-contract.md), [Launcher](launcher.md), [Identity](identity.md)
