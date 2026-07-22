# Work order — deep-dive findings and repair plan (2026-07-20)

## PLAYBACK CAMPAIGN COMPLETE (2026-07-22) — WO-6 SHIPPED

All four generations landed and validated (Phase 2 ledger): sidecar v2 +
self-healing recovery cd59fad (162/0, Darwin-checked); transport b88eda5
(--replay, tail scrub, chevron OSD, 168/0, Darwin); time-travel 33b12dd
(Ctrl-P double-tap into the viewer, pty tagging, nearest-clear repaint,
172/0, Darwin — correctly applied the spec over its stale brief); polish
748b63d (mode-scoped help, honest Deferred section in the spec, docs +
release notes; CONFIRMED-WITH-CORRECTION 6fec6d0 — suite is 174/0, the
builder grep-counted PASS lines). Final: Linux 174/0/0, tree 13,282
(11,771 + ~1,511 of WO-6 feature). Deferrals recorded in
docs/future/playback.md "Deferred". WO-6 items struck below as landed.

## (superseded) PLAYBACK CAMPAIGN IN FLIGHT (2026-07-22)

Phase 2 rewrite COMPLETE and sealed (11,771 lines; Linux 160/0/0, macOS
141/2/17 WO-8-proofs-only; ten generations, all validator-confirmed —
Phase 2 ledger in .agentic/reconstruction/contract-gaps.md). Now running:
the WO-6 playback campaign (workflow run wf_17013ef9-3d7), 4 generations:
sidecar-v2 → transport → time-travel → polish. BINDING SPEC =
docs/future/playback.md, complete with ALL of Rich's rulings through
2026-07-22 (double-tap Ctrl-P transparent jump from ANY attached surface,
never-released broker session, content-kind control sets, chevron OSD
1/2/3/4/8/16x, Space mode-scoped, self-healing sidecar ladder).

If the session was cut mid-generation: (1) `git status` — a dirty tree is
an unfinished builder; per the loop contract, `git checkout -- <code>` and
keep any committed generations; (2) suite must be ≥160/0 before anything
else; (3) resume the workflow from cache:
Workflow({scriptPath: <session workflows dir>/playback-campaign-*.js,
resumeFromRunId: "wf_17013ef9-3d7"}) or simply re-launch the remaining
generations from the spec — the spec, not the workflow, is the source of
truth. Time-travel gen's brief predates the final rulings; the SPEC
governs (its prompts say deviations are defects).

## RECONSTRUCTION ACTIVE (2026-07-21 evening) — read first on resume

The from-scratch reconstruction (Rich's directive: inventory, then rebuild;
same featureset, smaller tool) is underway. Phase 1+1.5 COMPLETE, all in
`.agentic/reconstruction/`: bill-of-materials.md (mechanism-verified all
layers, intrinsic total ~8,850, band 8,600–9,100 — target survives at band
top, sub-8.5k off the table), blueprint.md (five duplication-erasing ideas
+ layer DAG + reconciled budgets ~8,940), contract-gaps.md (43 gaps; 11 P1
pinned green, G42 wired test-040 into make test, D-G18 divergence recorded:
docs promise `logs -p -t/--date`, code rejects — recommend implement, not
de-document). Contract now 160 tests: Linux 160/0/0, macOS 141/2/17 (only
the two deliberate WO-8 observe.c proofs fail). Phase 2 = strangler swaps
per blueprint order, generation discipline (fresh builder + adversarial
validator per module, ledger, both platforms gate). Known agent failure
mode, thrice observed: builders piping the suite and "waiting on a
monitor" — Phase 2 prompts mandate synchronous suite-to-file with explicit
timeout. Open: D-6 (term keys+screen must net out vs absorbed tty.c code;
recommend build keys+screen, defer term/vt) and D-G18 above.

## RECONCILIATION PENDING (2026-07-21) — read first on resume

Two independent improvement lineages exist and must be reconciled:

1. **audit-refactor-2026-07-20 (ed2e316)** — the 07-20 campaign, committed.
   Unique: single native record schema + public-projection rewrite w/ leak
   tests; legacy JSON log path removal; 17-symbol dead sweep; resurrection
   + broker exit-stamp race fixes; macOS enablement (USER column,
   purge -s, BSD-portable harness); record.c 4-way split; consolidation
   tranche. Gates: Linux 133/0/0, macOS platform-gaps-only.
2. **origin/tempo-refinement (247596d, 07-04..06)** — TEMPO-driven pass,
   ledger preserved in-branch (194cdc1). Unique: restart supervisor
   records exit status; foreground exits with held process's status; env
   flags are recipe data (no setenv side effect); crash-safe record temps
   + two-phase purge; purge derives unlinks from layout; log -n
   semantics; +436-line viewer interactive navigation (WO-2/3 ground);
   "The code is the contract: prose docs leave the tree" (docs deleted).
   Overlap with (1), done differently: caps cut, ps-alias fix, stderr
   discipline — conflict-heavy in start.c/cli_main.c/tests.

