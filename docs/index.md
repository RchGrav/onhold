# On Hold documentation index

[Repository README](../README.md) | [Outer onboarding loop](quickstart.md) | [Technical reference loop](#technical-reference-loop) | [Current spec](SPEC.md) | [0.4.0 UX spec](HOLD_0_4_UX_SPEC.md) | [0.4.0 alignment](0.4.0-alignment.md)

This is the top-level guide to how On Hold works. Start with the [quickstart](quickstart.md): it walks from the first command to deterministic targeting, profiles, and scoped root delegation with simple diagrams, and links into the deeper subsystem pages as each concept appears.

On Hold is a daemonless process launcher and recorder. It starts a command in a new session, writes a durable run record and log path, and later uses that record to inspect, tail, stop, kill, attach, or prune the tracked process group.

> 0.4.0 redesign note: most reference pages describe the current 0.3.x/legacy command contract. The intended `hold` redesign and current branch gap matrix live in [Hold 0.4 UX and CLI specification](HOLD_0_4_UX_SPEC.md) and [0.4.0 branch alignment](0.4.0-alignment.md).
> The 2026-06-28 recovery/design artifacts are preserved as review material:
> [direction and decisions](0.4.0-direction-2026-06-28.md) and
> [security review](security-review-2026-06-28.md). They are not user-facing
> command references; use them to choose implementation work and reconcile
> release blockers.

The philosophy is simple:

- Make the easy path easy: `hold <cmd...>` gives users a run ID, log, and safe cleanup path.
- Make automatic choices predictable: invocation shape decides user-local versus system-managed behavior.
- Make precision available: `user:<target>`, `system:<target>`, profiles, and grants let users say exactly what they mean.
- Validate before signal: if On Hold cannot prove a recorded process group is still the intended run, it refuses instead of guessing.

## Navigation Model

The documentation has two layers:

- Outer loop: [Quickstart](quickstart.md), a clean onboarding walkthrough that can be read start to finish.
- Inner layer: [Technical reference loop](#technical-reference-loop), deep-dive pages for internals, edge cases, data flow, and implementation details.
- Bridge links: each quickstart step can detour into a matching deep dive, and each deep dive resumes at the next walkthrough step so the reader does not lose forward motion.

## Start Here

| If you want to... | Start with | Then go deeper |
| --- | --- | --- |
| Install On Hold | [Installing On Hold](install.md) | [Using On Hold in CI](ci.md) |
| Learn the normal workflow | [Quickstart](quickstart.md) | [Launcher](launcher.md), [Store](store.md) |
| Use On Hold in CI | [Using On Hold in CI](ci.md) | [CLI contract](cli-contract.md), [Identity](identity.md) |
| Understand target choices and collisions | [Quickstart targeting](quickstart.md#step-4-make-targeting-deterministic) | [Target resolution](target-resolution.md) |
| Create reusable names | [Quickstart profiles](quickstart.md#step-5-create-a-profile) | [Profiles and storage aliases](profiles-and-aliases.md) |
| Delegate one root-managed tool safely | [Quickstart delegation](quickstart.md#step-6-delegate-one-root-managed-tool) | [Security](security.md) |

## Core Flow

```mermaid
flowchart LR
    Start["hold <cmd...>"] --> RunId["stdout: run ID"]
    Start --> Log["stderr: human status"]
    Start --> Record["private run record"]
    Record --> Later["tail, dump, stop, kill, console, prune"]
    Later --> Resolve["resolve target"]
    Resolve --> Validate["validate recorded identity"]
    Validate --> Act["act or refuse"]

    classDef user fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef state fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef safety fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    classDef action fill:#dcfce7,stroke:#15803d,color:#14532d
    class Start,RunId,Log user
    class Record state
    class Later,Resolve action
    class Validate,Act safety
```

That is the promise On Hold makes to users: a simple launch command turns into a durable handle, and later management commands use that handle carefully instead of relying on a hand-copied PID.

## Architecture

```mermaid
flowchart TD
    User["CLI invocation"] --> Parse["Invocation parser"]
    Parse --> Start["Start command"]
    Parse --> Manage["Manage existing run"]
    Parse --> Access["Grant or revoke access"]

    Start --> Launcher["setsid and exec"]
    Launcher --> Log["Run log"]
    Launcher --> Record["Private record"]
    Launcher --> Public["Root public index"]
    Launcher --> Console["Optional console"]

    Manage --> Resolver["Target resolver"]
    Resolver --> UserStore["User store"]
    Resolver --> Public
    Resolver --> Sudo["sudo when system target needs root"]
    Sudo --> RootAction["Root On Hold"]
    RootAction --> RootStore["System store"]

    Manage --> Validator["Identity validator"]
    RootAction --> Validator
    Validator --> Signal["Signal process group"]
    Validator --> Refuse["Refuse stale or unknown"]

    Access --> Sudoers["Managed sudoers rule"]
    Sudoers --> Sudo

    classDef entry fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef launch fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef store fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef safety fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    classDef privilege fill:#ede9fe,stroke:#6d28d9,color:#3b0764

    class User,Parse entry
    class Start,Launcher,Console launch
    class Log,Record,Public,UserStore,RootStore store
    class Manage,Resolver,Validator,Signal,Refuse safety
    class Access,Sudo,Sudoers,RootAction privilege
```

## Technical Reference Loop

```mermaid
flowchart TD
    Quick["Outer loop quickstart"] --> Index["Docs index"]
    Index --> Install["Install"]
    Install --> Launcher["Launcher"]
    Launcher --> Store["Store"]
    Store --> Identity["Identity"]
    Identity --> Target["Target resolution"]
    Target --> Profiles["Profiles and storage aliases"]
    Profiles --> Security["Security"]
    Security --> Console["Console"]
    Console --> CLI["CLI contract"]
    CLI --> CI["Using On Hold in CI"]
    CI --> Quick
    CI --> Index
    Target --> Quick
    Security --> Quick

    classDef user fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef launch fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef state fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef safety fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    classDef privilege fill:#ede9fe,stroke:#6d28d9,color:#3b0764
    class Quick,Index,Install,CLI,CI user
    class Launcher,Console launch
    class Store,Profiles state
    class Identity,Target safety
    class Security privilege
```

Every subsystem page links back here, names the quickstart step it explains, resumes at the next walkthrough step, and points forward to the next technical concept, so readers can browse the inner layer as a reference system without losing the outer-loop return path.

## Inner Layer Pages

1. [Quickstart](quickstart.md): user workflow, automatic choices, deterministic targeting, profiles, and scoped root delegation.
2. [Installing On Hold](install.md): one-line install, root/user install mode, platform detection, checksums, and script handoff.
3. [Launcher](launcher.md): starts, fork/setsid/exec, logs, records, and launch rollback.
4. [Store](store.md): user-local and system-managed state, public redaction, atomic writes, and pruning.
5. [Identity and validation](identity.md): boot ID, starttime, executable identity, session membership, run states, and signal refusal.
6. [Target resolution](target-resolution.md): ID, prefix, profile, `user:`, `system:`, ambiguity, and action target expansion.
7. [Profiles and storage aliases](profiles-and-aliases.md): reusable launch recipes, SHA-256 fingerprints, profile starts, and `--multi`.
8. [Security and privilege boundaries](security.md): `--system`, sudo self-elevation, capability argv, and managed sudoers.
9. [Console](console.md): PTY console starts, private sockets, native attach, terminal sizing, and log teeing.
10. [CLI contract](cli-contract.md): parser behavior, stdout/stderr, flags, no-op behavior, and exit codes.
11. [Using On Hold in CI](ci.md): copyable CI patterns for start, readiness, logs, teardown, exit codes, and multiple helpers.

## Branch by Question

| If you want to understand... | Read |
| --- | --- |
| How to install On Hold and hand its path to scripts | [Installing On Hold](install.md) |
| How a command keeps running after the CI step or shell exits | [Quickstart](quickstart.md), then [Launcher](launcher.md) |
| Where run IDs, logs, profiles, and public root hints live | [Store](store.md) |
| Why `stop` is safer than `kill $PID` | [Identity and validation](identity.md) |
| How IDs, profiles, `user:`, and `system:` choose a target | [Target resolution](target-resolution.md) |
| How to reuse a recorded command as a profile | [Profiles and storage aliases](profiles-and-aliases.md) |
| How to let another user manage one root-run tool | [Quickstart](quickstart.md#step-6-delegate-one-root-managed-tool), then [Security](security.md) |
| How to script On Hold in CI | [Using On Hold in CI](ci.md), then [CLI contract](cli-contract.md) |

## Reference

- [Current implementation specification](SPEC.md)
- [Hold 0.4 UX and CLI implementation plan](HOLD_0_4_UX_SPEC.md)
- [0.4.0 branch alignment and follow-up matrix](0.4.0-alignment.md)
- [0.4.0 direction and decisions, 2026-06-28](0.4.0-direction-2026-06-28.md)
- [0.4.0 security review, 2026-06-28](security-review-2026-06-28.md)
- [Documentation plan and review notes](PLAN.md)
- [Repository README](../README.md)

## Future Work

- [Future-work notes](future/README.md): proposals that are not current behavior.

## Implementation map

For maintainers, the source is organized by layer under `src/` (with public APIs in
`include/hold/`): `core/` (primitives, JSON, SHA-256), `platform/` (OS facts),
`store/` (durable state), `console/` (attachable PTY), `access/` (privilege and
sudoers), `runtime/` (product behavior), and `cli.c` + `main.c`. The main source
anchors for this overview are `main` (`src/main.c`), `hold_perform_start`
(`src/runtime/start.c`), `hold_write_record_atomic` /
`hold_write_public_index_atomic` (`src/store/record.c`),
`hold_resolve_action_token` (`src/runtime/resolve.c`), `hold_eval_state`
(`src/runtime/state.c`), `hold_do_signal_action` (`src/runtime/signal.c`),
`hold_elevate_with_sudo_canonical` (`src/access/elevate.c`), and
`hold_cmd_elevated_capability_action` (`src/runtime/commands.c`).
