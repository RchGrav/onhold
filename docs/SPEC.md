# Sigmund Specification

This document describes the current Sigmund implementation contract: user-local state, root-managed state, public redacted discovery, sudo-aware ID resolution, argv-preserving fork/wait self-elevation, and process-safety behavior.

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
```

User-local state is private to the user. Sigmund does not scan every user's home directory and does not require globally unique run IDs across users.

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
/var/lib/sigmund/public/    public root run index
```

macOS uses the same layout under `/var/db/sigmund`.

Required permissions:

```text
root state dir:       0755 root:root
private run records:  0600 root:root
private logs:         0600 root:root
public dir:           0755 root:root
public index files:   0644 root:root
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
```

For Sigmund-owned commands, invocation switches may appear before or after the command arguments:

```bash
sigmund --system stop 7f3c2a
sigmund stop 7f3c2a --system
sigmund start "qemu-system-x86_64 -m 4096" --system
```

These canonicalize to the same root-side command shape when elevation is required.

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

## 5. Public root index

The public root index contains only safe discovery and elevation metadata.

Example:

```json
{
  "id": "7f3c2a",
  "root_managed": true,
  "requires_elevation": true,
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
```

Private root records are authoritative. Public records are derived discovery data.

Because Sigmund is daemonless and cannot continuously refresh root-public state after natural process exit, normal `sigmund list` displays public root rows as `unknown` rather than overselling stale `running` hints. Root/private list and root action commands evaluate authoritative state from private records.

## 6. Run IDs and collision checks

Run IDs are random opaque hex identifiers. Global uniqueness across all users is not required.

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

## 7. ID resolution

All action commands use the shared resolver. Action commands are:

```text
stop
kill
prune
tail
dump
```

The resolver returns one normative result:

```text
resolved user-local
resolved root-managed
not found
clean error
```

Final action execution must use only the resolved target. Action commands must not perform ad hoc store probing outside the shared resolver except for final execution against the already resolved target.

### 7.1 Normal non-root plain ID

```text
if user-local ID exists:
  target user-local
else if root public ID exists:
  target root-managed requiring elevation
else:
  not found
```

Important invariant:

```text
If user-local and root-managed public entries share a plain ID,
normal Sigmund targets the user-local run and does not self-elevate.
```

### 7.2 Root/sudo plain ID

```text
root_match = root-managed private ID exists
user_match = invoking-user-local ID exists, if sudo provenance exists

if root_match:
  target root-managed
else if user_match:
  target invoking-user-local
else:
  not found
```

This means `sudo sigmund stop <id>` can stop an invoking user's local run when that ID exists only in the invoking user's local state. In the rare conflict where both exist, root-managed wins.

Direct root without sudo provenance has no invoking-user context and resolves plain IDs only against root-managed state.

### 7.3 Explicit deterministic ID tokens

Plain IDs are the normal interface. Explicit tokens are supported for rare deterministic cases:

```text
user:<id>    force user-local lookup
system:<id>  force root-managed lookup
```

Rules:

```text
user:<id> never targets root-managed state.
system:<id> never targets user-local state.
```

If `system:<id>` is used from normal non-root invocation, Sigmund may self-elevate. If `user:<id>` is used from root/sudo invocation, Sigmund targets the invoking user's local state when sudo provenance exists. Direct root using `user:<id>` without sudo provenance returns a clean error.

## 8. Self-elevation boundary

Normal non-root Sigmund may self-elevate only when:

```text
the command is stop, kill, prune, tail, or dump;
no user-local plain-ID target matched;
a root-managed public entry matched;
the target requires elevation.
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
    <canonical-command...>,
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
```

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

`sigmund prune` removes prunable records and orphan logs. Running records are kept. `sigmund prune <id>` removes exactly one prunable target. `sigmund prune all` removes all prunable targets.

Root-managed prune follows the same resolver and elevation rules as other action commands.

## 14. Test harness contract

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

## 15. Non-goals

This implementation does not add root log visibility for normal users, global all-user private state scanning, global run-ID uniqueness across all users, or a daemon/supervisor. Root logs and private root records require root authority; normal users see only redacted public index rows.
