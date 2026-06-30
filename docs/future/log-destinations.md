# Logging destinations and capture semantics

[Future work](README.md) | [0.4 release cut](../0.4-release-cut.md) | [Viewer design before 0.5](viewer-fixes-before-0.5.md)

Status: planning note for journald and future logging destinations. It records the intended logging contract and the current implementation shape so destination work does not accidentally change stdout/stderr capture semantics.

## Capture contract

Hold captures managed child output regardless of whether the user is viewing it live.

For normal non-PTY runs:

- the child process has `stdout` and `stderr` connected to separate pipes;
- a logger process reads both pipes;
- every chunk/line is appended to the run's local raw log in capture order;
- every retained record also gets a `run.log.idx` entry containing offset, length, capture timestamp delta, and stream metadata;
- live foreground/follow/viewer modes read from the retained log path rather than bypassing the log;
- local raw logging remains the source of truth even when another destination is configured.

Therefore live viewing is a consumer of the captured log, not a replacement for capture. Starting a run attached, following logs, or opening the dynamic viewer must not disable local raw+idx capture.

For console/PTY runs:

- the child is attached to a PTY;
- stdout and stderr are merged by terminal semantics before Hold sees the bytes;
- the broker writes PTY output to the raw log with index metadata as stream `stdout` today;
- attached console clients receive replay/live bytes from the broker, while the broker still logs them.

This means stream fidelity differs by mode: normal pipe capture preserves stdout vs stderr; PTY/console capture cannot reliably preserve that distinction without an additional protocol inside the PTY, so destination code should not invent stderr for PTY output.

## Plain logs and dynamic viewer

Plain `hold logs` output prints the retained raw bytes. It is intentionally script-friendly and should not expose destination-specific metadata by default.

Dynamic viewer presentation may show timestamps/timezones as a view option, but those toggles are presentation-only. They must not change stored log records, filtering semantics, similarity exclusion, ordering, plain output, or destination emission.

## Destination model

The destination model should be additive:

1. Always write local raw+idx unless an explicit future policy says otherwise.
2. Mirror each captured log record to optional destinations.
3. Preserve stream identity for normal pipe captures.
4. Preserve run metadata consistently across destinations:
   - full run ID and 12-hex display ID;
   - run name;
   - profile name when present;
   - command display;
   - stream;
   - capture timestamp;
   - message bytes/text.
5. Destination failures should not corrupt the local raw log. Decide separately whether persistent destination failure is warning-only or run-fatal; do not let journald/syslog availability silently replace local retention.

## Journald notes

When adding journald, prefer structured fields rather than formatting everything into one string. Candidate fields:

```text
MESSAGE=<log line>
PRIORITY=6 for stdout, 3 for stderr
HOLD_RUN_ID=<full 64 hex run id>
HOLD_RUN_ID_SHORT=<first 12 hex>
HOLD_RUN_NAME=<generated/requested run name>
HOLD_PROFILE=<profile name or empty>
HOLD_STREAM=stdout|stderr
HOLD_COMMAND=<command display>
SYSLOG_IDENTIFIER=hold
```

For PTY/console output, set `HOLD_STREAM=stdout` or `HOLD_STREAM=pty` only if the code and docs agree on that new value. Do not claim real stderr separation for PTY bytes.

Journald timestamps are assigned by journald at receive time. Hold's sidecar index stores Hold's capture timestamp. If journald receives an explicit Hold timestamp later, document the distinction between capture time and journald receive time.

## Current code map

- `src/runtime/start.c`: normal-run pipe setup, logger process, syslog mirror, restart-supervisor capture.
- `src/core/logging.c`: raw log writer, HLOGIDX sidecar writer, and legacy JSON decoder fallback.
- `src/runtime/signal.c`: `hold logs`, follow, plain output, and viewer entry.
- `src/viewer/filter.c`: filter/viewer reads retained log text; future viewer work should use HLOGIDX for random access.
- `src/console/broker.c`: PTY broker logging and console replay.
