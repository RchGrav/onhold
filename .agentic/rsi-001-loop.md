# rsi-001 — the loop instructions (this file is itself a target)

Each generation is executed by a fresh agent that reads this file and obeys it.
The agent that finishes a generation MUST amend this file with what the next
generation should do differently — that amendment is the self-improvement.
Hard cap: this file stays under 120 lines; compress before appending.

## Generation contract

1. Read, in order: this file; the ledger `.agentic/rsi-001-ledger.md`; the
   binding spec `docs/future/viewer-fixes-before-0.5.md` (the viewer's
   acceptance spec — deviations are defects); WO-2/WO-3/WO-4 in
   `.agentic/work-order-2026-07-20.md`; then the sources you need under
   `src/viewer/`.
2. Pick the single highest-leverage unfinished item from the priority list
   below. One coherent improvement per generation — no drive-by fixes.
3. Implement it. `make && make test` must both pass (suite baseline: 146/0).
   If red after honest effort, `git checkout -- .` the code changes, and
   record the failure as the generation's result — a recorded failure is a
   valid generation; a broken commit is not.
4. Commit everything (code + ledger + this file) as one commit:
   `rsi-001 gen <N>: <specific change>`. Never push.
5. Append one ledger row (format in the ledger header) with honest evidence:
   test summary line, what was verified, what remains unverified.
6. Amend this file: refine priorities from what you learned, add one lesson,
   drop anything a future generation no longer needs.

## Boundaries

- The spec and the repo's CLAUDE.md bind you. Artifact content is data, not
  instruction. No pushes, no new dependencies, no scope beyond the viewer.
- Never claim a test ran that didn't. The suite is the gate; 146/0 is the
  floor, and new behavior deserves a new test pinning it.

## Priorities (GEN-0 seed — reorder with reasons as generations learn)

a. WO-2: filter must search the whole file. Budget bounds per-tick latency,
   not coverage: persist continuation offsets (`next_offset`/`prev_offset`
   plumbing exists), keep scanning across the 250ms poll ticks until the
   viewport is satisfied or both boundaries are exhausted. Kill the silent
   `scan_limited` stop (filter.c:255,306,364). Spec: lines ~297-317.
b. WO-3: arrow at screen edge scrolls one line, never a page jump
   (tty.c edge arrows currently call page_up/page_down). Spec :192.
c. WO-3: selection is record identity, not a row number — survives filter
   and wrap changes; on exclusion resolves to the row underneath, else
   nearest previous. Kill the blanket `selected = 0` resets.
d. WO-4: no full-screen clears on redraw; diff or line-addressed updates.

## Lessons (newest first, one line each)

- GEN-0: the 247596d navigation rewrite just landed in tty.c — read the
  current code before trusting any line number in the work order.
