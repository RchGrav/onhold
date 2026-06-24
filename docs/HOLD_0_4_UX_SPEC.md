# Hold 0.4 UX and CLI specification draft

Date: 2026-06-23
Status: authoritative 0.4.0 branch UX specification and release-plan target

## 1. Product stance

On Hold remains the project identity; the intended 0.4.0 user-facing command is `hold`.

`hold` is a guardian shell for background jobs: run it, leave it, find it, watch it, stop it safely.

Version 0.4.0 is intentionally allowed to break the current CLI because the tool has no established user base. This document defines the target product direction for that release. Until the tracked alignment matrix marks a section implemented, treat it as a requirement or follow-up rather than a release claim. The goal is to replace the legacy action-first/alias-based surface with two deliberately familiar surfaces:

1. **Normal shell CLI:** mimic Docker command names, option names, and flag semantics as closely as Hold's host-process model allows. Ease-of-use tweak: `run` may be optional for ad-hoc launches, so `hold -d sleep 30` is equivalent to the Docker-shaped `hold run -d sleep 30`.
2. **Captive CLI:** clone the Cisco IOS operator experience: User EXEC (`hold>`), Privileged EXEC (`hold#`), global configuration (`hold(config)#`), profile sub-configuration (`hold(config-profile:name)#`), `?` help, `enable`, `configure terminal`, `end`, `write`, `no`, `default`, staged config, and commit/install validation.

JSON remains the canonical on-disk profile storage format. CLI transcript config is a human Cisco-IOS-style editing/import/export format that compiles to the JSON profile model.

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

Concrete naming and size rules:

- Profile names are human handles for launching tools. Maximum length is **64** characters/bytes in the 0.4 target unless a later audited protocol budget proves this must shrink further.
- Profile names may be used as safe selectors only when the operation can resolve them unambiguously.
- Profile hashes are full SHA-256 hex strings: **64 lowercase hex characters**.
- Existing/current run IDs are 8 lowercase hex characters with reserved sentinels inherited from the 0.3.x model. The 0.4 target may extend displayed/generated run IDs to **12 lowercase hex characters** for Docker familiarity, but that is a deliberate storage/protocol migration decision, not an accidental rename.
- If sentinel selectors remain in the privileged protocol, `00000000` means “start/no concrete run yet” and all-`f` means an explicitly approved all-runs selector. The all-`f` sentinel length must match the active run-selector length.

Profile verbs should feel like service/template operations:

```sh
hold run web                  # start profile web in foreground, streaming stdout/stderr
hold run -d web               # start profile web detached and print the run ID
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
hold stop 04a7dda8            # graceful stop for one concrete execution, Docker-style
hold kill 04a7dda8            # force stop for one concrete execution
hold logs 04a7dda8            # log viewer for one concrete execution
hold inspect 04a7dda8         # detailed JSON/object view, Docker-style
hold rm 04a7dda8              # remove only if inactive
hold rm --force 04a7dda8      # stop, verify, then remove an active run ID
```

Profile names may be accepted by run-oriented commands only as safe selectors:

```sh
hold logs web                 # valid only if web resolves to one relevant run
hold stop web                 # valid only if web resolves to one active run or profile instance
```

If a profile selector has zero matching runs, say so and suggest `hold run <profile>`. If it has multiple matching runs, refuse and show candidate run IDs unless the command has an explicit `--all` policy.

### 2.3 Ad-hoc launches stay magic and direct

The easiest path remains a bare command:

```sh
hold npm run dev              # foreground: stream stdout/stderr, no stdin unless -i
hold -d python server.py      # detached: print run ID
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

### 2.5 Public/private global profile model

Global/root-managed profiles are ordinary reusable profiles first. They are **not**
grants. Grants and sudoers entries are an optional access-control layer over
root-managed profiles.

The root-managed profile model has two mandatory halves:

1. **Private root profile material**: canonical launch/rule data used to start
   the command and validate privileged requests.
2. **Public redacted discovery material**: safe metadata that lets normal users
   discover root-managed profiles/runs and trigger the expected sudo/elevation
   flow.

The public side is not optional. A root-managed profile or run that cannot be
discovered through redacted public state breaks the product model: users cannot
select it, understand why sudo is being requested, or recover the normal
profile/run relationship.

Public redacted state must not expose private launch secrets or sensitive root
paths. As a rule, do not publish raw argv, environment, log paths, console
socket paths, private store paths, process identity internals, or grant rule
payloads unless a later security review explicitly blesses a field as safe.

The current branch still uses aggregate JSON files inherited from the 0.3.x
model (`profiles.json` plus a public profile-name map under `public/`). That is
acceptable as current storage reality. Moving to one-file-per-profile is a
separate storage migration decision and must not be implied by the 0.4 UX
rename alone.

### 2.6 Profile hash authority, subject grants, and invalidation

The profile hash is a derived capability digest over the **private profile
material used by the granted subject**. It is not user-editable profile state and
must not become authoritative merely because a JSON object contains a hash field.

Grant storage has a subject-scoped private half:

```text
<root-store>/grants/
  users/
    alice/
      web-server.json        # private profile copy granted to alice
  groups/
    ops/
      web-server.json        # private profile copy granted to %ops
