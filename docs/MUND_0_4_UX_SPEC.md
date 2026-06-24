# Mund 0.4 UX and CLI specification draft

Date: 2026-06-23
Status: authoritative 0.4.0 branch UX specification and release-plan target

## 1. Product stance

Sigmund remains the project identity; the intended 0.4.0 user-facing command is `mund`.

`mund` is a guardian shell for background jobs: run it, leave it, find it, watch it, stop it safely.

Version 0.4.0 is intentionally allowed to break the current CLI because the tool has no established user base. This document defines the target product direction for that release. Until the tracked alignment matrix marks a section implemented, treat it as a requirement or follow-up rather than a release claim. The goal is to replace the legacy action-first/alias-based surface with one coherent command language shared by:

1. one-shot CLI commands;
2. captive shell commands;
3. importable/exportable CLI transcript config files.

JSON remains the canonical on-disk profile storage format. CLI transcript config is a human editing/import/export format that compiles to the JSON profile model.

The hardening backlog in section 11 is part of the same 0.4.0 release plan. The redesigned CLI should not be released as stable until the product surface and hardening work are both complete. `VERSION` is still `0.3.9` in the current branch, so wording in this document is branch/spec status unless explicitly tied to implemented source and tests. See [0.4.0 branch alignment and follow-up matrix](0.4.0-alignment.md) for the current implemented/deferred split.

## 2. Core model

### 2.1 Profile

A profile is a reusable launch definition for a tool. It is not itself a running process.

Profiles may contain:

- command/argv;
- working directory;
- environment variables;
- console preference;
- multi-run policy;
- readiness/health metadata;
- cleanup settings;
- grants/access policy;
- description/tags.

Profile operations are definition-oriented:

```sh
mund profile web show
mund profile web set command -- /usr/bin/python3 -m http.server 9000
mund profile web set cwd /srv/web
mund profile web set env PYTHONUNBUFFERED=1
mund profile web start
mund profile web export --format cli
```

### 2.2 Run

A run is one concrete execution. The run ID is the stable singular control handle.

Runs have:

- run ID;
- process identity;
- process group;
- state;
- log;
- optional profile label;
- scope;
- timestamps;
- console availability.

Execution-control actions target runs. A profile name may be accepted as a convenience selector only when it resolves safely.

```sh
mund stop 04a7dda8       # exact run
mund stop web            # valid only if web has exactly one running run
mund stop web --all      # explicitly affect all running web runs
mund logs web --follow   # valid only if web resolves to one relevant run
```

If profile `web` has zero matching runs, report that nothing matches and suggest a profile action such as `mund profile web start`. If it has multiple matching runs, refuse and show candidate run IDs unless `--all` or another explicit selector is supplied.

### 2.3 Ad hoc run command

`run` is a launch command, not a namespace for managing existing executions.

```sh
mund run -- npm run dev
```

Avoid awkward command forms such as:

```sh
mund run web stop       # do not use
```

Use natural execution verbs instead:

```sh
mund stop web
mund logs web
mund open web
mund status web
mund prune web
```

## 3. Proposed command grammar

The 0.4.0 CLI surface is the command set below. Implementation may stage internal refactors behind this surface, but the release should not defer major user-facing pieces of the grammar to a later minor release.

```text
mund run -- <cmd> [args...]

mund profile <name> show
mund profile <name> create -- <cmd> [args...]
mund profile <name> create-from-run <id> [--adopt]
mund profile <name> edit [--format cli|json]
mund profile <name> delete
mund profile <name> rename <new-name>
mund profile <name> start
mund profile <name> restart
mund profile <name> set command -- <cmd> [args...]
mund profile <name> set cwd <path>
mund profile <name> set env KEY=VALUE
mund profile <name> unset env KEY
mund profile <name> set console on|off
mund profile <name> set multi allow|deny
mund profile <name> export [--format cli|json]
mund profile <name> grant <principal> [actions]
mund profile <name> revoke <principal> [actions]

Current branch profile v1 evidence: `mund profile <name> create -- <cmd> [args...]`, `set command -- <cmd> [args...]`, `rename <new-name>`, and `delete` manage a user-local profile command recipe and round-trip through profile transcript/JSON export. `mund shell` also supports a first profile submode: `profile <name>` enters context, local `show/create/set/start/rename/delete` commands are rewritten through the same name-first grammar, and `back` returns to the top prompt. The remaining field editors (`cwd`, `env`, `console`, `multi`, readiness, cleanup, grants) are still release-gated follow-up work.

mund show runs
mund show profiles
mund show profile <name>
mund show grants [profile]
mund show tree <view>

mund status <target>
mund inspect <target>
mund logs <target> [--follow|-f] [--filter TEXT] [--similar TEXT] [--plain|--interactive]
mund view <target> [--follow|-f] [--filter TEXT] [--plain|--interactive]
mund open <target>
mund stop <target> [--all]
mund kill <target> [--all]
mund prune <target> [--all]
mund adopt <run-id> <profile>

mund clean exited|stale|failed|all [--dry-run|--yes]
mund doctor [target]
mund import <file> [--dry-run|--yes]
mund export profile <name> [--format cli|json]
```

