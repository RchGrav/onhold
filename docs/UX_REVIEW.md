# On Hold CLI UX review

Date: 2026-06-23

## Executive summary

## Focused 0.4 spec draft

The implementation-oriented version of these decisions now lives in [Hold 0.4 UX and CLI specification draft](HOLD_0_4_UX_SPEC.md), including the detailed pager/live-filter/similarity-filter requirements.


On Hold already has a strong product core: `hold -d <cmd>` is simpler than `nohup`, safer than PID files, and far lighter than `systemd`, while foreground `hold <cmd>` can feel familiar to Docker users. The current CLI is scriptable and technically coherent, but it exposes internal concepts too early: run IDs, profiles, public/root stores, target scopes, grants, TTY mode, pruning, and validation semantics all appear as separate pieces the user must assemble.

The highest-leverage UX move is to turn On Hold from a collection of legacy verbs into one guided command language for long-running jobs. For 0.4.0, the new `hold` UX is intended to replace the legacy primary surface, while preserving the good stdout/stderr, exit-code, and scriptability contracts for automation.

Recommended direction:

1. Move to one unified stacked command grammar shared by normal CLI, captive shell, and Cisco-style config files.
2. Make `profile`, `run`, `stop`, `down`, `kill`, `ps`, `show`, `logs`, `inspect`, `prune`, `rm`, `doctor`, `import`, and `export` the primary language everywhere.
3. Let users stack captive-shell commands directly from the normal prompt, e.g. `hold profile web set env.PORT=3000`, `hold show profile web`, `hold inspect web`, `hold logs web --follow`.
4. Make this a deliberate breaking CLI redesign for 0.4.0: replace current legacy commands (`alias`, `aliases`, `list`, `tail`, action-first forms) with the unified grammar instead of carrying them as primary UX. Keep `prune` because it fits the final language, replace `console` with Docker-style `-i/-t/-it`, and replace `dump` with Docker-style `inspect`.
5. Add a profile editor: `hold profile web edit` and an interactive `profile web` submode for advanced recipe, cwd, env, TTY/interactive flags, restart/readiness metadata, and access policy.
6. Add profile config import/export so every captive-shell edit can be represented as a Cisco-style command transcript, while JSON remains canonical on disk.
7. Add safer lifecycle affordances: save current run as a profile, singular-safe profile `stop`, concrete run-id `down`, “doctor/status”, suggested next commands, and cleanup prompts.

## Current UX strengths

- **Best possible first command**: `hold <command> [args...]` can run in Docker-like foreground mode, while `hold -d <command> [args...]` starts a background process without requiring config.
- **Scriptability is protected**: detached starts print only the run ID to stdout; banners go to stderr.
- **Safety story is excellent**: On Hold validates process identity before signaling, and refuses unsafe actions.
- **Short run IDs are approachable**: 8 hex chars are easier than full UUIDs.
- **Docs are unusually complete**: README, quickstart, technical loop, CLI contract, profiles, security, and TTY docs exist.
- **Root delegation is differentiated**: scoped sudoers-managed profiles are a powerful capability few small launchers offer.

## Current UX friction

### 1. The human workflow is still command-memory heavy

The help text is concise, but users still need to remember which verb applies now: `list`, `tail`, `dump`, `console`, `stop`, `kill`, `prune`, `profile`, `profiles`, `grant`, `revoke`, `start`, plus target scoping. This is fine for scripts, but not “embarrassingly easy”.

Suggested fix: add an interactive dashboard / command mode where users can type `?`, select a run, and see valid actions.

### 2. Profiles are the right reusable concept

The docs already discuss protected profiles, and the user-facing command should consistently be `profile`. For advanced features, a profile can represent a managed reusable launch target with policy, TTY behavior, environment, cwd, readiness checks, and access controls.

Suggested fix: make `profile` the primary noun and remove `alias` from the public 0.4.0 command surface.

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
- Or: print a warning and next step: “Pinned recipe as web. The existing run is still 04a7dda8; use `hold adopt 04a7dda8 web` to manage it as web.”
- Or: split verbs: `profile save <id> as web` creates the recipe only; `profile adopt <id> as web` labels the current run.

