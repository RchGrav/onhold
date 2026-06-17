# Sigmund Specification

This document describes the current Sigmund implementation contract: user-local state, root-managed state, public redacted discovery, alias/profile resolution, sudo-aware target resolution, argv-preserving fork/wait self-elevation, and process-safety behavior.

## 1. Storage contexts

Sigmund has two storage contexts.

### 1.1 User-local state

Normal non-root starts create records and logs under:

```text
~/.local/state/sigmund
```

Required permissions:

```text
directory: 0700 user:user
records:   0600 user:user
logs:      0600 user:user
aliases:   0600 user:user aliases.json
```

User-local state is private to the user. User aliases store direct launch recipes in `aliases.json`; they do not create a user-global protected profile object. Sigmund does not scan every user's home directory and does not require globally unique run IDs across users.

### 1.2 Root-managed state

Root, sudo, and `--system` starts create records and logs under a universal system store:

```text
Linux: /var/lib/sigmund
macOS: /var/db/sigmund
```

Layout:

```text
/var/lib/sigmund/runs/      private root run records
/var/lib/sigmund/logs/      private root logs
/var/lib/sigmund/profiles.json
/var/lib/sigmund/public/    public root run index
/var/lib/sigmund/public/aliases.json
```

macOS uses the same layout under `/var/db/sigmund`.

Required permissions:

```text
root state dir:       0755 root:root
private run records:  0600 root:root
private logs:         0600 root:root
private profiles:     0600 root:root
public dir:           0755 root:root
public index files:   0644 root:root
public aliases:       0644 root:root
```

A root-managed start must never create a run under `/root/.local/state/sigmund` and must never accidentally create a root-managed run inside the invoking user's home state.

Production builds use the compiled system store path. Test builds compiled with `SIGMUND_TESTING` may honor `SIGMUND_TEST_SYSTEM_STATE_DIR`; production root Sigmund must not honor arbitrary user-controlled environment paths for the system store.

## 2. Invocation context

Sigmund detects the current authority and provenance at startup:

```text
effective UID is root?
was --system requested?
was internal --elevated present?
was root reached through sudo?
who is the invoking user, if sudo provenance exists?
```

When `geteuid() == 0`, Sigmund checks:

```text
SUDO_UID
SUDO_GID
SUDO_USER
```

If all are valid, they identify the invoking user. Sigmund resolves the invoking user's home directory through the user database (`getpwuid()`), not `$HOME`. In test builds only, `SIGMUND_TEST_INVOKING_HOME` may override that resolved path so tests can avoid real user homes.

Root private records for system runs include private provenance metadata:

```text
invoked_by_uid
invoked_by_gid
invoked_by_user
invoked_via_sudo
```

This metadata is not written to the public index.

The internal `--elevated` flag marks an intentional sudo self-elevation boundary. It is not a user-facing feature. If `--elevated` is present while `geteuid() != 0`, Sigmund exits with a clean internal error.

## 3. Command parsing

Sigmund has two parsing modes.

### 3.1 Raw start form

```bash
sigmund <cmd...>
```

In raw start form, only invocation switches before the child command belong to Sigmund. Once the child command begins, all remaining arguments belong to the child.

```bash
sigmund --system qemu-system-x86_64 -m 4096
# --system belongs to Sigmund

sigmund qemu-system-x86_64 -m 4096 --system
# --system belongs to qemu-system-x86_64
```

Quoted child command text is opaque to Sigmund:

```bash
sigmund sh -c "qemu-system-x86_64 -m 4096 --system"
```

### 3.2 Sigmund-owned command form

If the first non-invocation argument is a known Sigmund command, Sigmund owns that command's argument list. Known commands include:

```text
list
stop
kill
tail
dump
prune
start
alias
aliases
grant
revoke
help
```

For Sigmund-owned commands, invocation switches may appear before or after the command arguments:

```bash
sigmund --system stop 7f3c2a9d
sigmund stop 7f3c2a9d --system
sigmund start "qemu-system-x86_64 -m 4096" --system
```

These canonicalize to the same root-side command shape when elevation is required.