```

The exact path spelling may vary by platform/store migration, but the model is
mandatory:

- Users and groups are separate subject namespaces.
- Each subject has a private grant/profile folder.
- Granting a profile copies the private profile material into the subject's
  folder.
- By default, that subject copy means: “this subject may run this exact CLI as
  root, with the embedded/default command and current allowed operations.”
- The subject copy starts as the canonical root profile material, not as a
  second independent product concept. Advanced per-subject narrowing can be
  added later, but 0.4.0 does not require advanced profile editing.

Hash authority rule:

1. Resolve the requesting subject and profile name.
2. Load that subject's private grant/profile copy.
3. Compute SHA-256 from the full subject-private grant/profile material.
4. Compare the computed hash with the hash carried by a capability or sudoers
   entry.
5. Refuse the request if they differ.

For the inherited 0.3.x/current model, the hash input is the resolved absolute
binary path plus argc/argv framing. For 0.4 grants, the hash input is the
full subject-private profile/grant copy, not just the embedded CLI. That means
the immutable command/argv recipe, allowed operations, persistent Docker-shaped
profile flags (`interactive`, `tty`, `detach`, `restart`, etc. as implemented),
operation/default-run policy, and any explicit 0.4 request-validation rules are
all hash material. In the current implementation the serialized private JSON
file is the hash material, so changing `actions` or any other private field
invalidates the cap. If/when this moves to a decoded canonical tree, that tree
must still include every semantic private field, not just `binary_path`/`argv`.
Do not include non-semantic descriptive metadata such as comments, display
labels, timestamps, or presentation formatting in future canonical material.

If profile material changes in a way that affects privileged launch semantics,
the hash changes. Any dependent subject-private copies and sudoers entries must
then be regenerated by the official tooling, invalidated, or intentionally
retained against the old immutable material. A profile edit must never silently
leave a sudoers entry pointing at a stale rule set while appearing to grant the
newly edited profile.

Collision/update rule:

- A subject may have at most one active grant copy for a profile name unless a
  future schema explicitly versions retained old copies.
- Regrant/update overwrites the subject-private copy only through the official
  atomic writer path, recomputes the SHA-256, rewrites the matching sudoers.d
  entry, validates with `visudo -cf`, then installs atomically.
- If sudoers update fails, the profile/grant update must roll back or leave the
  old grant/sudoers pair intact; never install a private copy whose sudoers hash
  does not match.

### 2.7 0.4 sudoers/capability protocol

The 0.4 privileged protocol makes the sudoers-visible command readable while
keeping semantic validation inside Hold. The variable operation request is
encoded into a single token and validated root-side against the subject-private
profile copy.

Target sudoers command shape:

```text
<subject> ALL=(root) NOPASSWD: /usr/bin/hold ^run <profile-name> --cap <subject-profile-sha256> <encoded-request>$
```

Concrete example:

```text
alice ALL=(root) NOPASSWD: /usr/bin/hold ^run web-server --cap af4764571f217a9bd2c50d8e97c54239bcacb15c835100e59fda84cb33603d14 [A-Za-z0-9_-]{1,768}$
```

Protocol specifics:

- The sudoers-visible command includes the human profile handle:
  `run web-server`.
- `--cap <subject-profile-sha256>` carries the full 64-hex SHA-256 digest of the
  subject-private grant/profile copy.
- `<encoded-request>` is one bounded token containing the variable request:
  operation verb (`start`, `stop`, future verbs), run selector/all/allocation
  choices, supplied parameters, and other user-controlled options that the
  private profile rules allow.
- The encoded request should use a shell/sudoers-friendly alphabet such as
  unpadded base64url (`[A-Za-z0-9_-]`) so the privileged boundary does not rely
  on spaces or shell quoting inside the variable payload.
- The example `{1,768}` request budget is intentional protocol pressure:
  sudoers and interactive terminal line-length limits must be budgeted before
  adding fixed fields.
- Sudoers should gate only a tight Hold-owned invocation shape. It should not
  try to validate every profile parameter semantically.
- Root-side Hold must resolve the subject, load the subject-private profile copy,
  recompute and verify the SHA-256, decode the request token, validate decoded
  values against that private copy, and only then start/operate.
- The encoded request is the authority for the requested operation. Do not infer
  the privileged verb from the sudoers regex alone.
- The encoded request must not repeat the profile name. The fixed CLI/sudoers
  envelope already carries `run <profile-name>`, and the cap hash binds that
  profile to the subject-private copy. Duplicating it inside JSON creates a
  second field that can drift.
- The root receiver must reject malformed base64url, oversized decoded payloads,
  unknown schema versions, unsupported verbs, cap/profile mismatches, and any
  operation that is not allowed by the subject-private copy.

Minimum decoded request schema:

```json
{
  "v": 1,
  "op": "start",
  "selector": null,
  "all": false,
  "force": false,
  "args": []
}
```

Operation semantics for 0.4.0:

- `start`: run the embedded/default CLI from the subject-private profile copy.
  Starting a second active instance requires explicit `--force` in the shell UX
  and `force:true` in the encoded request. Historical/current `--multi` wording
  should be updated to point users at `--force` for the “start one more” case.
  Numeric `--multi N` may remain/return later for explicit counted starts.
- `stop`: may target the profile name only when exactly one active run ID exists
  for that profile. If zero active runs exist, report nothing to stop. If more
  than one active run exists, refuse and show candidate run IDs unless `all:true`
  is explicitly encoded/authorized.
- Concrete run-ID operations remain singular. A run ID does not need to come from
  a profile, but subject-private grants operate only on runs that match the
  granted profile copy and subject policy.
- `--force` is the 0.4 shell word for starting an additional instance from a
  profile when one is already active. Update old “use --multi” diagnostics to
  “use --force” when the intended action is one extra instance.

Sudoers matcher constraint:

- sudo does **not** provide PCRE matching for command arguments. Do not design or
  document a PCRE/glob toggle.
- sudo 1.9.10 and newer can use POSIX ERE, egrep-style argument matching when
  the command argument pattern is anchored with `^...$`.
- Older sudo uses fnmatch/glob-style matching. That is not a syntax-equivalent
  fallback for this capability shape.
- fnmatch has no bounded repetition, so it cannot preserve an ERE budget such as
  `[A-Za-z0-9_-]{1,768}`. The closest alphabet-only glob is length-unbounded and
  moves the length cap entirely into root-side request decoding.
- The matching model may also differ: old sudo/fnmatch must not be assumed to
  match the same joined command-argument string as the ERE form. Verify exact
  sudo behavior before allowing any degraded old-sudo grant path.

Implementation consequence: parameterized encoded-request grants should either
require sudo with POSIX ERE support and refuse installation on older sudo, or
explicitly install a degraded old-sudo form whose weakened structural gate is
called out and whose alphabet/length/semantic checks are enforced root-side as
mandatory defense-in-depth. Do not claim a portable glob form preserves the ERE
structure; it does not.

## 3. Proposed command grammar and namespace rules

The 0.4.0 normal shell CLI should be Docker-shaped. Command names and switches should match Docker where Hold has an equivalent concept; deviations must be intentional and documented. The most important ease-of-use deviation is optional `run` for ad-hoc launches.

### 3.1 Normal shell command set

The normal shell CLI is Docker-shaped. `run` is supported for Docker familiarity, but optional for ad-hoc ease of use.

```text
Launch:
  hold run [run-options] <cmd|profile> [args...]
  hold [run-options] <cmd> [args...]            # ease-of-use alias for ad-hoc run
  hold [run-options] -- <cmd> [args...]         # only when cmd conflicts with Hold syntax