## 4. Captive shell and navigation

Bare `mund` on an interactive TTY should open a captive shell/dashboard. In non-TTY contexts, no-arg behavior should not unexpectedly enter an interactive UI.

The shell supports the same command language as the one-shot CLI, plus navigation commands such as `cd`, `back`, `pwd`, `ls`, and `tree`.

### 4.1 View namespaces

The shell should expose navigable indexed views over the same underlying profile/run objects:

```text
/runs                 concrete executions
/profiles             launch definitions
/running              running executions grouped by profile
/dormant              profiles with no running execution
/stopped              cleanly stopped/exited runs
/failed               failed runs grouped by profile
/stale                records that cannot be safely validated
/system               system-scoped visible objects
/user                 user-scoped objects
/grants               delegation/access view
/logs                 recent log-centric view
/time                 runs grouped by start time/calendar bucket
/uptime               running runs grouped/sorted by uptime
/recent               newest runs/events first
/oldest               oldest running runs first
```

The tree is an indexed navigation system, not a single hierarchy. The same run can appear under profile, state, scope, time, uptime, failure, and log views while resolving to the same underlying run record.

### 4.2 Reversible redirects

Some paths are convenience paths that canonicalize to another view. Navigation should preserve the route the user took so `back` feels natural.

Example:

```text
mund> cd profiles/web
mund(/profiles/web)> cd running
redirect: /profiles/web/running -> /running/web
mund(/running/web)> back
mund(/profiles/web)>
```

Direct navigation backs up canonically:

```text
mund> cd running/web
mund(/running/web)> back
mund(/running)>
```

### 4.3 Universal completion namespaces

Completion is a first-class feature of the CLI library, not a special case inside
`mund shell`. Any namespace that can be listed by `list`, viewed by `show`, or
described by `help` should also be available to tab completion through the same
provider API. The command tree, help output, and completion candidates should
share one source of truth wherever practical.

Completion namespaces include at minimum:

```text
commands              help, profile, save, recent, start, stop, status, show, logs, view, doctor, exit, back
profiles              named reusable command recipes
runs                  active and unpruned run IDs
recent-runs           newest unpruned run IDs, including stopped runs with reusable argv recipes
recent-commands       past CLI command strings / captured argv recipes that have not been saved as profiles
views                 /runs, /profiles, /running, /dormant, /failed, /stale, /recent, /time, /uptime
profile-subcommands   show, create, set, start, rename, delete, export, grant, revoke
state-actions         stop, logs, view, inspect, clean/prune where valid for the selected object
```

The completion provider must be context-aware:

```text
mund> p<Tab>                 -> profile
mund> profile w<Tab>         -> profile web
mund> save <Tab>             -> recent run IDs plus `last`
mund> save 8f3a<Tab> as      -> complete the run ID, then suggest `as`
mund> save last as <Tab>     -> suggest profile names only for explicit overwrite flows; otherwise suggest no existing name
mund(profile:web)> s<Tab>    -> show/start/set/... local to the profile context
mund> stop <Tab>             -> active run IDs plus profile names only when they resolve to one active run
```

For 0.4.0, the target is basic, reliable tab completion inside and outside the
captive shell. OS shell integration should complete real text using the same
namespace provider where possible. The captive shell should also use `Tab` to
complete real text from the same provider. This is the release-relevant UX win.

Tab completion should follow normal shell expectations:

- if there is exactly one match, complete it and add a trailing space when the token is complete;
- if multiple matches share a longer common prefix, extend only through the first non-unique character;
- if matches remain ambiguous, show the candidate list with short descriptions where available;
- pressing `Tab` again may reprint or page the candidate list rather than changing the input unexpectedly.

