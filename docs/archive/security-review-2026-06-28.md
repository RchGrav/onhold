# hold 0.4.0 — Security Review (2026-06-28, branch `mund-0.4.0-redesign`)

> 2026-07-02 status: this document is retained as the historical review that produced the privileged-hardening repair backlog. The CR-001 through CR-015 repair batch addresses the actionable findings below, removes the stale `VERSION` file in favor of tag-derived versions, and passes both `bash scripts/linux.sh` and `bash scripts/linux.sh root` in the Apple container Linux CI path. See `.agentic/code-review-repair-2026-07-02.md` for the current repair/validation report.

**Method:** end-to-end review by five focused reviewers reading the *actual source* (no inference from
docs/comments/notes), each citing `file:line` and marking findings VERIFIED vs NEEDS-CONFIRMATION.
Every CRIT/HIGH below was **personally re-verified** against the cited code (greps: `chdir` absent
from the launch path; `st_uid` checked only at `sudoers.c:590`; both `log_path` opens lack `O_NOFOLLOW`).

## Headline

**The authorization/cryptography half is well-built and held up. The containment half is essentially
not built.**

- ✅ **Sound:** the `hold run <alias> --cap <hash> <token>` capability machinery — canonical
  NUL-delimited SHA-256 digest is injective and re-verified root-side; the executed **binary and argv
  come from the root-owned grant store, not from user input**; subject binding via `SUDO_USER`; atomic
  sudoers write + `visudo -cf` + rollback; the parser cannot let a non-root user smuggle `--elevated`;
  cross-user run confinement; console socket `SO_PEERCRED` auth; PTY/fd hygiene.
- ❌ **Missing:** the **trusted-execution-context layer** the whole delegation safety story rests on
  (spec §2.4 / direction-doc §6) — pinned root-owned **cwd**, root-owned **binary + full ancestor
  chain**, **sanitized/hash-covered env**, and `O_NOFOLLOW` on the log. Until it lands, a granted
  delegation is only as safe as the *manual* placement of the binary, its directory chain, the
  caller's cwd, and the sudo env policy.

This directly gates the "keep delegation" decision: **delegation must not ship until the CRIT + HIGHs
below are fixed.**

---

## CRITICAL

### CRIT-1 — Privileged child inherits the caller's cwd (no chdir to a pinned root-owned dir) — VERIFIED
`hold_perform_start_with_metadata_options` reads `getcwd()` (`start.c:381`) and the forked child does
`setsid` → env → stdio → `execv` (`start.c:491-561`) with **no `chdir`**. Grep confirms `chdir` exists
nowhere in the launch path (only `console/socket.c`). So every root-managed start runs in the
**unprivileged caller's cwd**.
- **Impact:** unconditional. Any granted program that loads anything relative to cwd — Python
  `sys.path[0]`/`''`, `.env`/config auto-discovery, `./`-relative `dlopen`, bundler/gem, etc. — executes
  attacker-controlled files **as root**.
- **Fix:** `chdir()` to a profile-pinned, root-owned, ancestor-validated directory before `execv`;
  refuse to start if unavailable. (Implements §6 cwd-pinning.)

---

## HIGH

### HIGH-1 — Granted binary / path-args / ancestor chain are never validated root-owned — VERIFIED
The only ownership check in the tree is the self-binary check (`sudoers.c:590`). Grant time
(`write_subject_grant_copy`, `sudoers.c:195`) and run time (`hold_resolve_binary_path` → `realpath()`,
`paths.c:23-24`, which *follows symlinks* with no ownership/permission check, then `execv` as root) do
**no** root-ownership, ancestor-chain, symlink, or ACL/group-writability validation of the executed
binary or its path arguments.
- **Impact:** if root grants a global profile whose binary, **any ancestor directory**, or a path-arg
  target is writable by the grantee (e.g. `/home/svc/app`, `/opt/app/bin/x` under a service-user dir),
  the grantee swaps it → **root RCE**. The hash pins the *command line*, not the *bytes on disk*.
- **Fix:** implement the §6/§2.4 validator — binary + each `/`-bearing path arg + cwd must be
  root-owned with a **fully root-owned, non-other-writable ancestor chain** (sticky-bit & ACL aware,
  `lstat` the chain); enforce at grant time **and** re-check at run time.

