# Sigmund developer documentation plan

Phase 1 status: ready for review.

This plan is based on a full read of `src/sigmund.c`. Phase 2 should write the actual developer documentation under `docs/` without changing `README.md`. The code remains the source of truth; `docs/SPEC.md` is useful context, but every behavioral claim in the new design docs must be verified against `src/sigmund.c`.

## Proposed file tree

```text
docs/
  PLAN.md
  design.md
  launcher.md
  store.md
  identity.md
  target-resolution.md
  profiles-and-aliases.md
  security.md
  console.md
  cli-contract.md
  ci.md
  SPEC.md
```

`README.md` is intentionally excluded. It already exists at the repository root and should remain unchanged.

I do not plan to create `docs/roadmap.md` in Phase 2 unless a planned behavior must be mentioned. If any source-adjacent note describes behavior that is not implemented in `src/sigmund.c`, it should either be omitted from the current-design docs or quarantined in `docs/roadmap.md` with a clear `planned - not yet implemented` label.

## Source map

Phase 2 should anchor the docs to these source regions:

- Public data model: `struct record`, `struct store_paths`, `struct invocation`, `struct resolved_target`, and `struct public_index` near the top of `src/sigmund.c`.
- Store setup and invocation provenance: `init_user_store_from_home`, `ensure_user_store_for_current_user`, `init_system_store`, `ensure_system_store`, and `detect_invocation`.
- Profile fingerprinting and persistence: `profile_hash_for_argv`, `write_profile_atomic`, `load_profiles`, `write_profiles_atomic`, `alias_lookup_hash`, `alias_upsert_hash`, `alias_lookup_recipe`, and `alias_upsert_recipe`.
- Record and public-index writes: `write_record_atomic` and `write_public_index_atomic`.
- Process launch: `perform_start`, `perform_explicit_start`, `read_exec_handshake`, and `rollback_spawned_group`.
- Console mode: `make_console_listener`, `open_console_pty`, `run_console_broker`, `run_socat_console`, `attach_console_record`, and `cmd_console_action`.
- Process identity and state validation: `get_boot_id`, `read_process_ids_state`, `group_session_liveness`, `count_session_escapees`, `read_proc_stat_tokens`, `read_proc_exe`, `eval_state`, `do_signal_action`, and `do_print_signal_command`.
- Target resolution and actions: `parse_id_token`, `resolve_target`, `resolve_action_token`, `append_private_alias_targets`, `append_public_alias_elevation_target`, `cmd_signal_action`, `cmd_tail_action`, `cmd_dump_action`, and `cmd_prune_action`.
- Sudo crossing and capability checks: `resolve_self_executable_path`, `elevate_with_sudo_canonical`, `elevate_with_sudo_parsed`, `elevate_with_sudo_targets`, `elevate_start_token`, `verify_system_alias_cap`, `ensure_run_recorded_under_alias`, and `cmd_elevated_capability_action`.
- Sudoers grant management: `cmd_grant_revoke_action`, `validate_sigmund_self_for_sudoers`, `build_sudoers_line`, `write_sudoers_template_file`, and `unlink_sudoers_template_file`.
- CLI contract: `usage`, the `help_*` functions, `is_sigmund_owned_command`, `command_accepts_target_tokens`, and `main`.

## Planned documentation pages

### `docs/design.md`

Index and landing page for the design documentation. It should state Sigmund's operating model in contributor terms: a daemonless, single-binary launcher that records enough durable identity to validate before signaling, sitting between raw `nohup`/`setsid` and a service manager. It should include a top-level architecture diagram that shows the CLI parser, launch path, store layer, public root index, target resolver, sudo boundary, and process-safety evaluator. It should also provide a table of contents with relative links to each subsystem page.

Planned diagrams: one top-level Mermaid flowchart using block-like subgraphs, not `block-beta`.

### `docs/launcher.md`

Explains raw starts, explicit `start`, alias starts, `--tail`/`-f`, `--console`, fork/setsid/exec behavior, log redirection, stdout/stderr conventions, record creation, public-index rollback, and the close-on-exec handshake that distinguishes successful `exec*` from immediate launch failure. The "why" should be tied to the daemonless / single-binary constraint and the need to create a process group that can later be validated before signaling.

Planned diagrams: sequence diagram for `perform_start`; state diagram for start outcomes such as launched, recorded, followed, failed-before-record, and rolled back.

### `docs/store.md`

Documents the user-local and system-managed stores, including paths, permissions, private records, logs, console sockets, `profiles.json`, public aliases, and redacted public run indexes. It should describe the JSON record fields actually written by `write_record_atomic`, the public fields actually written by `write_public_index_atomic`, and the atomic temp + fsync + rename pattern. It should explicitly distinguish private authoritative records from public discovery records.

Planned diagrams: ER-style Mermaid diagram for records/profiles/aliases/public indexes; flowchart for atomic writes and rollback.

### `docs/identity.md`

Documents the validate-before-signal model: boot ID, PID/PGID/SID, Linux `/proc` starttime, executable device/inode, group/session membership, zombie handling, stale/unknown/refused states, and session escapee reporting. It should explain how these checks reduce PID-reuse and process-group footguns while staying daemonless and best-effort across Linux and macOS.

Planned diagrams: flowchart for `eval_state`; state diagram for `running`, `exited`, `stale`, `failed`, and `unknown`.

### `docs/target-resolution.md`

Explains how user input tokens become concrete targets for action commands. It should cover plain IDs and prefixes, aliases, `user:` and `system:` scopes, user-local precedence for normal users, root-private precedence under sudo, public root discovery, alias ambiguity, `--all`, verb-specific alias filtering, and the difference between `resolve_target` for alias creation and `resolve_action_token` for multi-target action execution.

