# On Hold CLI UX review

Date: 2026-06-23

## Executive summary

## Focused 0.4 spec draft

The implementation-oriented version of these decisions now lives in [Hold 0.4 UX and CLI specification draft](HOLD_0_4_UX_SPEC.md), including the detailed pager/live-filter/similarity-filter requirements.


On Hold already has a strong product core: `hold -d <cmd>` is simpler than `nohup`, safer than PID files, and far lighter than `systemd`, while foreground `hold <cmd>` can feel familiar to Docker users. The current CLI is scriptable and technically coherent, but it exposes internal concepts too early: run IDs, profiles, public/root stores, target scopes, grants, TTY mode, pruning, and validation semantics all appear as separate pieces the user must assemble.

The highest-leverage UX move is to turn On Hold from a collection of legacy verbs into one guided command language for long-running jobs. For 0.4.0, the new `hold` UX is intended to replace the legacy primary surface, while preserving the good stdout/stderr, exit-code, and scriptability contracts for automation.

Recommended direction:

1. Make the normal shell CLI mimic Docker commands and switches as precisely as Hold's host-process model allows; `run` is supported but optional for ad-hoc ease of use.
2. Make the captive CLI mimic Cisco IOS exactly: `hold>`, `hold#`, `configure terminal`, `hold(config)#`, `hold(config-profile:web)#`, `?`, `no`, `default`, `commit`, `end`, and `write`.
3. Use IOS-style profile configuration words in the captive CLI and transcript files: `binary`, `argv`, `env`, `alias`, `param`, `multi`, `pty-shim`, `no`, `default`, `info`, `commit`.
4. Make this a deliberate breaking CLI redesign for 0.4.0: replace current legacy launch-definition aliases and action-first forms with the Docker-shaped shell CLI plus Cisco IOS-shaped captive CLI. Keep `prune` where it fits, use Docker-style `-i/-t/-it` for shell interactivity, allow IOS EXEC `console` in the captive CLI, and replace `dump` with Docker-style `inspect`.
5. Add a Cisco IOS-style profile configuration mode for advanced recipe, env, params, PTY shim, multi-run, restart/readiness metadata, and access policy.
6. Add profile config import/export so every captive-shell edit can be represented as a Cisco-style command transcript, while JSON remains canonical on disk.
7. Add safer lifecycle affordances: save current run as a profile, Docker-style `stop`, `kill`, `rm`, “doctor/status”, suggested next commands, and cleanup prompts.

## Current UX strengths

- **Best possible first command**: `hold <command> [args...]` can run in Docker-like foreground mode, while `hold -d <command> [args...]` starts a background process without requiring config.
- **Scriptability is protected**: detached starts print only the run ID to stdout; banners go to stderr.
- **Safety story is excellent**: On Hold validates process identity before signaling, and refuses unsafe actions.
- **Short run IDs are approachable**: 12 hex chars are easier than full UUIDs.
- **Docs are unusually complete**: README, quickstart, technical loop, CLI contract, profiles, security, and TTY docs exist.
- **Root delegation is differentiated**: scoped sudoers-managed profiles are a powerful capability few small launchers offer.

## Current UX friction

### 1. The human workflow is still command-memory heavy

The help text is concise, but users still need to remember which verb applies now: `list`, `tail`, `dump`, `console`, `stop`, `kill`, `prune`, `profile`, `profiles`, `grant`, `revoke`, `start`, plus target scoping. This is fine for scripts, but not “embarrassingly easy”.

Suggested fix: add an interactive dashboard / command mode where users can type `?`, select a run, and see valid actions.

### 2. Profiles are the right reusable concept

The docs already discuss protected profiles, and the user-facing command should consistently be `profile`. For advanced features, a profile can represent a managed reusable launch target with policy, TTY behavior, environment, cwd, readiness checks, and access controls.

Suggested fix: make `profile` the primary launch-definition noun. If `alias` appears in 0.4.0, it should mean an IOS-style command/name alias table or profile-local command macro, not the old launch-definition noun.

### 3. Profile creation from a running command does not adopt that running command

Observed behavior:

```sh
id=$(hold sleep 20)
hold profile save "$id" as web
hold run web
```

