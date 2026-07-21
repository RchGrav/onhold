# Hold On reconstruction — Phase 1 bill of materials

Date: 2026-07-21. Inputs: verified layer inventories for core, platform, store, console
(mechanism-level, cross-checked against HEAD and against .agentic/audit-2026-07-20.md,
with the audit's stale entries corrected); measured line counts at HEAD.

## Baseline

Measured at HEAD (`wc -l` over src/ + include/, excluding include/hold/names/ which is
1,579 lines of generated word data, carried verbatim):

| layer | lines now | inventory status |
|---|---|---|
| core (src/core + core.h) | 1,712 | verified, mechanism-level |
| platform (src/platform + platform.h) | 714 | verified, mechanism-level |
| store (src/store + store.h + types.h) | 1,094 | verified, mechanism-level |
| console (src/console + console.h + console_internal.h) | 1,391 | verified, mechanism-level |
| runtime (src/runtime + runtime.h) | 5,741 | **projected only** |
| viewer (src/viewer + log_viewer.h) | 2,322 | **projected only** |
| cli (cli.c + cli_main.c + main.c + config.h) | 1,168 | **projected only** |
| access (invocation.c) | 62 | projected (trivial) |
| shared headers not attributed above | ~100 | — |
| **total (excl. names data)** | **~14,308** | (the "13.5k" working figure undercounts by ~0.8k) |

---

## Core — 1,712 now → ~930 intrinsic

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| SHA-256 primitive + hex + NUL-field hashing (run-id identity) | yes | 120 | 115 |
| Entropy source hold_rand_bytes (temp-name salt only) | no | 43 | 10 |
| Identity/name validation (valid_id/prefix/alias/record; one hex loop, name_looks_like_id) | yes | 85 | 40 |
| SUDO_UID/GID env parsing (checked narrowing) | yes | 31 | 15 |
| Safe-syscall substrate (die_errno, sig_note, checked_snprintf, write_all, has_suffix) | yes | 55 | 40 |
| Shell-quote argv formatter (COMMAND column) | yes | 79 | 45 |
| Exec handshake reader (errno over CLOEXEC pipe) | yes | 24 | 20 |
| Time formatting/parsing (RFC3339 + Docker HumanDuration, spec-pinned) | yes | 84 | 75 |
| JSON emit (escape + argv array writer) | yes | 35 | 30 |
| JSON scanner/tokenizer (ONE decode-next-codepoint core; skip/parse/match as modes) | yes | 214 | 115 |
| Typed JSON accessors + argv/env materialization (shared number tail, one array walk) | yes | 199 | 105 |
| No-symlink fs hygiene (open_dir_no_symlink + close_keep_errno; mkdir_p; hardened reader) | yes | 187 | 110 |
| Unique temp file creation (dot-prefix + .tmp contract) | yes | 26 | 20 |
| HLOGIDX sidecar writer (append-only entries, NO per-chunk header rewrite) | yes | 230 | 120 |
| HLOGIDX sidecar reader (map load / binsearch find / timestamp format) | yes | 120 | 90 |
| run_id_display (12-hex truncation) | yes | 12 | 8 |
| core.h | — | 112 | ~90 |
| **subtotal** | | **1,712** | **~930** (inventory range 900–950) |

Cut fuel: D8 triple escape-decoder, D1 open-dir ladder ×3, D6 close-keep-errno ×~10,
D7 write+index tail ×2, D5 hex loop ×3, i64/u64 clone, alias exports
(read_small_file, mkdir_p0700), write-only sidecar header fields, dead
META_CONTINUATION flag, unreachable STDIN/PTY stream constants, 5 zero-caller exports,
PROFILE_* vestiges. AUDIT-STALE: D2/D3 already resolved at HEAD (ed2e316) — not counted.

## Platform — 714 now → ~345 intrinsic

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| boot-id acquisition (one entry point, NULL-means-skip contract) | yes | 41 | 25 |
| process identity capture/verify (ONE parameterized /proc stat parser; exe dev/ino) | yes | 160 | 55 |
| group+session liveness tri-state scan | yes | 80 | 45 |
| session-escapee count (callback over shared process-table iterator) | yes | 58 | 15 |
| leader/group existence probes (EPERM-is-alive, ESRCH-is-gone) | yes | 34 | 22 |
| PATH-search binary resolution | yes | 47 | 32 |
| cwd-relative canonicalization + argv normalization (--flag=path aware) | yes | 72 | 45 |
| path containment check ('/'-boundary) | yes | 15 | 12 |
| self-executable resolution (sudo re-exec) | yes | 33 | 25 |
| passwd-by-uid + /etc/passwd fallback (fallback conditional on static-build decision) | yes | 80 | 45 |
| platform.h | — | 31 | ~25 |
| **subtotal** | | **714** | **~345** (~300 if static-build passwd fallback dropped) |

Also absorbs from runtime: observe.c's /proc-shaped primitives (hold_proc_read_ids /
read_cpu_rss / fd_target) plus its !__linux__ ENOSYS stubs — the confirmed layer leak.
Budgeted in the blueprint under platform, saving more than it costs.