Planned diagrams: flowchart for normal non-root resolution; flowchart for root/sudo resolution; compact decision table for verb-specific alias intent.

### `docs/profiles-and-aliases.md`

Documents reusable launch recipes and capability fingerprints. It should cover user-local recipe aliases, root-managed profile hashes, public alias-to-hash entries, protected profile loading, `start <alias>`, `--multi`, and why `profile_hash_for_argv` hashes only the fixed namespace string, resolved binary path, argc, and indexed argv fields. It should state that environment, cwd, uid, timestamps, host data, and versions are not part of the hash because the digest is a stable capability key in the current implementation.

Planned diagrams: flowchart from recorded run to alias/profile; record diagram for user alias recipe vs root profile hash.

### `docs/security.md`

Documents the root/system and sudo self-elevation model. It should cover invocation provenance from `SUDO_UID`, `SUDO_GID`, and `SUDO_USER`; `--system`; internal `--elevated`; argv-preserving `fork` + `execvp("sudo")`; executable path resolution; alias/hash capability handoff; selector sentinels `00000000` and `ffffffff`; run-alias verification before privileged action; sudoers grant/revoke file generation; `visudo` validation; and root-owned executable checks. The "why" should focus on preserving the validate-before-signal model across privilege boundaries without adding a daemon or shell command string.

Planned diagrams: sequence diagram for non-root action self-elevation; sequence diagram for `grant`/`revoke`; flowchart for capability verification.

### `docs/console.md`

Documents attachable console support as implemented: `--console` preflight for `socat`, PTY broker setup, private Unix socket path, log teeing, `sigmund console <target>`, root-managed console elevation, and the fact that public indexes do not expose socket paths. It should also explain that `tail` and `dump` keep using the normal log even for console runs.

Planned diagrams: sequence diagram for console start and attach; small component diagram for child, broker, PTY, log, socket, and `socat`.

### `docs/cli-contract.md`

Documents the scripting and command-line contract from `main`, `usage`, and the help functions. It should cover raw start form, Sigmund-owned commands, `--`, `--quiet`, `--print`, `--iso`/`-l`, `--multi`, exit codes, stdout for machine data, stderr for human status, and command-specific no-op/error behavior. It should avoid inventing flags and should match only the current parser.

Planned diagrams: parser flowchart showing pre-command switches, owned-command parsing, raw command parsing, and dispatch.

### `docs/ci.md`

Provides a real GitHub Actions workflow that uses the implemented scriptable contract. It should show starting a detached helper, capturing the bare run ID from stdout, using that ID in later steps, dumping or tailing logs, stopping the process group, pruning records, and handling exit codes. The workflow must use only flags present in `src/sigmund.c`.

Planned diagrams: none required; the workflow and prose should be clearer than a diagram here.

### `docs/SPEC.md`

Leave this file in place as an implementation-contract reference. Phase 2 may link to it where useful, but the new subsystem pages should not simply duplicate it. If Phase 2 discovers a mismatch between `docs/SPEC.md` and `src/sigmund.c`, the new docs must follow the source and note the mismatch for later correction.

Planned diagrams: no new diagrams planned in this file.

## Cross-page design rules for Phase 2

- Every page should explain the design, the engineering mechanics, why the implementation works this way, and the end result for contributors.
- Every "why" must tie back to one or both core constraints: validate-before-signal safety and daemonless / single-binary operation.
- Do not present public root-index data as authoritative. The private record is authoritative; public root entries are redacted discovery metadata.
- Do not present profile hashes as run IDs. Run IDs address runs; profile hashes address protected launch recipes and sudo capabilities.
- Do not claim global all-user private-state scanning or global all-user run ID uniqueness. The current implementation avoids specific collision scopes based on the active invocation.
- Do not claim environment capture or scrubbing by Sigmund. `perform_start` inherits the process environment, and privilege-crossing environment behavior is sudo policy.
- Use Mermaid diagrams only where they clarify control flow, state, data relationships, or privilege boundaries. Target GitHub's verified Mermaid `11.15.0` renderer, and prefer short labels plus plain flowcharts or sequence/state diagrams.
- Avoid `block-beta`; GitHub Mermaid support is unreliable for it.

## Phase 2 verification checklist

Before treating Phase 2 as complete:

1. Re-read `src/sigmund.c` after drafting.
2. Verify every function name and behavioral claim against the source.
3. Confirm `docs/design.md` is the index and includes the top-level architecture diagram plus a relative-link table of contents.
4. Confirm each subsystem page is linked from `docs/design.md`.
5. Confirm the CI workflow example uses only implemented flags and respects stdout/stderr and exit-code behavior.
6. Confirm every Mermaid block renders on GitHub-flavored Markdown syntax.
7. Confirm planned or ambiguous behavior is omitted or quarantined in `docs/roadmap.md` as `planned - not yet implemented`.
8. Run repository checks, at minimum `make test`, unless the local environment lacks required POSIX tooling.

## Phase 1 self-review

- Completeness: the plan covers launcher/session creation, identity validation, stores, root public redaction, target resolution, signal safety, sudo self-elevation, profile hashing, console support, CLI scripting, and the required CI example.
- Accuracy: page boundaries and source-map entries are drawn from actual structures and functions in `src/sigmund.c`, not from planned behavior.
- Design quality: the planned pages separate conceptual concerns that the source also separates, especially store persistence, target resolution, process validation, and privilege crossing. This should help contributors modify one subsystem without mistaking it for another.
- Diagram fit: planned diagrams are chosen by subsystem shape: sequences for handshakes and starts, state diagrams for lifecycle/state evaluation, flowcharts for resolution and validation, and ER-style diagrams for persisted records.