This starts a second `sleep 20` because the original run was not labeled `web`; future profile runs are labeled, but the source run is not. The docs say profiles allow later commands to use the name, and the target spec says `run <profile>` refuses a duplicate by default once the profile already has a running process. That is technically true only after a run was started through the profile, but it is surprising immediately after profile creation.

Suggested fix options:

- Prefer: `hold profile save <id> as <name>` should label/adopt the source run as `<name>` by default.
- Or: print a warning and next step: “Pinned recipe as web. The existing run is still 04a7dda8cafe; use `hold adopt 04a7dda8cafe web` to manage it as web.”
- Or: split verbs: `profile save <id> as web` creates the recipe only; `profile adopt <id> as web` labels the current run.

### 4. `list` omits key human context

Current `hold list` columns are `RUNID STATE STARTED RESULT CMD`. It does not show profile label, TTY availability, scope, or log size/path. The user must remember or inspect separate commands.

Suggested fix: default human list should include profile and affordances, e.g.:

```text
NAME     ID        STATE    AGE  MODE     CMD
web      04a7dda8cafe  running  12s  log      /usr/bin/python -m http.server 9000
api      fe21dfb8cafe  running  2m   tty      node server.js
-        a1b2c3d4e5f6  exited   1h   log      sleep 1
```

Keep current output available as `--plain`, `--columns`, or `--json` for scripts.

### 5. Cleanup/prune is conceptually separate from stop

For normal users, cleanup should be discoverable without overloading process-stop behavior.

Suggested fixes:

- Keep `hold prune` as Docker-like bulk cleanup for inactive retained run records.
- Use `hold rm <inactive-runid|profile>` for targeted deletion.
- Use `hold rm --force <active-runid>` when the user explicitly wants to stop, verify, and remove one active concrete execution.
- In interactive/captive mode, offer `stop`, `kill`, `rm inactive`, `rm --force active run`, and `prune inactive history` as visibly different actions.

### 6. Advanced root/system profile features need a dedicated editor

`grant` and root-managed profiles are powerful but currently command-shaped. An advanced “profile editing mode” would let On Hold grow without turning every setting into a long flag list.

Suggested fix: add Cisco IOS-style `configure terminal` / `profile <name>` sub-configuration with validation, plus an editor path later if useful.



## Target model correction: profiles define, run IDs execute

A profile is not itself a running thing. A profile is a reusable definition for launching a tool. A run ID is the stable identity for one concrete execution and is the safest singular target for `stop`, `kill`, `logs`, `inspect`, and targeted `rm`. Interactive TTY entry is handled by `-i/-t/-it`, especially `hold run -it <profile>` for profile starts or reconnects.

That means the command grammar distinguishes profile lifecycle verbs, concrete execution verbs, and ad-hoc launch:

```text
hold <cmd> [args...]          # launch one ad-hoc command and print a run ID
hold run <profile>            # launch a reusable profile
hold stop <profile>           # stop a profile only when singular/explicit
hold stop <runid>             # stop one concrete execution
hold logs <target>            # act on a concrete run or safe singular selector
```

`run` should be a profile lifecycle verb, not a branch/namespace for existing executions. One-shot commands like `hold run web stop` read backwards and should be avoided.

Profile-name targeting is a convenience resolver, not a different kind of process identity. It should work only when singular and safe:

- `hold logs web` is valid only if profile `web` has exactly one relevant run for the requested action.
- `hold stop web` is valid only if profile `web` has exactly one running run, unless `--all` is explicitly supplied.
- If profile `web` has zero running runs, report that there is nothing to stop and suggest `hold run web`.
- If profile `web` has multiple running runs, refuse with candidates and require a run ID, `--all`, or an explicit selector.

Preferred wording:

```text
hold npm run dev              # foreground ad-hoc command, streaming stdout/stderr
hold -d npm run dev           # detached ad-hoc command, printing a run ID
hold run web                  # foreground profile run, streaming stdout/stderr
hold run -d web               # detached profile run, printing a run ID
hold run web --force          # launch one more web instance
hold run web --multi 3        # launch exactly 3 web instances
hold stop web                 # stop web only if exactly one web run is running
hold stop web --all           # stop all running executions launched from web
hold stop 04a7dda8cafe            # stop this exact execution
hold logs web --follow        # follow logs only if web resolves to one run
```