## Store — 1,094 now → ~670 intrinsic

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| store-layout resolution/creation (table-driven {path,mode,owner} loop) | yes | 148 | 80 |
| atomic private-record writer (shared hold_atomic_write_json helper) | yes | 184 | 95 |
| atomic public-index writer (same helper; sanitized projection) | yes | 105 | 50 |
| public-index reader (tolerant, per-field defaults) | yes | 72 | 50 |
| strict record reader (single-exit parse; no Saved shim, no run_id fallback) | yes | 178 | 130 |
| checked i64 narrowing (one unsigned helper) | yes | 49 | 15 |
| id-prefix resolution — THE one resolver, exported | yes | 54 | 45 |
| record teardown (free argv arrays) | yes | 12 | 10 |
| exit stamp, purged-is-final rc=1 vs retry rc=-1, owner restore, ports-cleared projection | yes | 72 | 60 |
| domain types (types.h minus dead fields: binary_path, run_id, public error/paused/restarting/dead) | yes | 136 | 100 |
| api surface (store.h + hold_for_each_record iterator + reserve create/clear) | yes | 35 | 35 |
| **subtotal** | | **1,094** | **~670** (inventory range 640–700) |

Cut fuel: atomic-commit tail ×2, uid/gid narrowing clones, layout triplet ×7,
resolver ×3 across layers (store keeps the only one), write-only "normalized" argv
block, phantom run_id, Saved shim (decision D-4 in blueprint), free/return pairs ×10.

## Console — 1,391 now → ~830 intrinsic (of which ~300 relocates into the shared term module)

| mechanism | essential | now | intrinsic |
|---|---|---|---|
| PTY provisioning + child exec + errno handshake | yes | 190 | 100 |
| AF_UNIX bind/connect via chdir-relative sun_path (one with_console_dir seam) | yes | 166 | 90 |
| mutual peer-uid authentication (3-way platform ifdef) | yes | 70 | 50 |
| broker serve loop + target lifecycle (fd-context struct; drop_client; one reap path) | yes | 285 | 170 |
| 64 KiB replay ring | yes | 50 | 35 |
| wire protocol: magic sniff + 3-byte frames (raw-passthrough = decision D-5) | yes | 137 | 90 |
| attach client (raw termios, alt screen, SIGWINCH→resize, exit codes 5/3) | yes | 183 | 130 |
| detach-key FSM, 500 ms prefix timeout, configurable keys | yes | 86 | 60 |
| adopted-broker entry (hold shell/on) | yes | 34 | 25 |
| target-pid report fd ordered write (deadlock-free contract) | yes | 12 | 10 |
| SIGTERM forwarding to held group (async-signal-safe, no SA_RESTART) | yes | 25 | 20 |
| terminal-mode restoration (DEC resets → alt-screen exit → termios) | yes | 26 | 18 |
| hand-rolled raw termios (cfmakeraw exists) | no | 9 | 0 |
| 9-arg fail/cleanup plumbing (essential behavior, struct shape) | partly | 95 | 30 |
| headers | — | 113 | ~60 |
| **subtotal** | | **1,391** | **~830–890** |

Latent bugs the rebuild must NOT copy: C-R1 (O_NONBLOCK client fd + write_all retries
only EINTR → replay flush to a slow reader drops the client; pick blocking-fd or
explicit drop-on-backpressure) and C-R2 (errno==EIO checked without gating n<0).

## Projected layers (no mechanism inventory yet — Phase 1 gap)

Method: apply the reduction ratio evidence from the four verified layers (47–64%
intrinsic/now) adjusted by known cross-layer duplication that lands in these layers.

| layer | now | projected intrinsic | basis |
|---|---|---|---|
| runtime | 5,741 | ~3,050 (53%) | highest verified dup density: spawn path ×3 (start.c console child, shell.c, broker), resolver ×3 (resolve.c, call.c vs store), record-walk skeleton ×4–6 (audit G4), shell.c reimplements five console mechanisms (audit C-B, ~150–200 lines), observe.c platform leak (~150 lines relocate), inline detach FSM duplicate |
| viewer | 2,322 | ~1,830 (79%) | tty.c is a dense TUI state machine, little verified dup; known cuts: dead STDIN/PTY switch arms, filter/tty share via one match engine |
| cli | 1,168 | ~725 (62%) | hand-rolled per-command flag parsing → one flag engine over command_specs; help/usage generated from same table |
| access | 62 | ~50 | trivial |

