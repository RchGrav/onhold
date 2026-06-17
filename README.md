# sigmund

**A tiny, daemonless process launcher and recorder.**

`sigmund` launches commands into a new session / process group, captures their output, records a durable run handle, and lets you safely inspect or stop that tracked process group later. It is designed for CI jobs, integration-test harnesses, local development, and other environments where `nohup cmd &` or `setsid cmd &` leaves too much bookkeeping and too many cleanup footguns behind.

Many CI runners and non-interactive job systems terminate the invoking shell's process group at step completion. `sigmund` escapes that teardown boundary by starting the child in its own session and then recording enough identity information to avoid blindly signaling reused PIDs or unrelated process groups.

*(Name note: in Old English and related Germanic languages, **mund** relates to “protection/guardianship,” so `sigmund` reads naturally as “signal protection.”)*

## Why use `sigmund`?

* **More complete than `setsid`:** you get session isolation plus durable IDs, logs, listing, tailing, pruning, and safe teardown.
* **Better than `nohup &`:** every run has a recorded process group and log, so teardown does not depend on hand-copied PIDs.
* **Safer than bare `kill $PID`:** before signaling, `sigmund` validates boot identity, leader identity, and same-session process-group membership where the platform allows it.
* **Root-aware without changing the normal CLI:** normal runs remain user-local; root or `--system` runs use a universal system store and are discoverable through a redacted public index.
* **Lighter than `systemd-run` or `tmux`:** no daemon, no D-Bus, no service manager dependency.

## Review artifacts

This repository includes durable review artifacts:

- [`CHANGELOG.md`](CHANGELOG.md): file-by-file changes and verification history, including the root-managed state / sudo self-elevation update.
- [`REVIEW.md`](REVIEW.md): review process, rationale, verification commands, and known limitations.
- [`docs/SPEC.md`](docs/SPEC.md): current implementation contract.

## Quickstart

### Build

Requires a C11 compiler and POSIX process APIs. Linux and macOS are supported.

On Linux, `make` attempts to produce a static standalone binary named `sigmund` by default. On macOS, `make` builds a normal dynamically linked Mach-O binary because fully static system linking is not supported there.

```bash
make
./sigmund --help

# Optional on Linux: build a dynamically linked binary instead
make sigmund-dynamic
```

For cross-platform CI and releases, publish the artifact appropriate to each host:

- Linux static artifact (`make`): best portability across Linux hosts.
- Linux dynamic artifact (`make sigmund-dynamic`): smaller binary when runtime libc compatibility is acceptable.
- macOS artifact (`make` on macOS): normal platform-native binary.

### Basic usage

```bash
# Start a user-local tracked process
$ sigmund qemu-system-x86_64 -m 4096 -nographic
7f3c2a9d
sigmund  started  7f3c2a9d   qemu-system-x86_64 -m 4096 -nographic
         log      /home/alice/.local/state/sigmund/7f3c2a9d.log
         tail     sigmund tail 7f3c2a9d
         stop     sigmund stop 7f3c2a9d

# List user-local runs plus redacted root-managed public entries
$ sigmund list
RUNID      STATE    STARTED  RESULT     CMD
7f3c2a9d   running  12s      -          qemu-system-x86_64 -m 4096 -nographic

# Stop the run cleanly
$ sigmund stop 7f3c2a9d
sigmund: stopped 7f3c2a9d
```

Use `sigmund -- <cmd>` when the child command name overlaps with a `sigmund` subcommand.

## Concepts

### Ephemeral to immutable

Normal starts create ephemeral 8-hex-character run IDs. `sigmund alias <id> <name>` creates a friendly alias from a recorded run. User-local aliases store the launch recipe directly in the user's private `aliases.json`; root-managed aliases publish only `alias -> profile hash` while the protected recipe stays in root-private `profiles.json`.

