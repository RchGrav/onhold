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

One JSON record per call in a single native snake_case schema, written
atomically (temp file + rename) and never partially visible. Docker parity
lives at the CLI surface (flags and tables), not in storage; the parallel
Docker-shaped keys written before 0.7 are neither written nor read (one
legacy shim: the old `Saved` spelling of the saved flag still loads).
The record is the recipe: redial replays its argv, environment, and session
mode (`mode{}` bits). `saved: true` protects it from sweeps. IDs are 64-hex,
derived from creation material; names are generated `adjective_noun`,
unique per store, user-renameable (renaming saves).

## Logs

Raw captured bytes plus the `HLOGIDX` sidecar (offsets, lengths,
timestamps, stream metadata). The pair is a documented, stable format:
anything can read a call's log without Hold's help. Plain output never
requires the sidecar; the viewer's timestamps and source filters come only
from it.

Writers emit sidecar v2 (24-byte entries with a per-line CRC32 and a
64-bit ns delta); v1 sidecars stay readable and keep appending v1. The
sidecar self-heals on read: a corrupt index is realigned against the log
text by its per-line CRC anchors, a missing one is rebuilt with synthetic
50 ms timing from the file's birth time, and either path rewrites the
index as v2. Reconstructed timing is always labeled ("timing
reconstructed"), never presented as recorded; the raw log stays the sole
source of truth.

## Safety invariants

- Every managed call gets its own process group and session.
- Signals are delivered to the group only after revalidation: the boot id
  must match, and a live leader's recorded identity (start time, exe
  identity) must compare equal. When the leader is gone, the group is
  matched by its recorded pgid+sid within the same boot — a deliberate
  best effort, since no atomic verify-and-signal exists for a process
  group. A recycled PID is never knowingly signaled.
- Records are trusted only after validation; corrupt records are reported
  (and swept by purge), never silently obeyed.
- Purge removes only store-resident artifacts derived from the call id;
  paths stored in records are never followed for deletion.
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
