# Hold 0.4 UX and CLI specification draft

Date: 2026-06-23
Status: authoritative 0.4.0 branch UX specification and release-plan target

## 1. Product stance

On Hold remains the project identity; the intended 0.4.0 user-facing command is `hold`.

`hold` is a guardian shell for background jobs: run it, leave it, find it, watch it, stop it safely.

Version 0.4.0 is intentionally allowed to break the current CLI because the tool has no established user base. This document defines the target product direction for that release. Until the tracked alignment matrix marks a section implemented, treat it as a requirement or follow-up rather than a release claim. The goal is to replace the legacy action-first/alias-based surface with one coherent command language shared by:

1. one-shot CLI commands;
2. captive shell commands;
3. importable/exportable CLI transcript config files.

JSON remains the canonical on-disk profile storage format. CLI transcript config is a human editing/import/export format that compiles to the JSON profile model.

The hardening backlog in section 11 is part of the same 0.4.0 release plan. The redesigned CLI should not be released as stable until the product surface and hardening work are both complete. `VERSION` is still `0.3.9` in the current branch, so wording in this document is branch/spec status unless explicitly tied to implemented source and tests. See [0.4.0 branch alignment and follow-up matrix](0.4.0-alignment.md) for the current implemented/deferred split.

## 2. Core model

### 2.1 Profiles are definitions

A profile is a reusable launch definition for a tool. It is not itself a running process. Profiles may have zero, one, or many concrete executions associated with them over time.

Profiles may contain:

- command/argv;
- working directory;
- environment variables;
- terminal/TTY preference;
- Docker-familiar run flags and metadata (`--name`, `-d`, `-i`, `-t`, `-e`, `-p`, `-v`, `--rm`, `--restart`);
- multi-run and instance allocation policy;
- readiness/health metadata;
- cleanup settings;
- grants/access policy;
- description/tags.

Profile verbs should feel like service/template operations:

```sh
hold run web                  # start profile web
hold run web --force          # start one additional instance even if already active
hold run web --multi 3        # start exactly 3 instances; N is required
hold stop web                 # stop web only when exactly one active web run exists
hold stop web --all           # explicitly stop every active web run
```

`run` is a profile verb in the 0.4 target grammar. It should not become a namespace for managing existing executions. Avoid forms such as:

```sh
hold run web logs             # do not use
hold run web stop             # do not use
```

### 2.2 Run IDs are concrete executions

A run ID is one concrete execution and is the stable singular control handle. A run ID may have been created from a profile, or it may be an ad-hoc command with no profile.

Run records have:

- run ID;
- process identity;
- process group;
- state;
- log;
- optional profile label;
- scope;
- timestamps;
- TTY availability;
- observed launch/adoption facts;
- normalized launch/security facts.

Run-ID lifecycle verbs should act on concrete executions:

```sh
hold down 04a7dda8            # graceful stop for one concrete execution
hold kill 04a7dda8            # force stop for one concrete execution
hold logs 04a7dda8            # log viewer for one concrete execution
hold inspect 04a7dda8         # detailed JSON/object view, Docker-style
hold rm 04a7dda8              # remove only if inactive
hold rm --force 04a7dda8      # stop, verify, then remove an active run ID
```

Profile names may be accepted by run-oriented commands only as safe selectors:

```sh
hold logs web                 # valid only if web resolves to one relevant run
hold down web                 # valid only if web resolves to one active run
```

If a profile selector has zero matching runs, say so and suggest `hold run <profile>`. If it has multiple matching runs, refuse and show candidate run IDs unless the command has an explicit `--all` policy.

### 2.3 Ad-hoc launches stay magic and direct

The easiest path remains a bare command:

```sh
hold npm run dev
hold python server.py
hold -it bash                 # allocate an interactive TTY; detach leaves it running
hold --name web npm run dev   # create/label profile web from the launch recipe
```

Use `--` only when the executable name would otherwise be parsed as a Hold option or reserved command:

```sh
hold -- logs                  # launch an executable named "logs"
hold -it -- logs              # launch executable "logs" with an interactive TTY
```

Do not document strange flag-name executables as the primary mental model. The practical rule is: `--` ends Hold parsing when a child command conflicts with the Hold command language.