```mermaid
flowchart TD
    A["User runs: sigmund /bin/mydaemon --flag"] --> B["Generate ephemeral run ID<br/>example: a1b2c3d4"]
    B --> C{"Process runs"}
    C --> D[("Private run record<br/>a1b2c3d4.json")]
    C --> E[("Log file<br/>a1b2c3d4.log")]

    F["User runs: sigmund alias a1b2c3d4 web"] --> G["Extract recorded argv"]
    G --> H["Resolve argv[0] to absolute binary path"]
    H --> I{"Alias scope"}
    I -->|"user-local"| J[("User aliases.json<br/>web -> {bin,args}")]
    I -->|"root-managed"| K["Compute SHA-256 profile hash"]
    K --> L[("Root-private profiles.json<br/>hash -> {bin,args}")]
    K --> M[("Public aliases.json<br/>web -> hash")]

    J --> N["Future command: sigmund start web"]
    M --> O["Future command: sigmund stop web"]
    O --> P["Resolve by alias label and verb intent"]
    P --> Q["Act on one run, list ambiguity, or use --all"]
```

## Real-World Workflows

### 1. CI/CD pipeline integration tests

When integration tests need a database, web server, emulator, or other long-running helper, `sigmund` can start it in one CI step, keep it alive after that step exits, capture its logs, and tear down the full process group later.

```yaml
- name: Start test database
  run: |
    run_id="$(sigmund -- redis-server --port 6379)"
    echo "REDIS_RUN_ID=$run_id" >> "$GITHUB_ENV"
    sleep 2

- name: Run integration tests
  run: npm run test:integration

- name: Show database log on failure
  if: failure()
  run: sigmund dump "$REDIS_RUN_ID" || true

- name: Teardown test database
  if: always()
  run: |
    sigmund stop "$REDIS_RUN_ID" || true
    sigmund prune "$REDIS_RUN_ID" || true
```

### 2. Local development stack

For a local app with several cooperating processes, start each one once and keep one terminal free for normal work.

```bash
sigmund npm run dev:frontend
sigmund npm run dev:backend
sigmund celery -A myapp worker

sigmund list
sigmund tail <backend-run-id>

# Later, stop the pieces you started.
sigmund stop <frontend-run-id> <backend-run-id> <worker-run-id>
sigmund prune all
```

### 3. Root-managed helpers

Some test helpers need root privileges or system-visible state. Use `--system` for those runs. Normal users can still see that a root-managed run exists through the redacted public index, while private command details and logs stay root-only.

```bash
sigmund --system qemu-system-x86_64 -m 4096 -nographic

sigmund list
# root-managed rows appear as <root-managed> with STATE unknown

sudo sigmund tail system:<run-id>
sigmund stop system:<run-id>
```

## Storage model

`sigmund` has two storage contexts.

### User-local state

Normal non-root starts write to:

```text
~/.local/state/sigmund
```

User-local records and logs are private to that user:

```text
directory: 0700
records:   0600
logs:      0600
aliases:   0600 aliases.json
```

### Root-managed state

Runs started with root authority, through `sudo`, or through `sigmund --system ...` write to a universal system location:

```text
Linux: /var/lib/sigmund
macOS: /var/db/sigmund
```

The system store is split into private and public areas:

```text
/var/lib/sigmund/runs/      private root run records
/var/lib/sigmund/logs/      private root logs
/var/lib/sigmund/profiles.json
/var/lib/sigmund/public/    public redacted discovery index
```

macOS uses the same layout under `/var/db/sigmund`.

Private root records, logs, and profiles are root-owned and require root authority to read. Public index files are user-readable and contain only safe discovery fields: ID, root-managed marker, elevation requirement, non-authoritative state hint, and start time. The public alias dictionary maps alias names to profile hashes only. Public files do **not** expose argv, command display, log paths, PID/PGID/SID, boot identity, executable identity, environment, sudo provenance, or private filesystem paths.

