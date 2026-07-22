# Hold On reconstruction — contract gaps

The reconstruction is only safe behind a complete contract: any behavior the 149-test
suite does not pin can silently vanish or mutate in a rewrite. This maps every gap
found in the test-contract audit, each with a proposed pinning test. **43 gaps.**

Priority: **P1** = documented identity/spec surface with zero tests (a rewrite would
plausibly ship without it). **P2** = behavior exists and matters but only indirect or
unit-level pinning. **P3** = edge/exact-string pinning.

## Launch semantics

- **G1 (P1) `--env-file`** — zero tests. Pin: `test_env_file_populates_recipe_env` —
  write a file with `K=V` lines incl. blank/comment lines, launch with --env-file,
  assert record recipe.env matches and hold's own environ is untouched.
- **G2 (P2) `--name` collision** — "names unique per store" unpinned. Pin:
  `test_name_collision_rejected` — start two calls with the same --name; second fails
  rc!=0 with a diagnostic naming the holder, first record untouched.
- **G3 (P3) generated adjective_noun name format** — only nonempty asserted. Pin:
  `test_generated_name_shape` — regex `^[a-z]+_[a-z]+$`, and two launches yield
  distinct names.
- **G4 (P2) foreground Ctrl-C leaves the call held, exits 0 (bare launch)** — pinned
  only for `tail`. Pin: `test_foreground_ctrl_c_detaches_bare_launch` — script(1)
  wrapper, SIGINT the foreground hold, assert exit 0 and the call still running.
- **G5 (P2) redial of a purged or name-ambiguous target** — unpinned. Pin:
  `test_redial_purged_and_ambiguous_targets` — redial a purged name → clean error
  rc=5, no record created; two ended calls sharing a prefix → ambiguity error.

## Records / store

- **G6 (P2) record and log file mode 0600 in the user store** — only system-store
  modes asserted. Pin: `test_user_store_artifact_modes` — stat record/.log/.log.idx
  after a normal start, assert 0600.
- **G7 (P2) rename input validation** — bad chars / duplicate target name unpinned.
  Pin: `test_rename_rejects_invalid_and_duplicate_names` — rename to `a b`, `x/y`,
  64-hex-shaped, and an existing name; all refused, original name intact.
- **G8 (P3) no `unsave` verb** — identity says "no unsave". Pin:
  `test_unsave_verb_does_not_exist` — `hold unsave <id>` → usage error rc=5, record
  still saved.
- **G9 (P1) legacy `Saved` spelling shim** — SPEC keeps it, no test loads one. Pin:
  `test_legacy_saved_key_still_protects` — hand-author a record with `"Saved": true`,
  assert sweep skips it. (If decision D-4 drops the shim, amend SPEC and pin the
  *rejection* instead — either way the behavior must be pinned before rebuild.)

## Signals / safety

- **G10 (P1) TERM→KILL escalation for a plain (non-console) call** — only the console
  path pins escalation. Pin: `test_end_plain_call_escalates_to_kill` — start a
  SIGTERM-trapping script without --console, `hold end`, assert it dies and record
  shows the kill escalation.
- **G11 (P1) leader-gone group signaling by recorded pgid+sid** — SPEC safety
  invariant, path unexercised. Pin: `test_signal_delivers_to_group_after_leader_exit`
  — leader spawns children and exits; `hold end` must signal the group via recorded
  pgid+sid (children die), not fail on the dead leader.
- **G12 (P2) cross-boot recycled-PID refusal** — boot-id *mismatch* untested (only
  boot-source-missing). Pin: `test_boot_id_mismatch_marks_stale_never_signals` — under
  HOLD_TESTING, rewrite the record's boot_id; end/stop must refuse to signal and
  render Stale.

## Console / PTY / shell

- **G13 (P1) `hold on` / `hold off` verbs** — identity-doc rename with zero pinning
  (all tests say `shell`). Pin: `test_hold_on_off_spellings` — run the existing shell
  adoption scenario through `hold on`, and `hold off` as detach/exit spelling; assert
  same behavior as `shell` (or pin the aliasing explicitly).