Decisions Rich must make before merging: (a) which lineage leads the
reconciliation; (b) does "the code is the contract" stand (branch deletes
the prose docs that lineage 1 updated). Suggested method: TEMPO
Pressure-test — select/compose/sharpen per subsystem: storage+platform
from (1), behavior fixes+viewer from (2), caps/CLI resolved by hand.
TEMPO is now a real engine at /home/rich/tempo (worklog 07-06: 72 tests,
tempo run CLI, hash-chained ledger); Rich hinted the reconciliation may be
its first real workout. tempo/ and .tempo/ are gitignored in this repo.

**Open-decision registry (2026-07-21) — everything blocked on Rich, one
place, each with a recommended disposition (approve or overrule):**

1. Reconciliation lead lineage — (a) above.
   **Recommend: ed2e316 leads; cherry-pick tempo-refinement onto it.**
   ed2e316 is newer, sits on current main, and its schema/macOS/race work
   spans 41 files; the branch's unique wins (restart exit recording,
   foreground exit status, env-as-recipe, crash-safe temps, purge
   layout-derivation, log -n) are small, self-contained commits that
   cherry-pick cleanly. The +436 viewer navigation is additive in files
   ed2e316 barely touched. Replaying the campaign onto a three-week-old
   base is the strictly harder merge.
2. "The code is the contract" docs verdict — (b) above.
   **Recommend: reject the deletion; docs stay.** Design principle 4 says
   the design documents are *binding* — the viewer spec IS the acceptance
   checklist; §10 says docs are load-bearing and probe-verified. The
   branch's real complaint is drift, and the fix for drift is the
   claims-vs-tree verification pass (now standing), not deleting the
   contract. Salvage: prune any file that merely restates code, case by
   case.
3. WO-5 zap visibility + un-zap (§5).
   **Recommend: honor the spec both times — quiet chrome, Ctrl-R-only.**
   The binding spec bans counters (:294); a cue is cheap to add after
   real-usage confusion, impossible to remove cleanly once shipped.
   Stepwise un-zap spends novelty budget on mechanics (principle 3);
   revisit only if usage demands it.
4. WO-9 foreground Ctrl-C semantics (§6b).
   **Recommend: (a) announce-and-detach** — detach-on-interrupt IS the
   brand promise; one stderr line closes the honesty gap; honoring
   Ctrl-P Ctrl-Q in foreground gives Docker muscle-memory its exit.
   Option (b) makes bare `hold` kill things — the opposite of the name.
5. WO-6 rewind repaint (§6).
   **Recommend: replay-from-nearest-clear**, re-emit-from-start as the
   no-clear-found fallback — this is already how WO-10's term/vt
   reconstructs screens, so the decision falls out of the architecture;
   deciding otherwise would build a second mechanism.
6. Untracked `.agentic/hold-amalgamation-*` artifacts.
   **Recommend: delete.** Generated views, 12.8k lines, stale on the next
   refactor; the audit doc already records everything they proved.

Everything else in this document is either decided or mechanical.

Source: full-codebase deep dive (runtime, console, store/core/platform, CLI/tests/build)
plus design discussion with Rich. This document is the tracker; strike items as they land.
Code `file:line` citations are as of ed2e316 unless a section says otherwise —
re-grep before trusting one after further refactors.

## 0. Intent (read first, this governs every item below)

Hold is for people who live in a terminal and start processes they aren't ready
to commit to. In Rich's own enumeration (2026-07-20):

- CI and scripting — a tool to enhance CI processes; durable background
  helpers in a pipeline step that outlives the step.
- qemu, dev servers, workers — every tool where you need your prompt back.
- Anything you want to run "like a service without making it a service":
  no unit file, no daemon tools, no committing to systemd — and no writing a
  wrapper script just to run something in the background.
- Reboot-and-resume: restart the machine and have your held calls come back
  automatically, still without touching the system's service manager (WO-7).
- Seeing and understanding what ran: control it, list it, read the logs, and
  search them without stringing together greps and awks — and play a log back
  with real timing and transport controls (WO-6).
- Every *nix out there: Linux, macOS, and the BSDs (WO-8).

"More than `less`, less than systemd" — the viewer gives you more than a
pager; the tool stops far short of machine policy. It declines to compete with
Docker (substrate) or tmux (terminal emulation). Its user likes Docker's
muscle memory but runs real host processes and wants the truth about them.
When a test doesn't necessitate systemd, Hold is the answer.

Design principles, in Rich's words and priority:

1. **Compact, forgiving CLI.** `hold <command string>` just works. Minimal
   switches. Familiarity over configuration — "something you don't have to learn."
2. **A few killer features, polished.** Not breadth. The log viewer is one:
   text log + binary sidecar treated as a database, lightning navigation,
   professional TUI. Future: timed playback, integrity/self-healing index.