`--` is only for child commands that conflict with Hold syntax:

```text
hold -- logs                  # launch an executable named logs
hold -it -- logs              # launch executable logs with an interactive TTY
```

## Recommended command additions

### Friendly replacements for legacy verbs

Replace legacy primary verbs with memorable `hold` equivalents:

```text
hold ps                     -> list active run IDs
hold ps -a                  -> list active + inactive retained run IDs
hold show <view|object>     -> friendly read-only alias/view command
hold logs <target>          -> full-screen log viewer on TTY, script-friendly output off TTY
hold logs <target> --plain  -> force plain output
hold inspect <target>       -> Docker-style detailed JSON/object output
hold prune                  -> bulk inactive-history cleanup
hold rm <target>            -> remove inactive run ID or profile
hold rm --force <runid>     -> stop, verify, and remove one active run ID
hold restart <profile>      -> explicit relaunch sugar if release policy allows it
hold status [target]        -> richer inspection
hold doctor                 -> explain stores, permissions, stale records
```

### Docker-familiar flags to adopt

Borrow Docker flag names where the concept maps cleanly:

```text
hold run -d --name web -e NODE_ENV=production -- npm run start
hold -it --rm bash
hold run -i pythonshell
hold run -it web
hold run -d --restart always web
hold logs -f -n 100 web
hold inspect web
```

Target flag meanings:

- `-d` / `--detach`: explicit background mode and run-id printout; without it, run in Docker-like foreground mode.
- `-i` / `--interactive`: keep stdin open without requiring a PTY; e.g. `hold run -i pythonshell`.
- `-t` / `--tty`: allocate a pseudo-TTY for terminal behavior.
- `-it`: stdin-open plus TTY; the normal shell/full-terminal experience. `Ctrl+P Ctrl+Q` within 500ms detaches and leaves it running under Hold.
- `--detach-keys`: configure the detach sequence, Docker-style.
- `--name <name>`: assign a Docker-style run name, separate from explicit profile names.
- `-e` / `--env`, `--env-file`: launch/profile environment.
- `-p` / `--publish`: unsupported/rejected. Hold does not publish or forward ports; it observes in-use listening ports from the host process and lists them in `hold ps`.
- `-v` / `--volume`: unsupported/rejected. Hold does not mount/remap paths; pass host paths directly as argv/config, preferably absolute paths.
- `--rm`: auto-remove run-id data/logs when the run exits.
- `--restart`, `--restart-delay`: restart policy.
- `--privileged`: explicit high-risk privileged/elevated mode.

### Profile and configuration commands

```text
hold profile web -d -- npm run start      # create/update explicit profile
hold run -d web                           # launch profile detached
hold run -it web                          # launch/re-enter interactive profile run
hold inspect web                          # JSON/object inspection
hold export web as web.hold               # IOS-style transcript export
hold import web.hold as web               # IOS-style transcript import
hold                                      # enter captive IOS-style CLI
hold# configure terminal
hold(config)# profile web
hold(config-profile:web)# binary /usr/local/bin/nginx
hold(config-profile:web)# commit
```

### Friendly command aliases to evaluate after the core grammar lands

The core 0.4 grammar should land first: bare ad-hoc launch, `run/stop` for profile lifecycle, and concrete run IDs for `stop/kill/logs/inspect/rm`.

`show` can remain as a friendly alias because it is read-only and familiar. The
guardrail is that it routes to canonical views/actions (`show runs` to `ps -a`,
`show active` to `active`, `show profile web` to profile display, `show tree` to
navigation output) rather than becoming a second mutating command grammar.

After that is stable, optional shortcuts can be evaluated if they do not blur the profile/run-ID boundary:

```text
hold restart web            # stop/run a profile only when singular and explicit
hold status web             # explain profile plus active run IDs
```



## View navigation: keep IOS mode semantics authoritative

Earlier sketches explored slash/path namespaces (`/running/web`, `cd`, `back`, `pwd`, route-preserving redirects). Those ideas are useful as a future TUI or pager-view model, but they are **not** the 0.4 captive CLI contract. For 0.4, the captive CLI must remain a Cisco IOS clone:

- User EXEC: `hold>` for limited operational commands.
- Privileged EXEC: `hold#` after `enable`.
- Global config: `hold(config)#` after `configure terminal`.
- Profile sub-config: `hold(config-profile:web)#` after `profile web`.
- Discovery: contextual `?`, not Unix-like `cd`/`pwd` as the primary mental model.
- Persistence: staged config, `commit`, `end`, then `write`.

The multi-view idea should be expressed through IOS-style `show` commands and filterable viewers, for example:

```text
hold> show runs
hold> show profiles
hold> show version
hold# show running-config
hold# show profile web
hold# show runs | include web      # optional future filter syntax
```

Useful views can still exist under `show` and the pager/filter layer:

- active/running run IDs;
- inactive/recent retained run IDs;
- failed/stale records;
- runs grouped by profile;
- runs grouped by start time or uptime;
- grants/access summaries;
- log-centric views.

Guardrail: do not introduce a competing captive prompt model such as `hold(/running/web)>` for the 0.4 target. If a future TUI adds tree navigation, it should be clearly separate from the IOS-style CLI or implemented as an IOS `show`/viewer feature.

## Captive CLI / interactive mode proposal

### Entry points

```sh
hold                 # if no args and TTY: open Cisco IOS-style captive CLI
hold shell           # explicit system-shell capture mode, not the captive CLI
```

For non-TTY/no args, keep usage and exit nonzero for compatibility.

### User EXEC dashboard sketch

The opening screen may show a small status summary, but the prompt and command model remain IOS-like:

```text
hold v0.4  daemonless process guardian

Runs
  ALIAS     RUNID     STATE    AGE   MODE     COMMAND
  web       04a7dda8cafe  running  12s   log      python -m http.server 9000
  api       fe21dfb8cafe  running  2m    tty      node server.js
  -         a1b2c3d4e5f6  exited   1h    log      sleep 1

Type ? for help.
hold>
```

### Cisco IOS-style captive CLI modes

```text
hold> ?
hold> show runs
hold> enable
Password:
hold# configure terminal
hold(config)# profile web
hold(config-profile:web)# binary /usr/local/bin/nginx
hold(config-profile:web)# argv -c /etc/nginx.conf
hold(config-profile:web)# env LOG_LEVEL info
hold(config-profile:web)# param --port port
hold(config-profile:web)# no env LOG_LEVEL
hold(config-profile:web)# default multi
hold(config-profile:web)# commit
hold(config-profile:web)# end
hold# write
hold# disable
hold> stop web
hold> prune
```

Rejected historical sketches:

- Do not use a `run` submode such as `run api; tail; stop; prune` for 0.4.0. `run` is a profile lifecycle verb, and management actions stay as Docker-shaped verbs (`logs`, `stop`, `kill`, `inspect`, `rm`) over a run ID or singular safe profile selector.
- Do not use dotted `set env.KEY=value` as the target profile grammar. Profile config uses IOS words such as `binary`, `argv`, `env`, `param`, `no`, `default`, `info`, and `commit`.
- Do not use slash/path prompts such as `hold(/profiles/web)>` as the 0.4 captive CLI prompt model.

### Captive menu variant

A menu/TUI would be even easier for first-time users, but it is a later layer above the IOS-style CLI rather than a replacement for it:

- Arrow-key list of runs/profiles.
- Enter opens action palette.
- `l` logs, `i` inspect, `s` stop, `r` restart, `p` prune, `e` edit profile, `?` help.
- Prompts preview commands before privileged/destructive actions.

Implementation should start as a simple line REPL with IOS modes and contextual help, then graduate to an optional TUI later.

## Profile editor design

### Minimal v1 profile schema

Current profile hashing intentionally includes only resolved binary path and argv. For advanced features, do not silently change the existing hash contract. Add a versioned profile object with separately hashed launch identity and editable metadata.

Suggested conceptual shape:

```json
{
  "version": 2,
  "name": "web",
  "command": ["/usr/bin/python3", "-m", "http.server", "9000"],
  "cwd": "/srv/web",
  "env": {"PYTHONUNBUFFERED": "1"},
  "mode": {"tty": false, "interactive": false, "detach": false, "show_logs_on_run": false, "allow_multi": false},
  "readiness": {"type": "tcp", "host": "127.0.0.1", "port": 9000, "timeout_s": 10},
  "cleanup": {"stop_timeout_s": 5, "prune_on_success": false},
  "description": "local docs server",
  "tags": ["dev"],
  "access": {"alice": ["run", "stop", "logs", "inspect"]}
}
```