### 4. `list` omits key human context

Current `hold list` columns are `RUNID STATE STARTED RESULT CMD`. It does not show profile label, TTY availability, scope, or log size/path. The user must remember or inspect separate commands.

Suggested fix: default human list should include profile and affordances, e.g.:

```text
NAME     ID        STATE    AGE  MODE     CMD
web      04a7dda8  running  12s  log      /usr/bin/python -m http.server 9000
api      fe21dfb8  running  2m   tty      node server.js
-        a1b2c3d4  exited   1h   log      sleep 1
```

Keep current output available as `--plain`, `--columns`, or `--json` for scripts.

### 5. Cleanup/prune is conceptually separate from stop

For normal users, cleanup should be discoverable without overloading process-stop behavior.

Suggested fixes:

- Keep `hold prune` as Docker-like bulk cleanup for inactive retained run records.
- Use `hold rm <inactive-runid|profile>` for targeted deletion.
- Use `hold rm --force <active-runid>` when the user explicitly wants to stop, verify, and remove one active concrete execution.
- In interactive mode, offer `down`, `rm inactive`, `rm --force active run`, and `prune inactive history` as visibly different actions.

### 6. Advanced root/system profile features need a dedicated editor

`grant` and root-managed profiles are powerful but currently command-shaped. An advanced “profile editing mode” would let On Hold grow without turning every setting into a long flag list.

Suggested fix: add `hold profile <name> edit` that opens `$EDITOR` or an interactive form with validation.



## Target model correction: profiles define, run IDs execute

A profile is not itself a running thing. A profile is a reusable definition for launching a tool. A run ID is the stable identity for one concrete execution and is the safest singular target for `down`, `kill`, `logs`, `inspect`, and targeted `rm`. Interactive TTY entry is handled by `-i/-t/-it`, especially `hold run -it <profile>` for profile starts or reconnects.

That means the command grammar distinguishes profile lifecycle verbs, concrete execution verbs, and ad-hoc launch:

