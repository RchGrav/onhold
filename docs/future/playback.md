# Log playback and sidecar recovery (WO-6) — binding spec

Status: Rich's spec, 2026-07-21/22 (verbatim requirements from the /goal
directives; interpretation notes marked). Binding per design principle 4.
Builds on the reconstructed viewer; ships after the Phase 2 rewrite seals.

## Where transport exists

Playback is a mode, not a separate tool, and it is reachable from three
places with one set of physics:

1. **`--replay`** — play a finished (or live) log from the start.
2. **During tail/follow** — the transport keys are live while tailing: `.`
   `,` Space scrub the recorded history mid-tail; returning to the live
   edge resumes the tail.
3. **Attached to a live console (time-travel)** — `Ctrl-P Ctrl-W` freezes
   the console: the live view pauses, input to the held process is
   suspended, and the same transport keys scrub back through what the
   broker already teed to the indexed log. **Return to real time by either
   scrubbing forward to the realtime edge or pressing the freeze sequence
   again** — the live edge is never more than one gesture away. (Ctrl-P is
   already the detach prefix; the FSM grows one more suffix: Q detaches,
   W freezes. The lone-Ctrl-P 500 ms flush rule is unchanged.)

## ANSI TUI detection and recording

When the captured stream is an ANSI TUI (cursor addressing/alt-screen
escapes detected in the byte stream), record and mark it as such (the pty
stream tag the format already reserves): playback then knows it is
replaying a screen session rather than a line log, and the viewer can
announce "terminal recording". Detection is best effort — plain logs never
misclassify as TUI. Scrub-repaint for TUI recordings uses
replay-from-nearest-clear (decision 5); where no clear exists within
bounded distance, re-emit from start at max speed.

## Playback mode

`hold logs <target> --replay` (and a playback entry from the viewer) walks
the sidecar index, sleeps the recorded deltas, and emits bytes by pread —
O(1) memory, works on v1 sidecars today.

Transport keys (one set of physics — every key that exists in the log
viewer means the same thing here):

| key | action |
| --- | --- |
| Space | stop / resume at 1x |
| `.` | fast-forward, multistep (repeat presses step the rate up) |
| `,` | rewind, multistep (interpretation: Rich wrote "(.)" for both FF and RW; `,`/`.` is the matching keyboard pair — confirm on review) |
| Ctrl-T | timestamp prefix cycle, exactly the viewer's (off → time → date) |
| UTC vs Local | same keystroke as the viewer's display toggle |
| monotonic display | same keystroke family as the viewer |
| Esc / q | leave playback, exactly as the viewer quits |

Rate model: multistep — each `.` press steps the playback rate up through
the ladder **1x → 2x → 3x → 4x → 8x → 16x**, each `,` steps the rewind
ladder likewise; Space always returns to paused/1x resume. Rewind on a PTY
stream repaints via replay-from-nearest-clear (decision 5, registry).

### Mode-change OSD (Rich, 2026-07-22 — verbatim requirement)

Every playback mode change (rate step, play, stop) flashes an on-screen
indicator in the **upper right corner**: blank out **7 characters of the
top row and the second-to-top row**, and center the indicator text in that
blanked area. It shows for **2 seconds**, then the display returns to
normal (the blanked characters restore).

| state | indicator |
| --- | --- |
| play at 1x | `PLAY` |
| paused | `PAUSED` |
| FF 2x / 3x / 4x / 8x / 16x | `▶▶` `▶▶▶` `▶▶▶▶` `▶▶▶▶▶` `▶▶▶▶▶▶` |
| RW 2x / 3x / 4x / 8x / 16x | `◀◀` `◀◀◀` `◀◀◀◀` `◀◀◀◀◀` `◀◀◀◀◀◀` |

(Chevron count = ladder position + 1, so the glyph itself reads as speed.
`PAUSED` is 6 characters and the widest chevron run is 6 — both center in
the 7-character blank with a space to spare.)

## Sidecar recovery (self-healing index)

Three states, all handled without user ceremony:

1. **Sidecar present and sane** — use it.
2. **Sidecar corrupt** (bad header hash, torn entries, offsets that do not
   land on the text): attempt recovery using the per-line tiny hashes
   stored in the sidecar index — rescan the log text, hash each line,
   anchor-match the recovered hash sequence against the index (rsync-style
   realignment), re-derive offsets, rewrite the index. Recovery is best
   effort: data that cannot be re-anchored is dropped from the index, and
   missing data MAY be inferred from what was recovered (e.g. interpolated
   timestamps between recovered anchors).
3. **Sidecar missing** — rebuild from scratch: one entry per line, synthetic
   monotonic timestamps spaced at 50 ms, start time inferred from the log
   file's birth timestamp (statx btime on Linux, st_birthtimespec on
   macOS/BSD; fall back to mtime minus 50 ms × line count when birth time
   is unavailable). The rebuilt index is marked synthetic so playback can
   say "timing reconstructed" instead of implying recorded truth.

Per-line tiny hashes are the v2 entry format, already designed in the work
order (§6): **24 B per entry = offset:44 + len−1:20 + ns delta:64 + meta:16
+ per-line CRC32:32, 16 bits spare** (48-bit ns rolls over at ~78 h, hence
the full 64-bit delta). Header gains a monotonic-clock base alongside
realtime, a header hash for edit detection, and keeps version + entry_size
so v1 and v2 readers coexist. v1 sidecars stay readable (no CRCs → corrupt
recovery degrades to rebuild); recovery/rebuild always writes v2. The CRC32
is the "basic tiny hash": cheap to compute line-by-line on rescan, strong
enough to anchor-match sequences for realignment.

## Honesty rules

- Synthetic or recovered timing is labeled in the chrome — never presented
  as recorded timing.
- Replay emits display-stream records only (the capture invariant: input is
  annotation, never display; replaying an IN record is corruption).
- The plain `--replay` pipe (non-TTY) plays linearly at 1x with no
  transport, script-safe.
