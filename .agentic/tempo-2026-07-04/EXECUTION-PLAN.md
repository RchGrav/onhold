# TEMPO run 2026-07-04 — execution plan (halted mid-implementation)

A TEMPO improvement run (10 examiners, 5 play generators, 6 adversarial skeptics; all
findings tool-verified against v0.6.1) was halted after landing 1 of 23 planned commits.
This directory preserves everything needed to execute the rest without re-discovery.

## State when halted

- **DONE (on this branch, gated green: zero-warning build, 138/138 tests, lint):**
  `ea4a2e8` — console call records anchor to the held process group; broker forwards
  TERM. Closes the #1 critical: `hold end` on a console call used to TERM the broker,
  letting a HUP-ignoring target escape while hold reported `Exited (0)`.
- **IN FLIGHT, reverted to diff:** `partial-c1-commit2-post-eof-hang.diff` — unfinished,
  unverified start on c1 commit 2 (post-EOF attach hang). Review before reuse.
- **NOT STARTED:** everything else below.

## Files here

- `r1.json` — full RiskLedger (109 findings, file:line evidence, claim types) + 15 PlayCards.
- `r2.json` / `r2-c*.json` — six skeptic-revised change sets: per-play verdicts with
  binding amendments, every pinned test that constrains the change (line numbers),
  ranked risks, commit-by-commit plans, test plans. **These are the implementation
  specs — an implementer should work from `r2-<cluster>.json` directly.**

## Recommended execution order (each commit gated: zero-warning make, make check, lint)

1. **c1-console** (2 commits remain): post-EOF attach hang; EAGAIN client backlog (64KB, HOLD_TEST_CONSOLE_SNDBUF hook).
2. **c2-endings** (3): -e/--env stops mutating hold's own env (setenv at cli_main.c:266 — `hold -e HOME=X` relocates the store!); restart supervisor writes the final frame (false `Exited (0)` today); foreground exit-code propagation + contract redline.
3. **c3-logs** (5): pin sidecar byte layout with tests; delete legacy JSON-log decode shim; **`logs -p` full dump + `--tail` = LAST N (critical #2: today -p silently prints only the FIRST 50 lines)**; jbd2-style sidecar-vs-raw-size validation; rewrite docs/store.md as normative HLOGIDX v1 spec.
4. **c4-store-signals** (3): signal validation fails closed (identity token compared, not just present); unique temp files (crash-leftover .tmp bricks record rewrites today) + sweep honors .reserve (purge can race a starting call); purge derives unlink paths from layout, never from record-stored strings.
5. **c5-surface** (4): docs redline first (revertable), then **delete --cap-add/--cap-drop/--privileged** (identity cut table; justified by skeptic, flagged for owner), honest flag rejections, usage errors to stderr, help stops claiming "ps is an alias", --name grammar rejects flag-look-alikes.
6. **c6-hygiene** (5): delete ~320 lines of nm-verified dead exports; consolidate 3 /proc stat parsers; pin --env-file + hold on/off + PORTS IPv6 shapes; CI runs test-040 + version-sync (GitHub currently doesn't); archive pre-0.6 docs, delete dead profile-era example.

## Top findings not covered above (from r1.json, still open)

- `src/cli_main.c:397` [high] Launch surface accepts undocumented flags the identity cut table marked delete: --cap-add/--cap-drop perform real Linux capset changes; --restart-delay is also doc-absent

(All 109 findings with evidence are in r1.json: 2 critical, 23 high, 56 medium, 28 low.)

## Named deferrals (decided, do not re-litigate)

attach-drop client visibility (needs protocol v2 framing); --rm+--restart mutual
exclusion; state.c display-path fail-open; per-id record-write flock; redial's trust of
recorded log_path; foreground -it exit-code parity; docker 125/126/127 launch codes.

## Notes

- hold-on-identity.md still says "draft for redline"; every cut-table row except the
  capability machinery is executed. Confirm its status; the <10k-line target was treated
  as direction, not gate.
- The c5 capability deletion removes working, test-pinned functionality (3 tests deleted
  with the code) — it lands as its own revertable commit behind a docs-only redline commit.