Because there is no daemon continuously refreshing root state, normal `sigmund list` displays root-public rows with `STATE` as `unknown` and never prompts for sudo. Root/private records remain authoritative when root Sigmund evaluates the run, and root logs/private details require running the relevant command through root authority such as `sudo sigmund list`, `sudo sigmund tail <id>`, or `sudo sigmund dump <id>`.

Example normal list containing a redacted root-managed run:

```text
RUNID      STATE    STARTED  RESULT     CMD
7f3c2a9d   unknown  -        -          <root-managed>
```

```mermaid
flowchart LR
    User(["Standard user"])
    Admin(["Root / sudo"])

    subgraph UserStore["User local store   ~/.local/state/sigmund/"]
        direction TB
        UR["run records<br/>0600 private"]
        UL["logs<br/>0600 private"]
        UA["aliases.json<br/>0600 private"]
    end

    subgraph SystemStore["System store   /var/lib/sigmund or /var/db/sigmund"]
        direction TB
        SR["runs/<br/>0600 root private"]
        SL["logs/<br/>0600 root private"]
        SP["profiles.json<br/>0600 root private"]
        Public["public/<br/>0755 readable"]
        SI["public run index<br/>0644 redacted"]
        SA["public/aliases.json<br/>0644 alias -> hash"]
    end

    User -->|"sigmund <cmd>"| UserStore
    Admin -->|"sudo sigmund <cmd>"| SystemStore
    User -.->|"sigmund list"| SI
    User -.->|"resolve system aliases"| SA
```

## Root/system mode

`--system` is an invocation switch, not a subcommand and not an ID scope.

```bash
sigmund --system qemu-system-x86_64 -m 4096
sigmund start "qemu-system-x86_64 -m 4096" --system
sudo sigmund qemu-system-x86_64 -m 4096
```

Those forms create root-managed runs. A root/sudo/system start never writes to `/root/.local/state/sigmund` and never writes into the invoking user's local state.

When a non-root action needs root authority, `sigmund` forks a `sudo` child and waits for it using an argv-preserving command shape:

```text
sudo -- /absolute/path/to/sigmund --system --elevated <canonical-command...>
```

There is no shell reconstruction, no `sudo sh -c`, no string command payload, and no quoting layer. `stdin`, `stdout`, and `stderr` are inherited by the sudo/root-Sigmund child so sudo prompts, sudo diagnostics, root Sigmund diagnostics, and streamed logs behave like one foreground command. The non-root parent waits and returns the sudo/root-Sigmund exit status; if `sudo` itself cannot be executed, the child prints a clean diagnostic and exits `127`.

The internal `--elevated` flag is not a user-facing feature. If it appears without effective UID 0, Sigmund fails with an internal error instead of trying to elevate again.

## Command parsing

Sigmund has two command modes.

### Raw start form

```bash
sigmund <cmd...>
```

Only invocation switches before the child command belong to Sigmund:

```bash
sigmund --system qemu-system-x86_64 -m 4096    # --system belongs to Sigmund
sigmund qemu-system-x86_64 -m 4096 --system    # --system belongs to QEMU
```

### Sigmund-owned command form

For known Sigmund commands, invocation switches may appear before or after the owned arguments:

```bash
sigmund --system stop 7f3c2a9d
sigmund stop 7f3c2a9d --system
sigmund start "qemu-system-x86_64 -m 4096" --system
```

Known commands include `list`, `stop`, `kill`, `tail`, `dump`, `prune`, `start`, `alias`, `aliases`, `grant`, `revoke`, and `help`.

## Command reference

### Start commands

| Command | Description |
|---|---|
| `sigmund <cmd...>` | Starts a command in a new session / process group using user-local state. |
| `sigmund -f <cmd...>` | Starts a command and immediately follows its log. `--tail` is still accepted for compatibility. |
| `sigmund -- <cmd...>` | Starts a command whose name overlaps with a Sigmund subcommand. |
| `sigmund --system <cmd...>` | Self-elevates through sudo when needed and starts a root-managed run. |
| `sigmund start <cmd...>` | Explicit start form; Sigmund-owned switches such as trailing `--system` are parsed by Sigmund. |
| `sigmund start <alias>` | Starts the recipe behind an alias. Refuses if that alias already has a running process unless `--multi` is supplied. |
| `sigmund start <alias> --multi [N]` | Starts one or N additional runs for the alias. |