3. **Zero novel physics.** The viewer must feel like every text editor/pager
   ever used. Novelty budget is spent on *content* (timestamps, filter, zap),
   never on *mechanics* (scrolling, paging, selection).
4. **The design documents are binding, not suggestions.**
   `docs/future/viewer-fixes-before-0.5.md` is the viewer's acceptance spec
   (its own acceptance checklist, lines 539–564, is the test list).
   `docs/hold-on-identity.md` governs the surface. Deviations are defects.
5. **Tight code.** Small enough to review in an afternoon; reads properly; no
   duplication, no dead weight. Identity target: lean core 6–8k lines + viewer.

## 1. WO-1 — Cruft removal (correctness of the codebase's self-image)

**Acceptance principle (Rich, 2026-07-20): one concept, one mechanism.**
Line count is the symptom; the unit of bloat is the concept. 0.3.9 was
concept-bloated (profiles/aliases/grants/captive CLI — ~2.5-3k of its 9.7k,
all later cut as not-the-tool; even 9.7k was bloated). Today's tree is
mechanism-bloated: right concepts, duplicate implementations — two record
schemas, two log encodings, two tee loops, three collectors, hand-rolled
parsing beside a parser table. The cut is done when nothing exists twice:
one record schema, one log encoding, one tee, one spawn path with modes,
one table renderer. Expected landing ~7k core + ~1.5k viewer; the number is
the receipt, not the goal.

The 0.4.0 expansion (~9k → ~21.5k lines) was only half-reverted by the 0.5.0
cut. Measured (C src+headers, excluding `include/hold/names/` word lists):

| tag | lines |
| --- | --- |
| v0.3.9 (pre-expansion) | 9,021 |
| v0.4.0 (bloom) | 21,539 |
| v0.5.0 (the cut) | 14,309 |
| 0.6.x pre-campaign | 14,672 |
| HEAD after ed2e316 (measured 2026-07-21) | 13,517 |

Confirmed dead/cruft, remove:

- [x] `--cap-add` / `--cap-drop` — **LANDED ed2e316**, both halves of the
      either/or: plumbing deleted (parser, record fields, start-option
      threading, projection tests) AND the flags now honestly rejected with
      a capsh/systemd-run pointer. Decision was Rich's: "clearly cruft"
      (grants-era; a user runs sudo).
- [x] `hold_gen_id_for_store` — deleted in ed2e316. Live ID paths remain
      hash-based (`reserve_hashed_run_id`, `shell_hashed_adopt_id`).
- [x] `hold_sha256_file_hex` — deleted in ed2e316.
- [x] **Caller-less-function audit** — RAN 2026-07-20; all listed dead
      externals removed in ed2e316 (17 symbols total including
      late-orphaned `hold_rand_bytes`; ledger in
      `.agentic/audit-2026-07-20.md`).
- [x] **Legacy JSON-lines log path** — removed in ed2e316 (writers +
      transparent decode in filter.c/signal.c/logging.c). Release note
      still owed: pre-0.5 logs now read as plain text.