## Cross-layer invariants (binding on the rebuild — every one, grouped)

### Atomicity & store
1. Atomic visibility: unique temp in SAME dir (O_CREAT|O_EXCL|O_NOFOLLOW, caller mode) → write → fflush → fsync(fd) → close → rename → fsync(parent); dir-fsync warn-only, everything earlier fatal; ferror checked before commit; temp unlinked on failure.
2. Temp naming contract `.<prefix>.<pid>.<nonce>.tmp` — the purge sweep recognizes orphans by exactly this shape; changing it silently breaks two-phase-creation cleanup.
3. Public/private projection: public index never contains argv/env/cmdline/owning uid-gid — only id, name?, state_hint, timestamps, pids, observed_ports, exit_code, running. Absent observed_ports = "not observed yet", not "no ports".
4. Permission matrix: private records 0600; public files 0644 + fchown(0,0) when euid==0; user store dirs 0700; system base+public/ 0755 root; runs/logs/console 0700 root; sudo-created user stores chowned back through ~/.local → ~/.local/state → base.
5. Purged-is-final: mark_finished rc=1 (record vanished, terminal, never recreate) vs rc=-1 (retryable); the stat-to-rename window is accepted, must not widen.
6. Exit stamp: WIFEXITED→status, WIFSIGNALED→128+sig (+term_signal), else 255; root restores prior file uid/gid; public projection rewritten with ports cleared.
7. Strict private-record parse: required fields hard-fail (version, id, pid/pgid/sid, uid/gid, proc_starttime_ticks, exe_dev, exe_ino); every id-like int narrows with ERANGE; name only if valid_alias; console_sock only if absolute; post-load valid_record + embedded-id==filename-id.
8. Read fallback chains: created←started (ns and rfc3339); cmdline←argv-render←cmdline_display←"?"; public created←started←"-"; public running derives from state_hint=="running".
9. Reserve-file contract: .<id>.reserve lives from id reservation to record rename; writer unlinks it; sweep spares young reserves, reaps stale. Rebuild centralizes reserve/commit/abort in the store API.
10. Path layout is on-disk ABI: user store FLAT (+console/); system store runs/logs/public/console; HOLD_TESTING overrides compile out of release; HOME realpath'd first.
11. ID-prefix semantics: exact id wins if file exists; else prefix must match exactly one *.json or fail.
12. Optional fields: absent has_* flag ⇒ key omitted on write, absence not an error on read.

### Identity & signal safety
13. No signal on pid alone — identity is (pid, starttime token, exe dev+ino) captured at launch and re-verified, plus (pgid, sid) match, gated by boot-id. Every link stays.
14. Boot-id: NULL means "evaluate without boot check", never fabricate; macOS synthesized id stable within / distinct across boots.
15. Liveness is tri-state-plus-error: LIVE / ZOMBIE_ONLY / EMPTY / SCAN_ERROR all distinct; zombie never reads live; scan failure never reads empty.
16. Group scans filter pgid AND sid together; guard pgid<=1 || sid<=0 before any group op (kill(-1) is a massacre).
17. kill(x,0): EPERM = exists; only ESRCH = gone; other errno = error, not "no".
18. starttime tokens opaque and platform-local (Linux raw ticks, field 22 parsed after strrchr(')'); macOS usec); compare only within platform+boot.
19. exe identity is (st_dev, st_ino) of the resolved binary, never the path string.
20. Run-id derivation: SHA-256 over NUL-delimited fields (NUL appended even for NULL→"") — injective; 64 lower-hex, displayed as 12.

### Hardening
21. No-symlink discipline: every store-controlled open O_NOFOLLOW (O_DIRECTORY for dirs) + fstat re-verify on the fd; act on fd, never path; ownership check root-or-self-or-root-owned; MAX_RECORD_BYTES cap BEFORE malloc.
22. JSON parser hardening: depth cap 64; reject escaped NUL (backslash-u0000), surrogate escapes, raw control chars; bounds-checked outputs; strict number/bool terminators; get_i64 rejects '+', get_u64 rejects '+'/'-'.
23. checked_snprintf: truncation is ENAMETOOLONG, never a silently shortened path.
24. EINTR retry on every IO loop; write()==0 is EIO; close paths preserve errno; /proc reads O_RDONLY|O_CLOEXEC.
25. mkdir_p chmods only dirs it created; non-dir component → ENOTDIR. chown_if_root is a silent no-op when euid!=0.
26. die_errno captures errno before stdio; sig_note respects quiet.