Listing and inspection:
  hold ps                                      # active run IDs, Docker-shaped
  hold ps -a                                   # active + inactive retained run IDs
  hold logs <target> [--follow|-f] [--tail|-n N] [--filter TEXT] [--similar TEXT] [--plain|--interactive]
  hold inspect <target>                        # detailed JSON/object view, Docker-style

Lifecycle:
  hold stop <target> [--all]
  hold kill <target>
  hold prune                                   # bulk cleanup of inactive past runs
  hold rm <inactive-runid|profile>
  hold rm --force <active-runid>

Hold-specific utility/config entry points:
  hold                                         # enter Cisco IOS-style captive CLI when TTY
  hold shell                                   # explicit captive/operator shell
  hold import <file> as <profile>              # import IOS-style transcript
  hold export <profile> as <file>              # export IOS-style transcript
  hold doctor [target]
  hold help [topic]
```

`ps` always lists run IDs. If a run ID came from a profile, show the profile name in a column; otherwise show `-`.

`show` belongs primarily to the Cisco IOS-style captive CLI. If exposed from the
normal shell as a convenience, it must remain read-only and route to canonical
Docker-shaped commands or IOS views; mutating operations must not hide under
`show`.

`logs` owns log text. Former dump-style log output belongs under `logs <target> --plain`.

`inspect` is the canonical Docker-style structured object command. It displays
detailed JSON/object data for run IDs, profiles, and diagnostic store objects.
Do not keep `dump` as a primary 0.4 command; if a raw developer-only dump remains
temporarily, hide it from the main help and route users to `inspect`.

### 3.1.1 Docker-familiar launch and log flags

Hold should reshape existing/target Hold capabilities behind Docker-familiar
flag names wherever Hold has the same operator capability. This is not a
speculative container feature list: it is a CLI rename/remapping plan for
capabilities Hold already has or is implementing, so users can transfer muscle
memory from `docker run`, `docker ps`, `docker logs`, and `docker inspect`
without learning a novelty grammar.

Launch flags:

| Short | Long | Target meaning in Hold |
| --- | --- | --- |
| `-d` | `--detach` | Run in the background and print the run ID. Without `-d`, Hold stays attached to the child output like `docker run` foreground mode. |
| `-i` | `--interactive` | Keep stdin open for the child process without requiring a PTY. This supports raw interactive stdin flows, just like Docker `-i`. |
| `-t` | `--tty` | Allocate the existing Hold PTY/console capability using Docker-shaped spelling. Replaces shell-side `console` as the primary 0.4 UX. |
| `-it` | `--interactive --tty` | Combine stdin-open plus Hold's PTY/console capability: the normal terminal/shell experience. For profiles, `hold run -it <profile>` starts the profile attached when inactive, or reconnects to the singular active interactive/TTY run for that profile. The detach key sequence leaves the run alive under Hold. |
| `-e KEY=VALUE` | `--env KEY=VALUE` | Set environment variables for the launch/profile. |
| — | `--env-file FILE` | Load environment variables from a file. |
| `-p SPEC` | `--publish SPEC` | Record published-port metadata for display, readiness/open helpers, and profile config. Hold is not a container runtime, so this must not falsely claim network namespace port forwarding unless a future backend actually implements it. |
| `-v SPEC` | `--volume SPEC` | Record volume/path metadata for profile config and future backend support. Hold host-process launches already see the host filesystem, so this is not isolation or remounting by itself. |
| — | `--name PROFILE` | Create/label a profile from the normalized launch recipe and associate the new run with that profile. |
| — | `--detach-keys SEQ` | Set the key sequence used to detach from an attached TTY without killing the run. Default should follow Docker familiarity: `Ctrl+P Ctrl+Q`, unless platform constraints require a documented alternative. |
| — | `--rm` | Remove run-id data/logs when the run stops; useful for tests and ephemeral commands. |
| — | `--restart POLICY` | Set restart behavior: `no`, `always`, `unless-stopped`, or `on-failure[:max-retries]`. |
| — | `--restart-delay DURATION` | Optional delay between restart attempts. |
| — | `--privileged` | Docker-shaped spelling for Hold's elevated/root-managed execution path. This is an authorization-sensitive flag and should require clear validation and prominent help text. |

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
- If the profile name exists and the recipe differs, refuse and suggest `hold run <profile>`, Cisco IOS captive configuration (`hold`, `enable`, `configure terminal`, `profile <name>`), or an explicit replacement flow. Do not silently mutate a profile from a launch command.

Log flags:

| Short | Long | Meaning |
| --- | --- | --- |
| `-f` | `--follow` | Continue streaming/watching logs. |
| `-n N` | `--tail N` | Show only the last N lines before continuing/returning. |

If a profile target supplied to `logs` resolves to multiple active/recent run
IDs, refuse the ambiguous stream and show the filtered run-id list unless a
future explicit multi-log mode is designed.

### 3.1.2 Operator flag reference

This is the user-facing quick reference for Docker-shaped flags. The wording is
intentionally familiar to Docker users, but each flag must map to a real Hold
capability or recorded Hold metadata. Hold runs host processes/profiles; `-p`,
`-v`, and `--privileged` must not imply container isolation unless a later
backend actually provides it.

Quick lookup:

| Short | Long | Purpose | Common use |
| --- | --- | --- | --- |
| `-d` | `--detach` | Run the process/profile in the background, leaving the terminal free. | Long-running services |
| `-i` | `--interactive` | Keep stdin open for sending raw data/commands. | Interactive shells, debugging |
| `-t` | `--tty` | Allocate Hold's PTY/console path under Docker-shaped spelling. | Interactive terminal sessions |
| `-e` | `--env` | Pass environment variables into the process/profile. | Configuration management |
| `-n` | `--tail` | Limit log history output. | Log inspection with `hold logs` |
| `-f` | `--follow` | Keep log streams open and watch live. | Real-time monitoring with `hold logs` |
| — | `--detach-keys` | Change the escape sequence used to leave a TTY without killing the run. | Custom TTY exit behavior |
| — | `--rm` | Auto-delete run-id data/logs when the run stops. | Cleanup, tests, ephemeral runs |
| — | `--restart` | Set crash/reboot/relaunch handling rules. | Reliability, auto-recovery |
| — | `--privileged` | Use Hold's elevated/root-managed execution path. | Root-managed/profile operations; use with caution |

Terminal and interactivity examples:

```sh
# Start a service/profile and return to the terminal immediately
hold run -d web