This applies equally to prior command/argument strings. If recent commands share
a prefix, completion extends only through the shared prefix; it does not guess
which full argv recipe the user intended.

Inside the captive shell, Up/Down may cycle candidate replacements for the
current token or command line. The original text, including blank input, is part
of the cycle so the user can always return to exactly where they started:

```text
mund> s<Tab>       # ambiguous: save/show/start/status/stop, completes only shared prefix if any
mund> s<Down>      # save
mund> <Down>       # show
mund> <Down>       # start
mund> <Up>         # show
mund> <cycle...>   # eventually returns to the original `s` or blank input
```

No ghost text is part of the design. Completion either inserts real text, shows
candidates, or cycles concrete candidate replacements in Mund-owned interactive
contexts.

The reusable CLI library should therefore expose for 0.4.0:

- line editing and history for the captive shell;
- a pluggable completion namespace/provider API;
- longest-common-prefix completion for ambiguous matches;
- candidate-list rendering with descriptions;
- command/help metadata usable by both `help` and completion;
- a plain shell-completion mode that emits candidates without terminal UI control;
- captive-shell `Tab` completion and Up/Down candidate cycling using the same candidates.

## 5. CLI transcript config

CLI transcript config is a human import/export format. JSON remains canonical on disk.

Example:

```text
profile web
  set description "local docs server"
  set command -- /usr/bin/python3 -m http.server 9000
  set cwd /srv/web
  set env PYTHONUNBUFFERED=1
  set console off
  set multi deny
  set readiness tcp 127.0.0.1 9000 timeout 10s
  set cleanup stop-timeout 5s
exit
```

Commands:

```sh
mund export profile web --format cli > web.mund
mund export profile web --format json > web.json
mund import web.mund --dry-run
mund import web.mund --yes
```

Import/apply should validate and show a change summary before overwriting unless `--yes` is supplied.

## 6. Pager-style live filter viewer

The log/list/tree viewer is a key feature. It should feel like a vi/page-up-page-down viewer with immediate dynamic filtering.

When the user is inside a viewer and types printable characters, a filter field appears at the top and the visible buffer is narrowed on every keystroke.

Example:

```text
┌ filter: error_                         12/834 lines ┐
├──────────────────────────────────────────────────────┤
│ 10:04:12 web ERROR missing config                    │
│ 10:05:01 web ERROR retry failed                      │
│ 10:05:07 api ERROR timeout                           │
└──────────────────────────────────────────────────────┘
```

Key model:

```text
plain typing     open/update live filter field
Backspace        remove one character and redraw; repeated Backspace clears the filter
Ctrl-u           optional acceleration to clear all filter text
Esc              leave filter mode or dismiss overlay, depending on state
Enter            pin current filter and return to navigation
/                search mode: highlight/jump while keeping all rows
f                filter mode: hide non-matches
n/N              next/previous search match
j/k              line down/up
PgUp/PgDn        page up/down
g/G              top/bottom
q                quit viewer
```

Backspace to an empty query restores the full view immediately. A dedicated clear key is optional, not required.

Current branch v1 evidence: `mund view <target>` keeps plain output for scripts and opens an interactive TTY viewer by default when stdin/stdout are TTYs. `--plain` forces script-style output and `--interactive` fails closed when no TTY is available. The intended live-log UX is dynamic: `mund logs <target> --follow` / `mund view <target> --follow` opens the live viewer, printable keys update the top filter field per keystroke, Backspace relaxes the filter, and matching live output appears without restarting the command. `--filter TEXT` remains a scripting/seed option, not the primary human flow. Non-TTY follow streams matching lines until the recorded run exits; TTY follow refreshes while running and marks the view exited when the run ends. The v1 keys are printable type-to-filter, Backspace, Space to toggle the highlighted line as a similarity example, arrows/`j`/`k`, PgUp/PgDn, and `q`.

### 6.1 Search vs filter

- Search mode highlights/jumps to matches but keeps the full buffer.
- Filter mode hides non-matching rows and shows match counts.
- Type-to-filter mode is the fast path when printable text has no other meaning in the current viewer.

This viewer applies to:

- logs;
- run lists;
- profile lists;
- tree views;
- `doctor` diagnostics;
- grants/access tables.