### Management commands

| Command | Description |
|---|---|
| `sigmund list [alias]` | Lists all visible runs, optionally filtered to a recorded alias label. Default time is relative; use `--iso` or `-l` for absolute timestamps. Never prompts for sudo. |
| `sigmund tail <target>` | For a run ID, follows that run's log. For an alias, resolves the currently running alias-labeled run. |
| `sigmund dump <target>` | Prints the saved output log for a run and exits. |
| `sigmund stop <target>` | Sends `SIGTERM` to the tracked process group, waits up to 5 seconds, then sends `SIGKILL` if needed. Use `--all` to stop all matching alias runs. |
| `sigmund kill <target>` | Immediately sends `SIGKILL` to the tracked process group. |
| `sigmund stop --print <target>` | Prints the validated `kill -TERM -- -<pgid>` command without signaling. |
| `sigmund kill --print <target>` | Prints the validated `kill -KILL -- -<pgid>` command without signaling. |
| `sigmund alias <id> <name>` | Creates or updates an alias from a recorded run. User aliases store a direct recipe; system aliases publish an alias-to-hash pointer. |
| `sigmund aliases [-v]` | Lists visible aliases by name. User-local aliases show their command and `-` for hash; system aliases show `<root-managed>` and a truncated hash unless `-v` is used. |
| `sigmund grant <alias> <user> [actions]` | Adds root-managed NOPASSWD sudoers entries for `start,stop,kill,tail,dump,prune`. The user may be a username, `%group`, or `all`; omitted actions means all supported actions for that alias. |
| `sigmund revoke <alias> <user> [actions]` | Removes matching Sigmund-managed sudoers entries; omitted actions removes the managed file. |
| `sigmund help [topic]` | Shows main help or focused help for `profiles`, `targets`, `access`, `system`, `scripting`, and individual actions. |
| `sigmund prune` | Removes past run data for finished/failed records and unreferenced logs. |
| `sigmund prune <target>` | Removes exactly one prunable run record and associated log. |
| `sigmund prune all` | Removes all prunable runs and associated output. |

### Targeting IDs and aliases

A run ID points at one recorded process group. A profile hash is not a run ID. Hashes identify protected launch recipes and sudoers capabilities; normal action commands target run IDs/prefixes or aliases.

Aliases are exact human labels. Runs started through an alias record that alias label, so later `stop web`, `tail web`, `dump web`, and `prune web` select from runs recorded under `web`. Selection is based on the verb:

- `start <alias>` starts the alias recipe and refuses if one matching run is already running, unless `--multi` is supplied.
- `stop` and `kill` select running alias-labeled runs.
- `tail` selects the currently running alias-labeled run; a run ID can still tail an already-finished run directly.
- `dump` selects runs with logs.
- `prune` selects prunable past data.
- If an alias selection has more than one candidate, Sigmund exits `6` and prints the filtered candidates. `--all` resolves that ambiguity for `stop`, `kill`, and `prune`.

For rare deterministic conflict cases, explicit target tokens are supported:

```bash
sigmund stop user:7f3c2a9d
sigmund stop system:7f3c2a9d
sigmund start web-test
sigmund stop system:web-test
sudo sigmund stop user:7f3c2a9d
sudo sigmund stop system:7f3c2a9d
```

Rules:

- Normal non-root action on a plain target checks user-local state first. If user-local and root-managed public targets share the same token, user-local wins and Sigmund does not self-elevate.
- Normal non-root action on a root-only plain target, or on `system:<target>`, self-elevates for `stop`, `kill`, `prune`, `tail`, and `dump`.
- Root/sudo action on a plain target checks root-managed private state first, then the invoking user's local state when sudo provenance exists. In a conflict, root-managed wins.
- `user:<target>` never targets root-managed state. `system:<target>` never targets user-local state.
- When a root-managed alias crosses the sudo boundary, Sigmund passes an internal capability shape: `<verb> <runid_sel> <alias> <hash>`. `runid_sel` is an 8-hex run ID, `00000000` for start, or `ffffffff` for an approved `--all` action. Root Sigmund verifies the alias/hash pair and then verifies the selected run records are recorded under that alias before acting.

```mermaid
flowchart TD
    Start(["User runs: sigmund stop web"]) --> Parse["Parse target token"]
    Parse --> Scope{"Explicit scope?"}

    Scope -->|"user:"| UserOnly["Check user-local run ID or alias"]
    Scope -->|"system:"| SystemOnly["Check system public run ID or alias"]
    Scope -->|"none"| LocalFirst["Check user-local run ID or alias"]

    LocalFirst --> LocalFound{"User-local match?"}
    LocalFound -->|"yes"| LocalExec["Resolve to concrete local run ID<br/>send SIGTERM"]
    LocalFound -->|"no"| SystemOnly

    UserOnly --> UserFound{"User-local match?"}
    UserFound -->|"yes"| LocalExec
    UserFound -->|"no"| NotFound["error: not found"]

    SystemOnly --> SystemFound{"System match?"}
    SystemFound -->|"no"| NotFound
    SystemFound -->|"run ID"| NeedRootID{"Already root?"}
    SystemFound -->|"alias"| NeedRootAlias{"Already root?"}

    NeedRootID -->|"no"| SudoID["sudo -- sigmund --system --elevated stop system:&lt;id&gt;"]
    NeedRootAlias -->|"no"| SudoAlias["sudo -- sigmund --system --elevated stop &lt;runid_sel&gt; &lt;alias&gt; &lt;hash&gt;"]
    NeedRootID -->|"yes"| RootExec["Read private root record<br/>send SIGTERM"]
    NeedRootAlias -->|"yes"| RootResolve["Resolve alias by verb intent"]
    RootResolve --> RootExec
    SudoID --> RootExec
    SudoAlias --> RootResolve
```

### Aliases and protected profiles

`sigmund alias <id> <name>` reads the target run's recorded argv and resolves argv[0] to an absolute binary path. Its confirmation is human stderr, not machine stdout; scripts should read alias state through `sigmund aliases` or the managed state files.

For user-local aliases, Sigmund writes the recipe directly to the user's private `aliases.json`:

```json
{
  "web": {"bin": "/usr/bin/redis-server", "args": ["redis-server", "--port", "6379"]}
}
```

For root-managed aliases, Sigmund computes a SHA-256 profile hash, stores the protected recipe privately, and publishes only `name -> hash` in `/var/lib/sigmund/public/aliases.json` or `/var/db/sigmund/public/aliases.json`.

The protected profile map is `profiles.json`:

```json
{
  "fb736e64274bb2fd4861ff5d239288d4abc74aa3ae233b733b6201da507868ee": {
    "bin": "/usr/bin/redis-server",
    "args": ["redis-server", "/etc/redis.conf"]
  }
}
```

The profile hash is a stable SHA-256 fingerprint over a NUL-delimited byte stream containing only the fixed domain string `sigmund-profile`, the resolved absolute binary path, the argc count, and each argv element tagged by index. The domain string is a namespace label, not a version, and it must not be changed for ordinary format churn. Environment, cwd, uid, timestamps, hostnames, and other context are deliberately excluded so aliases, profiles, and sudoers grants stay stable.

`sigmund grant <alias> <user> [actions]` writes an alias/user-specific sudoers template such as `/etc/sudoers.d/sigmund_web-test_alice`. The alias is resolved to its immutable profile hash before writing, so the generated file contains one tightly scoped internal rule with a single anchored sudoers argument regex, an action alternation, an 8-hex `runid_sel` slot, the fixed alias, and the fixed hash. Omitted actions expand to all supported Sigmund actions for that alias (`start,stop,kill,tail,dump,prune`), not arbitrary sudo access.