# List running executions
hold ps

# Keep stdin open without a PTY
hold run -i pythonshell

# Full interactive terminal mode
hold run -it shell
```

Environment examples:

```sh
# Set one environment variable
hold run -e DATABASE_URL=postgres://localhost myapp

# Set multiple environment variables
hold run -e DATABASE_URL=postgres://localhost \
         -e DEBUG=true \
         -e LOG_LEVEL=info myapp

# Load environment from a file
hold run --env-file ./config.env myapp
```

Logging examples:

```sh
# View last 50 lines of logs
hold logs -n 50 <profile-or-runid>

# Stream logs in real time
hold logs -f web

# Combine recent history plus live follow
hold logs -f -n 50 web
```

If the log target is a profile, it must resolve to one relevant run. If the
profile has multiple active/recent runs, Hold presents the filtered run-id list
and requires a concrete run ID or an explicitly designed multi-log mode.

Cleanup and restart examples:

```sh
# Run a profile and delete run-id data/logs when it stops
hold run --rm test-profile

# Always restart a detached profile
hold run -d --restart always web

# Restart on failure with max retries and delay
hold run -d --restart on-failure:5 --restart-delay 5 web

# Options: no | always | unless-stopped | on-failure[:max-retries]
```

Advanced examples:

```sh
# Change detach sequence to Ctrl+A
hold run -it --detach-keys="ctrl-a" shell

