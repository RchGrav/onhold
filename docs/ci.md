# CI example

This workflow uses only Sigmund behavior implemented in `src/sigmund.c`: start a detached helper, capture the run ID from stdout, use the run ID in later steps, inspect logs, stop the process group, prune state, and handle exit codes.

```yaml
name: sigmund integration example

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

      - name: Start helper
        id: helper
        shell: bash
        run: |
          set -Eeuo pipefail
          run_id="$(./sigmund python3 -m http.server 8765)"
          echo "run_id=$run_id" >> "$GITHUB_OUTPUT"
          echo "SIGMUND_RUN_ID=$run_id" >> "$GITHUB_ENV"

      - name: Wait for helper
        shell: bash
        run: |
          set -Eeuo pipefail
          for i in $(seq 1 30); do
            if curl -fsS http://127.0.0.1:8765/ >/dev/null; then
              exit 0
            fi
            sleep 1
          done
          ./sigmund dump "$SIGMUND_RUN_ID" || true
          exit 1

      - name: Run integration checks
        shell: bash
        run: |
          set -Eeuo pipefail
          curl -fsS http://127.0.0.1:8765/ >/dev/null

      - name: Show helper log on failure
        if: failure()
        shell: bash
        run: |
          ./sigmund dump "$SIGMUND_RUN_ID" || true

      - name: Teardown helper
        if: always()
        shell: bash
        run: |
          set +e
          ./sigmund stop "$SIGMUND_RUN_ID"
          stop_rc=$?
          case "$stop_rc" in
            0) ;;
            2) echo "Sigmund refused to signal for safety" >&2 ;;
            5) echo "Run was already pruned or never recorded" >&2 ;;
            *) echo "sigmund stop failed with exit code $stop_rc" >&2 ;;
          esac
          ./sigmund prune "$SIGMUND_RUN_ID" || true
          exit "$stop_rc"
```

## Why the workflow is shaped this way

The `Start helper` step captures only stdout because `perform_start` prints the bare run ID there. Human start banners go to stderr, so they do not pollute `run_id`.

The failure-log step uses `dump`, not `tail`, because `dump` prints the current log and exits. A live `tail` is useful interactively, but CI teardown steps should not depend on an open-ended follower unless the job specifically wants that behavior.

The teardown step handles exit code 2 separately because that means Sigmund refused for safety. In a real CI workflow, that should be visible rather than silently ignored: it means the validate-before-signal model could not prove the recorded process group was still the intended target.

`prune` is best-effort cleanup after stop. Running valid runs are not pruned, so the workflow stops first and then prunes the past run data.

## Source anchors

Primary functions: `perform_start`, `cmd_dump_action`, `cmd_signal_action`, `do_signal_action`, `cmd_prune_action`, `help_scripting`, and `main`.