### Paths & accounts
27. Everything compared/recorded is realpath'd; path_is_within_dir requires '/'-boundary prefix.
28. argv normalization rewrites only tokens resolving to EXISTING paths; --flag=path rewrites only after first '='; all else byte-identical.
29. PATH search: explicit slash bypasses PATH; empty PATH → /usr/local/bin:/usr/bin:/bin; access(X_OK) before realpath.
30. passwd order: platform account db first, file parse second; absent fields empty; oversize → ENAMETOOLONG.

### Sidecar / logs
31. HLOGIDX v1 on-disk format: magic "HLOGIDX\0", LE, versioned header with base_unix_us; entries pack 44-bit offset / 20-bit len-1 / 48-bit µs delta / 16-bit meta; existing sidecars must load or the version field gates a documented migration (sidecar-v2 WO owns the entry-layout decision).
32. Crash recovery: trust min(header count, physical count); corrupt header → truncate+re-init, never fatal; index-append failures ignored — indexing never fails or reorders the raw write; raw log is sole source of truth.
33. Record splitting at '\n'; indexed at pre-write EOF offset; that offset is the viewer's lookup key; single-writer assumption.
34. Oversize records saturate at 2^20 with META_TRUNCATED; len stored as len-1; len==0 skipped.

### Console / terminal
35. Exec handshake: child writes errno on ANY pre-exec failure then _exit(127); parent: EOF=success, errno=child failure, read-error=broker failure.
36. Ordering: target pid written to target_pid_fd BEFORE parent_pipe closes — parent's blocking pid read can never deadlock.
37. Broker failure pre-handshake: errno to parent_pipe, close all fds, unlink socket, kill+reap forked target, _exit(127) never exit().
38. Child: setsid + TIOCSCTTY + dup2×3; broker opens slave O_NOCTTY; PTY has nonzero winsize before exec (80x24 fallback).
39. Socket: umask(077)+0600 in 0700 console dir; chdir-relative sun_path; cwd saved as fd, fchdir restore BEFORE fork/exec, failure fatal; unlink/stat via absolute path.
40. Mutual authn: broker allows root|owner|allowed_peer_uid, writes "attach denied" before close; client verifies socket-file owner. Exactly one interactive client.
41. Replay ring flushed at accept before client goes live; ALL master output goes to the indexed log unconditionally.
42. Adopted mode: NEVER kill/waitpid the adopted group; exit via master EOF/EIO or liveness scan; SIGHUP hup_pid after; never set the SIGTERM-forward target.
43. Child mode: exit persisted via mark_finished, in-loop best-effort then post-loop retry (50×100 ms); rc==1 is final.
44. SIGTERM at broker forwards to -pgid via async-signal-safe handler, installed WITHOUT SA_RESTART, cleared at every reap site. SIGPIPE ignored, prior handler restored.
45. Frame protocol: magic "holdv1\0\0"; frames type+be16 len; 'D' data, 'W' resize (zero dims ignored), 'X' detach; detach default C-p C-q, ≤8 bytes configurable; held prefix flushes after 500 ms or mismatch.
46. Client stdin EOF ⇒ SHUT_WR, keep relaying output; exit codes 5 (no socket/name too long), 3 (other), 0 (clean).
47. Terminal restore order: DEC mode resets → leave alt screen → show cursor → termios TCSAFLUSH.
48. Serve loop 1000 ms poll doubles as post-exit drain: break only on quiet tick or master EOF — eager exit truncates final output.

### Spec-pinned formatting
49. format_duration_human is character-for-character go-units HumanDuration incl. int(hours+0.5) — do not "improve".
50. parse_rfc3339 rejects t<=0 — zero-value timestamp parses as absent, not 1970.

## Sum and honest cross-check

| | now | intrinsic |
|---|---|---|
| verified layers (core, platform, store, console) | 4,911 | **~2,775** (2,745–2,865) |
| projected layers (runtime, viewer, cli, access) | 9,293 | **~5,655** (5,300–6,000) |
| names word data (carried verbatim, excluded both sides) | 1,579 | 1,579 |
| **code total (excl. names)** | **14,204** | **~8,430** (range ~8,050–8,850) |

The sum lands inside the 8–9k target **but only 33% of it (2,775 lines) is backed by
verified mechanism-level inventories**. The runtime/viewer/cli figures are ratio
projections seasoned with the verified cross-layer duplication list; they are
defensible as estimates, not as a bill of materials. If viewer's tty.c turns out to be
as intrinsic as it looks (79% assumed), and runtime's business logic compresses worse
than its duplication suggests, the realistic landing zone is the TOP of the 8–9k band,
not the bottom. Phase 1 is incomplete until audit-launch / audit-mgmt / audit-viewer /
audit-cli style inventories exist for the remaining 9.3k lines. Do not commit to a
sub-8k number.