### 2.4 Observed vs normalized launch facts

The moment an execution becomes a run ID, Hold must solve enough path and identity information to make storage, profile conversion, public visibility, and privilege decisions deterministic.

Store both truths:

```json
{
  "observed": {
    "exe": "/usr/bin/python3",
    "argv": ["python3", "./server.py", "--config", "configs/dev.yml"],
    "cwd": "/home/rich/project"
  },
  "normalized": {
    "argv": [
      "/usr/bin/python3",
      "/home/rich/project/server.py",
      "--config",
      "/home/rich/project/configs/dev.yml"
    ]
  }
}
```

Observed facts are for audit and UI explanation. Normalized facts are authoritative for replay, profile creation, trust boundaries, and public/system eligibility.

Normalization rules:

1. Resolve the executable image path.
2. Capture the working directory.
3. For each relative argv token that is path-like (`./x`, `../x`, or contains `/`), resolve it against captured cwd when it exists.
4. For known interpreter shapes such as `python ./app.py`, `node ./server.js`, `ruby ./tool.rb`, `bash ./script.sh`, and `perl ./thing.pl`, treat the script argument as code payload and apply the same public/system trust rule used for executable targets.
5. If normalized executable or interpreted payload is inside the invoking user's home or otherwise not an allowed system target, it must not become a public/system run ID or protected public profile merely because it was launched through an elevated interpreter.

Profile conversion uses normalized launch facts, while profile editing can show observed facts as explanatory context.

## 3. Proposed command grammar and namespace rules

The 0.4.0 CLI surface should be a small set of top-level verbs plus navigable object namespaces. Top-level commands are fast paths; captive-shell contexts let users omit the target already selected.

### 3.1 Top-level command set

```text
Ad-hoc launch:
  hold [run-options] <cmd> [args...]
  hold [run-options] -- <cmd> [args...]         # only when cmd conflicts with Hold syntax

Profile lifecycle:
  hold run [run-options] <profile> [--force|--multi N]
  hold stop <profile> [--all]

Concrete execution lifecycle:
  hold down <target>
  hold kill <target>

Viewing and inspection:
  hold ps                                      # active run IDs
  hold ps -a                                   # active + inactive retained run IDs
  hold active                                  # active run-id view/list
  hold recent                                  # inactive/recent retained run-id view/list
  hold show <view|object>                      # read-only alias/view command
  hold logs <target> [--follow|-f] [--filter TEXT] [--similar TEXT] [--plain|--interactive]
  hold inspect <target>                        # detailed JSON/object view, Docker-style

Cleanup/removal:
  hold prune                                   # bulk cleanup of inactive past runs
  hold rm <inactive-runid|profile>
  hold rm --force <active-runid>

Profiles:
  hold profile                                 # enter/list profile namespace
  hold profile <name>                          # show/select profile
  hold profile save <runid> as <name>
  hold import <file> as <profile>
  hold export <profile> as <file>

Operations:
  hold shell                                   # explicit captive/operator shell
  hold doctor [target]
  hold help [topic]
```

`ps` always lists run IDs. If a run ID came from a profile, show the profile name in a column; otherwise show `-`.

`show` is allowed as a friendly read-only alias/view command. It should never be
the only way to reach important data, and it should not blur the core nouns:
`show runs` may route to `ps -a`, `show active` may route to `active`, `show
profile web` may route to profile display, and `show tree` may render navigable
views. Mutating operations should not hide under `show`.

`logs` owns log text. Former dump-style log output belongs under `logs <target> --plain`.

`inspect` is the canonical Docker-style structured object command. It displays
detailed JSON/object data for run IDs, profiles, and diagnostic store objects.
Do not keep `dump` as a primary 0.4 command; if a raw developer-only dump remains
temporarily, hide it from the main help and route users to `inspect`.

### 3.1.1 Docker-familiar launch and log flags

Hold should borrow Docker's familiar flag shape where the concepts map cleanly.
This makes the product easier to learn and easier to sell: users can transfer
muscle memory from `docker run`, `docker ps`, `docker logs`, and `docker inspect`
without learning a novelty grammar.

Launch flags:

| Short | Long | Target meaning in Hold |
| --- | --- | --- |
| `-d` | `--detach` | Run in the background and print the run ID. This is Hold's normal service posture; the explicit flag is still accepted for Docker familiarity and for disambiguating `-it` flows. |
| `-i` | `--interactive` | Keep stdin open for the child process without requiring a PTY. This supports raw interactive stdin flows, just like Docker `-i`. |
| `-t` | `--tty` | Allocate a pseudo-TTY for terminal behavior such as prompts, line editing, colors, and full-screen programs. |
| `-it` | `--interactive --tty` | Combine stdin-open plus PTY: the normal terminal/shell experience. For profiles, `hold run -it <profile>` starts the profile attached when inactive, or reconnects to the singular active interactive/TTY run for that profile. The detach key sequence leaves the run alive under Hold. |
| `-e KEY=VALUE` | `--env KEY=VALUE` | Set environment variables for the launch/profile. |
| — | `--env-file FILE` | Load environment variables from a file. |
| `-p SPEC` | `--publish SPEC` | Record published-port metadata for display, readiness/open helpers, and profile config. Hold is not a container runtime, so this must not falsely claim network namespace port forwarding unless a future backend actually implements it. |
| `-v SPEC` | `--volume SPEC` | Record volume/path metadata for profile config and future backend support. Hold host-process launches already see the host filesystem, so this is not isolation or remounting by itself. |
| — | `--name PROFILE` | Create/label a profile from the normalized launch recipe and associate the new run with that profile. |
| — | `--detach-keys SEQ` | Set the key sequence used to detach from an attached TTY without killing the run. Default should follow Docker familiarity: `Ctrl+P Ctrl+Q`, unless platform constraints require a documented alternative. |
| — | `--rm` | Remove run-id data/logs when the run stops; useful for tests and ephemeral commands. |
| — | `--restart POLICY` | Set restart behavior: `no`, `always`, `unless-stopped`, or `on-failure[:max-retries]`. |
| — | `--restart-delay DURATION` | Optional delay between restart attempts. |
| — | `--privileged` | Explicitly request privileged/elevated behavior where supported. This is a high-risk flag and should require clear authorization and prominent help text. |

Examples:

```sh
hold -d --name web -p 8080:3000 -e NODE_ENV=production npm run start
hold -i python
hold -it --rm bash
hold run -i pythonshell
hold run -it web
hold run -d --restart always web
hold run -d --restart on-failure:5 --restart-delay 5 web
hold logs -f -n 100 web
hold inspect web
```

`--name` rule:

- If the profile name does not exist, create a profile from the normalized launch recipe and label the first run with that profile.
- If the profile name exists and the launch recipe matches, treat it as a profile launch.
- If the profile name exists and the recipe differs, refuse and suggest `hold run <profile>`, `hold profile <profile> edit`, or an explicit replacement flow. Do not silently mutate a profile from a launch command.

Log flags:

| Short | Long | Meaning |
| --- | --- | --- |
| `-f` | `--follow` | Continue streaming/watching logs. |
| `-n N` | `--tail N` | Show only the last N lines before continuing/returning. |

If a profile target supplied to `logs` resolves to multiple active/recent run
IDs, refuse the ambiguous stream and show the filtered run-id list unless a
future explicit multi-log mode is designed.

Interactive TTY rule:

- `hold -i <cmd> [args...]` starts an ad-hoc command with stdin kept open but no PTY.
- `hold -t <cmd> [args...]` allocates a PTY; by itself it does not imply stdin should remain open.
- `hold -it <cmd> [args...]` starts an ad-hoc command with stdin and a PTY: the familiar interactive terminal shape.
- `hold run -i <profile>` starts or reconnects to a singular active interactive non-PTY profile run. Example: `hold run -i pythonshell` creates a stdin-open Python shell without PTY behavior.
- `hold run -it <profile>` starts the profile attached when no matching active run exists.
- If that profile already has exactly one active compatible interactive/TTY run, `hold run -i <profile>` or `hold run -it <profile>` reconnects to it instead of starting another copy.
- If that profile has exactly one active run that is not compatible with the requested interactive mode, refuse and suggest `logs` or a new `run --force -i/-it` only if an additional instance is intended.
- If that profile has multiple active runs, refuse and show candidate run IDs. Do not guess.
- No separate primary `console` or `attach` command is needed in the 0.4 target grammar.

