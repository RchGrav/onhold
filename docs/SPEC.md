# On Hold Specification

> Status: This is a retained implementation specification. The current 0.4 repair contracts are [Hold 0.4 UX and CLI specification](HOLD_0_4_UX_SPEC.md), [0.4 object format repair](0.4-object-format-repair.md), and [0.4 release cut](0.4-release-cut.md). Historical planning links point into `archive/`.
[Docs index](index.md) | [Quickstart](quickstart.md) | [Archived documentation plan](archive/PLAN.md) | [Repository README](../README.md)

This document describes the current On Hold implementation contract: user-local state, root-managed state, public redacted discovery, profile/internal-alias resolution, sudo-aware target resolution, argv-preserving fork/wait self-elevation, and process-safety behavior.

## 1. Storage contexts

On Hold has two storage contexts.

### 1.1 User-local state

Normal non-root starts create records and logs under:

```text
~/.local/state/hold
```

Required permissions:

```text
directory: 0700 user:user
records:   0600 user:user
logs:      0600 user:user
console:   0700 user:user console sockets, 0600 per socket
aliases:   0600 user:user aliases.json
```

User-local state is private to the user. User profiles store direct launch recipes in `aliases.json`; they do not create a user-global protected profile object. On Hold does not scan every user's home directory and does not require globally unique run IDs across users.

### 1.2 Root-managed state

Root, sudo, and `--system` starts create records and logs under a universal system store:

```text
Linux: /var/lib/hold
macOS: /var/db/hold
```

Layout:

```text
/var/lib/hold/runs/      private root run records
/var/lib/hold/logs/      private root logs
/var/lib/hold/console/   private root console sockets
/var/lib/hold/profiles.json
/var/lib/hold/public/    public root run index
/var/lib/hold/public/aliases.json
```

macOS uses the same layout under `/var/db/hold`.

Required permissions:

```text
root state dir:       0755 root:root
private run records:  0600 root:root
private logs:         0600 root:root
private console dir:  0700 root:root
private sockets:      0600 root:root
private profiles:     0600 root:root
public dir:           0755 root:root
public index files:   0644 root:root
public profile map:   0644 root:root
```

A root-managed start must never create a run under `/root/.local/state/hold` and must never accidentally create a root-managed run inside the invoking user's home state.

### Global eligibility and granting

Three related operations, three separate rules.

**Universal path normalization:** every executable and path-like argument stored
in a record/profile must be normalized to an absolute path when it resolves to a
real filesystem object. Relative paths are acceptable user input, but they are
not acceptable as stored authority. This applies before deciding whether a run is
personal, global, or grantable.

**Elevated run routing:** when a root/elevated start would otherwise become
root-managed, On Hold must inspect the normalized executable and every
normalized path-like argument. If any of those paths is inside the invoking
user's home folder, the start becomes an invoking-user personal run ID instead
of a public/system run ID. This is a routing rule, not a root-ownership rule.

**To become a global profile:** none of the paths in the command may be located in a user's
home folder. This applies to *every* path in the command — the program (`argv[0]`), every
path argument (a script such as `server.py`, a config file, an `--output`/log target), and
the working directory. Paths were already resolved to absolute form when the command ran
(resolution is a run-time step, not a grant-time one), so this is a pure check: if any
resolved path falls inside a user's home folder, the command **cannot become a global
profile** — On Hold refuses.

**To grant a global profile to a user:** every target in the command must pass
the grant security validator. Granting delegates an existing global profile — it
makes the per-user private copy and writes the sudoers entry — but only if the
binary and every path argument are root-owned, unwritable by the grantee, and
pass any other grant-time checks required to keep the delegated execution context
immutable. This is a *stronger and separate* bar from global eligibility: global
only excludes home-folder paths, whereas a granted profile runs as root on
behalf of a non-root user, so every target it touches must be root-controlled and
therefore untamperable by that user. A profile can be global yet still not
grantable (e.g. a target owned by some non-root service account).

If a grant is refused, On Hold should list every reason it found, then print two
operator follow-up commands:

1. the same grant command with `--secure` added, labeled **Recommended**, which
   means “let On Hold apply safe remediations or stricter checks where it knows
   how”; and
2. the same grant command with `--force` added, labeled as a security-weakening
   override.