# Request Hold's elevated/root-managed execution path where authorized
hold run --privileged maintenance
```

`--privileged` is the Docker-shaped analog for Hold's elevated/root-managed
execution path. It is not a container capability boundary and does
not create isolation. It should require Hold's normal elevated-profile/grant
validation and must be described with security warnings anywhere it appears in
help or docs.

Common combinations:

```sh
# Typical web app setup
hold run -d -p 8080:3000 -e NODE_ENV=production web

# Interactive debugging session
hold run -it --rm -v $(pwd):/code debug-shell

# Persistent service with auto-restart
hold run -d --restart always -v db_data:/data database

# Log monitoring
hold logs -f -n 100 web
```

Quick reference by use case:

| Scenario | Flags |
| --- | --- |
| Run a service in background | `-d` |
| Interactive shell | `-it` |
| Persist path/volume metadata | `-v /host:/name` |
| Configure runtime | `-e KEY=VALUE` |
| Temporary test | `--rm` |
| Auto-restart on crash | `--restart always` |
| Watch logs live | `-f` with `hold logs` |

Interactive TTY rule:

- `hold -i <cmd> [args...]` starts an ad-hoc command with stdin kept open but no PTY.
- `hold -t <cmd> [args...]` allocates a PTY; by itself it does not imply stdin should remain open.
- `hold -it <cmd> [args...]` starts an ad-hoc command with stdin and a PTY: the familiar interactive terminal shape.
- `hold run -i <profile>` starts or reconnects to a singular active interactive non-PTY profile run. Example: `hold run -i pythonshell` creates a stdin-open Python shell without PTY behavior.
- `hold run -it <profile>` starts the profile attached when no matching active run exists.
- If that profile already has exactly one active compatible interactive/TTY run, `hold run -i <profile>` or `hold run -it <profile>` reconnects to it instead of starting another copy.
- If that profile has exactly one active run that is not compatible with the requested interactive mode, refuse and suggest `logs` or a new `run --force -i/-it` only if an additional instance is intended.
- If that profile has multiple active runs, refuse and show candidate run IDs. Do not guess.

Foreground/detach behavior:

- Without `-d`, `hold <cmd>` and `hold run <profile>` stay in the foreground. Child stdout and stderr stream to the invoking terminal.
- Without `-i`, child stdin is not connected for input; the user may simply see output or a quiet/blinking cursor while the process runs.
- `-i` connects stdin without adding terminal behavior.
- `-t` allocates terminal behavior.
- `-d` is the scriptable/background form and is the form that prints only the run ID to stdout.
- `Ctrl-C` and other explicit terminal signals in foreground mode are forwarded to the child process group according to the selected terminal mode.
- Closing the client terminal or losing the foreground client should be treated as a detach when Hold can do so safely; it should not be the normal lifecycle mechanism for stopping a run. Users stop runs with `stop`, `kill`, or `rm --force`.
- The detach key sequence leaves the run alive and returns the user to the shell/captive CLI.

Detach key handling:

- Default detach sequence: `Ctrl+P` then `Ctrl+Q`, matching Docker muscle memory.
- If `Ctrl+Q` arrives within 500ms after `Ctrl+P`, Hold consumes both keys and detaches; neither byte is forwarded to the child.
- If `Ctrl+Q` does not arrive within 500ms, Hold forwards the pending `Ctrl+P` to the child and continues normally.
- If a different byte arrives before the timeout, Hold forwards the pending `Ctrl+P` and then handles/forwards the new byte normally.
- `--detach-keys SEQ` changes this sequence for the current run/profile.

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
env HTTP_PORT 8000++
argv --port 8000++
```

