# Security and privilege boundaries

Sigmund's root-aware behavior is built around two rules: normal state stays user-local unless root/system authority is requested, and privileged actions are re-validated after crossing sudo. There is no daemon and no shell command payload.

The primary functions are `detect_invocation`, `elevate_with_sudo_canonical`, `elevate_with_sudo_targets`, `elevate_start_token`, `verify_system_alias_cap`, `cmd_elevated_capability_action`, and `cmd_grant_revoke_action`.

## Invocation context

`detect_invocation` records whether the effective UID is root, whether `--system` was requested, whether the internal `--elevated` flag is present, and whether sudo provenance is available from `SUDO_UID`, `SUDO_GID`, and `SUDO_USER`.

When root was reached through sudo, Sigmund resolves the invoking user's home directory from the user database. In test builds, `SIGMUND_TEST_INVOKING_HOME` can override that path. Root-managed records include `invoked_by_uid`, `invoked_by_gid`, `invoked_by_user`, and `invoked_via_sudo` in the private record only.

`--elevated` is internal. If it appears without root authority, `main` exits with an internal error.

## Sudo self-elevation

```mermaid
sequenceDiagram
    participant User as Non-root Sigmund
    participant Resolver as Resolver
    participant Sudo as sudo
    participant Root as Root Sigmund
    participant Store as System store

    User->>Resolver: resolve target
    Resolver-->>User: system target needs elevation
    User->>User: build canonical argv
    User->>Sudo: exec sudo -- abs_sigmund --system --elevated ...
    Sudo->>Root: run with root authority
    Root->>Store: load private record or profile
    Root->>Root: verify alias/hash and run label
    Root->>Root: validate state
    Root-->>User: exit status through waitpid
```

Before invoking sudo, `resolve_self_executable_path` finds the current Sigmund executable through `/proc/self/exe` on Linux, `_NSGetExecutablePath` plus `realpath` on macOS, or `realpath(argv[0])` when `argv[0]` includes a slash. If the path cannot be determined, elevation fails before running sudo.

`elevate_with_sudo_canonical` forks and execs `sudo` with an argv array. It does not use a shell. It waits for sudo/root-Sigmund and returns that exit status. The child inherits stdin, stdout, and stderr, so sudo prompts, diagnostics, streamed logs, and Ctrl-C behavior remain attached to the user's terminal.

## Capability argv

```mermaid
flowchart TD
    Public["Public alias/hash"] --> Build["Build capability argv"]
    Build --> Sudo["sudo boundary"]
    Sudo --> Verify["verify_system_alias_cap"]
    Verify -->|alias still maps to hash| Selector["Check run selector"]
    Verify -->|mismatch| Deny["deny"]
    Selector -->|00000000 and start| Profile["Load profile and start"]
    Selector -->|ffffffff and all action| Matches["Collect current alias matches"]
    Selector -->|run ID| Label["ensure_run_recorded_under_alias"]
    Label --> Action["Validate then act"]
    Matches --> Action
```

Root-managed alias capabilities use three argv fields:

```text
<runid_sel> <alias> <hash>
```

`00000000` is valid only for `start`. `ffffffff` is valid only for approved `--all` actions. Concrete run IDs must still be recorded under the supplied alias. `cmd_elevated_capability_action` verifies the alias/hash pair first, then checks the selector rules, then loads records or profiles from the private system store.

This second verification is essential because public data was read before the sudo boundary. The alias could have changed between non-root resolution and root execution.

## Managed sudoers

```mermaid
sequenceDiagram
    participant Admin as Root admin
    participant Sigmund as Sigmund
    participant Store as System profiles
    participant File as sudoers.d file
    participant Visudo as visudo

    Admin->>Sigmund: grant alias user actions
    Sigmund->>Sigmund: validate root authority
    Sigmund->>Sigmund: validate Sigmund executable
    Sigmund->>Store: resolve alias to profile hash
    Sigmund->>File: write temp file mode 0440
    Sigmund->>Visudo: validate candidate
    Visudo-->>Sigmund: ok
    Sigmund->>File: rename into place
```

`grant` and `revoke` require root authority. `validate_sigmund_self_for_sudoers` refuses to manage grants unless the resolved Sigmund executable is a regular root-owned file with group/world writes disabled and no whitespace in the path.

Managed files are written under `/etc/sudoers.d` in production, or `SIGMUND_TEST_SUDOERS_DIR` in test builds. `write_sudoers_template_file` writes a temp candidate with mode `0440`, validates it with `visudo -cf`, and renames it into place. The sudoers command grants only canonical root Sigmund invocations with `--system --elevated`, selected verbs, one alias, one profile hash, and one 8-hex run selector slot.

## Why this design works

The sudo boundary is narrow and argv-based. Sigmund never asks sudo to run an interpreted shell string, and root Sigmund never trusts the public pre-sudo selection by itself. That keeps privilege crossing compatible with the validate-before-signal model: root authority is used only after the target has been re-bound to private state and re-validated.

The single-binary constraint also explains managed sudoers. There is no daemon authorization API to query, so the durable authorization object is a sudoers file whose command pattern points back to the same Sigmund executable and a fixed profile hash.

## Source anchors

Primary functions: `detect_invocation`, `resolve_self_executable_path`, `elevate_with_sudo_canonical`, `elevate_with_sudo_parsed`, `elevate_with_sudo_targets`, `elevate_start_token`, `verify_system_alias_cap`, `ensure_run_recorded_under_alias`, `cmd_elevated_capability_action`, `validate_sigmund_self_for_sudoers`, `build_sudoers_line`, `write_sudoers_template_file`, `unlink_sudoers_template_file`, and `cmd_grant_revoke_action`.