`--force` must not bypass absolute-path normalization, personal-vs-global
routing, or the rule that only global profiles can be granted. If it exists, it
only overrides grant-hardening checks that are explicitly designed to be
operator-overridable.

> Implementation status (2026-06-30): `hold_start_target_is_within_invoking_home` checks the
> resolved executable plus existing path-like argv values, including bare path tokens,
> `--flag=/path`, and `--flag /path` forms where the following token resolves as a path. If any
> checked path is inside the invoking user home, the run is routed to the invoking user-local store.

Production builds use the compiled system store path. Test builds compiled with `HOLD_TESTING` may honor `HOLD_TEST_SYSTEM_STATE_DIR`; production root On Hold must not honor arbitrary user-controlled environment paths for the system store.

## 2. Invocation context

On Hold detects the current authority and provenance at startup:

```text
effective UID is root?
was --system requested?
was internal --elevated present?
was root reached through sudo?
who is the invoking user, if sudo provenance exists?
```

When `geteuid() == 0`, On Hold checks:

```text
SUDO_UID
SUDO_GID
SUDO_USER
```

If all are valid, they identify the invoking user. On Hold resolves the invoking user's home directory through the user database (`getpwuid()`), not `$HOME`. In test builds only, `HOLD_TEST_INVOKING_HOME` may override that resolved path so tests can avoid real user homes.

Root private records for system runs include private provenance metadata:

```text
invoked_by_uid
invoked_by_gid
invoked_by_user
invoked_via_sudo
```

This metadata is not written to the public index.

The internal `--elevated` flag marks an intentional sudo self-elevation boundary. It is not a user-facing feature. If `--elevated` is present while `geteuid() != 0`, On Hold exits with a clean internal error.

## 3. Command parsing

On Hold has two parsing modes.

### 3.1 Raw start form

```bash
hold <cmd...>
```

In raw start form, only invocation switches before the child command belong to On Hold. Once the child command begins, all remaining arguments belong to the child.

```bash
hold --system qemu-system-x86_64 -m 4096
# --system belongs to On Hold

hold qemu-system-x86_64 -m 4096 --system
# --system belongs to qemu-system-x86_64
```

Quoted child command text is opaque to On Hold:

```bash
hold sh -c "qemu-system-x86_64 -m 4096 --system"
```

### 3.2 On Hold-owned command form

If the first non-invocation argument is a known On Hold command, On Hold owns that command's argument list. Known commands include:

```text
list
run
start
stop
kill
tail
logs
status
inspect
view
console
dump
prune
profiles
profile
show
clean
doctor
shell
grant
revoke
help
```

For On Hold-owned commands, invocation switches may appear before or after the command arguments:

```bash
hold --system stop 7f3c2a9dcafe
hold stop 7f3c2a9dcafe --system
hold start "qemu-system-x86_64 -m 4096" --system
```

These canonicalize to the same root-side command shape when elevation is required.

`hold help [topic]` is a On Hold-owned documentation command and never starts a child command named `help`.

## 4. Start behavior

### 4.1 Target store selection

```text
if --system was requested:
  self-elevate through sudo when not already root
  start in root-managed state
else if geteuid() == 0:
  start in root-managed state
else:
  start in user-local state
```

### 4.2 User-local start

Normal non-root start:

```bash
hold <cmd...>
```

writes:

```text
~/.local/state/hold/<id>.json
~/.local/state/hold/<id>.log
```

### 4.3 Root-managed start

These create root-managed runs:

```bash
sudo hold <cmd...>
hold --system <cmd...>
hold start <cmd...> --system
```

A root-managed start writes:

1. a private root run record;
2. a private root log;
3. a public root index entry.

If public index creation fails, startup fails and rolls back. A root-managed run that cannot be discovered violates the storage model.

### 4.4 Profile start

`hold start <profile>` starts the launch recipe currently assigned to that profile.

For user-local profiles, On Hold loads the direct recipe from the user's private `aliases.json` and records the profile label on the new run. For root-managed profiles, On Hold resolves the public profile name to its protected profile hash, crosses sudo with the internal capability argv shape `<verb> <runid_sel> <profile> <hash>` when needed, verifies that profile/hash pair as root, loads the protected root-private profile for `start`, and records the profile label on the new run. `aliases.json` and record field `alias` remain internal storage names in the current implementation.