Semantics: a profile-level allocator chooses deterministic per-instance values when `--force` or `--multi N` starts additional instances. The JSON model should store the allocator explicitly; the `++` form is human transcript sugar, not the only storage representation.

## 4. Captive CLI: Cisco IOS clone

Bare `hold` on an interactive TTY should open a captive CLI. This surface is not a Docker clone. It is intentionally a Cisco IOS clone with Hold nouns.

Design rule: if a network operator who knows Cisco IOS can guess the mode, prompt, help behavior, staging behavior, and commit flow, Hold is doing the right thing.

### 4.1 IOS mode model

Hold captive CLI modes:

| Mode | Prompt | Purpose |
| --- | --- | --- |
| User EXEC | `hold>` | Safe, limited operational commands. |
| Privileged EXEC | `hold#` | Administrative operational commands plus entry to configuration. |
| Global configuration | `hold(config)#` | Configure global dictionaries and enter profile configuration. |
| Profile sub-configuration | `hold(config-profile:web)#` | Stage changes for one profile. |

Mode commands:

- `?` shows valid commands for the current mode.
- `enable` enters Privileged EXEC, with authentication when configured.
- `disable` returns from Privileged EXEC to User EXEC.
- `configure terminal` enters global configuration mode.
- `profile <name>` enters profile sub-configuration mode.
- `exit` returns one mode outward or closes the session from EXEC mode.
- `end` returns to Privileged EXEC from any configuration mode.
- `write` persists the running configuration from Privileged EXEC.
- `no <command>` negates/removes configuration.
- `default <command>` resets configuration to the default.
- `commit` validates and installs staged profile changes.

### 4.2 User EXEC mode

You start in User EXEC mode with limited commands:

```text
hold> ?
Exec commands:
  attach     Attach to a compatible running instance when supported
  console    Attach with a PTY (interactive)
  enable     Enter privileged mode
  list       List running instances
  ping       Check daemon-less liveness of a run
  run        Start an instance from a profile or binary
  show       Show running system information
  exit       Close the session
```

`show ?` behaves like IOS contextual help:

```text
hold> show ?
  aliases    Show public alias table
  runs       Show running instances (runid, alias, hash)
  profiles   Show profile names and hashes (no details)
  version    Show hold version
```

Example:

```text
hold> show runs
RUNID     ALIAS      HASH        STATE
a3f7b2c1  web        7f3e9c2a    running
9d4e8f02  report     1c5d3b7f    running
```

In the captive CLI, `alias` means an operator-facing command/name alias table. It must not revive the old “alias as launch definition” model; profiles remain launch definitions.

### 4.3 Privileged EXEC mode

```text
hold> enable
Password:
hold#
```

Privileged EXEC exposes administrative commands and configuration entry:

```text
hold# ?
Exec commands:
  attach      Attach to a compatible running instance when supported
  configure   Enter configuration mode
  console     Attach with a PTY (interactive)
  grant       Grant a user capability for a profile
  revoke      Revoke a user capability
  list        List running instances
  run         Start an instance
  show        Show system information
  write       Write running config to storage
  disable     Return to user EXEC mode
  exit        Close the session
```

Configuration entry mirrors IOS:

```text
hold# configure ?
  terminal   Configure from the terminal

hold# configure terminal
Enter configuration commands, one per line. End with CNTL/Z.
hold(config)#
```

### 4.4 Global configuration mode

```text
hold(config)# ?
Configuration commands:
  profile    Create or edit a profile
  pattern    Manage the pattern dictionary
  switch     Manage the switchionary
  no         Negate a command
  default    Reset a command to default
  exit       Exit configuration mode
  end        Return to privileged EXEC mode
```

Profile entry:

```text
hold(config)# profile ?
  WORD       Name of the profile to create or edit

hold(config)# profile web
hold(config-profile:web)#
```

### 4.5 Profile sub-configuration mode

Profile sub-configuration mode stages one profile's configuration. Changes are not installed until `commit` succeeds.

```text
hold(config-profile:web)# ?
Profile configuration commands:
  binary        Set the target executable (immutable base)
  argv          Append a base argv token
  env           Set an environment variable
  alias         Define a profile-local alias
  param         Expose a validated optional parameter
  multi         Allow multiple concurrent instances
  interactive   Keep stdin open for profile runs
  tty           Allocate a pseudo-TTY for profile runs
  detach        Default profile runs to detached mode
  pty-shim      Start under the PTY shim
  no            Negate a command
  default       Reset a command to default
  info          Show staged profile state
  commit        Re-hash, regenerate sudoers, validate, install
  exit          Return to global config mode
```

