# On Hold — implementation invariants

[Docs index](index.md) | [Identity and surface contract](hold-on-identity.md) | [Docker familiarity contract](docker-parity-contract.md)

The command surface is specified by [hold-on-identity.md](hold-on-identity.md)
and the borrowed Docker behaviors by
[docker-parity-contract.md](docker-parity-contract.md). Mechanism references:
[launcher](launcher.md), [store](store.md), [identity](identity.md),
[console](console.md). This document records only the on-disk invariants that
every one of those relies on. The full pre-0.5 implementation spec is
history, in [archive/](archive/).

## Storage contexts

Two stores, one shape:

```text
user-local:    ~/.local/state/hold          (0700, owned by the user)
root-managed:  /var/lib/hold  (Linux)       /var/db/hold  (macOS)
```

User-local layout: call records (`<64hex>.json`, 0600) and logs
(`<64hex>.log` + `.log.idx`, 0600) directly under the base; console sockets
under `console/` (0700 dir, 0600 sockets).

Root-managed layout: `runs/`, `logs/`, `console/` are 0700 root-only —
command lines, environment, and ownership never leave them. `public/` is
0755: one world-readable redacted projection per global call.

## The public projection

A global call's public entry carries only what every user may see: the call
id, a generated name when present, an honest state hint, timestamps, and the
listening/bound ports root last observed (never outbound connections). The
invoking user and the command line are deliberately absent — user-facing
views render them as `hidden`. An entry whose private record is gone is an
orphan; `sudo hold purge` sweeps it so no view can resurrect it.

## Records

One JSON record per call, Docker-inspect-style keys, written atomically
(temp file + rename) and never partially visible. The record is the recipe:
redial replays its argv, environment, and session mode. `Saved: true`
protects it from sweeps. IDs are 64-hex, derived from creation material;
names are generated `adjective_noun`, unique per store, user-renameable
(renaming saves).

## Logs

Raw captured bytes plus the `HLOGIDX` sidecar (offsets, lengths,
timestamps, stream metadata). The pair is a documented, stable format:
anything can read a call's log without Hold's help. Plain output never
requires the sidecar; the viewer's timestamps and source filters come only
from it.

## Safety invariants

- Every managed call gets its own process group and session.
- Signals are delivered to the group only after the recorded identity
  (start time, exe identity) revalidates — a recycled PID is never signaled.
- Records are trusted only after validation; corrupt records are reported
  (and swept by purge), never silently obeyed.
- stdout carries machine data; human notes go to stderr; `--quiet`
  silences them.
- Hold never elevates on a user's behalf, with one deliberate exception:
  `hold purge --system` re-executes itself through `sudo` (which does the
  prompting) at a single commented dispatch site.

## Non-goals

No daemon or supervisor. No root-log visibility for normal users. No
global run-id uniqueness across users. No scanning of other users'
private stores by anyone but root. Tests never touch the real system
store: `make test` compiles with `HOLD_TESTING` and uses
`HOLD_TEST_SYSTEM_STATE_DIR`.