`--multi` is a profile-start modifier. Without `--multi`, `start <profile>` refuses when that profile already has a running process. Bare `--multi` starts one additional run and bypasses that guard; `--multi N` and `--multi=N` start N runs.

### 4.5 Console start

`--console` is a start modifier. It may be used with raw starts or profile starts:

```bash
hold --console <cmd...>
hold start <profile> --console
```

A console run records a private `console_sock` path and starts the command behind a per-run PTY broker. Output is tee'd to the normal log, so `tail` and `dump` keep their normal behavior. Console sockets are private state and must not be exposed through the public root index.

## 5. Public root index and profile map

The public root index contains only safe discovery and elevation metadata.

Example:

```json
{
  "id": "7f3c2a9dcafe0711502b4632bc1db9e89a70c4c3d2437fc6b0c9d2699188",
  "root_managed": true,
  "requires_elevation": true,
  "alias": "web-test",
  "state_hint": "running",
  "started_at": "2026-06-15T18:42:11Z",
  "created_at": "2026-06-15T18:42:11Z",
  "State": {
    "Status": "running",
    "Running": true,
    "Paused": false,
    "Restarting": false,
    "Dead": false,
    "Pid": 1234,
    "Pgid": 1234,
    "Sid": 1234,
    "ExitCode": 0,
    "Error": "",
    "StartedAt": "2026-06-15T18:42:11Z",
    "FinishedAt": "0001-01-01T00:00:00Z"
  }
}
```

The public index must not include:

```text
argv
cmdline_display
log_path
console_sock
environment
boot_id
process start time
executable identity
sudo provenance
private filesystem paths
profile hashes
```

Private root records are authoritative. Public records are derived discovery data.

Private run records store the resolved executable path in `argv[0]`. Before a run record is written, On Hold also resolves existing path-like argv values after `argv[0]` against the launch cwd, including bare path tokens and `--flag=/path` forms (or, for `hold shell` adoption, against the adopted foreground process cwd). Profiles created from a run therefore inherit an absolute, replayable launch recipe instead of depending on the profile creator's old current directory.

Because On Hold is daemonless, public state is a projection written by Hold at start and by the Hold-owned supervisor/reaper on exit. Normal `hold list` may display that projected state, but root/private list and root action commands still evaluate authoritative state from private records before acting.

`public/aliases.json` is a flat JSON object mapping validated profile names to 64-character profile hashes:

```json
{
  "web-test": "fb736e64274bb2fd4861ff5d239288d4abc74aa3ae233b733b6201da507868ee"
}
```

Public profile-map entries must not include argv, binary paths, log paths, console socket paths, environment, process identity, or sudo provenance.

## 6. Run IDs and collision checks

Run IDs are stable opaque 64-character lowercase hex identifiers. Display surfaces may abbreviate to the first 12 hex characters, Docker-style. The ID is generated from launch material such as profile/origin name, resolved executable, cwd, timestamp, launcher PID, argv, and a collision counter; it is a tracking number, not a mutable content signature.

### 6.1 User-local start avoids

```text
that user's local records
that user's local logs
that user's local console sockets
that user's local reservation files
root-managed public index IDs
```

### 6.2 Root-managed start with sudo provenance avoids

```text
root-managed private records
root-managed private logs
root-managed private console sockets
root-managed reservation files
root-managed public index IDs
the invoking user's local records/logs/console sockets/reservations
```

### 6.3 Direct root start avoids

```text
root-managed private records
root-managed private logs
root-managed private console sockets
root-managed reservation files
root-managed public index IDs
```

If old state, manual files, or cross-user private state still create a collision, correctness is preserved by invocation-based resolution.

## 7. Target resolution

Action commands use the shared resolver. Action commands are:

```text
stop
kill
prune
tail
console
dump
```

Targets may be:

```text
run ID prefix
profile
user:<target>
system:<target>
```

Profile names must be 1 to 64 characters and must contain only `[A-Za-z0-9_-]`. They must not contain `/` or `.`, and must not parse as a full profile hash. If a profile name also looks like a run-ID prefix, concrete run-ID resolution wins before profile lookup.

A profile hash is not a run ID and is not a normal action target. Hashes identify protected launch recipes and sudoers capabilities. Action commands resolve profiles by matching the recorded profile label on run records.

Profile resolution is verb-specific:

```text
start: running profile-labeled runs gate new starts unless --multi is supplied
stop/kill: running profile-labeled runs
tail: running profile-labeled runs
console: running console-enabled profile-labeled runs
dump: profile-labeled runs with logs
prune: prunable profile-labeled past data
```

If a profile selection has zero candidates but the profile exists, the action is a successful no-op. If the profile is unknown, the action returns not found. If a profile selection has more than one candidate, On Hold exits 6 and prints the filtered candidates. `--all` resolves that ambiguity for `stop`, `kill`, and `prune`.

The resolver returns one normative result:

```text
resolved user-local
resolved root-managed
not found
clean error
```

Final action execution must use only the resolved target. Action commands must not perform ad hoc store probing outside the shared resolver except for final execution against the already resolved target.

### 7.1 Normal non-root plain target

```text
if user-local ID exists:
  target user-local
else if user-local profile exists:
  target user-local
else if root public ID exists:
  target root-managed requiring elevation
else if root public profile exists:
  target root-managed requiring elevation as <runid_sel> <profile> <hash>
else:
  not found
```

Important invariant:

```text
If user-local and root-managed public targets share a plain token,
normal On Hold targets the user-local match and does not self-elevate.
```

### 7.2 Root/sudo plain target

```text
root_match = root-managed private ID exists, or profile matches the verb-specific root-managed run set
user_match = invoking-user-local ID exists, or profile matches the verb-specific invoking-user run set, if sudo provenance exists

if root_match:
  target root-managed
else if user_match:
  target invoking-user-local
else:
  not found
```

This means `sudo hold stop <id>` can stop an invoking user's local run when that ID exists only in the invoking user's local state. In the rare conflict where both exist, root-managed wins.

Direct root without sudo provenance has no invoking-user context and resolves plain targets only against root-managed state.

### 7.3 Explicit deterministic target tokens

Plain targets are the normal interface. Explicit tokens are supported for rare deterministic cases:

```text
user:<target>    force user-local lookup
system:<target>  force root-managed lookup
```

Rules:

```text
user:<target> never targets root-managed state.
system:<target> never targets user-local state.
```

If `system:<target>` is used from normal non-root invocation, On Hold may self-elevate. If `user:<target>` is used from root/sudo invocation, On Hold targets the invoking user's local state when sudo provenance exists. Direct root using `user:<target>` without sudo provenance returns a clean error.

### 7.4 Profiles

Protected profile state is stored in `profiles.json` as a hash-keyed object:

```json
{
  "fb736e64274bb2fd4861ff5d239288d4abc74aa3ae233b733b6201da507868ee": {
    "bin": "/usr/bin/redis-server",
    "args": ["redis-server", "/etc/redis.conf"]
  }
}
```

The profile hash is SHA-256 over this stable, NUL-delimited byte stream:

```text
hold-profile
resolved absolute binary path
argc
argv[0] index, argv[0]
argv[1] index, argv[1]
...
```

The domain string `hold-profile` is a fixed namespace label, not a version. Do not append versions or add environment, cwd, uid, timestamp, hostname, or other context to this hash input. Existing profile mappings, profiles, and sudoers grants are keyed by this digest; changing the input silently invalidates them.

On Hold does not scrub, allowlist, capture, or hash the launched command's environment. `perform_start` and profile starts use the inherited process environment unchanged. Privilege-crossing starts rely on sudo's standard `env_reset` behavior before root On Hold reaches `perform_start`; disabling `env_reset` or preserving loader variables through sudoers is host sudo policy, not On Hold policy.

When a normal user targets a root-managed profile, On Hold resolves the public profile name to the current protected hash before self-elevation and carries the internal capability argv shape over sudo:

```text
<verb> <runid_sel> <profile> <hash>
```

`runid_sel` is always present. It is a concrete 12-hex run ID, `000000000000` for `start`, or `ffffffffffff` for an approved `--all` action. Root On Hold must verify that the internal alias field for that profile still points at the supplied hash and that selected concrete run records are recorded under that profile before acting.

## 8. Self-elevation boundary

Normal non-root On Hold may self-elevate only when:

```text
the command is stop, kill, prune, tail, or dump;
no user-local plain target matched;
a root-managed public ID or profile matched;
the target requires elevation.
```

`hold --system start <profile>` may also self-elevate. Before the sudo boundary, a root-managed profile must be resolved to the internal start capability shape:

```text
start 000000000000 <profile> <hash>
```