```text
hold <cmd> [args...]          # launch one ad-hoc command and print a run ID
hold run <profile>            # launch a reusable profile
hold stop <profile>           # stop a profile only when singular/explicit
hold down <runid>             # stop one concrete execution
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
hold down 04a7dda8            # stop this exact execution
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
hold logs <target>          -> follow/plain log viewer
hold logs <target> --plain  -> plain tail-style output
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
hold -d --name web -p 8080:3000 -e NODE_ENV=production npm run start
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
- `--name <profile>`: create/label a profile from the normalized launch recipe.
- `-e` / `--env`, `--env-file`: launch/profile environment.
- `-p` / `--publish`: port metadata for display/readiness/open helpers; do not pretend to implement container networking for host processes.
- `-v` / `--volume`: volume/path metadata for profile config and future backend support; do not pretend to remount host paths for normal host-process launches.
- `--rm`: auto-remove run-id data/logs when the run exits.
- `--restart`, `--restart-delay`: restart policy.
- `--privileged`: explicit high-risk privileged/elevated mode.

### Profile commands

```text
hold profile                              # enter/list profile namespace
hold profile <name>                       # show/select one profile
hold profile save <runid> as <name>       # create profile from run ID
hold import <file> as <profile>
hold export <profile> as <file>
hold profile <name> edit
hold profile <old> rename <new>
hold profile <name> delete
hold profile <name> grant <user> [actions]
hold profile <name> revoke <user> [actions]
```

Compatibility:

```text
hold profiles                     -> profile
hold run <profile>                -> launch reusable profile
```

### Friendly command aliases to evaluate after the core grammar lands

The core 0.4 grammar should land first: bare ad-hoc launch, `run/stop` for profile lifecycle, and concrete run IDs for `down/kill/logs/inspect/rm`.

`show` can remain as a friendly alias because it is read-only and familiar. The
guardrail is that it routes to canonical views/actions (`show runs` to `ps -a`,
`show active` to `active`, `show profile web` to profile display, `show tree` to
navigation output) rather than becoming a second mutating command grammar.

After that is stable, optional shortcuts can be evaluated if they do not blur the profile/run-ID boundary:

```text
hold restart web            # stop/run a profile only when singular and explicit
hold status web             # explain profile plus active run IDs
```

Avoid shortcuts such as profile-level `up/down` until the semantics are unmistakable; `down` is reserved for concrete execution stop in the target grammar.


## Navigable view namespaces and reversible redirects

The captive shell should not only execute commands; it should let users navigate the process universe from different useful viewpoints. These views are namespaces over the same underlying objects. Some paths are canonical, and some are convenience redirects, but navigation should preserve the path the user took so `back` feels natural.

Use precise state words in views; `active` and `recent` are good top-level entry points, while detailed rows can use:

- **running**: currently validated as alive.
- **stopped**: intentionally stopped or exited cleanly.
- **failed**: exited nonzero or launch failed.
- **stale**: record exists but identity can no longer be validated.
- **dormant**: profile exists but has no current running execution.
- **all**: every visible profile/run.

Suggested view names for the interactive shell and future slash/path-style navigation:

```text
/runid                concrete executions, grouped/filterable by state
/profile              launch definitions
/active               active/running executions grouped by profile
/recent               inactive/recent retained executions
/running              synonym/view over active executions grouped by profile
/dormant              profiles with no running execution
/failed               failed runs grouped by profile
/stale                stale/unknown records
/system               system-scoped visible objects
/user                 user-scoped objects
/grants               delegation/access view
/logs                 recent log-centric view
/time                 runs grouped by start time / calendar bucket
/uptime               running runs grouped or sorted by current uptime
/oldest               oldest running runs first
```

The tree should be an indexed navigation system, not a single hierarchy. The same run can appear under state, profile, scope, start-time, uptime, failure, and log views. These are all views over one underlying run record.

Time-oriented examples:

```text
/time
  today
    10:14  web  04a7dda8  running
    09:58  api  fe21dfb8  failed
  yesterday
    16:20  worker a1b2c3d4 stopped

/uptime
  under-1m
    web  04a7dda8  12s
  1m-10m
    api  fe21dfb8  2m
  over-1h
    cache 9910aaef  3h

/recent
  04a7dda8  web     started 12s ago
  fe21dfb8  api     failed 2m ago
  a1b2c3d4  worker  stopped 1h ago
```

Useful view commands:

```text
show tree time
show tree uptime
show recent
show oldest
cd time/today
cd uptime/over-1h
cd recent
```

Example tree view:

```text
hold> tree running
/running
  web
    04a7dda8  12s  no tty  python -m http.server 9000
  api
    fe21dfb8  2m   tty on   node server.js
```

Profile-first view:

```text
hold> tree profiles
/profiles
  web
    definition
    running
      04a7dda8
    failed
      a1b2c3d4
  api
    definition
    dormant
```

Canonicalization rule:

- `/running/web` is the canonical path for currently running executions of profile `web`.
- `/profiles/web/running` is a convenience path that redirects to `/running/web`.
- The shell should remember that the user arrived through `/profiles/web/running`, so `back` returns to `/profiles/web`, not `/running`.

Example navigation:

```text
hold> cd profiles/web
hold(/profiles/web)> cd running
redirect: /profiles/web/running -> /running/web
hold(/running/web)> ls
04a7dda8  running  12s  python -m http.server 9000
hold(/running/web)> back
hold(/profiles/web)>
```

Direct navigation should backtrack canonically:

```text
hold> cd running/web
hold(/running/web)> back
hold(/running)>
```

This gives users multiple mental entry points without duplicating behavior. A profile-centric user can start from `profile web`; an operator can start from `running`; a debugger can start from `failed`; all routes resolve to the same concrete run objects.

Command implications:

```text
show tree running
show tree profiles
cd profiles/web
cd running/web
cd failed/web
back
pwd
ls
```

Inside a view, commands can be context-relative:

```text
hold(/running/web)> logs 04a7dda8
hold(/running/web)> down 04a7dda8
hold(/profiles/web)> run
hold(/profiles/web)> export --format cli
hold(/failed/web)> logs a1b2c3d4
hold(/failed/web)> inspect a1b2c3d4
```

The important UX property: namespaces can connect backwards for discoverability, but concrete actions still resolve to stable run IDs or singular safe selectors before acting.

## Captive CLI / interactive mode proposal

### Entry points

```sh
hold                 # if no args and TTY: open dashboard, not usage error
hold shell
hold menu
hold profile web edit
```

For non-TTY/no args, keep usage and exit nonzero for compatibility.

### Top-level dashboard sketch

```text
hold v0.4  daemonless process guardian