`sigmund help [topic]` is a Sigmund-owned documentation command and never starts a child command named `help`.

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
sigmund <cmd...>
```

writes:

```text
~/.local/state/sigmund/<id>.json
~/.local/state/sigmund/<id>.log
```

### 4.3 Root-managed start

These create root-managed runs:

```bash
sudo sigmund <cmd...>
sigmund --system <cmd...>
sigmund start <cmd...> --system
```

A root-managed start writes:

1. a private root run record;
2. a private root log;
3. a public root index entry.

If public index creation fails, startup fails and rolls back. A root-managed run that cannot be discovered violates the storage model.

### 4.4 Alias start

`sigmund start <alias>` starts the launch recipe currently assigned to that alias.

For user-local aliases, Sigmund loads the direct recipe from the user's private `aliases.json` and records the alias label on the new run. For root-managed aliases, Sigmund resolves the public alias to its protected profile hash, crosses sudo with the internal capability argv shape `<verb> <runid_sel> <alias> <hash>` when needed, verifies that alias/hash pair as root, loads the protected root-private profile for `start`, and records the alias label on the new run.

`--multi` is an alias-start modifier. Without `--multi`, `start <alias>` refuses when that alias already has a running process. Bare `--multi` starts one additional run and bypasses that guard; `--multi N` and `--multi=N` start N runs.

## 5. Public root index and aliases

The public root index contains only safe discovery and elevation metadata.

Example:

```json
{
  "id": "7f3c2a9d",
  "root_managed": true,
  "requires_elevation": true,
  "alias": "web-test",
  "state_hint": "unknown",
  "started_at": "2026-06-15T18:42:11Z"
}
```

The public index must not include:

```text
argv
cmdline_display
log_path
environment
pid
pgid
sid
boot_id
process start time
executable identity
sudo provenance
private filesystem paths
profile hashes
```

Private root records are authoritative. Public records are derived discovery data.

Because Sigmund is daemonless and cannot continuously refresh root-public state after natural process exit, normal `sigmund list` displays public root rows as `unknown` rather than overselling stale `running` hints. Root/private list and root action commands evaluate authoritative state from private records.

`public/aliases.json` is a flat JSON object mapping validated alias names to 64-character profile hashes:

```json
{
  "web-test": "fb736e64274bb2fd4861ff5d239288d4abc74aa3ae233b733b6201da507868ee"
}
```

Public aliases must not include argv, binary paths, log paths, environment, process identity, or sudo provenance.

## 6. Run IDs and collision checks

Run IDs are random opaque 8-character lowercase hex identifiers. `00000000` and `ffffffff` are reserved internal sentinels and must never be generated. Global uniqueness across all users is not required.

### 6.1 User-local start avoids

```text
that user's local records
that user's local logs
that user's local reservation files
root-managed public index IDs
```

### 6.2 Root-managed start with sudo provenance avoids

```text
root-managed private records
root-managed private logs
root-managed reservation files
root-managed public index IDs
the invoking user's local records/logs/reservations
```

### 6.3 Direct root start avoids

```text
root-managed private records
root-managed private logs
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
dump
```

Targets may be:

```text
run ID prefix
alias
user:<target>
system:<target>
```

Alias names must be 1 to 64 characters and must contain only `[A-Za-z0-9_-]`. They must not contain `/` or `.`, and must not parse as a full profile hash. If an alias also looks like a run-ID prefix, concrete run-ID resolution wins before alias lookup.

A profile hash is not a run ID and is not a normal action target. Hashes identify protected launch recipes and sudoers capabilities. Action commands resolve aliases by matching the alias label recorded on run records.

Alias resolution is verb-specific:

```text
start: running alias-labeled runs gate new starts unless --multi is supplied
stop/kill: running alias-labeled runs
tail: running alias-labeled runs
dump: alias-labeled runs with logs
prune: prunable alias-labeled past data
```

If an alias selection has zero candidates but the alias exists, the action is a successful no-op. If the alias is unknown, the action returns not found. If an alias selection has more than one candidate, Sigmund exits 6 and prints the filtered candidates. `--all` resolves that ambiguity for `stop`, `kill`, and `prune`.

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
else if user-local alias exists:
  target user-local
else if root public ID exists:
  target root-managed requiring elevation
else if root public alias exists:
  target root-managed requiring elevation as <runid_sel> <alias> <hash>
else:
  not found
```

Important invariant:

```text
If user-local and root-managed public targets share a plain token,
normal Sigmund targets the user-local match and does not self-elevate.
```

### 7.2 Root/sudo plain target

```text
root_match = root-managed private ID exists, or alias matches the verb-specific root-managed run set
user_match = invoking-user-local ID exists, or alias matches the verb-specific invoking-user run set, if sudo provenance exists

if root_match:
  target root-managed
else if user_match:
  target invoking-user-local
else:
  not found
```

This means `sudo sigmund stop <id>` can stop an invoking user's local run when that ID exists only in the invoking user's local state. In the rare conflict where both exist, root-managed wins.

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

If `system:<target>` is used from normal non-root invocation, Sigmund may self-elevate. If `user:<target>` is used from root/sudo invocation, Sigmund targets the invoking user's local state when sudo provenance exists. Direct root using `user:<target>` without sudo provenance returns a clean error.

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
sigmund-profile
resolved absolute binary path
argc
argv[0] index, argv[0]
argv[1] index, argv[1]
...
```

The domain string `sigmund-profile` is a fixed namespace label, not a version. Do not append versions or add environment, cwd, uid, timestamp, hostname, or other context to this hash input. Existing aliases, profiles, and sudoers grants are keyed by this digest; changing the input silently invalidates them.

Sigmund does not scrub, allowlist, capture, or hash the launched command's environment. `perform_start` and profile starts use the inherited process environment unchanged. Privilege-crossing starts rely on sudo's standard `env_reset` behavior before root Sigmund reaches `perform_start`; disabling `env_reset` or preserving loader variables through sudoers is host sudo policy, not Sigmund policy.

When a normal user targets a root-managed alias, Sigmund resolves the public alias to the current protected hash before self-elevation and carries the internal capability argv shape over sudo:

```text
<verb> <runid_sel> <alias> <hash>
```

`runid_sel` is always present. It is a concrete 8-hex run ID, `00000000` for `start`, or `ffffffff` for an approved `--all` action. Root Sigmund must verify that the alias still points at that hash and that selected concrete run records are recorded under that alias before acting.

## 8. Self-elevation boundary

Normal non-root Sigmund may self-elevate only when:

```text
the command is stop, kill, prune, tail, or dump;
no user-local plain target matched;
a root-managed public ID or alias matched;
the target requires elevation.
```

`sigmund --system start <alias>` may also self-elevate. Before the sudo boundary, a root-managed alias must be resolved to the internal start capability shape:

```text
start 00000000 <alias> <hash>
```

The elevation boundary is argv-preserving `fork()` + `waitpid()`:

```text
parent:
  fork()
  waitpid(sudo_child)
  return sudo/root-Sigmund exit status

child:
  execvp("sudo", [
    "sudo",
    "--",
    "/absolute/path/to/sigmund",
    "--system",
    "--elevated",
    <canonical-command using system:<id> for ID targets or <runid_sel> <alias> <hash> for alias capabilities>,
    NULL
  ])
```

No shell is used. There is no quoting layer, no `sudo sh -c`, and no string command payload.

Before building the sudo argv, Sigmund resolves its own executable path:

```text
Linux: /proc/self/exe
macOS: _NSGetExecutablePath() + realpath()
fallback: realpath(argv[0]) when argv[0] contains '/'
```

If it cannot determine a safe executable path, elevation fails before invoking sudo:

```text
sigmund: cannot determine executable path for sudo self-elevation
```

For action commands, `stdin`, `stdout`, and `stderr` are inherited by the sudo/root-Sigmund child. Sigmund does not pipe or capture terminal I/O across the elevation boundary. This preserves sudo password prompting, sudo diagnostics, root Sigmund diagnostics, streamed output, and Ctrl-C behavior while still letting the non-root parent return the child status.

Exit-code contract:

```text
If sudo successfully starts root Sigmund:
  final exit code is root Sigmund's exit code.

If sudo cannot authenticate, is denied, or is cancelled:
  sudo owns the failure and its stderr explains it.

If the child cannot exec sudo at all:
  the child prints a diagnostic and exits 127;
  the non-root parent returns 127.