- [x] **Single-schema records** — landed in ed2e316, but the decision
      **inverted** from this item's original wording: Rich ruled later the
      same day that the *native snake_case* schema is the one format and
      the Docker shape goes ("it doesn't need to retain the docker shape,
      just these cli commands"). Docker parity is CLI-surface only; the
      one storage invariant kept is the public/private projection split
      (ports visible, cmdline/env/owner never public). Pre-0.7 records
      rely on a read shim for `Saved` — upgrade-fixture test recommended.
- [ ] **Consolidation tier** (~1,000–1,500) — PARTIAL in ed2e316:
      `sweep_orphaned_artifacts` (3 sweep blocks), `protect_record_action`
      (save/rename fusion), `write_str_field` (8 has_* blocks),
      `parse_id_number_env`, shell.c's duplicate proc-ids reader. Mapped
      remainder (~1k, file:line maps in the audit doc): rollback ladder,
      command dispatch table, flag-triplet table, reader has_* table,
      G1 logger pump, G2 record builders, prune.c extraction. Also from
      the original list: shell.c background logger vs
      broker tee; adoption record-assembly vs start.c's; restart supervisor's
      duplicated spawn/env logic; list.c triple collectors / double printers;
      filter.c forward/backward engine overlap.
- [ ] **Boilerplate tier** (~1,500–2,000): table-driven flag parsing off the
      existing command_specs[]; field-table macro for record.c's ~30 has_*
      repetitions; render_legacy/--debug-stats removal once WO-3/4 retests
      against real chrome.
      Rich's target (2026-07-20): ~5k lines removed → tree at ~10k with the
      viewer, i.e. the identity budget. Growth ledger 0.3.9→HEAD: record.c
      +647, start.c +719, signal.c +556 (keeps: earns it), list.c +471,
      cli +747; resolve.c −547 proves cutting works.
- [ ] **Duplication pass:** the 0.4 expansion left mass behind beyond dead
      functions (~5.7k lines above the pre-expansion base even net of the
      ~1.8k intended viewer). Hunt copy-paste siblings, over-general helpers
      with one caller, and parser paths only the deleted verbs used.
- [ ] Re-measure after each stage; record the number here.
      2026-07-21 (post-ed2e316 + record.c split + consolidation tranche):
      **13,517** (method: src+include \*.c/\*.h, excl. names word lists).
      Campaign net −1,155 vs pre-campaign. Distance to the ~10k identity
      budget: ~3.5k — the mapped consolidation/boilerplate tiers plus
      whatever the tempo-refinement reconciliation absorbs.

Acceptance: no symbol without a caller; no flag without behavior; line count
moves meaningfully toward the identity budget; `make lint && make test` green.

## 2. WO-2 — Viewer: filter must search the whole file (correctness bug)

**STATUS 2026-07-21: core fix LANDED** by experiment rsi-001 gen 1 (8cdf8bc,
validator-confirmed, 147/0): continuation across poll ticks via persisted
anchors, silent scan_limited stop gone, needle-past-budget test pins it.
Remaining: center-out discovery order (spec:297-315) and the typed-filter
pinned-page tension recorded in the rsi-001 ledger. Gen 2 (319468a) landed
the WO-3 edge-arrow fix (148/0); gen 4 (f9c74e2) landed selection-as-record-
identity + spec:189-190 selector placement (149/0). Ledger:
`.agentic/rsi-001-ledger.md`.

**Spec violation.** viewer-fixes-before-0.5.md:317 — the engine "must not
pretend a match does not exist just because an early budget expired. It should
remember continuation positions and continue."

Current behavior: scan budget = `visible_rows × VIEWER_SCAN_BYTES_PER_ROW`,
1 MiB floor (`src/viewer/tty.c:423`). On exhaustion `scan_limited` is set
(`src/viewer/filter.c:255,306,364`) and the scan **stops silently** — the
polished UI never surfaces it (only the frozen `render_legacy` debug view
prints `" | partial"`, `tty.c:889`). Matches past the window are reported as
nonexistent. Rich: "patently broken."

Fix shape (plumbing already exists — results carry `next_offset`/`prev_offset`
resume anchors and `reached_eof`; the viewer runs a 250 ms poll loop):

- [ ] Budget bounds **per-tick latency, not total coverage**: persist
      continuation offsets; keep scanning in cooperative slices across ticks
      until viewport satisfied, query/anchor changes, or both file boundaries
      exhausted (spec's exact rule).
- [ ] Discovery order **center-out from the selected record** (spec:297-315):
      R, R+1, R−1, R+2… Render always in log order.
- [ ] No permanent "searching" noise in chrome (spec:87,315) — results simply
      keep arriving; transient state only if genuinely slow.
- [ ] Test: match placed at the tail of a >100 MB log must appear; filter typed
      while scan is mid-file must restart discovery from the new query.

**Sequencing note (2026-07-21):** origin/tempo-refinement carries +436
lines of viewer interactive navigation (see RECONCILIATION, top). Land or
reject that branch's viewer work *before* implementing WO-2/WO-3 fresh —
otherwise the reconciliation becomes a three-way merge.

## 3. WO-3 — Viewer: navigation/selection feel (the "every editor" contract)

Spec violations, all in `src/viewer/tty.c` (same sequencing note as WO-2):

- [ ] **Arrow at edge must scroll one line, not jump a page** (spec:192).
      Current: edge arrows call `page_up()`/`page_down()` (`tty.c:1171-1181`)
      — a screenful lurch.
- [ ] **Selection is record identity, not a row number.** Current code resets
      `selected = 0` on nearly every state change (`tty.c:970,992,1005,1036,
      1066,1079`). Spec: selection survives wrap toggles (:177), and on
      exclusion resolves to the row underneath, else nearest previous (:179-185).
      PageUp puts selector at top of screen, PageDown at bottom (:189-190).
- [ ] **Key map per spec** (:187-207): Ctrl-Home/Ctrl-End = top/live-edge jumps;
      Home/End become *horizontal* (column 0 / longest visible); Left/Right
      column scroll, Ctrl-Left/Right ×10; wrap off ⇒ horizontal scrolling.
      Horizontal movement never pauses follow (:207).
- [ ] **Source toggles** Ctrl-1/2/3/4 per stream + Ctrl-Y column display
      (:252-260) — verify all exist; report only confirmed Ctrl-Y.
- [ ] Follow pause/resume rules exactly as spec:140-167 (nav away pauses,
      Ctrl-End resumes, PageDown resumes only when landing on final page).

Acceptance = the spec's own checklist (:539-564), turned into tests where the
harness can drive the TTY.

## 4. WO-4 — Viewer: visual polish ("it should look polished and it's not")

The chrome exists (header/status/footer/help match the spec's layout) but the
rendering standard isn't met. Bind to spec:11-14, 54-114, 136:

- [ ] Persistent header, **no flicker or full-screen clears** during redraw
      (:63, :136) — audit render path for whole-screen repaints; diff-render or
      line-addressed updates.
- [ ] No debug-looking output in any user-visible state; `render_legacy`
      remains test-contract-only.
- [ ] Resize redraws to new dimensions without clearing when avoidable (:136).
- [ ] A deliberate visual pass against the 80×24 target: spacing, the
      restrained filter field (:66), grouped footer cluster (:96-101).
      "Feel like a high-quality terminal tool, not a debug harness" (:11).

## 5. WO-5 — Zap/filter loop (semantics right, two decisions open)

Implemented and correct per spec: type = literal include filter; Space =
similarity-exclude selected record (Dice ≥0.45 over hashed tokens,
`src/viewer/filter.c:66-86`); Backspace relaxes filter without unexcluding
(:283); Ctrl-R restores everything (:284).

Open design decisions for Rich (spec tension, do not implement unilaterally):

- [ ] Spec bans exclusion counters/indicators in default chrome (:294). But
      zapped state is invisible — a viewer silently hiding records can mislead.
      Decide: stay pure (Ctrl-R is the escape hatch, chrome stays quiet) vs. a
      minimal live cue. Spec default wins unless Rich overrules.
- [ ] Stepwise un-zap (editor-style undo of the last exclusion) vs. Ctrl-R-only.
      Spec defines only full reset. Decide before adding keys.
- [ ] Zap correctness depends on WO-2: exclusion currently only applies within
      the scanned window. WO-2 fixes this for free; add a test.

## 6. WO-6 — Sidecar v2 + playback (killer-feature backlog, post-repair)

**ACTIVATED 2026-07-21 by Rich's /goal directive — binding spec now at
`docs/future/playback.md`** (transport keys: Space pause/resume-1x, `.` FF
multistep, `,` RW multistep [interpretation of his "(.)" pair — confirm],
Ctrl-T/UTC-local/monotonic matching the viewer; sidecar self-healing:
corrupt → CRC-anchor realignment, missing → synthetic rebuild @50 ms from
file birth time, synthetic timing labeled in chrome). Second /goal (07-22)
added: transport live during tail; time-travel attach via Ctrl-P Ctrl-W
freeze with return-to-realtime by edge-scrub or repeat sequence; ANSI TUI
detection + pty stream tagging. Ships as the "final touches" campaign
immediately after the Phase 2 rewrite seals.

v1 sidecar (16 B entries: offset:44, len−1:20, delta_µs:48, meta:16; header
carries version/entry_size — cleanly upgradeable). Vision from 2026-07-20
discussion:

- [ ] **`hold logs --replay`** — walk index entries, sleep deltas, emit bytes
      by `pread`; O(1) memory. Works on v1 today. For `-it`/console calls the
      log is raw PTY bytes ⇒ this is a session recording (asciinema-equivalent)
      Hold already captures. Ship first; needs no v2.
- [ ] **Transport controls, not just linear replay** (Rich, 2026-07-20): pause,
      fast-forward, rewind/seek, speed control — playback is a mode of the
      viewer, not a separate dumb pipe. The timestamp-prefix toggle must be the
      same keystroke as the viewer's (Ctrl-T family): one set of physics.
      Rewind/seek is index-trivial (entries are the seek table); "rewind" on a
      PTY stream needs a repaint strategy (replay-from-nearest-clear or
      re-emit from start at max speed — decide during design).
- [ ] **Time-travel attach** (Rich, 2026-07-20): when attached to a console
      call, detect it and allow rewinding the *recording* of the live console
      — pause the live view, scrub back through what the broker already teed
      to the indexed log, then jump back to real time (input suspended while
      time-traveling; the live edge is one keystroke away). The broker
      captures everything with timing today; this unifies attach, viewer, and
      replay into one surface. The flagship demo of the whole tool.
- [ ] **Logger stays in the binary.** Rich floated splitting the logger into a
      separate tool "or if the code was super tight… I wouldn't mind."
      Decision: keep one static binary (identity doc's core promise; a split
      creates version-skew and packaging surface). The viewer spec already
      blesses the internal shape: reusable library inside, single binary
      outside. Tightness is WO-1's job.
- [x] **v2 entry (24 B)** — LANDED 2026-07-22 (sidecar-v2 campaign): offset:44
      + len−1:20 + ns delta:64 + meta:16 + per-line CRC32, 16 bits spare.
      Header gains monotonic base alongside realtime, header CRC32, and
      synthetic/recovered provenance flags. Writers emit v2; a sane existing
      v1 sidecar keeps appending v1 (version/entry_size discipline).
- [x] **Integrity + self-healing** — LANDED 2026-07-22 with the above: the
      full ladder lives in `hold_logidx_map_load` (sane → use; corrupt →
      CRC-anchor realignment + index rewrite, interpolating unanchored lines;
      missing or corrupt-v1 → synthetic rebuild @50 ms from file birth time,
      statx btime/st_birthtimespec/mtime fallback). Provenance surfaces as
      `map.synthetic`/`map.recovered` — viewer/playback chrome labeling still
      owed. Pinned by tests/logidx_recovery_test.c + two harness tests.
- [ ] **Block skip-summaries** (per ~64 lines) so filter can skip blocks
      without touching text — makes search sidecar-fast on multi-GB logs.
      Note: today's "fast" filter is `strstr` over raw text; the sidecar
      accelerates navigation only.

## 6b. WO-9 — The front door: bare `hold`, help, and forgiving targeting

Rich (2026-07-20): "running hold by itself should show its most basic shape…
very concise… help dumps a ton of bs… it should literally be the command you
already understand."

- [ ] **Bare `hold` today prints a 37-line usage dump** (`hold_usage`, reached
      from `cli_main.c:499`). Replace with the basic shape — roughly ten
      lines: the three launch forms, list, attach, logs, end, purge, and one
      pointer (`hold --help` for the rest). Layer the rest: `--help` = today's
      full page, `hold help <topic>` = depth. Nothing above the fold that a
      first-time user doesn't need in their first five minutes.
- [ ] **Command-string targeting** (forgiving resolution): when a target token
      matches no id/id-prefix/name, try it as a substring of recorded command
      lines. Unique hit ⇒ act on it (`hold logs http.server` just works).
      Multiple hits ⇒ **list the matching calls** (the list *is* the
      disambiguation UI — table to stderr, exit 6 unchanged so scripts stay
      safe). Resolution precedence stays strict: exact name → id/prefix →
      cmdline substring; never guess among multiple.
- [ ] **Foreground semantics are already "held" — make it legible.**
      Foreground `hold <cmd>` is only a log tail; Ctrl-C exits the tail loop
      and the call survives (`src/runtime/signal.c:24,186-194`). This already
      fulfills "keep it active unless -d" — but silently, which violates the
      honesty ethic (user expects Ctrl-C = dead; reality = still holding a
      port). Decide, then implement:
      (a) keep detach-on-Ctrl-C — it *is* the brand promise — but print one
          stderr line on interrupt: `hold: <name> continues on hold — 'hold
          end <name>' stops it` (recommended), and honor detach keys in
          foreground on a TTY so Ctrl-P Ctrl-Q works everywhere; or
      (b) Docker-parity Ctrl-C = forward TERM to the call, detach only via
          Ctrl-P Ctrl-Q. Non-TTY stays a plain pipe either way.
- [ ] Verify `hold on` / `hold off` prominence in the concise front door —
      the session verbs are the identity; they belong above the fold.

## 7. WO-7 — Reboot resume (new capability, daemonless by design)

Rich's intent: "reboot and have things start automatically without dealing
with your system's daemon and service tools and without committing to
configuring systemd."

Today this **does not exist**: on boot-id change every record is stale and
non-signalable (`hold_eval_state`, `src/runtime/state.c:22`), the restart
supervisor dies with the machine, and nothing redials. Note this also means
`--restart always` currently promises less than its Docker counterpart
(Docker restarts on daemon start; Hold has no equivalent moment) — an honesty
gap worth closing.

Daemonless shape (keep the identity: Hold never becomes a service manager):

- [ ] A resume verb (`hold resume` or similar) that scans the store for calls
      whose recorded restart policy warrants revival after a boot-id change —
      `always` unconditionally; `unless-stopped` when the call wasn't
      explicitly ended (requires recording end-by-user vs. died, check what
      `hold_mark_run_finished` captures today) — and redials each via the
      existing recipe machinery (`restart_existing_run`, `src/runtime/call.c:82`).
- [ ] Idempotent: running calls are skipped (exists already — redial refuses
      RUNNING); safe to invoke repeatedly.
- [ ] The trigger is deliberately not Hold's problem (Rich): one cron
      `@reboot hold resume` entry, or a line in `.bashrc`/login profile —
      whatever the user already uses. Hold ships the verb and documents the
      one-liners; it never installs triggers, generates units, or grows a
      daemon. Idempotency (above) is what makes the bashrc variant safe.
- [ ] Decide interaction with stale-purge: resume must run before/instead of
      treating stale as garbage; a resumed call gets a fresh identity triple
      under the same call id (redial already reuses id+log).

## 8. WO-8 — Portability: the BSDs ("I need a bsd build, lol")

Target: "every *nix/OSX-like CLI out there." Current platform reality:
Linux first-class; macOS first-class **except** `hold on` adoption
(`adopt_foreground_group` is Linux-only, `src/runtime/shell.c:396`); BSDs
absent. The macOS sysctl path is the template — FreeBSD's
`KERN_PROC_PID`/kvm and OpenBSD/NetBSD equivalents are cousins of it:

- [ ] Platform audit: everything `/proc`-bound is concentrated in
      `src/platform/process.c`, `src/platform/boot.c`, and — the hard part —
      `src/runtime/observe.c` (group pid walking, cpu/rss, socket-inode →
      `/proc/net` port mapping), which is Linux-only with empty stubs today.
      BSD equivalents: kvm/sysctl process lists, and per-OS socket
      enumeration (FreeBSD `kern.file`/libprocstat, OpenBSD `kvm_getfiles`).
- [ ] Boot id synthesis from `kern.boottime` (macOS already does this,
      `src/platform/boot.c:18`) works on all BSDs.
- [ ] Static linking and the Makefile's per-OS flags; a FreeBSD CI lane
      (cirrus or a qemu smoke — fittingly, run under Hold).
- [ ] Close the macOS adoption gap while in here: `TIOCGPGRP` is POSIX; the
      Linux-only part is the `/proc` scan for group members — the sysctl
      process list replaces it.

## 8b. WO-10 — Terminal control layer (`src/term/`) — enabling infrastructure

Rich (2026-07-20): the distinctly missing piece is proper terminal control —
a consequence of zero-dependency (no ncurses/terminfo/libvterm). Three parts,
one new low layer beside core (pure computation, no OS calls — highly
unit-testable):

- [ ] **term/screen** (~400 lines): cell-grid model + diff renderer against a
      committed ANSI/xterm subset (no terminfo — document the subset as a
      contract). Emits minimal updates; kills flicker and full-screen clears.
      WO-4's "no flicker" bar is unreachable without this.
- [ ] **term/keys** (~300 lines): input state machine — ESC-vs-sequence
      timeout disambiguation, modifier parsing (Shift/Ctrl arrows, Ctrl-Home/
      End per viewer spec), bracketed paste awareness. Absorbs tty.c read_key.
- [ ] **term/vt** (~1.5–2k lines): subset VT emulator — cursor addressing,
      erase, scroll regions, SGR, wrap, alt-screen. **No scrollback buffer:
      the raw log + sidecar already is the scrollback**; only current-screen
      state, reconstructible by replaying forward from the nearest clear.
      Unlocks: faithful reattach repaint (replaces the 64 KiB ring replay),
      rewind/seek screen reconstruction for WO-6 replay, screens-not-soup
      console log viewing, and replaces the heuristic positioning-escape
      scan in adoption detection.

Budget honesty: +2.5–3k lines against an over-budget tree, offset by what it
absorbs (tty.c ad-hoc escape emission, attach.c mode-reset blob, escape-scan
heuristics) and justified as identity-doc "hard-won plumbing" — three killer
features stand on it. Scope discipline: the subset emulator must never chase
full xterm fidelity (that is the C-Monster's front door).

## 9. Known soft spots (recorded, not scheduled)

- `--restart` supervisor reuses one log, records no per-iteration exit codes
  (`src/runtime/start.c:288`).
- No cgroups: stats/ports/group-kill see only the pgid+session; a
  double-forking descendant escapes observation. Accepted ceiling of the
  "less than systemd" position.
- `hold on` adoption is Linux-only (`adopt_foreground_group`, `shell.c:396`)
  despite macOS being first-class elsewhere.
- Reattach replays a 64 KiB ring, not a screen snapshot (no terminal
  emulation — deliberate; keep the docs honest about it).

## 10. Verified sound — do not break while cutting

- Signal safety: `validate_signal_target` (`src/runtime/signal.c:89`) — boot-id
  → starttime → exe dev/ino → pgid/sid chain; refuses token-less records; even
  `--print` validates. README's recycled-PID claim holds.
- Store: atomic temp+rename+fsync writes, `.reserve` O_EXCL ID claims, no
  locks needed by design; torn-index recovery by `min(header, physical)`.
- Console: broker never claims the PTY (child does TIOCSCTTY); mutual
  SO_PEERCRED auth both directions; AF_UNIX short-path chdir dance with
  fatal-on-fchdir-failure; detach FSM flushes a lone Ctrl-P after 500 ms.
- Docs tell the truth: three probes (ID derivation, signal safety, store
  layout) all confirmed doc claims. Keep it that way — docs are load-bearing.
- Test discipline: `-DHOLD_TESTING` object tree isolation, anti-vacuous-pass
  harness, layer-DAG lint shared byte-identical between local and CI.

## Addendum 2026-07-20 — macOS live-fire session (Richards-Mini, M1, macOS 26.2)

Ran the full suite on real macOS for the first time: initially **39/135
passing**; final state **114 passed / 3 failed / 18 skipped** (the failures
are the two known /proc-shaped WO-8 gaps — ports, stdio inspect — plus one
detach-keys timing flake). Linux finished **135/135**. Race fix verified both
ways on-box: purged record STAYS GONE, fast-exit still RECORDED.

Harness portability fixes (tests/test_hold.sh):
- BSD `wc -l` pads output; two `[ "$count" = "1" ]` string compares → `-eq`
  (rescued 38 tests alone). Same padding fixed inside the restart workload.
- `pty_run` helper: detects util-linux (`script -qfec`) vs BSD
  (`script -q /dev/null /bin/sh -c`); all 34 call sites converted.
- `/bin/true` → PATH-resolved `true` (macOS has no /bin/true).
- macOS TMPDIR round-trips through the `/private` symlink; socket-path
  assertion accepts both spellings.
- Viewer-chrome test: feeder startup grace + double-`q` (on macOS the byte
  stream around help-toggle differs and the first q lands as help-dismiss).

Product fixes:
- **USER column showed uid on macOS** — `hold_lookup_passwd_by_uid` parsed
  /etc/passwd only; real macOS users live in OpenDirectory. Now getpwuid()
  first, file-parse fallback kept for static Linux builds (paths.c).
- **`purge -s` sudo re-exec never fired on macOS** — cli_main.c hardcoded
  `readlink("/proc/self/exe")`; now uses the portable
  `hold_resolve_self_executable_path` (which the tree already had).
- **Record resurrection race (cross-platform, mac-reproducible):**
  `purge --force` of a live call unlinks the record, then the reaper's
  `hold_mark_run_finished` read-modify-write recreates it (proven: gone at
  t+0.2s, back at t+0.4s with ExitCode 143). Commit 8e3ded2 ("harden the
  live force-purge test against scheduler jitter") had treated this as test
  flakiness. Fixed: mark_run_finished re-checks existence before the atomic
  rewrite and returns ENOENT; the reaper treats ENOENT as terminal (purged
  is final). Residual window is stat-to-rename, microseconds.

Remaining genuine macOS gaps (now with failing tests as proof — feeds WO-8):
- `hold ports` empty and `inspect` stdio fd targets absent: observe.c is
  Linux-/proc-only. sysctl/libproc equivalents needed.
- `hold on` adoption: still the `#if !defined(__linux__)` bail (untested by
  Rich to date; WO-8 makes it first item).

Logging finding (feeds WO-6 replay): the HLOGIDX format reserves four stream
tags (stdout/stderr/stdin/pty) and the viewer renders all four, but writers
only ever emit stdout/stderr — the console broker tags PTY bytes "stdout"
(broker.c:408, mapping logging.c:348). Tagging the broker's tee as "pty"
makes every log self-describing (viewer can announce "terminal recording";
--replay can pick playback mode automatically). Decision (revised per Rich,
2026-07-20): stdin capture is safe to enable **gated on the terminal's own
echo state** — the output log already contains everything echo ever showed,
so logging IN records only while ECHO is on adds attribution (human-typed vs
program-printed) with zero new exposure; echo-off input (password prompts,
raw-mode TUIs) is dropped exactly as echo already drops it. Governing invariant (Rich, 2026-07-20): **the raw log's display stream is
byte-faithful to what the screen received** — input is annotation, never
display. Logging consumed-but-undisplayed input into the display stream is
a *corruption* failure (broken recording), distinct from the echo-off
*safety* failure (leaked secret); both fall out of the one invariant.
Therefore: echo on → capture keystrokes as IN-tagged sidecar records
(attribution only); echo off → don't capture (consumed, not displayed);
plain dump and --replay emit display-tagged records only — replaying an IN
record is by definition corruption. Implementation care: check the echo bit
synchronously per input frame (one tcgetattr per keypress), not via the
broker's dwell-smoothed termios sampling — a prompt flips echo off
microseconds before the user types, and a lagging sample would leak the
first keystroke. The broker's existing termios machinery covers the rest.

Cross-platform note for WO-3/WO-4: the viewer's help-dismiss consumed a
keystroke differently on macOS vs Linux under identical input bytes — when
reworking the key loop, make dismiss semantics explicit and byte-exact, and
mind BSD line-discipline specials (Ctrl-T = VSTATUS/SIGINFO, Ctrl-Y = VDSUSP)
anywhere keys are read pre-raw-mode.

## Suggested order

Status 2026-07-21: WO-1's confirmed-dead + schema stages are DONE
(ed2e316); remaining WO-1 = consolidation/boilerplate tiers, interleaved
as below. **Step zero is now the tempo-refinement reconciliation** — it
front-runs WO-2/3 (viewer navigation), touches the same files as the
remaining cut, and its behavior fixes (restart exit recording, foreground
exit status, crash-safe temps) are cheap wins whichever lineage leads.

Reconciliation → WO-2 (correctness; no term dependency) → WO-10 screen+keys (the substrate) →
WO-3 (feel, on term/keys) → WO-4 (polish, on term/screen) → WO-1 (cut,
interleaved as review passes — offsets WO-10's added mass) → WO-5 decisions →
WO-7 reboot resume → WO-6 linear playback (needs no emulator) → WO-8 BSD
lane → WO-10 term/vt → WO-6 rewind/seek + faithful reattach (on term/vt) →
WO-6 v2 format. Rationale: fix what lies to users first; lay the substrate
before polishing what stands on it; shrink while building; ship the cheap
promise-expanders (resume, linear replay); then the emulator and the
features only it makes possible.