### HIGH-2 — Log opened without `O_NOFOLLOW` in a euid-root, user-owned-dir path → symlink root write — VERIFIED
`open(log_path, O_WRONLY|O_CREAT|O_APPEND, 0600)` at `start.c:552` (and `start.c:145` in the restart
supervisor) — the **only** artifact opens lacking `O_NOFOLLOW`. When `sudo hold <binary-in-invoker's-home>`
runs, the store is the **invoking user's local store** (`start.c:1303-1308`), whose `log_dir == base`
is a `0700` dir **owned by that user** (`layout.c:34-36`); `chown_user_local_artifacts` confirms the
euid-root + user-local + sudo combination (`start.c:692`). The id is observable pre-`open` via the
pre-fork `.<id>.reserve` file in that same user dir, so the user can plant `<id>.log ->
/etc/sudoers.d/x` (or any target) before the child opens it, and the root child appends
attacker-controlled output through the symlink. The follow-up `chown(log_path, …)` (`start.c:702`,
plain `chown`, not `lchown`) also follows the symlink and can hand the target's ownership to the user.
- **Impact:** local root privilege escalation from a constrained `sudo hold` grant.
- **Fix:** open logs `O_NOFOLLOW` (create via `openat` on a dir fd; consider `O_EXCL` against the
  reserved id); use `lchown`/`fchown`, not path `chown` (`start.c:699,702,706`).