- **G14 (P1) `hold attach <target>` primary spelling** — only `console` alias tested.
  Pin: `test_attach_verb_matches_console` — attach via `hold attach <id>`, round-trip
  bytes, detach; usage text names `attach`.
- **G15 (P2) automatic console detection heuristics** (termios dwell line-vs-raw,
  positioning-escape full-screen tag, tty_nr sampling) — zero tests. Pin:
  `test_shell_adoption_classifies_session_type` — adopt (a) a line-mode loop, (b) a
  raw-mode/alt-screen program; assert the recorded session classification differs.
- **G16 (P3) inspect session-type field value** — classification field unpinned. Pin:
  extend inspect test: assert `plain` vs `console` vs `fullscreen` values for three
  known launches.
- **G17 (P2) broker adopted-mode attach round-trip** — only the shell-side adoption is
  tested. Pin: `test_adopted_console_attach_round_trip` — after C-p C-q adoption,
  `hold attach <id>`, assert replay + live echo + detach works against the adopted
  broker.

## Logs / viewer

- **G18 (P1) `logs -p -t/--time` and `--date` CLI flags** — identity feature,
  formatting only unit-pinned. Pin: `test_logs_plain_timestamp_flags` — run
  `hold logs -p -t` and `--date`, assert sidecar-derived prefixes (regex HH:MM:SS /
  date), and that a log with no sidecar degrades gracefully.
- **G19 (P1) auto-plain when stdout is not a TTY** — identity: "plain output
  automatically when not a TTY". Pin: `test_logs_non_tty_auto_plain` — pipe
  `hold logs <id>` through cat; assert raw log bytes, no escape sequences, rc=0.
- **G20 (P2) interactive stream filtering (OUT-only/ERR-only views)** — masking is
  unit-only; Ctrl-Y pins only the column. Pin: extend the tty chrome test — toggle
  into ERR-only view, assert stdout-origin rows disappear from the pane.
- **G21 (P2) sidecar v1 record byte layout as external format** — only magic pinned
  independently. Pin: `test_logidx_entry_byte_layout` (unit) — hand-pack a 16-byte
  entry from documented shifts, write header+entry with dd-style bytes, assert hold's
  reader decodes offset/len/delta/stream exactly; guards D-2 regressions.
- **G22 (P3) informative line for a misleadingly empty log** — identity sentence,
  no test. Pin: `test_empty_log_prints_informative_line` — call that wrote nothing;
  `hold logs` emits exactly one explanatory line to the user, nothing to the log.
- **G23 (P2) viewer terminal resize** — no test. Pin:
  `test_viewer_survives_resize` — drive __view in a PTY, shrink/grow winsz via
  TIOCSWINSZ, assert redraw within budget and no crash/garbled footer.

## CLI parity / surface

- **G24 (P1) `hold list -u/--user`** — documented flag, zero tests. Pin:
  `test_list_user_scope_only_personal` — under sudo, `list -u` shows only the
  invoking user's calls, no global/redacted rows.
- **G25 (P1) `drop` purge alias** — identity lists rm/prune/drop; drop never run.
  Pin: `test_drop_alias_matches_prune` — `hold drop <id>` removes record+log
  identically to prune.
- **G26 (P2) `stop` ≡ `end` aliasing** — both used, equivalence never asserted. Pin:
  `test_stop_and_end_are_same_verb` — same target class, assert identical exit codes,
  stderr shape, and record end-state via both spellings.
- **G27 (P3) `Created` STATUS phrase** — parity doc lists it, never asserted. Pin:
  assert a created-but-not-started record renders STATUS `Created` in ps.
- **G28 (P2) PORTS column contents in ps/list for own call** — only bare `hold ports`
  and global projection pinned. Pin: `test_ps_ports_column_own_call` — listener call,
  assert `127.0.0.1:N/tcp` appears in the table PORTS cell.