For follow-mode logs, filtering applies to the retained visible buffer and continues filtering new incoming lines. The UI must clearly indicate an active filter so users do not think logs have stopped.

## 7. Example-line similarity filtering

The viewer should support a fast deterministic “more like this” filter.

Interaction model:

```text
Space            toggle current line as an example
S                show similar lines to selected examples
X                exclude lines similar to selected examples
A                add current line to positive examples
D                add current line to negative examples
U                unmark all examples
Enter            pin resulting similarity filter
Esc              leave similarity mode / return to normal viewer
```

Example:

```text
10:04:12 web INFO  listening on :9000
10:05:01 web ERROR missing config .env       [selected]
10:05:07 api ERROR missing config config.yml [selected]
10:06:44 api WARN  retrying database
```

After pressing `S`:

```text
┌ similar: 2 examples                    8/834 lines ┐
│ 10:05:01 web ERROR missing config .env             │
│ 10:05:07 api ERROR missing config config.yml        │
│ 10:08:19 worker ERROR missing config worker.toml    │
└─────────────────────────────────────────────────────┘
```

### 7.1 Similarity scoring

The first implementation should be local, deterministic, and fast. It should not depend on network services or model embeddings.

Suggested scoring approach:

1. tokenize log lines into words, severity tokens, profile names, paths, exit/status terms, commands, and structured fragments;
2. normalize case and obvious punctuation;
3. downweight timestamps, PIDs, run IDs, UUIDs, hex IDs, ports, counters, and monotonic numbers;
4. upweight severity, error class, command/profile, repeated message terms, path basenames, and exit codes;
5. hash tokens to compact feature IDs;
6. compare with weighted Jaccard or cosine-like scoring over sorted hashed token features;
7. support positive and negative examples with a score such as `positive_similarity - penalty * negative_similarity`.

Conceptual structs:

```c
struct token_feature {
    uint64_t hash;
    uint16_t weight;
};

struct line_fingerprint {
    uint64_t line_no;
    uint16_t feature_count;
    struct token_feature features[LINE_FEATURE_MAX];
};
```

Store byte offsets for rendering:

```c
struct match_ref {
    off_t line_start;
    uint32_t line_len;
    float score;
};
```

## 8. Directional lazy filtering for large logs

This is the critical performance requirement for the killer log feature.

The viewer must not eagerly process a million-line file just because a filter is active. Navigation should be viewport-driven and directional.

### 8.1 Core rule

On each navigation action, fill a complete viewport in the direction the user just moved, plus a small overscan/cache, before returning control. Do not scan the full file unless the user explicitly requests it.

Examples:

- User presses `PgDn`: scan forward only until the next filtered page plus lookahead is available, then render.
- User presses `PgUp`: scan backward only until the previous filtered page plus lookbehind is available, then render.
- User types another filter character: invalidate/refine filtered buffers and fill the current viewport from the current cursor region.

The UI should feel as if the file was instantly filtered, even though only enough data was processed to satisfy the visible view.

Current branch v1 evidence: the filter engine records byte offsets for visible matches plus previous/next scan anchors. PgDn can resume from the already-discovered newer boundary, and PgUp uses bounded backward scanning from the oldest visible match to fill older windows without reading the whole file. The TTY viewer owns a visible-row cache, so simple selection movement re-renders cached rows instead of invoking another filter scan.

### 8.2 Two-buffer model

Use two conceptual buffers:

1. **raw/source scan buffer**: reads chunks from the file and splits them into candidate lines;
2. **filtered match buffer**: stores only matching line references ready for viewport rendering and paging.

Conceptually:

```text
file bytes
  -> raw scan buffer / line splitter
  -> scorer/filter
  -> filtered match ring/deque
  -> visible viewport
```

Maintain before/visible/after match caches:

```text
[filtered before cache] [visible page] [filtered after cache]
```

A single deque with a visible slice is also acceptable, but the behavior should preserve fast PgUp/PgDn movement. The current branch TTY implementation uses a smaller first step: one cached visible page plus byte anchors/history for directional rescans. That gives the intended responsiveness without claiming a full persistent before/after deque as current behavior.

### 8.3 Text is byte-random-access, not line-random-access

Text files do not provide O(1) access to arbitrary line numbers because lines have variable length. The viewer should use byte offsets, not line numbers, as the primary navigation primitive.

