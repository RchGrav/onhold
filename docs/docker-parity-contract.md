# Docker parity contract

[Docs index](index.md) | [0.4 UX spec](HOLD_0_4_UX_SPEC.md)

Status: normative. This document is the source of truth for Hold's public
command surface. When Hold output or behavior disagrees with this document,
Hold is wrong. Tests should assert the shapes written here.

## Hold core invariants

The Docker-shaped CLI is a value-add skin. The core of Hold is holding host
processes — including `hold shell` adoption of a live foreground process.
These invariants belong to that core and bind **every** path that creates a
run (`run`, the bare convenience form, `shell` adoption, captive-CLI starts,
restarts):

- every run has a full 64-hex run ID;
- every run gets a generated `adjective_noun` name at creation;
- every run has a captured log (raw bytes + sidecar index);
- every live console/TTY run is reattachable by ID or name.

A new launch path that skips one of these is a bug, not a variant.

## The one rule

For every Docker verb Hold mimics, Hold behaves **byte-for-byte like Docker**,
with exactly two vocabulary substitutions:

| Docker | Hold |
| --- | --- |
| image | profile |
| container ID | run ID |

There is no third difference. If `docker run -d` prints one line containing
the full 64-hex ID and nothing else, so does `hold run -d`. If `docker ps -a`
humanizes times as `2 days ago`, so does `hold ps -a`. If Docker sends a
message to stderr, Hold sends it to stderr.

## Target resolution

`hold <token> [args...]` resolves `<token>` in strict priority order:

1. **Profile name** — launch the profile.
2. **Run ID / run name** — a retained (not yet pruned) run relaunches with its
   recorded recipe: a `-it` recipe reattaches as a console, a `-d` recipe
   detaches, otherwise it runs foreground-attached.
3. **Command** — resolve on `PATH` and launch as a new convenience run.

Escapes from resolution:

- `hold run -- <cmd>` and `hold console -- <cmd>` force command
  interpretation; nothing after `--` is treated as a profile or run ID.
- `hold console -- <cmd>` launches a **new** attached console run (PTY),
  bypassing profile/run resolution entirely.
- `hold attach <target>` (the Docker verb) and `hold console <target>` both
  attach to an existing run's console.
- `hold save` does not exist and must not be added with a profile-saving
  meaning: `docker save` exports an image, which is Hold's `export`. The
  run-to-profile verb is `hold commit <run> <profile>` (Docker `commit`).

A run's recorded flags are its recipe. Relaunching by ID or name replays the
recipe exactly, Docker-restart-style.

## Launch semantics

- `hold run <cmd|profile>` — foreground, attached, streams output. Prints
  **nothing** of its own before the process output (Docker parity).
- `hold run -d ...` — detached. stdout is exactly one line: the full 64-hex
  run ID. No banner, no help note.
- `hold run -it ...` — attached PTY/console.
- Any run creates a recorded recipe that can be relaunched by run ID or run
  name, and saved as a profile (`hold commit <run> <profile>`, mirroring
  `docker commit <container> <image>`).
- The bare convenience form (`hold <cmd>`, `hold -d <cmd>`) is Hold-native,
  not Docker-shaped: it may keep the 12-hex scripting ID on stdout and the
  stderr help note.

## Listing

`hold ps` / `hold ps -a` renders exactly Docker's table:

- Columns: `RUN ID  PROFILE  COMMAND  CREATED  STATUS  PORTS  NAMES`
  (Docker's layout with the two vocabulary substitutions).
- Column widths computed from content, as Docker does — never fixed printf
  widths that shear when a value is long.
- `CREATED` is always humanized: `6 minutes ago`, `2 days ago`. Never a raw
  ISO timestamp in the table.
- `STATUS` uses Docker phrasing: `Up 2 minutes`, `Exited (0) 2 days ago`,
  `Created`.
- `NAMES` is never empty for a user-visible run; every run gets a generated
  name at creation.
- Root-managed runs the invoking user cannot inspect do **not** appear in a
  normal user's `ps` output. They are visible with the `system:` scope or
  under sudo.

## Cleanup

- `hold rm <target>` removes one inactive run (Docker `rm`).
- `hold prune` prompts/clears all inactive past-run data; `-a`, `--all`, and
  `all` are all accepted and equivalent. A flag spelling variant must never
  be parsed as a target name.
- Every run visible in `ps -a` must be removable by the user who can see it,
  through `rm` or `prune`, with a clear error when it is not.

## Bare invocation and the captive CLI

- `hold` with no arguments prints help and exits 0, exactly like `docker`.
- The Cisco-IOS-style captive CLI (profile and grant editor) lives behind
  `hold cli`. It is never the default entry point.
- Inside the captive CLI, IOS conventions apply: `show running-config`-style
  dumps of a profile's exact stored configuration, `wr t`-style shorthands.

## Output discipline

- stdout carries machine data only: IDs, tables, logs, JSON.
- Human banners, hints, confirmations, and errors go to stderr; `--quiet`
  suppresses the normal human status.
- Docker-verb invocations print nothing Docker would not print.

## Help

`hold --help` (and bare `hold`) is structured like Docker's help:

1. Usage line.
2. The Docker-mimicked verbs, grouped and phrased the way Docker phrases
   them, so a Docker user can transfer their habits directly.
3. Hold-native additions (convenience launch, `cli`, `grant`, `shell`)
   clearly separated below.

Help text, parser behavior, and this contract must agree for every public
command. A command that exists must be in help; a command in help must parse.