```

## 9. List behavior

Normal list:

```bash
sigmund list
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
sudo sigmund list
sigmund --system list
```

The implementation may later add non-interactive hydration through `sudo -n`, but any such hydration must be optional and must fall back silently to redacted public rows when unavailable.

## 10. Record format

One JSON record is written per run.

Core fields:

```text
version
id
run_id
pid
pgid
sid
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
profile_hash
alias
```

`alias` is present when a run was started through an alias. `profile_hash` is present for protected root-managed profile starts; user-local alias starts record the alias label and do not need a profile hash.

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

Sigmund launches the child in a new session / process group. Child `stdin` is redirected from `/dev/null`; child `stdout` and `stderr` are redirected to the per-run log.

Start writes only the bare 8-character run ID to stdout. Human banners and confirmations, including `alias`, `grant`, `revoke`, `stop`, `kill`, and `prune` status lines, go to stderr and are suppressed by `--quiet` where applicable. `sigmund -f <cmd...>` starts and follows the log immediately; `--tail` remains accepted as a compatibility spelling.

An exec-success handshake distinguishes successful `execvp()` from immediate exec failure:

```text
parent creates close-on-exec pipe
child writes errno if execvp fails
successful exec closes the pipe through CLOEXEC
parent treats EOF as exec success
```

Exec-launch failure must leave no record and no orphan log.

## 12. State evaluation and signaling safety

Before signaling, Sigmund evaluates the record against the current process table.

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

`sigmund prune` removes prunable past run data and unreferenced logs. Running records are kept. `sigmund prune <id>` removes exactly one prunable target. `sigmund prune all` removes all prunable targets.

Root-managed prune follows the same resolver and elevation rules as other action commands.

## 14. Sudoers grants

`sigmund grant` and `sigmund revoke` manage only Sigmund-owned sudoers entries. They require root authority and operate on root-managed profiles only:

```text
sigmund grant <alias> <user> [start,stop,kill,tail,dump,prune]
sigmund revoke <alias> <user> [start,stop,kill,tail,dump,prune]
```

The grant target must be an existing root-managed alias. The `<user>` argument may be a username, `%group`, or `all`. Sigmund resolves the alias to its immutable profile hash before writing sudoers; the managed filename is keyed by alias and user, and the sudoers command carries a fixed alias/hash pair plus an 8-hex `runid_sel` slot so root can verify the alias/hash pair and selected run records before acting. If the action list is omitted, all supported Sigmund actions for that alias are selected. This is a wildcard over Sigmund's supported alias actions, not arbitrary sudo command access. `purge` is not a supported action; the command is `prune`.

Before writing sudoers, Sigmund resolves its own executable path and refuses to proceed unless that file is root-owned, regular, and not writable by group or world. Managed sudoers lines grant NOPASSWD access only to tightly scoped canonical invocations with one anchored argument regex, such as:

```text
alice ALL=(root) NOPASSWD: /usr/bin/sigmund ^--system --elevated (start|stop) [0-9a-f]{8} web-test <hash>$
```

The managed file path is `/etc/sudoers.d/sigmund_<alias>_<user>` in production. Test builds may use `SIGMUND_TEST_SUDOERS_DIR`. Writes go to a same-directory `.tmp` candidate, use mode `0440`, are validated with `visudo -cf <tmp>`, and then `rename()` into place.

## 15. Test harness contract

The test suite must validate Sigmund contexts explicitly rather than inheriting the test runner's EUID.

The harness creates three actors:

```text
USER_ACTOR = normal non-root Sigmund context
ROOT_ACTOR = direct root/system Sigmund context
SUDO_ACTOR = root Sigmund with SUDO_UID/SUDO_GID/SUDO_USER for USER_ACTOR
```

Normal-behavior tests run through the user actor. Root/system behavior tests run through the root actor. Mixed-resolution tests use both actors.

When CI starts as root, the harness creates a temporary non-root test user. When CI starts as non-root, the current user is the user actor and `sudo -n` is used for root actor tests when available.

Tests must not touch real `/var/lib/sigmund` or `/var/db/sigmund`. `make test` compiles with `SIGMUND_TESTING` and uses `SIGMUND_TEST_SYSTEM_STATE_DIR`.

## 16. Non-goals

This implementation does not add root log visibility for normal users, global all-user private state scanning, global run-ID uniqueness across all users, or a daemon/supervisor. Root logs and private root records require root authority; normal users see only redacted public index rows.
