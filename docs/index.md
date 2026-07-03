# Hold documentation

Hold is a daemonless host-process manager. `nohup` hangs up gracefully; Hold
keeps the line open. It places ordinary commands as durable **calls** —
recorded, logged, listable, reattachable — and later ends, redials, saves, or
purges them.

This index points to the current documentation. Historical planning, review
notes, and documentation for removed subsystems live in
[`archive/`](archive/) and are not the command contract.

## The contract

- [Hold On — identity and cut plan](hold-on-identity.md): what the tool is,
  the full verb surface, and what deliberately does not exist.
- [Docker familiarity contract](docker-parity-contract.md): the flags and
  table appearance borrowed from Docker, exactly.

## Start here

| Need | Read |
| --- | --- |
| Install Hold | [Installing Hold](install.md) |
| Learn the workflow | [Repository README](../README.md) |
| Understand security posture | [Security review notes](archive/security-review-2026-06-28.md) |

## Technical references

- [Launcher](launcher.md): fork/session/process-group launch model.
- [Store](store.md): user-local and root-managed state layout.
- [Identity](identity.md): validation before signaling.
- [Console](console.md): PTY-backed calls, attach/detach, adoption.
- [Implementation invariants](SPEC.md): the on-disk contract everything relies on.
- [CI parity](CI-PARITY.md): keeping local checks equal to CI.

## Future work

- [Future notes](future/README.md)
- [Dynamic log viewer design](future/viewer-fixes-before-0.5.md)
- [Additional log destinations](future/log-destinations.md)

## Source map

Public headers live under `include/hold/`; implementation is layered under
`src/`:

- `core/`: primitives, JSON, SHA-256, logging/index helpers
- `platform/`: OS facts, paths, user lookup, process facts
- `store/`: durable call records and layout
- `console/`: attachable PTY handling — the held line
- `access/`: invocation context (euid, invoking user)
- `runtime/`: product commands and lifecycle behavior
- `viewer/`: the built-in full-screen log viewer