### Editor UX options

1. **Guided form** for common fields:
   - command
   - working directory
   - environment variables
   - TTY/log mode
   - multi-run policy
   - readiness check
   - grants/access policy, surfaced through privileged EXEC `grant`/`revoke`
2. **Text editor mode** for advanced users:
   - writes a temp JSON/TOML/YAML-like file
   - validates before save
   - shows diff from previous profile
3. **Command submode** for incremental changes:
   - `binary /usr/bin/node`
   - `argv server.js`
   - `env PORT 3000`
   - `no env DEBUG`
   - `alias myapp "run myapp --with-flags"`

Keep JSON as the canonical on-disk storage because On Hold already has JSON infrastructure, but make the human import/export format the Cisco-style CLI transcript.


## Profile import/export and config files

The captive CLI should not become a configuration island. Every profile that can be created or edited interactively should also be importable, exportable, diffable, and reviewable from the normal CLI. This is essential for backups, GitOps-style workflows, examples, sharing, CI setup, and scripted provisioning.

Recommended commands:

```text
hold export <name>                      # print CLI transcript config to stdout
hold export <name> as web.hold          # write CLI transcript config
hold export --all as profiles/          # write one CLI transcript per profile
hold import web.hold as web             # validate and create/update profile from transcript
hold import profiles/                   # import a directory of transcript files
hold import web.hold as web --dry-run  # check transcript without writing
hold export <name> > current.hold       # compare with normal review/diff tools
hold inspect <name>                     # JSON/object inspection for automation/debugging
```

For captive CLI parity, import/export should correspond to IOS-style configuration workflows: enter configuration mode, edit a profile, `commit`, `end`, and `write`; JSON/object details remain available through `inspect`.

Design requirements:

- **JSON remains canonical on disk**: the existing profile store shape should stay the authoritative internal storage format.
- **Cisco-like config is the import/export format**: export renders canonical JSON profiles as the same commands a user would type in captive mode; import parses those commands and writes validated JSON profiles. Transcript files should look like IOS configuration commands, not JSON and not Docker flags.
- **Round-trip safe**: `export -> import -> export` should preserve semantically meaningful fields, even if comments/spacing are not preserved.
- **Stdout friendly**: export defaults to stdout so users can pipe to files, review tools, or version control.
- **Dry-run by default for risky writes**: import/apply should validate and show a summary before overwriting unless `--yes` is supplied.
- **Secret aware**: environment values may need redaction/export modes, e.g. `--redact-secrets`, `--include-secrets`, or future secret references.
- **Versioned schema in JSON**: include `version` in the canonical JSON so profile config can evolve without breaking old files.
- **Scope explicit**: both JSON and CLI-config import should say whether the target is `user` or `system`, but system writes should still require explicit elevation.
- **Machine inspection still available**: `inspect` / `--json` should print detailed JSON/object data for automation. Do not make JSON the default import/export UX.

Example CLI-config export:

```text
configure terminal
  profile web
    binary /usr/local/bin/nginx
    argv -c /etc/nginx.conf
    env LOG_LEVEL info
    param --port port
    no env LOG_LEVEL
    default multi
    commit
end
write
```

Equivalent usage:

```sh
hold export web > web.hold
hold import web.hold as web --dry-run
hold import web.hold as web --yes
hold inspect web > web.json              # JSON/object inspection, not config export
```

This makes the interactive/captive CLI a friendly editor for the same durable JSON artifact the CLI can manage, while giving humans a readable Cisco-style config transcript.


## Pager-style live filter viewer

For log, list, and tree views, `hold` should include a pager-style viewer with vi-like movement and immediate keystroke filtering. This is not just a command prompt filter. When the user is inside a page-up/page-down or vi-style viewer, typing should reveal a small filter field at the top and dynamically narrow the visible buffer on every keystroke.