Represent lines as byte ranges:

```c
struct line_ref {
    off_t start;
    uint32_t len;
};
```

Rendering a matched line is then:

```text
bytes[line_start : line_start + line_len]
```

Use `mmap` for regular files when practical, with `pread` fallback. Store discovered line ranges in a sparse/chunked line index so revisiting nearby regions is instant.

### 8.4 Forward scanning

Forward scan algorithm:

```text
while filtered_after has fewer than viewport_rows + overscan_rows:
    read next raw chunk from current byte offset
    split chunk into lines, carrying partial line if needed
    fingerprint/score each line
    append matches to filtered_after
    stop once enough matches exist
```

### 8.5 Backward scanning

Backward scan algorithm:

```text
offset = current_start_offset
while filtered_before has fewer than viewport_rows + overscan_rows:
    start = max(0, offset - BLOCK_SIZE)
    read bytes[start:offset]
    scan backward for '\n'
    emit line ranges in reverse order
    fingerprint/score each line
    prepend matches to filtered_before
    offset = start
```

Carry partial lines across block boundaries.

Current branch implementation note: the filter engine supports a bounded backward
window from a byte anchor. At the live edge, active TTY filters scan backward
from EOF to fill the visible screen with the most recent matching rows instead
of restarting at line 1. If the match set is outside the current scan budget,
the viewer renders the local partial result and marks it `partial` rather than
blocking on a full-log scan.

### 8.6 Directional fill behavior

On `PgDn`:

```text
1. direction = forward
2. consume existing filtered_after matches if available
3. if not enough rows, scan forward from last forward scan offset
4. score candidates and append matches
5. stop when visible page + one extra page/lookahead is available
6. render and return control
```

On `PgUp`:

```text
1. direction = backward
2. consume existing filtered_before matches if available
3. if not enough rows, scan backward from last backward scan offset
4. score candidates and prepend matches
5. stop when visible page + one extra page/lookbehind is available
6. render and return control
```

### 8.7 Sparse matches and responsiveness

If matches are sparse and the viewer cannot fill a complete page within a short budget, it may render a partial page with a clear status indicator and continue scanning opportunistically. However, the target behavior is to fill the page before yielding whenever it can do so quickly.

Suggested status messages:

```text
similar: local matches, scanning forward…
filter: 18 shown, scanning backward…
```

Full-file scanning should be explicit:

```text
F                complete full-file similarity/filter scan in background
```

Default behavior is local and immediate.

### 8.8 Follow-mode logs

For growing logs:

- v1 implementation: `mund logs <target> --follow` routes through `mund view --follow` for a dynamic TTY filter field. `--filter TEXT` can seed/script the same engine, but the human design is type-to-filter after the viewer is open. Plain `mund logs <target>` remains tail-compatible.
- active live filters are anchored at the tail by default; PgUp moves to older matching windows, and PgDn walks back toward the live edge;
- when the user is browsing older matches, new log data does not yank the viewport to EOF; follow ticks filter bounded appended slices and keep a separate scan-progress cursor, so sparse matches in large bursts are deferred across ticks rather than skipped; the header reports `newer below` only after a new matching row is found;
- `--debug-stats` includes `scan_gen`, which increments only when the filter engine refills the visible cache;
- future/full model: keep a raw tail ring of recent bytes/lines;
- future/full model: append new lines as they arrive;
- future/full model: fingerprint/score new lines against active filters;
- future/full model: append matching lines to the filtered after-cache;
- current and future behavior: auto-scroll only if the user is already at the bottom;
- current and future behavior: otherwise show a “new matching lines below” indicator after a matching row is found.

## 9. Output contract

The new grammar should preserve Sigmund’s good scripting discipline:

- stdout is for machine data when a command is intended to be captured;
- stderr is for human status and diagnostics;
- `--json` provides structured output;
- `--quiet` suppresses routine human status;
- exit codes remain meaningful and documented.

Example:

```sh
id="$(mund run -- sleep 30)"
```

The command should print only the run ID to stdout on success.

## 10. Alignment status and decisions

Current branch status is tracked in [0.4.0 branch alignment and follow-up matrix](0.4.0-alignment.md). That matrix is part of this specification: if the matrix says a feature is a follow-up, this document describes the intended product target rather than current release readiness.

Resolved decisions for the current 0.4.0 direction:

1. Use **Sigmund** for the project and **`mund`** for the intended 0.4.0 operator command.
2. Keep `run` launch-only. Do not make `mund run <target> stop/logs/open` a management namespace.
3. Omit `profile <name> stop` as a primary command so the definition/run distinction stays clear; use `mund stop <target>` with singular profile resolution rules.
4. Treat current Space-selected, local deterministic similarity as the minimum implemented v1 slice until the fuller `S/X/A/D/U` interaction model is implemented.

Still-open release decisions:

1. Whether `mund profile <name> restart` is safe sugar or must require explicit policy such as singular-only vs `--all`.
2. Whether `/active` and `/history` are primary shell namespaces or aliases over `/running` and state-specific history views.
3. Whether full before/visible/after deques, raw tail ring, sparse indexes, and richer similarity controls are required before the 0.4.0 release or can remain post-0.4 follow-up with the current dynamic filter as the core feature.

## 11. 0.4.0 engineering hardening backlog

The 0.4.0 CLI redesign should ship with hardening work that makes the new surface safe to iterate on. These items are release criteria for the breaking 0.4.0 line, not optional polish or a separate post-0.4 hardening phase.

### 11.1 Stabilize the test suite

Requirements:

- Modify `tests/test_sigmund.sh` so every `run_test` invocation has a per-test timeout.
- Default timeout: `SIGMUND_TEST_TIMEOUT=25` seconds.
- Print `RUN: <description>` before each test.
- On timeout, print:
  - failing test name/description;
  - `TEST_ROOT`;
  - relevant `ps` output;
  - bounded `find`/tree listing of the test directory.
- Ensure cleanup still runs after timeout failures.
- Add CI job-level timeouts where appropriate.

Acceptance:

- `make test` can no longer hang indefinitely.
- A hung console/process test fails with actionable diagnostics.

### 11.2 Tighten CLI argument validation

Requirements:

- In `src/main.c`, enforce exact arity for current owned commands while the parser is being replaced/refactored:
  - `tail`: exactly one target;
  - `dump`: exactly one target;
  - `console`: exactly one target;
  - `prune`: zero or one target.
- Reject extra args with the correct usage message and exit code `5`.
- Fix split-form `--multi` parsing so invalid non-option values after `--multi` are rejected instead of treated as target tokens.
- Add regression tests.

Acceptance:

- No owned command silently ignores unexpected positional args.

### 11.3 Move toward table-driven CLI command specs

Requirements:

- Introduce a small command-spec table for owned commands with:
  - command name;
  - min/max args;
  - allowed flags;
  - usage string/help topic;
  - whether `--` is meaningful;
  - whether target resolution permits `--all`.
- Keep execution dispatch readable and minimize churn.
- Use the table to reject unsupported flags/args consistently.
- Ensure docs/help usage and parser behavior agree.

Acceptance:

- All owned commands have one parser truth source for arity/flag validation.
- Help text and parser behavior agree for all owned commands.

### 11.4 Harden console attach authorization

Requirements:

- Add a platform helper to retrieve AF_UNIX peer credentials:
  - Linux: `SO_PEERCRED`;
  - macOS/BSD: `getpeereid` or `LOCAL_PEERCRED` where available.
- In the console broker, after `accept`, verify peer UID before replaying output or forwarding input.
- Permit:
  - run owner;
  - root;
  - any explicitly intended invoking user for elevated/system runs, if current behavior relies on that. Preserve this intentionally and test it.
- Keep console socket file permissions at `0600`.
- Add tests proving unrelated users cannot attach.

Acceptance:

- Console socket file permissions remain `0600`.
- Peer credential checks are enforced before any output replay or input forwarding.

### 11.5 Separate display liveness from signal safety

Requirements:

- Keep list/status behavior user-friendly, but make signal paths stricter.
- Add a stricter validation function used by:
  - `stop`;
  - `kill`;
  - `--print` before printing `kill(-pgid, sig)`.
- Require positive validation of boot ID and either:
  - group/session liveness; or
  - direct leader PID/PGID/SID/start-time identity.
- Refuse to signal uncertain records.
- Add regression tests with tampered records.

Acceptance:

- `stop`/`kill` refuse records that cannot be tied to the recorded process group/session.

### 11.6 Harden store filesystem operations

Requirements:

- Audit `sigmund_mkdir_p0700` and `sigmund_mkdir_p_mode`.
- Replace stat-following path walks with `lstat`/`openat`-style symlink refusal where practical.
- Ensure `chmod`/`chown` operations cannot be redirected through symlinks.
- Rename `sigmund_read_owned_file_no_symlink` if ownership is not checked, or add ownership/mode checks where security-sensitive.
- Add tests for symlinked store directories and temp files.

Acceptance:

- System store initialization refuses symlinked critical directories.

### 11.7 Harden atomic writers

Requirements:

- Update aliases/profiles atomic writers to use unique temp names with:
  - `O_EXCL`;
  - `O_CLOEXEC`;
  - `O_NOFOLLOW` where available.
- Preserve:
  - file `fsync`;
  - `rename`;
  - parent directory `fsync`.
- Add tests for:
  - pre-existing temp files;
  - symlink temp files.

Acceptance:

- No fixed-name temp file can be truncated/followed unexpectedly.

### 11.8 Fix source-tree versioning

Requirements:

- Update `Makefile` so builds outside a Git checkout use `VERSION` directly, without `-dev-dirty`.
- Preserve Git tag/hash/dirty metadata inside a Git checkout.
- Add a lightweight test or documented manual check.

Acceptance:

- Source ZIP builds report the `VERSION` file value.

### 11.9 Clarify static build/install behavior

Requirements:

- Update `README.md` and `docs/install.md` to explain glibc static NSS caveats.
- Recommend musl static artifacts for true standalone Linux installs.
- Do not overstate GNU static portability.
- Distinguish artifact families precisely:
  - GNU dynamic: glibc-targeted and dynamically linked to the host glibc/NSS stack;
  - GNU static: glibc-targeted and statically linked, but still subject to glibc NSS module/runtime behavior for user, group, host, DNS, and related name-service lookups;
  - musl static: statically linked with musl and the recommended Linux artifact when standalone deployment matters.
- Document that `make` on Linux defaults to `STATIC_LDFLAGS=-static`; with a glibc compiler this creates a GNU static binary with the caveats above, not the same portability profile as a musl static artifact.

Acceptance:

- Docs accurately describe GNU static, GNU dynamic, and musl static artifacts.
- The recommended path for true standalone Linux installs is `SIGMUND_FLAVOR=musl-static` or an equivalent musl-targeted static build.

### 11.10 Harden release and installer scripts

Requirements:

- Remove any release step that deletes an existing GitHub release before publishing.
- Use `overwrite_files` or an immutable release policy instead.
- Replace installer temp dir creation with `mktemp -d` and `umask 077`.
- Prefer mandatory `SHA256SUMS` verification over release-body checksum scraping.
- Validate archive layout instead of finding the first file named `sigmund`/`mund`.
- Make `package_tarball.sh` deterministic where GNU tar is available.
- Add installer tests for:
  - missing/malformed checksum;
  - checksum mismatch;
  - unsupported platform;
  - archive missing expected binary.

Acceptance:

- Release flow is non-destructive.
- Installer fails closed.

### 11.11 Remove or replace stale `REVIEW.md`

Requirements:

- Delete `REVIEW.md` or replace it with a current review-status document.
- If replaced, list actual verification commands and caveats.
- Do not claim all tests pass unless `make test` completes successfully.
- Keep any replacement scoped to release-readiness/status. It should not preserve stale test counts, obsolete file names, or claims from previous review rounds.

Acceptance:

- No stale test counts or obsolete file references remain.
- `REVIEW.md`, if present, clearly says docs-only checks are not implementation test evidence.

## 12. 0.4.0 release acceptance summary

0.4.0 should be considered ready only when both product and hardening criteria are met:

- full `mund` command grammar and product direction in this document are implemented coherently and documented;
- profile/run target semantics are unambiguous;
- CLI transcript import/export is specified and tested;
- pager/live-filter/similarity-filter behavior is implemented to the 0.4.0 interaction contract, with later minor releases limited to refinement rather than deferring the core product feature;
- test suite cannot hang indefinitely;
- parser rejects unsupported flags and extra positionals;
- console attach authorization is peer-credential enforced;
- signal safety is stricter than display liveness;
- system store and atomic writes reject symlink/temp-file attacks;
- source ZIP builds report `VERSION` correctly;
- installer/release flow fails closed and avoids destructive release deletion;
- stale review claims are removed or replaced with current verification evidence.