- **Status:** code facts VERIFIED; real-world reach NEEDS-CONFIRMATION that `sudo hold <binary-in-home>`
  is a supported usage and the open-race is reliably winnable (it's a race, aided by the reserve file).

---

## MEDIUM

- **MED-1 — Privileged child inherits hold's environment; env not sanitized and not hash-covered.**
  `apply_child_env` only `setenv`s grant vars; never `clearenv` (`start.c:208-227`); the cap path
  carries no env, so the root child inherits whatever `sudo` passed. Safe only under default sudo
  `env_reset` (strips `LD_PRELOAD`/`NODE_OPTIONS`/`PYTHONPATH`…); a weakened `env_keep` reopens
  interpreter injection. Fix: build a clean env (clearenv + grant-pinned vars) and include env in the
  canonical digest. (impact NEEDS-CONFIRMATION, sudo-config dependent)
- **MED-2 — Group-liveness fallback signals a `(pgid,sid)` match with no per-member start-time anchor.**
  When the recorded leader pid is gone, `validate_signal_target` falls to `hold_group_session_liveness`
  (`signal.c:84`, `process.c:175-182`) and then `kill(-pgid)` (`signal.c:181`). Same-boot pid reuse as a
  `pgid==sid` session leader (before prune) → root SIGKILLs an unrelated user's session. Fix: require a
  member whose start-time ≤ recorded launch, or refuse when only `(pgid,sid)` matches.
- **MED-3 — Shell-adoption TOCTOU / pid-reuse** (`shell.c:511-520,286-370`). `TIOCGPGRP` → `/proc` scan
  → per-field `/proc` reads are independent lookups; if the foreground process exits and the pid is
  reused mid-window, hold records the wrong process's argv/exe/cwd and later signals it. **Same-uid
  only** (no privilege crossing) — metadata corruption / wrong-target signal. Fix: capture
  starttime+exe dev/ino first and re-validate before writing the record.
- **MED-4 — Relative path-args normalized against caller cwd** (`start.c:400` → `paths.c:79-104`).
  Same root cause as CRIT-1; resolve against the pinned root-owned cwd.
- **MED-5 — Profile hash covers `binary_path`+`argv` only; `env` not hashed and `cwd` not even stored**
  (`profile.c:33-50`). "What env/cwd runs" is bound only by store-uniqueness (`EEXIST`) + file perms,
  not by the capability token. Fix: hash argv+env+cwd; persist cwd.

## LOW

- sudoers temp opened without `O_EXCL|O_NOFOLLOW` at a fixed name (`sudoers.c:774-777`) — prod-safe
  (`/etc/sudoers.d` root-only); a hazard only if `HOLD_TEST_SUDOERS_DIR` points somewhere writable.
- `abs_hold` interpolated into the sudoers line with only a whitespace check (`sudoers.c:574-597`) — a
  `,`/`:`/regex char in the install path would be mis-parsed; admin-controlled, low.
- TOCTOU between signal validation and `kill(-pgid)` (`signal.c:170-210`) — inherent to `kill(-pgid)`;
  consider pidfd.
- `prune all` unlinks records that merely fail to load (`list.c:299-305`) — can orphan a live run on a
  transient read error; skip-and-warn instead.
- `SUDO_USER` not charset-validated on the *consumption* side (`invocation.c:24,40`) before use as a
  path component — reachable only already-root; validate for defense-in-depth.
- `hold_read_owned_file_no_symlink` does no uid check despite its name (`fs.c:211-272`) — backstopped by
  root-only dirs; misleading name invites misuse.
- Write-path ids not `hold_valid_id`-guarded before path interpolation (`record.c:7-20,193-196`) —
  ids are root-generated/validated upstream; add an assert.
- Private record not `fchmod`/`fchown(0,0)`-pinned like the public index (`record.c:22`) — safe via
  0700 dir; latent umask dependence.
- Framed-client oversized frame length stalls the single client slot (`frame.c:119-167`) — memory-safe
  (overflow-guarded), same-user attach-slot self-DoS; reject `len > pending` up front.
- `relay_shell_pty` Ctrl-P flush uses a per-iteration (not absolute) deadline (`shell.c:428-443`) — UX
  latency, not security; mirror `attach.c`'s absolute deadline.

## INFO

- Captive `enable` grants privileged *mode* with no secret (`captive.c:866-869`) — cosmetic; every real
  action re-checks `euid_root`. Document as decorative.
- `allowed_peer_uid` console grant is unreachable for the system store (0700 root `console_dir`) — fails
  **closed** (`start.c:439-441`, `broker.c:79-84`, `layout.c:147-149`).
- sudo-without-regex fails **closed** (literal `^run`/`{1,768}$` won't match real argv) — no bypass.
- self-binary check is grant-time only and omits the ancestor chain (`sudoers.c:585-595`, `stat` not
  `lstat`) — same root cause as HIGH-1.

---

## Verified-sound (positives)

Canonical digest injectivity + root-side re-verification (`sudoers.c:283-358,476-486`); binary/argv
sourced from root store not user input (`commands.c:1238-1245`, `sudoers.c:460`); `--cap` shape +
scoped sudoers line (`sudoers.c:646-657`); atomic sudoers write + visudo + rollback; non-root
`--elevated` refused before any privileged dispatch (`main.c:1106-1112`, re-checked `commands.c:1209,1263`);
`--`/owned-command parsing robust both directions; resolution confinement to caller's store + public
root-managed (`resolve.c:492-580`); strict validators applied at sinks (`validate.c:48-75`); JSON parser
bounded (depth 64, 1 MiB cap, O_NOFOLLOW); store public/private split leaks nothing sensitive; perms
0700/0600/0644 + `fchown(0,0)` + `O_EXCL|O_NOFOLLOW`; console `SO_PEERCRED` default-deny auth
(`socket.c:137-179`, `broker.c:266-279`); PTY controlling-terminal + CLOEXEC hygiene; `/proc` `comm`
parsing handles parens/spaces; viewer sanitizes non-printable log bytes (`tty.c:526-534`).

---

## Remediation priority (gating for "keep delegation")

1. **CRIT-1** — pin + validate cwd before exec.
2. **HIGH-1** — implement the §2.4/§6 trusted-path validator (binary + path-args + cwd, full ancestor
   chain, `lstat`, ACL/group-aware) at grant time and re-check at run time.
3. **HIGH-2** — `O_NOFOLLOW` on log opens + `lchown`/`fchown`.
4. **MED-1** — clean-env in the privileged child + cover env (and cwd) in the canonical digest (also MED-5).
5. **MED-2 / MED-3** — close the signal and adoption pid-reuse windows with start-time anchors.

Items 1–4 are the security work that must precede shipping delegation; 5 hardens identity correctness.