The elevation boundary is argv-preserving `fork()` + `waitpid()`:

```text
parent:
  fork()
  waitpid(sudo_child)
  return sudo/root-On Hold exit status

child:
  execvp("sudo", [
    "sudo",
    "--",
    "/absolute/path/to/hold",
    "--system",
    "--elevated",
    <canonical-command using system:<id> for ID targets or <runid_sel> <profile> <hash> for profile capabilities>,
    NULL
  ])
```

No shell is used. There is no quoting layer, no `sudo sh -c`, and no string command payload.

Before building the sudo argv, On Hold resolves its own executable path:

```text
Linux: /proc/self/exe
macOS: _NSGetExecutablePath() + realpath()
fallback: realpath(argv[0]) when argv[0] contains '/'
```

If it cannot determine a safe executable path, elevation fails before invoking sudo:

```text
hold: cannot determine executable path for sudo self-elevation
```

For action commands, `stdin`, `stdout`, and `stderr` are inherited by the sudo/root-On Hold child. On Hold does not pipe or capture terminal I/O across the elevation boundary. This preserves sudo password prompting, sudo diagnostics, root On Hold diagnostics, streamed output, and Ctrl-C behavior while still letting the non-root parent return the child status.

Exit-code contract:

```text
If sudo successfully starts root On Hold:
  final exit code is root On Hold's exit code.

If sudo cannot authenticate, is denied, or is cancelled:
  sudo owns the failure and its stderr explains it.

If the child cannot exec sudo at all:
  the child prints a diagnostic and exits 127;
  the non-root parent returns 127.
```

## 9. List behavior

Normal list:

```bash
hold list
```

shows:

```text
user-local private rows
root-managed public redacted rows
```

Normal list must never prompt for sudo. Redacted root rows use:

```text
CMD   = <root-managed>
STATE = unknown
RESULT = -
```

Root/system list reads authoritative private root records:

```bash
sudo hold list
hold --system list
```

The implementation may later add non-interactive hydration through `sudo -n`, but any such hydration must be optional and must fall back silently to redacted public rows when unavailable.

## 10. Record format

One JSON record is written per run.

Core fields:

```text
version
id
run_id
start_unix_ns
argv
uid
gid
log_path
boot_id
started_at
ended_at
state
exit_code
term_signal
launch_error
alias
console_sock
```

`alias` is the current internal record field for the public profile label. `console_sock` is present only for console-enabled runs. Run records never store protected profile hashes; hashes live in root-private `profiles.json`, public root-managed `aliases.json`, and sudo capability argv only.

Best-effort process identity fields:

```text
proc_starttime_ticks
exe_dev
exe_ino
```

Root-managed private records also include:

```text
invoked_by_uid
invoked_by_gid
invoked_by_user
invoked_via_sudo
```

Record writes are atomic:

```text
write temp file in same directory
fsync temp file
rename to final name
fsync containing directory when possible
```

## 11. Process creation and logging

On Hold launches the child in a new session / process group. Child `stdin` is redirected from `/dev/null`; child `stdout` and `stderr` are redirected to the per-run log.

Docker-shaped launches (`hold <cmd...>` and `hold run <cmd|profile>`) run in the foreground and stream logs by default. Detached launches (`hold -d <cmd...>` or `hold run -d ...`) print the bare 12-character run ID to stdout. Human banners and confirmations, including `profile`, `grant`, `revoke`, `stop`, `kill`, and `prune` status lines, go to stderr and are suppressed by `--quiet` where applicable. `hold -f <cmd...>` remains accepted as a compatibility spelling for explicit follow mode.

An exec-success handshake distinguishes successful `execvp()` from immediate exec failure:

```text
parent creates close-on-exec pipe
child writes errno if execvp fails
successful exec closes the pipe through CLOEXEC
parent treats EOF as exec success
```

Exec-launch failure must leave no record and no orphan log.

## 12. State evaluation and signaling safety

Before signaling, On Hold evaluates the record against the current process table.

Checks include:

1. current boot marker when available;
2. leader PID identity;
3. same-session process-group membership when the leader is gone but group members remain.

Linux uses `/proc/<pid>/stat`, `/proc/<pid>/exe`, and `/proc/sys/kernel/random/boot_id` where available. macOS uses `sysctl`/kernel process metadata, best-effort executable identity, and `kern.boottime`.