## Stdio and logging

Child process output is captured:

* child `stdin` is redirected from `/dev/null`;
* child `stdout` and `stderr` are redirected to the per-run log file;
* start writes the bare 8-character run ID to stdout and writes the human banner to stderr;
* alias, grant, revoke, stop, kill, and prune confirmations are human stderr.

`sigmund -f <cmd...>` starts a command and follows its log immediately. `--tail` is kept as a compatibility alias. `sigmund tail <id>` follows the log of an existing running process, or prints finished/stale/unknown logs from the beginning. Press Ctrl-C to detach from tailing while the background process keeps running.

Action-command self-elevation does **not** pipe or capture terminal I/O. The `sudo`/root-Sigmund child inherits the original terminal descriptors so password prompts, diagnostics, Ctrl-C behavior, and root-side output are preserved, while the non-root parent waits and returns the child status.

`sigmund` does not scrub, allowlist, capture, or hash the child environment. User-local and direct-root starts inherit Sigmund's current environment unchanged. Privilege-crossing runs rely on sudo's standard `env_reset` behavior for environment hygiene before root Sigmund starts the command; reimplementing sudo's policy inside Sigmund is intentionally out of scope.

## Architecture and safety guarantees

State updates use atomic temp-file writes, `fsync()`, and `rename()` so records are not left half-written during power loss or interruption.

```mermaid
sequenceDiagram
    participant Shell as Caller shell
    participant Parent as Sigmund parent
    participant Child as Sigmund child
    participant Target as Target process

    Shell->>Parent: exec sigmund <cmd>
    Parent->>Parent: create O_CLOEXEC handshake pipe
    Parent->>Child: fork()

    rect rgb(40, 44, 52)
        Note right of Child: child preparation
        Child->>Child: setsid()
        Child->>Child: redirect stdin to /dev/null
        Child->>Child: redirect stdout/stderr to log
        Child->>Target: execvp(cmd) or execv(profile bin)
    end

    alt exec succeeds
        Target-->>Parent: handshake pipe closes on exec
        Parent->>Parent: write run record and public index if system
        Parent-->>Shell: print run ID and exit 0
        Note over Target: target survives caller shell exit
    else exec fails
        Child-->>Parent: write errno through pipe
        Parent->>Parent: rollback reservation, log, and process group
        Parent-->>Shell: print error and exit 1
    end
```

Before sending a signal, Sigmund checks:

1. Whether the current boot marker matches the recorded one when available. Linux uses `/proc/sys/kernel/random/boot_id`; macOS uses `kern.boottime`.
2. Whether the leader process identity still matches. Linux checks `/proc/<pid>/stat` start time and, when available, `/proc/<pid>/exe` device/inode. macOS checks kernel process start time and best-effort executable path device/inode.
3. If the leader PID has exited but the process group still has members, whether those members still belong to the recorded session.

If a boot or identity check fails, the state evaluates as `stale` and signals are blocked. If the operating system cannot provide enough information to validate a target safely, the state evaluates as `unknown` and signaling commands refuse it. Stale and unknown records remain visible until explicitly pruned.

`sigmund` does **not** restart processes after reboot and is **not** a supervisor.

## Test notes

`make test` builds with `-DSIGMUND_TESTING` and uses a test-only `SIGMUND_TEST_SYSTEM_STATE_DIR` override. Production builds do not honor an arbitrary environment variable for the system store.

The test harness creates explicit actors:

```text
USER_ACTOR = non-root Sigmund context
ROOT_ACTOR = direct root/system Sigmund context
SUDO_ACTOR = root Sigmund with sudo provenance for the user actor
```

This keeps tests independent of whether the test runner itself starts as root or non-root.

## License

Apache-2.0. See `LICENSE`.