Viewer behavior:

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
Esc              clear filter or leave filter mode
Enter            keep/pin current filter and return to navigation
Backspace        remove one character and redraw; repeated Backspace naturally clears the filter
Ctrl-u           optional acceleration to clear all filter text
/                explicit search mode, highlight/jump matches
f                explicit filter mode, hide non-matches
n/N              next/previous search match
j/k              line down/up
PgUp/PgDn        page up/down
g/G              top/bottom
q                quit viewer
```

Because filtering is dynamic, clearing should feel like editing text: Backspace back to an empty query restores the full view immediately. A dedicated clear key is optional, not required for the core UX.

Important distinction:

- **Search mode** highlights and jumps while keeping the full buffer.
- **Filter mode** hides non-matching rows. Static match counters should not appear in the default dynamic log viewer chrome.
- **Type-to-filter mode** is the fast path: in views where text entry is otherwise meaningless, normal printable keystrokes immediately build the filter.

This should work consistently for:

- `hold logs <target>`
- `hold ps` / `hold ps -a` when opened interactively
- `hold profile` lists
- Docker-shaped `hold ps` / `hold ps -a` and IOS-shaped `show runs` / `show profiles` views
- `hold doctor` diagnostic output
- grants/access tables

For follow-mode logs, filtering should apply to the retained visible buffer and continue filtering new incoming lines. The UI should indicate when a filter is active so users do not think logs have stopped.

This viewer is one of the features that can make `hold` feel dramatically easier than raw shell pipelines: users can enter a view first, then discover the thing they need by simply typing.


## Example-line similarity filtering

The live filter viewer can go beyond text filtering by letting users mark representative lines and ask for visually/structurally similar rows. This should feel instant, not like a slow AI workflow.

Interaction model:

```text
Space            toggle current line as an example
S                show similar lines to selected examples
X                exclude similar lines to selected examples
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

User presses `S`; the viewer keeps lines that look like the selected failures:

```text
┌ similar: 2 examples                    8/834 lines ┐
│ 10:05:01 web ERROR missing config .env             │
│ 10:05:07 api ERROR missing config config.yml        │
│ 10:08:19 worker ERROR missing config worker.toml    │
└─────────────────────────────────────────────────────┘
```

Fast implementation should be lexical/structural first, semantic later if useful:

- tokenize log lines into words, numbers, paths, ids, timestamps, severity, profile, command, exit/status terms;
- downweight timestamps, PIDs, run IDs, UUIDs, hex IDs, ports, and monotonic counters;
- upweight severity, error class, command/profile, repeated message terms, path basenames, exit codes;
- score candidate lines with a cheap token-set similarity such as weighted Jaccard / cosine over hashed tokens;
- support positive and negative examples: “like these, not those”;
- update immediately on mark/toggle without network or model dependency.

This gives users a fast “find more like this” workflow for logs, failed-run lists, doctor diagnostics, and grants/access tables. It should be described as **similarity filtering** rather than AI search unless a later optional semantic backend exists.


### Viewport-driven similarity for huge logs

For very large logs, similarity filtering should not eagerly score every line. The viewer only needs enough matching rows to fill the terminal plus a small cache ahead/behind the viewport. Use an outward-expanding distance filter from the cursor/selected examples.

Algorithm shape:

1. User selects one or more example lines.
2. Build the example fingerprint.
3. Start at the current cursor/selection region.
4. Scan outward above and below by file distance.
5. Score candidate lines as they are encountered.
6. Add matching lines until the viewport is full, plus a small overscan buffer.
7. Stop scanning.
8. When the user pages up/down, continue scanning in that direction and fill the next viewport.

This keeps interaction fast even for million-line logs because the first response is proportional to terminal height and local match density, not file size.

Example behavior:

```text
selected line: 513,220
terminal height: 40
overscan: 80

scan 513,220 +/- outward until 120 matching rows are found
render nearest 40 matches
cache the next 80
```

Scrolling semantics:

- PageDown continues the outward/downward scan if the cache is low.
- PageUp continues the outward/upward scan if the cache is low.
- `G` / jump-to-end may scan from the end backward instead of from the original selection.
- `g` / jump-to-start may scan from the start forward.
- The UI can show partial-state text such as `similar: local matches` until a full-file scan is explicitly requested.

Optional explicit full scan:

```text
F                complete full-file similarity scan in background
```