### 3.2 `prune` vs `rm`

`prune` is bulk housekeeping:

```sh
hold prune
```

It removes inactive past runs that are no longer active. It must not stop active processes and must not remove profiles.

`rm` is targeted deletion:

```sh
hold rm 04a7dda8              # remove inactive run record/artifacts
hold rm web                   # remove profile web, subject to safety checks
hold rm --force 04a7dda8      # stop and remove one active run ID
```

`rm --force` should be concrete-run focused. If given a profile name that resolves to multiple active run IDs, refuse and show candidates unless a future explicit all-instances policy is designed.

### 3.3 Profile multiplicity and instance allocation

Default profile start is singular-safe:

```sh
hold run web
```

If profile `web` already has an active run, refuse and suggest:

```sh
hold run web --force          # start one more instance
hold run web --multi 3        # start exactly 3 instances
```

`--multi` requires a positive number. Boolean `--multi` is not a valid 0.4 target UX.

Profiles may contain instance-aware values. The friendly profile editing syntax may use compact forms such as:

```text
set env HTTP_PORT=8000++
set arg --port 8000++
```

Semantics: a profile-level allocator chooses deterministic per-instance values when `--force` or `--multi N` starts additional instances. The JSON model should store the allocator explicitly; the `++` form is human transcript sugar, not the only storage representation.

## 4. Captive shell and navigation

Bare `hold` on an interactive TTY should open a captive shell/dashboard. In non-TTY contexts, no-arg behavior should not unexpectedly enter an interactive UI.

The shell supports the same command language as the one-shot CLI, plus navigation commands such as `cd`, `back`, `pwd`, `ls`, and `tree`.

### 4.1 Namespace principles

The captive shell should feel like Cisco IOS, diskpart, and Docker had one small object shell:

1. **Bare namespace commands enter that namespace.** `profile` enters `hold(profile)>`; `runid` enters `hold(runid)>`.
2. **Namespaces list and select.** Namespace prompts expose `ls`, `select`, `help`, and `back` before object-specific actions.
3. **Objects expose actions.** A selected profile or run ID lets the user omit the target because the prompt already supplies context.
4. **Top-level verbs still exist.** `run`, `stop`, `down`, `logs`, `inspect`, `prune`, and `rm` remain first-class top-level commands; navigation is for discoverability, not mandatory ceremony. `show` may remain as a read-only alias/view command.
5. **`ps` always lists run IDs.** At root it lists active run IDs; `ps -a` includes inactive retained run IDs. Inside a profile, `ps` lists that profile's run IDs.
6. **Profiles can have run IDs; run IDs do not need profiles.** Profile-originated run IDs appear under both the profile context and the run-id views. Ad-hoc run IDs show profile as `-`.
7. **Top-level commands require explicit source/destination.** Context commands inherit context and can use shorter forms.

Prompt shape:

```text
hold>                         root
hold(profile)>                profile namespace
hold(profile:web)>            selected profile
hold(profile:web:abcd1234)>   selected run ID reached through profile web
hold(runid)>                  run-id namespace
hold(runid:abcd1234)>         selected run ID
```

Universal captive-shell commands:

```text
ls                 list children or relevant rows in this context
select <name|id>   enter a child object
help               show valid commands for this context
back               return to the previous context
..                 alias for back
/                  return to root
exit               leave the captive shell
```

### 4.2 Root and namespace contexts

Root prompt examples:

```text
hold> ps                       # active run IDs
hold> ps -a                    # active + inactive retained run IDs
hold> profile                  # enter profile namespace
hold> runid                    # enter run-id namespace
hold> run web                  # start profile web
hold> stop web                 # stop profile web only if singular active
hold> down abcd1234            # stop concrete run ID
hold> logs web                 # valid only if web resolves safely
hold> inspect abcd1234         # detailed JSON/object view
hold> prune                    # inactive history cleanup
hold> rm abcd1234              # targeted inactive run deletion
hold> rm --force abcd1234      # stop and delete active concrete run
hold> import ./web.hold as web
hold> export web as ./web.hold
```