Help is contextual:

```text
hold(config-profile:web)# binary ?
  WORD       Absolute path to the executable

hold(config-profile:web)# param ?
  WORD       Long flag to expose (e.g. --port)

hold(config-profile:web)# param --port ?
  WORD       Pattern name from the pattern dictionary
```

Example profile configuration:

```text
hold(config-profile:web)# binary /usr/local/bin/nginx
hold(config-profile:web)# argv -c /etc/nginx.conf
hold(config-profile:web)# env LOG_LEVEL info
hold(config-profile:web)# param --port port
hold(config-profile:web)# interactive
hold(config-profile:web)# tty
hold(config-profile:web)# no env LOG_LEVEL
hold(config-profile:web)# no interactive
hold(config-profile:web)# no tty
hold(config-profile:web)# default multi
```

Docker-shaped shell flags may be persisted in profile configuration, but the
captive/config transcript spelling uses full IOS-style words:

```text
hold(config-profile:web)# interactive
hold(config-profile:web)# tty
hold(config-profile:web)# detach
hold(config-profile:web)# no interactive
hold(config-profile:web)# no tty
hold(config-profile:web)# no detach
```

That maps to the shell concepts `--interactive`, `--tty`, and `--detach`
without importing Docker flag syntax into profile configuration mode.

Review and commit:

```text
hold(config-profile:web)# info
  base argv : /usr/local/bin/nginx -c /etc/nginx.conf
  params    : [--port] -> port  (optional)
  staged    : uncommitted changes present

hold(config-profile:web)# commit
  Re-hashing profile ............ 7f3e9c2a
  Generating sudoers entry ...... ok
  Validating with visudo -cf .... ok
  Installing /etc/sudoers.d/hold-web ... done
  Profile committed. Hash: 7f3e9c2a

hold(config-profile:web)# end
hold# write
Building configuration...
[OK]
hold#
```

### 4.6 Relationship to Docker-shaped shell CLI

Do not mix the mental models:

- Normal shell commands mimic Docker: `hold run -d ...`, `hold run -it ...`, `hold ps`, `hold logs -f -n 100 ...`, and `hold inspect ...`.
- Captive CLI mimics Cisco IOS: `enable`, `configure terminal`, mode prompts, contextual `?`, `show`, `write`, `no`, `default`, `commit`.
- Profile configuration words are IOS-style (`binary`, `argv`, `env`, `param`, `multi`, `pty-shim`, `no`, `default`, `info`, `commit`), not Docker flags.
- Docker flags may be saved into a profile, but their persisted configuration is expressed with IOS-style words in captive/config transcript mode.

Concrete mapping examples:

| Shell CLI | Captive/config spelling | Hold capability |
| --- | --- | --- |
| `-i`, `--interactive` | `interactive` / `no interactive` | Keep stdin open. |
| `-t`, `--tty` | `tty` / `no tty` | Existing PTY/console capability. |
| `-it` | `interactive` + `tty` | Interactive PTY run. |
| `-d`, `--detach` | `detach` / `no detach` | Detached/background run behavior. |
| `--privileged` | privileged/elevated profile policy | Root-managed execution path. |

The normal shell surface should not teach `console` as the primary command for
interactive terminal runs. Use `hold run -it <profile>` / `hold -it <cmd>` in
shell examples. The IOS-style captive CLI may still expose `console` as an EXEC
operator command where that wording fits the mode.

### 4.7 Captive namespace and context navigation

The captive CLI should be mode-based like IOS, but object contexts still need a
small, predictable navigation vocabulary so users can drill into profiles and
run IDs without memorizing every fully qualified command.

General context rules:

- `?` / `help` shows valid commands for the current mode/context.
- `exit` leaves the current mode/context.
- `back` may be accepted as friendly sugar for leaving an object context, but
  IOS `exit` remains canonical.
- `ls` lists objects in the current context.
- `select <name-or-id>` enters the selected child context when the current view
  is a list of profiles or run IDs.
- `profile` by itself must not be a no-op. It should list/enter the profile
  namespace or show contextual profile commands.

Top-level profile namespace example:

```text
hold# profile
hold(profile)# ?
Profile namespace commands:
  ls        List profiles
  run       Start a profile
  rm        Remove a profile
  select    Enter a profile context
  import    Import profile transcript
  export    Export profile transcript
  help      Show context help
  exit      Return to privileged EXEC

hold(profile)# ls
NAME      HASH         ACTIVE  COMMAND
web       7f3e9c2a     1       /usr/local/bin/nginx -c /etc/nginx.conf
report    1c5d3b7f     0       /opt/report/run

hold(profile)# select web
hold(profile:web)#
```

