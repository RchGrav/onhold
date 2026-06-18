# Store

[Docs index](index.md) | [Previous: Launcher](launcher.md) | [Next: Identity](identity.md) | Related: [Profiles and aliases](profiles-and-aliases.md), [Security](security.md)

Sigmund has two stores: a user-local store for normal starts and a system-managed store for root, sudo, and `--system` starts. The store layer is defined by `struct store_paths` and initialized by `init_user_store_from_home`, `ensure_user_store_for_current_user`, `init_system_store`, and `ensure_system_store`.

The store is the replacement for a daemon's memory. Every future command has to recover intent from files, then validate live process state separately.

## Layouts

User-local state lives under:

```text
~/.local/state/sigmund
```

The user store uses the base directory for run records, logs, `profiles.json`, `aliases.json`, and a `console/` subdirectory. `ensure_user_store_for_current_user` creates the base and console directory as `0700`.

System-managed state lives under:

```text
Linux: /var/lib/sigmund
macOS: /var/db/sigmund
```

In test builds, `SIGMUND_TEST_SYSTEM_STATE_DIR` can override that path. The production code does not take an arbitrary environment override for the system store.

```text
<system>/
  runs/
  logs/
  console/
  profiles.json
  public/
    <id>.json
    aliases.json
```

The system base and public directory are `0755`; private root records, logs, console sockets, and profiles are under `0700` directories or `0600` files. Public index files and public aliases are `0644` discovery metadata.

## Persisted records

```mermaid
flowchart LR
    Run["Private run record<br/>id, run_id, pid, pgid, sid<br/>argv, cmdline_display<br/>log_path, boot_id<br/>proc_starttime_ticks<br/>exe_dev, exe_ino<br/>state and result fields"]
    Provenance["Root provenance<br/>invoked_by_uid<br/>invoked_by_gid<br/>invoked_by_user<br/>invoked_via_sudo"]
    Public["Public index<br/>id<br/>root_managed<br/>requires_elevation<br/>alias<br/>state_hint<br/>started_at"]
    Profile["Root-private profile<br/>hash<br/>bin<br/>args"]
    Alias["Alias entry<br/>name<br/>hash or recipe"]

    Run --> Provenance
    Run -->|"redacted discovery"| Public
    Alias -->|"system alias"| Profile
    Alias -->|"user alias"| Run

    classDef authority fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef public fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef profile fill:#ede9fe,stroke:#6d28d9,color:#3b0764
    classDef user fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    class Run,Provenance authority
    class Public public
    class Profile profile
    class Alias user
```

`write_record_atomic` writes one private JSON record per run. Required identity fields include `id`, `run_id`, `pid`, `pgid`, `sid`, `start_unix_ns`, `argv`, `uid`, `gid`, `proc_starttime_ticks`, `exe_dev`, and `exe_ino`. Optional fields are written only when the corresponding `has_*` flag is set, including `alias`, `console_sock`, `started_at`, `ended_at`, `state`, `exit_code`, `term_signal`, `launch_error`, `log_path`, `boot_id`, and root invocation provenance.

`write_public_index_atomic` writes only a redacted system discovery record: `id`, `root_managed`, `requires_elevation`, optional `alias`, `state_hint`, and `started_at`. It does not write argv, command display, log paths, console socket paths, PID/PGID/SID, boot ID, executable identity, sudo provenance, environment, or profile hashes.

## Atomic writes

```mermaid
flowchart TD
    Start["Prepare final path"] --> Temp["Open same-dir temp file"]
    Temp --> Write["Write JSON"]
    Write --> Flush["fflush and fsync file"]
    Flush --> Rename["rename temp to final"]
    Rename --> DirSync["fsync containing dir if possible"]
    DirSync --> Done["Record visible"]
    Temp --> Fail["Failure"]
    Write --> Fail
    Flush --> Fail
    Rename --> Fail
    Fail --> Cleanup["unlink temp"]

    classDef write fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef durable fill:#fef3c7,stroke:#b45309,color:#78350f
    classDef fail fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    class Start,Temp,Write,Flush,Rename write
    class DirSync,Done durable
    class Fail,Cleanup fail
```

Private run records are written as `.<id>.tmp` and renamed to `<id>.json`. Public index records use the same pattern in the public directory. Profile and alias files are also written through temp files and atomic rename, then the containing directory is fsynced where the code can do so.

This pattern matters because Sigmund has no daemon to reconstruct partially written state. If a process is recorded, future actions must be able to trust that the record is syntactically complete enough to load and evaluate.

## Public and private authority

Private records are authoritative. Public root indexes exist so normal users can discover that a root-managed run exists and decide whether an action should self-elevate. Normal list output reads both the user-local private store and the system public index, while root/system list reads private system records.

Public root rows use `state_hint` set to `unknown` when written. Since no daemon refreshes state after natural exit, the public file must not claim authoritative liveness. Root actions re-load the private record and evaluate live state.

## Pruning

Pruning is storage cleanup plus safety. `prune_one_run` removes records only when the evaluated state is exited, failed, stale, or otherwise allowed by the caller. Running valid runs are not pruned. Root-managed public index files and console sockets are removed with the corresponding private record when possible, and `cmd_prune_store_all` also clears orphaned logs and console sockets.

## Why this design works

The store makes a daemonless process manager possible: every command can rediscover known runs from disk. The split between public discovery and private authority preserves root confidentiality while still allowing normal users to target root-managed runs through a controlled sudo boundary. Atomic writes prevent half-records from becoming false authority for later validate-before-signal decisions.

## Source anchors

Primary functions and structs: `struct store_paths`, `struct record`, `struct public_index`, `init_user_store_from_home`, `ensure_user_store_for_current_user`, `init_system_store`, `ensure_system_store`, `write_record_atomic`, `write_public_index_atomic`, `load_record`, `load_public_index`, `prune_one_run`, and `cmd_prune_store_all`.

## Continue

[Back to docs index](index.md) | [Top](#store) | [Next: Identity](identity.md) | Branch to: [Profiles and aliases](profiles-and-aliases.md), [Security](security.md)