Profile namespace:

```text
hold> profile
hold(profile)> ls
hold(profile)> select web
hold(profile:web)>
```

Inside `hold(profile)>`:

```text
ls                         list profiles
select <profile>           enter selected profile
run <profile>              start selected profile by name
rm <profile>               remove selected profile, subject to safety checks
import <file> as <profile> import explicit profile name
export <profile> as <file> export explicit profile name
help
back
```

Selected profile context:

```text
hold(profile:web)> ls                  # profile summary and available actions
hold(profile:web)> ps                  # active run IDs for web
hold(profile:web)> ps -a               # active + inactive retained run IDs for web
hold(profile:web)> run                 # start web
hold(profile:web)> run --force         # start one additional instance
hold(profile:web)> run --multi 3       # start exactly 3 instances
hold(profile:web)> stop                # stop only if singular active
hold(profile:web)> stop --all
hold(profile:web)> edit
hold(profile:web)> inspect             # detailed profile object
hold(profile:web)> import ./web.hold   # context supplies profile name
hold(profile:web)> export ./web.hold   # context supplies profile name
hold(profile:web)> rm                  # remove profile, subject to safety checks
hold(profile:web)> select abcd1234     # enter a run ID associated with this profile
hold(profile:web:abcd1234)>
```

Run-id namespace:

```text
hold> runid
hold(runid)> ls
hold(runid)> ps
hold(runid)> ps -a
hold(runid)> select abcd1234
hold(runid:abcd1234)>
```

Inside `hold(runid)>`, users browse execution records, not profiles. Commands are intentionally small: `ls`, `ps`, `ps -a`, `select <runid>`, `help`, and `back`.

Selected run-id context:

```text
hold(runid:abcd1234)> ls
hold(runid:abcd1234)> logs
hold(runid:abcd1234)> down
hold(runid:abcd1234)> kill
hold(runid:abcd1234)> inspect           # full structured run record
hold(runid:abcd1234)> save web          # create profile web from this run ID
hold(runid:abcd1234)> rm                # remove if inactive
hold(runid:abcd1234)> rm --force        # stop, verify, and remove if active
hold(runid:abcd1234)> back
```

If a selected run ID was reached through a profile, keep that route in the prompt and back-stack:

```text
hold(profile:web)> select abcd1234
hold(profile:web:abcd1234)> logs
hold(profile:web:abcd1234)> back
hold(profile:web)>
```

Top-level `as` rule:

```text
hold> profile save abcd1234 as web
hold> import ./web.hold as web
hold> export web as ./web.hold
```

Context rule:

```text
hold(runid:abcd1234)> save web          # run ID is selected, so no `as`
hold(profile:web)> export ./web.hold    # profile is selected, so no `as`
```

### 4.2.1 Reversible redirects

Views may connect backwards for discoverability, but `back` should follow the route the user took.

Example: from profile to its active concrete run view:

```text
hold(profile:web)> ps
RUNID     STATE    PROFILE  AGE  COMMAND
abcd1234  active   web      2m   npm run dev

hold(profile:web)> select abcd1234
hold(profile:web:abcd1234)> back
hold(profile:web)>
```

If a future shortcut redirects `hold(profile:web)> active` to an active-run view, the back-stack still returns to `hold(profile:web)>`, not to the canonical root active view.

### 4.3 Universal completion namespaces

Completion is a first-class feature of the CLI library, not a special case inside
`hold shell`. Any namespace that can be listed by `ps`, `ls`, or namespace entry
commands such as `profile`/`runid`, inspected by `inspect`, or described by `help`
should also be available to tab completion through the same provider API. The
command tree, help output, and completion candidates should share one source of
truth wherever practical.

Completion namespaces include at minimum:

```text
commands              help, profile, runid, run, stop, down, kill, ps, active, recent, show, logs, inspect, prune, rm, import, export, doctor, exit, back
profiles              named reusable launch definitions
runids                active and unpruned run IDs
recent-runids         newest unpruned run IDs, including inactive retained executions
recent-commands       past ad-hoc argv recipes that have not been saved as profiles
views                 active, recent, profile, runid, time, uptime, failed, stale, grants
profile-context       ls, ps, run, stop, edit, inspect, import, export, rm, select, help, back
runid-context         ls, logs, down, kill, inspect, save, rm, help, back
state-actions         logs, down, kill, inspect, prune/rm where valid for the selected object
```