Profile object context example:

```text
hold(profile:web)# ?
Profile commands:
  ps        List run IDs for this profile
  run       Start this profile
  stop      Stop this profile only when singular or explicit
  logs      Open logs for a singular selected run
  inspect   Show profile details
  select    Enter a run ID context
  export    Export this profile transcript
  rm        Remove this profile when safe
  exit      Return to profile namespace

hold(profile:web)# ps
RUNID         STATE     UPTIME  PROFILE
04a7dda8      running   12m     web

hold(profile:web)# select 04a7dda8
hold(profile:web:04a7dda8)#
```

Run ID context example:

```text
hold(runid:04a7dda8)# ?
Run commands:
  logs      Open log viewer
  inspect   Show detailed JSON/object view
  stop      Gracefully stop this concrete run
  kill      Force stop this concrete run
  save      Save normalized launch facts as a profile
  rm        Remove when inactive
  exit      Return to previous context

hold(runid:04a7dda8)# save web-copy
```

`save <profile-name>` in a run ID context creates a profile from that run's
normalized launch facts. This is the context-local form of the shell-level
profile-from-run operation.

## 5. CLI transcript config

CLI transcript config is a human import/export format. JSON remains canonical on disk. The transcript should look like the commands an operator would type in the Cisco IOS-style captive CLI, not like JSON and not like Docker flags.

Export/import rules:

- `export` decompiles canonical JSON profile storage into IOS-style CLI transcript commands.
- `import` compiles IOS-style transcript commands back into canonical JSON profile storage.
- `inspect` is the JSON/object view. Do not make JSON the default import/export UX.
- Transcript files should use IOS mode and configuration words: `configure terminal`, `profile`, `binary`, `argv`, `env`, `alias`, `param`, `multi`, `pty-shim`, `no`, `default`, `info`, `commit`, `end`, `write`.
- Docker shell flags may be saved into a profile, but transcript export should render their persistent meaning with IOS profile configuration words.

Example transcript:

```text
configure terminal
  profile web
    binary /usr/local/bin/nginx
    argv -c /etc/nginx.conf
    env LOG_LEVEL info
    interactive
    tty
    param --port port
    no env LOG_LEVEL
    default multi
    commit
end
write
```

Commands:

```sh
hold export web as web.hold            # write IOS-style CLI transcript form
hold export web                        # print IOS-style CLI transcript to stdout
hold import web.hold as web --dry-run  # parse transcript and show changes
hold import web.hold as web --yes      # compile transcript into canonical JSON profile storage
hold inspect web                       # detailed JSON/object view
```

Import/apply should validate and show a change summary before overwriting unless `--yes` is supplied.

## 5.1 Completion and command history

Tab completion is a first-class 0.4 CLI library feature, not one-off shell glue.
Anything visible in the current namespace through `help`, `?`, `ls`, `show`, or
`ps` should be available to completion providers.

Completion domains:

- commands valid in the current shell/captive mode;
- profile names;
- run IDs and unique run-ID prefixes;
- view names such as `runs`, `profiles`, `version`, and future tree/list views;
- context-local actions such as `save`, `select`, `back`/`exit`, `logs`, and
  `inspect`;
- recent command strings when completing at the start of an empty or compatible
  input line.

Behavior:

- If there is one match, Tab completes it.
- If there are multiple matches, Tab expands through the first non-unique
  character.
- Repeated Tab lists candidates or cycles according to the active line editor.
- Arrow Up/Down navigates history/candidates.
- The original blank/current input is part of the navigation cycle, so a user
  can return to a plain cursor after browsing completions/history.
- Ghost text/typeahead is explicitly out of scope for 0.4. Standard completion
  inside the captive CLI and installable external shell completion are the
  target.

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
id="$(hold -d sleep 30)"
```

`-d`/`--detach` starts should print only the run ID to stdout on success. Foreground starts without `-d` stream child stdout/stderr instead of reserving stdout for the run ID.

## 10. Alignment status and decisions

Current branch status is tracked in [0.4.0 branch alignment and follow-up matrix](0.4.0-alignment.md). That matrix is part of this specification: if the matrix says a feature is a follow-up, this document describes the intended product target rather than current release readiness.

Resolved decisions for the current 0.4.0 direction:

1. Use **On Hold** for the project and **`hold`** for the intended 0.4.0 operator command.
2. Keep ad-hoc launch as bare `hold <cmd> [args...]`; use `--` only when a child executable conflicts with Hold syntax.
3. Use `run` and `stop` as profile lifecycle verbs, not as a namespace for managing existing executions. `hold run web logs` remains invalid.
4. Use concrete run IDs as the safest singular handles for `stop`, `kill`, `logs`, `inspect`, and targeted `rm`. Profile-name selectors are allowed only when they resolve safely.
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