Runs
  NAME      ID        STATE    AGE   MODE     COMMAND
  web       04a7dda8  running  12s   log      python -m http.server 9000
  api       fe21dfb8  running  2m    tty      node server.js
  scratch   a1b2c3d4  exited   1h    log      sleep 1

Profiles: web, api, cache(system)

Type: run <profile>, run -it <profile>, stop <profile>, down <runid>, logs <name|id>, inspect <name|id>, profile, ps, prune, help, quit
hold>
```

### Cisco/diskpart-style submodes

```text
hold> profile web
hold(profile:web)> show
hold(profile:web)> cwd /srv/web
hold(profile:web)> set env.DATABASE_URL=postgres://...
hold(profile:web)> set env.LOG_LEVEL=info
hold(profile:web)> alias myapp "run myapp --with-flags"
hold(profile:web)> interactive
hold(profile:web)> tty
hold(profile:web)> readiness tcp localhost:3000 timeout 10s
hold(profile:web)> save
hold(profile:web)> run
hold(profile:web)> exit
hold> logs web
hold> stop web
hold> prune
```

Rejected historical sketch: do not use a `run` submode such as `run api; tail; stop; prune` for 0.4.0. `run` is a profile lifecycle verb, and management actions stay as natural verbs (`logs`, `down`, `kill`, `inspect`, `rm`) over a run ID or singular safe profile selector.

### Captive menu variant

A menu/TUI would be even easier for first-time users:

- Arrow-key list of runs/profiles.
- Enter opens action palette.
- `l` logs, `i` inspect, `s` stop, `r` restart, `p` prune, `e` edit profile, `?` help.
- Prompts preview commands before privileged/destructive actions.

Implementation can start as a simple line REPL without curses, then graduate to an optional TUI later.

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
   - grants
2. **Text editor mode** for advanced users:
   - writes a temp JSON/TOML/YAML-like file
   - validates before save
   - shows diff from previous profile
3. **Command submode** for incremental changes:
   - `command -- node server.js`
   - `env PORT=3000`
   - `set env.DATABASE_URL=postgres://...`
   - `no env DEBUG`
   - `alias myapp "run myapp --with-flags"`
   - `grant alice run,stop,logs`

Keep JSON as the canonical on-disk storage because On Hold already has JSON infrastructure, but make the human import/export format the Cisco-style CLI transcript.


## Profile import/export and config files

The captive shell should not become a configuration island. Every profile that can be created or edited interactively should also be importable, exportable, diffable, and reviewable from the normal CLI. This is essential for backups, GitOps-style workflows, examples, sharing, CI setup, and scripted provisioning.

Recommended commands:

```text
hold export <name>                      # print CLI transcript config to stdout
hold export <name> as web.hold          # write CLI transcript config
hold export --all as profiles/          # write one CLI transcript per profile
hold import web.hold as web             # validate and create/update profile from transcript
hold import profiles/                   # import a directory of transcript files
hold profile validate web.hold          # check transcript without writing
hold profile diff <name> web.hold       # compare stored profile to transcript
hold inspect <name>                     # JSON/object inspection for automation/debugging
```

For captive shell parity:

```text
hold> profile web
hold(profile:web)> export
hold(profile:web)> export web.json
hold(profile:web)> import web.json
hold(profile:web)> diff web.json
hold(profile:web)> validate
```

Design requirements:

- **JSON remains canonical on disk**: the existing profile store shape should stay the authoritative internal storage format.
- **Cisco-like config is the import/export format**: export renders canonical JSON profiles as the same commands a user would type in captive mode; import parses those commands and writes validated JSON profiles. Common fields can use direct commands, while dotted structured fields can use `set path=value`.
- **Round-trip safe**: `export -> import -> export` should preserve semantically meaningful fields, even if comments/spacing are not preserved.
- **Stdout friendly**: export defaults to stdout so users can pipe to files, review tools, or version control.
- **Dry-run by default for risky writes**: import/apply should validate and show a summary before overwriting unless `--yes` is supplied.
- **Secret aware**: environment values may need redaction/export modes, e.g. `--redact-secrets`, `--include-secrets`, or future secret references.
- **Versioned schema in JSON**: include `version` in the canonical JSON so profile config can evolve without breaking old files.
- **Scope explicit**: both JSON and CLI-config import should say whether the target is `user` or `system`, but system writes should still require explicit elevation.
- **Machine inspection still available**: `inspect` / `--json` should print detailed JSON/object data for automation. Do not make JSON the default import/export UX.

Example CLI-config export:

```text
profile web
  description local docs server
  command /usr/bin/python3 -m http.server 9000
  cwd /srv/web
  set env.DATABASE_URL=postgres://localhost/app
  set env.LOG_LEVEL=info
  no interactive
  no tty
  alias myapp "run myapp --with-flags"
  multi deny
  readiness tcp 127.0.0.1 9000 timeout 10s
  cleanup stop-timeout 5s
exit
```

Equivalent usage:

```sh
hold export web > web.hold
hold import web.hold as web --dry-run
hold import web.hold as web --yes
hold inspect web > web.json              # JSON/object inspection, not config export
```

This makes the interactive shell a friendly editor for the same durable JSON artifact the CLI can manage, while giving humans a readable Cisco-style config transcript.


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
- **Filter mode** hides non-matching rows and shows match counts.
- **Type-to-filter mode** is the fast path: in views where text entry is otherwise meaningless, normal printable keystrokes immediately build the filter.

This should work consistently for:

- `hold logs <target>`
- `hold ps` / `hold ps -a` when opened interactively
- `hold profile` lists
- `hold active`, `hold recent`, and `hold runid` views
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
hold logs web --similar-line 120
hold logs web --similar 'missing config'
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

### Phase 2: Hybrid interactive shell

- Add `hold shell` line-oriented REPL.
- Support `help`, `ls`, `select`, `show`, `logs`, `down`, `kill`, `inspect`, `rm`, `prune`, `run`, `stop`, `ps`, `runid`, and `profile`.
- Implement command completion/history if practical.
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

The best drastic change for 0.4.0 is not to preserve old commands and add a menu beside them. It is to replace the legacy surface with a **one-language, three-surface product**:

- **One-shot CLI**: `hold run -d web`, `hold run -it web`, `hold logs web --follow`, `hold stop web`.
- **Captive shell**: `profile web`, then `run`, `logs`, `stop`, `env ...`, `no env ...`.
- **Config transcript**: the same captive-shell commands saved in a file and imported/applied.

This gives beginners an embarrassingly easy path while giving power users a composable, scriptable grammar. JSON remains the canonical on-disk store; the command language becomes the human UX contract.

## Product decisions status

The implementation-oriented decisions now live in [Hold 0.4 UX and CLI specification](HOLD_0_4_UX_SPEC.md) and [0.4.0 branch alignment](0.4.0-alignment.md). Current direction: use `hold` for the operator command, make `profile` the primary noun, keep ad-hoc launch as bare `hold <cmd>`, use `run`/`stop` as profile lifecycle verbs, use concrete run IDs for `down`/`kill`/`logs`/`inspect`/targeted `rm`, allow `show` as a friendly read-only alias/view command, keep `prune` as bulk inactive-run cleanup, and build the line-oriented Cisco/Docker-style captive shell before a richer TUI. Remaining release decisions include restart policy, exact `runid` namespace spelling, instance allocator scope, and how much of the full similarity/deque architecture must ship before 0.4.0.
