# Hold

Hold keeps host processes alive, visible, and controllable after your terminal or
CI step moves on.

It gives a normal Unix command a durable run ID, a human run name, captured logs,
and safe lifecycle commands. It is more deliberate than `nohup` and `&`, lighter
than a service manager, and Docker-shaped where that analogy is honest.

Hold does **not** create containers. It does not isolate filesystems, publish
ports, mount volumes, manage images, or pretend a host process has Docker
networking. It manages ordinary host processes and tells the truth about them.

```sh
hold run -d --name web -- python3 -m http.server 8080
hold ps -a
hold logs -f web
hold inspect web
hold stop web
hold rm web
```

## What Hold is for

Use Hold when you need to start something now and still be able to find it,
watch it, stop it, or turn it into a reusable launch profile later:

- development servers
- CI helper processes
- temporary workers
- integration-test dependencies
- interactive shells you want to detach from and reattach to
- constrained root-managed tools exposed through reviewed profiles/grants

Hold sits in the gap between raw shell backgrounding and permanent machine
service management:

```text
nohup / &        too little state and safety
Hold             durable process handle, logs, profiles, safe stop/inspect
systemd          machine service supervision and boot policy
Docker           containers, images, isolation, networking
```

## Core concepts

| Concept | Meaning |
| --- | --- |
| Run | One concrete execution. It has a stable 64-hex run ID, a generated `adjective_noun` run name, state, and logs. |
| Profile | A named launch configuration. Profiles are image-like: users choose the name, and running one creates runs. |
| Grant | A constrained privileged profile authorization. Grant safety is tied to canonical profile content pinned in sudoers. |
| Log | Captured stdout/stderr. Local logs are raw bytes plus a sidecar index for timestamps, stream metadata, and fast viewing. |

Run names are generated. Profile names are not.

## Quick start

Build from source:

```sh
make
./hold --help
```

Start a foreground run with Docker-style `run`:

```sh
hold run -- python3 -m http.server 8080
```

Detach/background explicitly:

```sh
id="$(hold run -d -- python3 -m http.server 8080)"
hold ps
hold logs -f "$id"
hold stop "$id"
```

The convenience form keeps the original small-tool feel. Without `run`, Hold is
optimized for quickly putting a command under management:

```sh
id="$(hold python3 -m http.server 8080)"
```

Use `--` when a profile or child command could conflict with Hold syntax:

```sh
hold run -- ./stop --not-a-hold-command
```

## Common commands

```sh
hold run [opts] -- <cmd> [args...]   # Docker-shaped explicit launch
hold <cmd> [args...]                 # convenience launch
hold ps [-a]                         # list runs
hold logs [-f] [-n N] <target>        # plain/script-friendly logs
hold inspect <target>                # structured run details
hold stop <target>                   # graceful process-group stop
hold kill <target>                   # force process-group kill
hold rm <target>                     # remove retained run state
hold prune [target]                  # clean stopped retained runs
hold profile <name> [opts] -- <cmd>  # create/update a profile
hold run <profile>                   # start from a profile
hold console <target>                # attach to a PTY-backed run
```

Targets can be full run IDs, safe ID prefixes, run names, or profile names when
the command supports profile targeting. If a target is ambiguous, Hold refuses
instead of guessing.

## Logs

Plain logs are script-friendly by default:

```sh
hold logs web
hold logs -f web
hold logs -n 100 web
```

Hold captures stdout and stderr for managed runs regardless of whether you are
watching live output. The default local log format is raw captured bytes with an
`HLOGIDX` sidecar index containing record offsets, lengths, timestamps, and
stdout/stderr metadata. That keeps plain logs simple while allowing the built-in
viewer to jump and render timestamps without rewriting the hot path as JSON.

Future viewer polish and additional destinations such as journald/syslog are
tracked under [`docs/future/`](docs/future/).

## Profiles

Profiles make a command reusable:

```sh
hold profile web -d -- python3 -m http.server 8080
hold run web
```

A profile stores launch configuration such as executable path, args, cwd,
environment, terminal mode, detach behavior, capabilities, and log driver.
Profile names are user-chosen and may be renamed; the profile ID stays stable.

Hold rejects fake Docker substrate flags such as `-p/--publish` and `-v/--volume`.
Ports are observed from running host processes and displayed in `hold ps`; Hold
does not publish, forward, or remap ports. Host paths are just host paths and
should be passed directly as absolute arguments/configuration.

## Console and captive CLI

PTY-backed runs can be detached and reattached:

```sh
id="$(hold run -it -- bash)"
hold console "$id"
```

Press `Ctrl-P Ctrl-Q` to detach without stopping the run.

Running `hold` with no arguments opens the captive CLI, an operator-style
namespace editor for profiles, runs, logs, and privileged mode. It is inspired by
network-device configuration shells for discoverability and constrained editing;
it is not a networking shell.

## Safety model

Hold is daemonless, so every command reopens the recorded state and revalidates
what it can before acting.

Important safety properties:

- normalize executable/cwd/path-like launch data to absolute paths
- create a new process group/session for managed runs
- capture logs independently of live viewing
- validate recorded identity before signaling where the platform allows it
- signal process groups, not guessed one-off PIDs
- keep user-local and root-managed stores separate
- reject ambiguous targets and unsupported Docker-like flags loudly
- gate privileged grants through canonical profile content and path ownership checks

Root-managed work is intentionally stricter than user-local work. A profile that
can become privileged must survive ownership, path, and canonical-content checks.
If the command points into a user home directory, it remains user-scoped rather
than becoming a global privileged profile.

## Storage

User-scoped state lives under the user's local Hold state directory, normally:

```text
$XDG_STATE_HOME/hold
# or
$HOME/.local/state/hold
```

Root-managed state lives under the system Hold store, normally:

```text
/var/lib/hold
```

macOS builds use the corresponding system/user locations selected by the
platform path layer.

## Build and verify

```sh
make
make test
bash scripts/ci.sh
```

`bash scripts/ci.sh` is the release-gate local verification path: static and
dynamic builds, contract tests, sanitizer tests, shell syntax checks, static
analysis when available, and packaging/build-script checks.

## Documentation

Start here:

- [Documentation index](docs/index.md)
- [Install](docs/install.md)
- [CI usage](docs/ci.md)
- [CLI contract](docs/cli-contract.md)
- [Profiles and aliases](docs/profiles-and-aliases.md)
- [Security](docs/security.md)

Current specs and release contracts:

- [Implementation spec](docs/SPEC.md)
- [Hold 0.4 UX/CLI spec](docs/HOLD_0_4_UX_SPEC.md)
- [0.4 object format repair contract](docs/0.4-object-format-repair.md)
- [0.4 release cut](docs/0.4-release-cut.md)
- [0.4 repair ledger](docs/0.4-repair-ledger.md)

Historical planning/review notes are kept in [`docs/archive/`](docs/archive/).
They are context, not the current command contract.

## Name

The project began as `sigmund`; the 0.4 line turns it into `hold`.

The name is literal: Hold keeps a process under management. It also happens to
fit the shipping metaphor that Docker already lives in: a ship's hold is where
cargo is kept, and a holdman is a dock worker in that hold. The commands stay
plain because the tool is practical, not nautical roleplay.

## License

Apache-2.0. See [LICENSE](LICENSE).