The completion provider must be context-aware:

```text
hold> p<Tab>                 -> profile
hold> profile w<Tab>         -> profile web
hold> profile s<Tab>        -> profile save
hold> profile save <Tab>    -> recent run IDs plus `last`
hold> profile save 8f3a<Tab> as -> complete the run ID, then suggest `as`
hold(profile:web)> r<Tab>    -> run/rm local to the selected profile context
hold(runid:8f3a)> s<Tab>     -> save local to the selected run-id context
hold> stop <Tab>             -> profile names with singular active runs, plus explicit candidates after ambiguity help
hold> down <Tab>             -> active run IDs
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
hold> p<Tab>       # profile/ps/prune, completes only shared prefix if any
hold> p<Down>      # profile
hold> <Down>       # ps
hold> <Down>       # prune
hold> <Up>         # ps
hold> <cycle...>   # eventually returns to the original `p` or blank input
```

No ghost text is part of the design. Completion either inserts real text, shows
candidates, or cycles concrete candidate replacements in Hold-owned interactive
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
  set interactive off
  set tty off
  set publish 9000:9000
  set multi deny
  set readiness tcp 127.0.0.1 9000 timeout 10s
  set cleanup stop-timeout 5s
exit
```

Commands:

```sh
hold export web as web.hold            # write CLI transcript form
hold export web                        # print CLI transcript to stdout
hold import web.hold as web --dry-run  # parse transcript and show changes
hold import web.hold as web --yes      # compile transcript into canonical JSON profile storage
hold inspect web                       # detailed JSON/object view
```

Import/export are intentionally the human CLI transcript format. JSON remains the
canonical on-disk profile storage, but `export` should decompile that JSON into
the Cisco-style CLI form and `import` should compile the CLI form back into JSON.
Use `inspect` for JSON/object inspection; do not make JSON the default
import/export user experience.

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

Current branch v1 evidence: `hold view <target>` keeps plain output for scripts and opens an interactive TTY viewer by default when stdin/stdout are TTYs. `--plain` forces script-style output and `--interactive` fails closed when no TTY is available. The intended live-log UX is dynamic: `hold logs <target> --follow` / `hold view <target> --follow` opens the live viewer, printable keys update the top filter field per keystroke, Backspace relaxes the filter, and matching live output appears without restarting the command. `--filter TEXT` remains a scripting/seed option, not the primary human flow. Non-TTY follow streams matching lines until the recorded run exits; TTY follow refreshes while running and marks the view exited when the run ends. The v1 keys are printable type-to-filter, Backspace, Space to toggle the highlighted line as a similarity example, arrows/`j`/`k`, PgUp/PgDn, and `q`.

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

- v1 implementation: `hold logs <target> --follow` routes through `hold view --follow` for a dynamic TTY filter field. `--filter TEXT` can seed/script the same engine, but the human design is type-to-filter after the viewer is open. Plain `hold logs <target>` remains tail-compatible.
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

The new grammar should preserve On Hold’s good scripting discipline:

- stdout is for machine data when a command is intended to be captured;
- stderr is for human status and diagnostics;
- `--json` provides structured output;
- `--quiet` suppresses routine human status;
- exit codes remain meaningful and documented.

Example:

```sh
id="$(hold sleep 30)"
```

The command should print only the run ID to stdout on success.

## 10. Alignment status and decisions

Current branch status is tracked in [0.4.0 branch alignment and follow-up matrix](0.4.0-alignment.md). That matrix is part of this specification: if the matrix says a feature is a follow-up, this document describes the intended product target rather than current release readiness.

Resolved decisions for the current 0.4.0 direction:

1. Use **On Hold** for the project and **`hold`** for the intended 0.4.0 operator command.
2. Keep ad-hoc launch as bare `hold <cmd> [args...]`; use `--` only when a child executable conflicts with Hold syntax.
3. Use `run` and `stop` as profile lifecycle verbs, not as a namespace for managing existing executions. `hold run web logs` remains invalid.
4. Use concrete run IDs as the safest singular handles for `down`, `kill`, `logs`, `inspect`, and targeted `rm`. Profile-name selectors are allowed only when they resolve safely.
5. Keep `prune` for bulk inactive history cleanup and `rm` for targeted deletion; `rm --force` is the explicit stop-and-remove form for an active concrete run ID.
6. Treat current Space-selected, local deterministic similarity as the minimum implemented v1 slice until the fuller `S/X/A/D/U` interaction model is implemented.

Still-open release decisions:

1. Whether restart exists in 0.4.0 at all, and if so whether it is profile-only, singular-run-only, or requires an explicit all-instances policy.
2. Whether the run-id namespace should be spelled `runid`, `id`, or both, while avoiding public `run` as an execution-record noun.
3. How much of the instance allocator syntax (`8000++`) must ship with `--multi N`, versus documenting the JSON model first and adding transcript sugar later.
4. Whether full before/visible/after deques, raw tail ring, sparse indexes, and richer similarity controls are required before the 0.4.0 release or can remain post-0.4 follow-up with the current dynamic filter as the core feature.

## 11. 0.4.0 engineering hardening backlog

The 0.4.0 CLI redesign should ship with hardening work that makes the new surface safe to iterate on. These items are release criteria for the breaking 0.4.0 line, not optional polish or a separate post-0.4 hardening phase.

### 11.1 Stabilize the test suite

Requirements:

- Modify `tests/test_hold.sh` so every `run_test` invocation has a per-test timeout.
- Default timeout: `HOLD_TEST_TIMEOUT=25` seconds.
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
- A hung TTY/process test fails with actionable diagnostics.

### 11.2 Tighten CLI argument validation

Requirements:

- In `src/main.c`, enforce exact arity for current owned commands while the parser is being replaced/refactored:
  - `tail`: exactly one target;
  - `inspect`: exactly one target;
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

### 11.4 Harden interactive TTY authorization

Requirements:

- Add a platform helper to retrieve AF_UNIX peer credentials:
  - Linux: `SO_PEERCRED`;
  - macOS/BSD: `getpeereid` or `LOCAL_PEERCRED` where available.
- In the interactive TTY broker, after `accept`, verify peer UID before replaying output or forwarding input.
- Permit:
  - run owner;
  - root;
  - any explicitly intended invoking user for elevated/system runs, if current behavior relies on that. Preserve this intentionally and test it.
- Keep TTY socket file permissions at `0600`.
- Add tests proving unrelated users cannot connect to an interactive TTY run.

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

- Audit `hold_mkdir_p0700` and `hold_mkdir_p_mode`.
- Replace stat-following path walks with `lstat`/`openat`-style symlink refusal where practical.
- Ensure `chmod`/`chown` operations cannot be redirected through symlinks.
- Rename `hold_read_owned_file_no_symlink` if ownership is not checked, or add ownership/mode checks where security-sensitive.
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
- The recommended path for true standalone Linux installs is `HOLD_FLAVOR=musl-static` or an equivalent musl-targeted static build.

### 11.10 Harden release and installer scripts

Requirements:

- Remove any release step that deletes an existing GitHub release before publishing.
- Use `overwrite_files` or an immutable release policy instead.
- Replace installer temp dir creation with `mktemp -d` and `umask 077`.
- Prefer mandatory `SHA256SUMS` verification over release-body checksum scraping.
- Validate archive layout instead of finding the first file named `hold`/`hold`.
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

- full `hold` command grammar and product direction in this document are implemented coherently and documented;
- profile/run target semantics are unambiguous;
- CLI transcript import/export is specified and tested;
- pager/live-filter/similarity-filter behavior is implemented to the 0.4.0 interaction contract, with later minor releases limited to refinement rather than deferring the core product feature;
- test suite cannot hang indefinitely;
- parser rejects unsupported flags and extra positionals;
- Interactive TTY authorization is peer-credential enforced;
- signal safety is stricter than display liveness;
- system store and atomic writes reject symlink/temp-file attacks;
- source ZIP builds report `VERSION` correctly;
- installer/release flow fails closed and avoids destructive release deletion;
- stale review claims are removed or replaced with current verification evidence.
