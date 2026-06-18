# Sigmund documentation index

[Repository README](../README.md) | [Specification](SPEC.md)

This is the top-level developer documentation for `src/sigmund.c`. Start here, branch into the subsystem that matches the code you are changing, then use each page's related links to move sideways through the logic. Every subsystem page links back here and forward to the next major concept, so the documentation forms a loop instead of a dead end.

Sigmund is a daemonless process launcher and recorder. It starts a command in a new session, writes a durable run record and log path, and later uses that record to inspect, tail, stop, kill, or prune the tracked process group. The design is intentionally "more than nohup, less than systemd": there is no resident supervisor, but Sigmund records enough identity to validate a target before it sends a signal.

The two constraints that shape the code are:

- Validate before signal: an action must prove that the recorded PID/process group still matches the intended run, or refuse the action.
- Daemonless single binary: all state must be recoverable from files and the current process table because no daemon is refreshing state in the background.

## Architecture

```mermaid
flowchart TD
    User["CLI invocation"] --> Main["main"]
    Main --> Parse["Parse raw or owned command"]
    Parse --> Start["Start path"]
    Parse --> Action["Action path"]
    Parse --> Access["Grant or revoke"]

    Start --> Launcher["fork, setsid, exec"]
    Launcher --> Log["Run log"]
    Launcher --> Record["Private run record"]
    Launcher --> Public["Root public index"]
    Launcher --> Console["Optional console broker"]

    Action --> Resolver["Target resolver"]
    Resolver --> UserStore["User store"]
    Resolver --> Public
    Resolver --> Sudo["sudo self-elevation"]
    Sudo --> RootAction["Root Sigmund"]
    RootAction --> RootStore["System store"]

    Action --> Validator["State validator"]
    RootAction --> Validator
    Validator --> Signal["TERM or KILL group"]
    Validator --> Refuse["Refuse stale or unknown"]

    Access --> Sudoers["Managed sudoers file"]
    Sudoers --> Sudo

    UserStore --> Record
    RootStore --> Record
    RootStore --> Public

    classDef entry fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef launch fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef store fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef safety fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    classDef privilege fill:#ede9fe,stroke:#6d28d9,color:#3b0764

    class User,Main,Parse entry
    class Start,Launcher,Console launch
    class Log,Record,Public,UserStore,RootStore store
    class Action,Resolver,Validator,Signal,Refuse safety
    class Access,Sudo,Sudoers,RootAction privilege
```

`main` is the dispatch center. It distinguishes raw command starts from Sigmund-owned commands, builds the invocation context, initializes the relevant store, and routes to start, list, action, alias, or grant/revoke handlers. Starts flow through `perform_start`. Actions flow through target resolution and then, for signal-bearing commands, through `do_signal_action`. Root-managed public records are deliberately redacted discovery hints; private records remain authoritative.

## Reading Paths

```mermaid
flowchart TD
    Index["Docs index"] --> Launcher["Launcher"]
    Index --> Store["Store"]
    Index --> Identity["Identity"]
    Index --> Target["Target resolution"]
    Index --> Profiles["Profiles and aliases"]
    Index --> Security["Security"]
    Index --> Console["Console"]
    Index --> CLI["CLI contract"]
    CLI --> CI["Using Sigmund in CI"]

    Launcher --> Store
    Store --> Identity
    Identity --> Target
    Target --> Security
    Target --> Profiles
    Profiles --> Launcher
    Security --> Target
    Console --> Launcher
    Console --> Target
    CI --> CLI
    CI --> Index

    classDef entry fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef launch fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef state fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef safety fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    classDef privilege fill:#ede9fe,stroke:#6d28d9,color:#3b0764
    classDef script fill:#ccfbf1,stroke:#0f766e,color:#134e4a

    class Index entry
    class Launcher,Console launch
    class Store,Profiles state
    class Identity,Target safety
    class Security privilege
    class CLI,CI script
```

## Main Loop

1. [Launcher](launcher.md): starts, fork/setsid/exec, logs, records, and launch rollback.
2. [Store](store.md): user-local and system-managed state, record shape, public redaction, atomic writes, and pruning.
3. [Identity and validation](identity.md): boot ID, starttime, executable identity, session membership, run states, and signal refusal.
4. [Target resolution](target-resolution.md): ID, prefix, alias, `user:`, `system:`, ambiguity, and action target expansion.
5. [Profiles and aliases](profiles-and-aliases.md): reusable launch recipes, SHA-256 fingerprints, alias starts, and `--multi`.
6. [Security and privilege boundaries](security.md): `--system`, sudo self-elevation, capability argv, and managed sudoers.
7. [Console](console.md): PTY console starts, private sockets, `socat` attach, and log teeing.
8. [CLI contract](cli-contract.md): parser behavior, stdout/stderr, flags, no-op behavior, and exit codes.
9. [Using Sigmund in CI](ci.md): copyable CI patterns for start, readiness, logs, teardown, exit codes, and multiple helpers.

## Branch by Task

| If you are changing... | Start with | Then read |
| --- | --- | --- |
| Process launch, logs, or tailing | [Launcher](launcher.md) | [Store](store.md), [Identity](identity.md), [CLI contract](cli-contract.md) |
| JSON records, public index, pruning | [Store](store.md) | [Identity](identity.md), [Target resolution](target-resolution.md) |
| Signal safety or process-state checks | [Identity](identity.md) | [Launcher](launcher.md), [Target resolution](target-resolution.md) |
| IDs, aliases, `user:`/`system:` lookup | [Target resolution](target-resolution.md) | [Profiles and aliases](profiles-and-aliases.md), [Security](security.md) |
| Alias start behavior or profile hashes | [Profiles and aliases](profiles-and-aliases.md) | [Store](store.md), [Security](security.md) |
| Sudo, grants, or root-managed actions | [Security](security.md) | [Target resolution](target-resolution.md), [Profiles and aliases](profiles-and-aliases.md) |
| Interactive console support | [Console](console.md) | [Launcher](launcher.md), [Target resolution](target-resolution.md), [Security](security.md) |
| Scripting or CI behavior | [CLI contract](cli-contract.md) | [Using Sigmund in CI](ci.md), [Launcher](launcher.md) |

## Reference

- [Current implementation specification](SPEC.md)
- [Documentation plan and review notes](PLAN.md)
- [Repository README](../README.md)

## Source Anchors

The main source anchors for this overview are `main`, `perform_start`, `write_record_atomic`, `write_public_index_atomic`, `resolve_action_token`, `eval_state`, `do_signal_action`, `elevate_with_sudo_canonical`, and `cmd_elevated_capability_action` in `src/sigmund.c`.