But full scan should be opt-in. The default should feel immediate and local: find enough similar nearby lines to fill what the user can see.


Useful commands/labels:

```text
similar selected
more-like-this
exclude-like-this
filter similar
```

In non-interactive CLI, expose a deterministic version later if useful:

```sh
# Similarity is selected inside the full-screen viewer, e.g. press Space on a highlighted line.
```

## Safety and automation compatibility

Protect the automation contract:

- Detached `-d` starts print only the run ID to stdout; foreground starts stream child stdout/stderr.
- Stdout/stderr separation, exit codes, `--quiet`, and `--json` should remain stable for scripts even while 0.4.0 replaces legacy primary verbs.
- Add `--json` for machine-readable rich output rather than changing default parseable assumptions.
- Interactive mode should only activate on TTY with no args or explicit `shell/menu`.
- Destructive interactive actions should show a preview and require confirmation unless the action is already non-destructive or scoped.

## Phased roadmap

### Phase 1: Clarify and make current UX friendlier

- Add public `profile` commands over existing internal alias/profile storage.
- Add profile column to `ps`.
- Add `status` / `inspect` command.
- Decide whether `profile save <id> as <name>` adopts the source run or warns clearly.
- Add targeted `rm` and keep Docker-like `prune` for inactive history cleanup.
- Improve run banner with profile/name hints.

### Phase 2: Captive CLI and shell-capture split

- Add the Cisco IOS-style captive CLI on bare interactive `hold`: `hold>`, `hold#`, `configure terminal`, `hold(config)#`, `hold(config-profile:name)#`, `?`, `enable`, `disable`, `show`, `write`, `no`, `default`, `info`, and `commit`.
- Implement `hold shell` as the separate system-shell capture mode, not as a line-oriented Hold REPL: launch a real shell under a PTY/session wrapper, create no runid on normal `exit`, and use `Ctrl-P Ctrl-Q` to adopt the PTY foreground process group as a Hold run.
- Implement command completion/history for the captive CLI when practical.
- Add contextual help and suggested next actions.

### Phase 3: Profile editing mode

- Add versioned editable profile metadata.
- Add `profile show/edit/validate/from-run/delete/rename/import/export/diff`.
- Add readiness checks and cwd/env support if not already present.
- Keep legacy profile hash behavior isolated or explicitly versioned.

### Phase 4: TUI / captive menu polish

- Add full-screen dashboard if desired.
- Add action palette and keyboard shortcuts.
- Add safe privileged-flow previews.
- Add guided onboarding: “Start your first command”, “Save as profile”, “Run it again”.

## Highest-impact drastic change

The best drastic change for 0.4.0 is not to preserve old commands and add a menu beside them. It is to replace the legacy surface with a **two-model product built from familiar tools**: Docker for the normal shell CLI, Cisco IOS for the captive CLI/config experience.

- **Normal shell CLI**: Docker-shaped `hold run -d web`, `hold run -it web`, `hold ps`, `hold logs -f web`, `hold inspect web`, with optional `run` for ad-hoc ease.
- **Captive CLI**: Cisco IOS-shaped `enable`, `configure terminal`, `profile web`, then `binary`, `argv`, `env`, `param`, `commit`, `end`, `write`.
- **Config transcript**: the same Cisco IOS-style captive CLI commands saved in a file and imported/applied.

This gives beginners an embarrassingly easy path while giving power users a composable, scriptable grammar. JSON remains the canonical on-disk store; the command language becomes the human UX contract.

## Product decisions status

The implementation-oriented decisions now live in [Hold 0.4 UX and CLI specification](HOLD_0_4_UX_SPEC.md) and [0.4.0 branch alignment](0.4.0-alignment.md). Current direction: use `hold` for the operator command, make the normal shell CLI Docker-shaped with optional `run` for ad-hoc launches, make `profile` the primary launch-definition noun, use concrete run IDs for `stop`/`kill`/`logs`/`inspect`/targeted `rm`, keep `prune` as bulk inactive-run cleanup, and build the captive CLI as a Cisco IOS clone before considering any richer TUI. Remaining release decisions include restart policy, exact `runid` namespace spelling, instance allocator scope, and how much of the full similarity/deque architecture must ship before 0.4.0.