- **G29 (P2) UDP and IPv6 port formats** — `[::]:80/tcp`, `/udp` untested. Pin:
  `test_ports_formats_udp_and_ipv6` — bind an IPv6 TCP and a UDP socket; assert both
  documented spellings.
- **G30 (P2) stats default streaming mode** — only --no-stream pinned. Pin:
  `test_stats_streams_by_default` — run `hold stats` in a PTY, assert ≥2 refresh
  frames then clean Ctrl-C exit.
- **G31 (P3) inspect uptime + full JSON shape** — unpinned beyond Stdio/tty/id. Pin:
  golden-key test: assert the documented key set and uptime monotonicity for a
  running call.
- **G32 (P3) humanized CREATED edge phrases** — exact strings ("Less than a second
  ago") unasserted. Pin: unit-test format_duration_human at the documented ladder
  boundaries (0s, 1s, 59s, 1min, 1h rounding, 2y).
- **G33 (P2) root refresh of observed ports into public entries** — the write half of
  the eventual-consistency mechanism unpinned. Pin:
  `test_root_list_refreshes_public_ports` (root lane) — user call binds a port; after
  `sudo hold list -a`, assert the public entry now carries observed_ports.

## Purge / prune

- **G34 (P2) `purge -a` includes stale** — identity says -a includes stale; only
  saved/ended exercised. Pin: `test_purge_all_sweeps_stale` — fabricate a Stale
  record (boot-id mismatch), plain purge skips it, `purge -a` removes it.
- **G35 (P1) orphan public entry sweep** — SPEC: public entry whose private record is
  gone is swept by `sudo hold purge`. Pin: `test_orphan_public_entry_swept` (root
  lane) — delete a private record, leave the public file, sweep, assert gone and no
  view resurrects it.
- **G36 (P2) purge of a live unsaved call without --force** — behavior unpinned
  (refuse? kill? skip?). Decide, then pin: `test_purge_live_unsaved_behavior` —
  assert the chosen contract (proposal: targeted purge of a live call refuses rc=2
  without --force, mirroring the saved wording).

## Restart

- **G37 (P1) `unless-stopped` policy** — parity-doc listed, never launched. Pin:
  `test_restart_unless_stopped` — crash → restarts; `hold end` → no restart, loop
  stops, term status recorded.
- **G38 (P3) explicit `no` policy** — untested. Pin: `--restart no` accepted, failing
  child not restarted, policy recorded.
- **G39 (P2) `--restart-delay` actually delays** — only validation and delay-0 run.
  Pin: `test_restart_delay_observed` — on-failure with 2s delay; assert inter-attempt
  gap ≥ delay via record/log timestamps (sidecar deltas make this deterministic).

## Platform / build / release

- **G40 (P2) macOS root store path /var/db/hold** — never asserted. Pin: macOS lane
  unit/integration — layout resolver returns /var/db/hold for the system store
  (compile-time testable without root via HOLD_TESTING indirection).
- **G41 (P2) macOS shell adoption contract** — permanently skipped, so unpinned. Pin:
  either implement + unskip on the macOS runner, or add
  `test_macos_shell_adoption_unsupported_is_honest` asserting the documented
  graceful refusal (an unpinned skip is not a contract).
- **G42 (P1) test_040_dockerish.sh not in `make test`** — its assertions are
  droppable without CI noticing. Pin: make `check` (or CI) invoke test-040; add a
  harness assertion that the target ran (marker file), so silent de-wiring fails.
- **G43 (P3) HOLD_TESTING isolation is convention, not asserted** — SPEC non-goal
  "tests never touch real system store". Pin: harness-level tripwire — run the suite
  with a canary file in the real store path (or fake-root), assert untouched;
  plus assert HOLD_TEST_SYSTEM_STATE_DIR is honored by every store-writing test verb.

## Tier summary

- **P1 (pin before any Phase 2 code): 13** — G1, G9, G10, G11, G13, G14, G18, G19,
  G24, G25, G35, G37, G42
- **P2: 21** — G2, G4, G5, G6, G7, G12, G15, G17, G20, G21, G23, G26, G28, G29, G30,
  G33, G34, G36, G39, G40, G41
- **P3: 9** — G3, G8, G16, G22, G27, G31, G32, G38, G43

Total: 13 + 21 + 9 = **43**.

## P1 pinning pass (2026-07-21) — results and divergences

All 13 P1 gaps now have pinning tests in `tests/test_hold.sh` (registered at the
end of the suite). Twelve pinned green on first run against the current binary:
G1 (`test_env_file_populates_recipe_env`), G9 (`test_legacy_saved_key_still_protects`),
G10 (`test_end_plain_call_escalates_to_kill`), G11
(`test_signal_delivers_to_group_after_leader_exit`), G13
(`test_hold_on_off_spellings`), G14 (`test_attach_verb_matches_console`), G19
(`test_logs_non_tty_auto_plain`), G24 (`test_list_user_scope_only_personal`,
root lane), G25 (`test_drop_alias_matches_prune`), G35
(`test_orphan_public_entry_swept`, root lane), G37 (`test_restart_unless_stopped`).
G42 is closed structurally: `make test` now invokes `make test-040`, so the
orphaned surface smoke can no longer be de-wired without CI going red.

**Divergences (code ≠ docs), test written but unregistered pending a decision:**

- **D-G18 `logs -p -t/--time` and `--date`** — hold-on-identity.md documents
  `-t/--time prepends times, --date adds date+time` on `hold logs <call> -p`,
  but the logs parser (`src/runtime/signal.c`, `hold_cmd_view_action`) accepts
  neither: `hold logs <id> -p -t` exits 5 with
  `usage: hold logs <target> [--follow|-f] [--plain|--interactive]`.
  `test_logs_plain_timestamp_flags` is written and commented out at its
  `run_test` line; either implement the flags (sidecar-derived, per the doc)
  and re-register it, or amend the identity doc and delete the test.

Highest reconstruction risk, restated: the identity-doc verb surface (on/off, attach,
drop, list -u) is entirely unpinned while tests pin the old spellings — a faithful
test-driven rewrite would reconstruct yesterday's CLI, not the documented one. Close
G13/G14/G24/G25 (or formally retire the renames) before writing any cli/specs code.

rsi validator: ACCEPTED — fresh-context verification, 2026-07-21 ~19:55. HEAD is
41c2841 with the P1 work uncommitted on top (contract-gaps.md, Makefile,
tests/test_hold.sh — the tree was NOT clean at verification; the builder had not
committed, consistent with their "monitor will wake me at the summary line" claim,
and no suite process was running). Clean `make clean && make` succeeds (only the
pre-existing glibc static-link warning). Independent full `make test` run:
**summary: 160 passed, 0 failed, 0 skipped** (149 baseline + 11 new), plus
viewer-filter-test, version/installer tests, and test-040 ("hold-on surface smoke")
all green, rc=0. All 11 registered P1 tests read as substantive — each carries
positive and negative assertions against real binary behavior, none vacuous. The
D-G18 divergence is genuine: independently reproduced `hold logs <id> -p -t` →
rc 5 with `usage: hold logs <target> [--follow|-f] [--plain|--interactive]`, while
hold-on-identity.md:64 promises `-t/--time` and `--date`; the test exists, is
commented out at its `run_test` line with an evidence comment pointing here. G42:
`make test` now invokes `make test-040`, verified in the run. Two nits: (1) the
pass summary above says "Twelve pinned green" but lists eleven (13 P1 = 11 green
+ G18 diverged + G42 structural); (2) G42's proposed marker-file tripwire was not
implemented — the wiring itself is still silently removable from the Makefile.
Neither blocks acceptance. This verdict commit carries the builder's doc section
in this file; the Makefile and test_hold.sh changes are left for the builder to
commit.

## Phase 2 ledger

- 2026-07-21 core swap 1cbfe17 — CONFIRMED. Layer wc -l 1712 -> 1232 (fs 146, json 291, logidx 280, sha256 106, util 223, validate 76, core.h 110), all counts verified against parent. Fresh `make clean && make` + `make lint` (layer dependency direction: clean) + one synchronous `make test`: rc=0, summary: 160 passed, 0 failed, 0 skipped, incl. viewer filter engine and hold-on surface smoke. Diff is structural; behavior deltas are disclosed and BOM/blueprint-sanctioned (hold_rand_bytes cut with temp nonce -> clock+counter under O_EXCL, append-only HLOGIDX header with count from st_size superseding invariant 32's min() rule, stricter scan_string skip/match modes, empty argv now -1 in get_argv_display). Old v1 sidecar headers verified byte-compatible (magic/version/base offsets unchanged). Budget note: json 291 vs 250, logidx 280 vs 210, fs 146 vs 130, core.h 110 vs 90; layer total 1232 vs ~1140 rows-sum (~8% over the "~" budgets), util/sha256/validate under.

- 2026-07-21 platform swap a8e1fcb — CONFIRMED. Layer wc -l 736 -> 614 (boot 44, paths 148, process 365, platform.h 57; .c-only 557 vs 540 blueprint rows-sum, ~3% over, boot 44 vs 25 the overage; process 365 under its 370), all before/after counts verified against parent. Bonus: observe.c 473 -> 175, observe.h 52 -> 39. Fresh `make clean && make` (zero compiler warnings; only pre-existing glibc static-link getpwuid note) + `make lint` (layer dependency direction: clean) + one synchronous `make test`: rc=0, summary: 160 passed, 0 failed, 0 skipped, incl. PASS: viewer filter engine and PASS: hold-on surface smoke. Diff verified structural: stat parsing unified behind proc_stat_fields/nth_field, both process-table walks behind for_each_process, scanner moved verbatim in behavior (LISTEN/unconnected-bound filters, hex addr decode, pipe:hold labeling, denied semantics all preserved); observe.c has zero /proc opens. Nit: commit message's "Platform is now the only /proc opener" overstates — runtime/shell.c retains its pre-existing /proc reads (untouched here; already tracked in bill-of-materials.md:171 as a separate cut). Does not block.

- 2026-07-21 store swap 419806c — CONFIRMED. Layer wc -l 1,094 -> 907 (atomic 49, layout 99, lifecycle 89, public_index 112, record 162, record_read 129, resolve 53, store_internal.h 49, store.h 34, types.h 131), all counts verified at HEAD; tree clean, commit matches claim. Fresh `make clean && make` (zero compiler warnings; only the pre-existing glibc static-link getpwuid note) + `make lint` (layer dependency direction: clean) + one synchronous `make test`: rc=0, summary: 160 passed, 0 failed, 0 skipped. Structure verified: one hold_record_fields table (record.c) drives writer + strict reader (record_read.c reads it via store_internal.h); hold_atomic_write_json is the only commit tail (record.c 0600 + public_index.c 0644, no rename/mkstemp elsewhere in the layer); hold_resolve_record_id exported and used at the 5 runtime matrix call sites plus the public-index path (runtime's 34-line resolve_run_id dir scan deleted, privilege/scope/intent matrix untouched); hold_for_each_record at all 6 former hand-rolled walks (resolve.c x1, call.c x2, list.c x3). Dead schema drops verified zero-caller (recipe.binary_path gone — remaining hits are the unrelated hold_resolve_binary_path; public error/paused/restarting/dead gone — only a names-adjective "dead" remains). Budget: 907 vs ~700 store rows + (135) headers = ~835, ~9% over (builder's own ~670 framing makes it ~237 over); overage is disclosed and test-pinned (run_id, normalized.argv, always-written saved, legacy "Saved" shim required by tests/test_hold.sh:3166/:4078/:4352/:4460).

- 2026-07-21 term swap e84b09e — CONFIRMED. New layer wc -l 230 (spawn.c 172, pump.c 23, term.h 35); console 1389 -> 1266 (broker 424, socket 250, console_internal.h 53; attach 276 / frame 207 / console.h 56 untouched); diffstat +258/-149 matches claim exactly; tree clean, commit at HEAD of audit-refactor-2026-07-20. Fresh `make clean && make` (zero compiler warnings; only the pre-existing glibc static-link getpwuid note; src/term objects linked) + `make lint` (layer dependency direction: clean, term row present, term forbidden in core/platform/store rows) + one synchronous `make test`: rc=0, summary: 160 passed, 0 failed, 0 skipped. Diff verified structural: PTY provisioning moved verbatim (O_NOCTTY both ends, O_CLOEXEC, 80x24 winsize fallback before exec); child-claims-TTY discipline preserved (setsid + TIOCSCTTY in the child only, broker never claims); errno-handshake semantics identical including child_errno?:EIO mapping, read-errno on handshake<0, and SIGKILL + EINTR-looped waitpid reap (old path did the same via broker_cleanup_and_exit's target>0 branch); old child's explicit fd closes replaced by CLOEXEC shedding — verified every broker-owned fd is CLOEXEC (listener SOCK_CLOEXEC, logfd O_CLOEXEC, logidxfd O_CLOEXEC via openat, master O_CLOEXEC); pump preserves log-then-replay-then-client ordering and EOF/EIO end-of-target gating; SO_PEERCRED auth, attach.c detach FSM, and broker rc protocol (127 fail / 0 clean) untouched. Nits, non-blocking: Makefile was 104 -> 105, not the claimed 105 -> 106 (delta +1 correct); spawn.c's header comment "no second copy anywhere else" overstates — runtime/shell.c retains its pre-existing posix_openpt/TIOCSCTTY ladder, explicitly deferred by the builder's own scope gate to the shell wave.

- 2026-07-21 shell-session swap 5a4679a — CONFIRMED. shell.c 754 -> 733 (diffstat +41/-62 matches claim); term/ unchanged at 195 (spawn 172, pump 23); start.c 1316 and broker.c 424 untouched — diff confined to src/runtime/shell.c; tree clean, commit at HEAD. Fresh `make clean && make` (zero compiler warnings; only the pre-existing glibc static-link getpwuid note) + `make lint` (layer dependency direction: clean) + one synchronous `make test`: rc=0, summary: 160 passed, 0 failed, 0 skipped (builder's first-run "redial honors a recorded foreground recipe" flake did not reproduce here). Diff verified structural: private open_pty_master/apply_window_size/spawn_shell_child ladder deleted, session rides hold_term_pty_spawn; the old in-child execl(shell)->execl(/bin/sh) fallback is correctly preserved because the engine's errno handshake surfaces exec failure as a -1 return, so the second spawn attempt fires (argv[0] "sh", exec_path /bin/sh, same as before); adoption fallback logger drains via hold_term_pump_master with identical break gating (EOF/EIO -> 0, other errors -1 with caller's EINTR retry kept, "stdout" stream tag and ignored log-write errors preserved); HOLD_ON_PID setenv still precedes the spawn so the child inherits it; relay/detach FSM (Ctrl-P Ctrl-Q) untouched. Budget deviation verified legitimate: start.c console child (start.c:922-949) is broker daemonization only — the setsid at start.c:907 claims no terminal, no TIOCSCTTY/posix_openpt anywhere outside term/spawn.c, and the only hold_term_pty_spawn call sites are broker.c:188 and shell.c:97/107, so there was no spawn-engine copy left to swap. Frozen invariants untouched by inspection: reaper mark rc 0/1/-1 purged-is-final (start.c:888-900, broker.c, shell.c mark paths) and SO_PEERCRED mutual auth. Disclosed behavior deltas, non-blocking: no-tty `hold on` now gets an 80x24 preset (engine rule) instead of kernel 0x0; a failed first spawn retries on a fresh PTY via a second fork rather than an in-child second execl (user-visible result equivalent); PTY-allocation failure now dies with "hold: failed to start shell" instead of the old dedicated "failed to allocate shell PTY" message.