States:

```text
running
exited
stale
failed
unknown
```

Signaling refuses `stale` and `unknown` targets. A zombie leader with live same-session group members remains `running`; an empty or zombie-only same-session group is treated as exited.

## 13. Prune behavior

`hold prune` removes prunable past run data, unreferenced logs, and orphan console sockets. Running records are kept. `hold prune <id>` removes exactly one prunable target. `hold prune all` removes all prunable targets.

Root-managed prune follows the same resolver and elevation rules as other action commands.

### 13.1 Console behavior

`hold console <target>` attaches to a running console-enabled run through the recorded private socket. Interactive attaches save the local terminal, enter an alternate screen, forward terminal resize events to the child PTY, and restore the original terminal state when the attach exits. Ctrl-P Ctrl-Q detaches without stopping the run. Non-interactive attaches stream stdin/stdout without changing screen state. On attach, the broker replays recent PTY output before live output so reattaches can redraw an idle console without sending input to the child.

A run ID targets that one run directly. A profile resolves by recorded profile label and the `console` verb intent-set: running runs with `console_sock`. More than one matching profile run exits 6 and prints candidates; zero candidates for a known profile exits 0. A finished run reports that it has exited and points the user to `hold dump <id>`. A non-console run reports that it has no console.

Console is interactive process access and follows the same privilege boundary as `stop` and `kill`: user-local sockets are private to that user, and root-managed console attaches require root authority or a matching sudoers grant. The public root index must not expose console socket paths.

## 14. Sudoers grants

`hold grant` and `hold revoke` manage only On Hold-owned sudoers entries. They require root authority and operate on root-managed profiles only:

```text
hold grant <profile> <user> [start,stop,kill,tail,dump,prune,console]
hold revoke <profile> <user> [start,stop,kill,tail,dump,prune,console]
```

The grant target must be an existing root-managed profile. The `<user>` argument may be a username, `%group`, or `all`. On Hold resolves the profile to its immutable profile hash before writing sudoers; the managed filename is keyed by profile and user, and the sudoers command carries a fixed profile/hash pair plus an 12-hex `runid_sel` slot so root can verify the profile/hash pair and selected run records before acting. If the action list is omitted, all supported On Hold actions for that profile are selected. This is a wildcard over On Hold's supported profile actions, not arbitrary sudo command access. `purge` is not a supported action; the command is `prune`.

Before writing sudoers, On Hold resolves its own executable path and refuses to proceed unless that file is root-owned, regular, and not writable by group or world. Managed sudoers lines grant NOPASSWD access only to tightly scoped canonical invocations with one anchored argument regex, such as:

```text
alice ALL=(root) NOPASSWD: /usr/bin/hold ^--system --elevated (start|stop) [0-9a-f]{12} web-test <hash>$
```

The managed file path is `/etc/sudoers.d/hold_<profile>_<user>` in production. Test builds may use `HOLD_TEST_SUDOERS_DIR`. Writes go to a same-directory `.tmp` candidate, use mode `0440`, are validated with `visudo -cf <tmp>`, and then `rename()` into place.

## 15. Test harness contract

The test suite must validate On Hold contexts explicitly rather than inheriting the test runner's EUID.

The harness creates three actors:

```text
USER_ACTOR = normal non-root On Hold context
ROOT_ACTOR = direct root/system On Hold context
SUDO_ACTOR = root On Hold with SUDO_UID/SUDO_GID/SUDO_USER for USER_ACTOR
```

Normal-behavior tests run through the user actor. Root/system behavior tests run through the root actor. Mixed-resolution tests use both actors.

When CI starts as root, the harness creates a temporary non-root test user. When CI starts as non-root, the current user is the user actor and `sudo -n` is used for root actor tests when available.

Tests must not touch real `/var/lib/hold` or `/var/db/hold`. `make test` compiles with `HOLD_TESTING` and uses `HOLD_TEST_SYSTEM_STATE_DIR`.

## 16. Non-goals

This implementation does not add root log visibility for normal users, global all-user private state scanning, global run-ID uniqueness across all users, or a daemon/supervisor. Root logs and private root records require root authority; normal users see only redacted public index rows.

## Continue

[Back to docs index](index.md) | [Quickstart](quickstart.md) | [Top](#hold-specification) | [Archived documentation plan](archive/PLAN.md)
