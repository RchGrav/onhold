# Hold documentation

Hold is a daemonless host-process manager. It starts ordinary commands, records a
durable run object, captures stdout/stderr, and later uses that record to list,
inspect, log, stop, kill, restart from profiles, or clean up the process group.

This index points to the current documentation. Historical planning and review
notes live in [`archive/`](archive/) and are not the command contract.

## Start here

| Need | Read |
| --- | --- |
| Install Hold | [Installing Hold](install.md) |
| Learn the normal workflow | [Repository README](../README.md), then [Quickstart](quickstart.md) |
| Use Hold in CI | [Using Hold in CI](ci.md) |
| Understand supported commands/exit codes | [CLI contract](cli-contract.md) |
| Understand profiles | [Profiles and aliases](profiles-and-aliases.md) |
| Understand security/root-managed behavior | [Security](security.md) |

## Current specs and release contracts

- [Implementation spec](SPEC.md)
- [Hold 0.4 UX/CLI spec](HOLD_0_4_UX_SPEC.md)
- [0.4 object format repair contract](0.4-object-format-repair.md)
- [0.4 release cut](0.4-release-cut.md)
- [0.4 repair ledger](0.4-repair-ledger.md)

## Technical references

- [Launcher](launcher.md): fork/session/process-group launch model.
- [Store](store.md): user-local and root-managed state layout.
- [Identity](identity.md): validation before signaling.
- [Target resolution](target-resolution.md): IDs, names, profiles, ambiguity, and scopes.
- [Console](console.md): PTY-backed interactive runs.
- [CLI contract](cli-contract.md): parser behavior, stdout/stderr rules, and exits.
- [Using Hold in CI](ci.md): copyable CI patterns.

## Future work

- [Future notes](future/README.md)
- [Dynamic log viewer polish before 0.5](future/viewer-fixes-before-0.5.md)
- [Additional log destinations](future/log-destinations.md)
- [Parameterized profiles](future/parameterized-profiles.md)

## Source map

Public headers live under `include/hold/`; implementation is layered under
`src/`:

- `core/`: primitives, JSON, SHA-256, logging/index helpers
- `platform/`: OS facts, paths, user lookup, process facts
- `store/`: durable records, profiles, aliases, public/private projections
- `console/`: attachable PTY handling
- `access/`: privilege crossing and sudoers support
- `runtime/`: product commands and lifecycle behavior
- `cli.c` / `main.c`: parser and command dispatch
