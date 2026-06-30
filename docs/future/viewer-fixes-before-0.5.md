# Dynamic log viewer design before 0.5

[Future work](README.md) | [Docs index](../index.md) | [Logging destinations](log-destinations.md)

Status: near-future work before 0.5.0. This is the target design, not a claim about current behavior. The 0.4.0 cut should move forward without trying to finish this viewer redesign.

This document supersedes the older dynamic-viewer notes and records the final product direction from the 2026-06-29 design discussion: a polished, fast, quiet full-screen log viewer with center-out filtering, private Hold-managed logs, and a durable sidecar index for captured logs.

## Design goals

The viewer should feel like a high-quality terminal tool, not a debug harness.

- Polished, stable chrome: intentional header/footer placement, no lazy left-justified status dump.
- Everything needed, nothing more: no static match counters, no character counters, no noisy search indicators in normal operation.
- Fast interaction: filtering should update as fast as the user types under normal conditions.
- Viewport-first behavior: the current screen and selected row are the priority, not the beginning or end of the file.
- Center-out search: discover matches nearest the current selected record first.
- Log-order rendering: regardless of discovery order, visible records are always rendered oldest-to-newest.
- Private logs remain accessible through Hold commands; do not expand the public CLI more than needed for 0.4/0.5.
- Internally reusable viewer/library shape is welcome, but the product remains a single standalone binary.

## Release scope

Do not block 0.4.0 on this work.

0.4.0 should ship the coherent Docker-shaped process-management surface after its release tests are triaged. The viewer redesign belongs in the near-future queue before 0.5.0.

## Command surface

Plain log commands remain script-friendly by default:

```sh
hold logs <target>
hold logs -f <target>
hold logs -n 100 <target>
```

The full-screen dynamic viewer is explicit:

```sh
hold logs --dynamic <target>
hold logs --dyn <target>
```

For active runs, dynamic logs open at the newest output and follow live output by default.

A general viewer can be considered later, but it should not complicate the 0.4.0 CLI. If added, it should reuse the same internal viewer engine:

```sh
hold view <text-file>   # possible future; memory-only index by default
hold view -             # possible future; capture stdin into Hold-owned log storage
```

## Minimum terminal and layout target

Design target: 80x24 minimum.

Smaller terminals may degrade compactly, but the polished layout is planned around 80 columns and 24 rows. At that size there is enough room for a real header/footer layout; do not collapse into debug-looking left-justified text.

### Header

The top line is persistent and must not flicker or clear during redraws.

- Left/top cluster: `hold logs:` plus the run name/profile/name when available.
- Upper-right: current view mode, right-justified.
- Filter line: when typing, the filter appears in the header, visually distinct but not noisy. A restrained inverted/green field is acceptable.

Example:

```text
 hold logs: amazing_shamir                         FOLLOWING ACTIVE
 filter: sparse-needle
```

When no filter is active, the second header line may be blank or used for compact context. Do not put the raw log path there.

### Footer

The footer is operational chrome only.

- Lower-left: short run ID / target ID.
- Lower-center: compact mode cluster.
- Lower-right: `Ctrl-H Help`, right-justified.
- Do not show the full path by default.
- Do not show static match counters.
- Do not show character counts.
- Do not show permanent `scanning...` noise.

Example:

```text
 098e755e9827                                      Ctrl-H Help
                  ts:time local   src:all   wrap:on
```

The center cluster should be visually grouped so the operator can look in one place:

```text
ts:off local   src:all   wrap:on
ts:date UTC    src:out,err   wrap:off
```

### Help

`Ctrl-H` pauses follow/scrolling and temporarily replaces the footer/bottom area with one or two concise help lines. The next key dismisses help and restores the normal footer.

Example help lines:

```text
 PgUp/PgDn scroll  Ctrl-End follow  Ctrl-T time  Ctrl-U UTC/local  Ctrl-R reset
 Ctrl-1 out  Ctrl-2 err  Ctrl-3 in  Ctrl-Y source  Space exclude  q quit
```

Help must not permanently replace the normal chrome and should not require a full-screen clear.

## View mode/status

The current viewer state is pinned to the upper-right corner of the top line.

Status values:

```text
FOLLOWING ACTIVE
VIEWING ACTIVE
VIEWING EXITED (code)
```

Meanings:

```text
FOLLOWING ACTIVE       The process is active and the viewer is pinned to the live edge.
VIEWING ACTIVE         The process is active, but the user is viewing older output or has paused follow by navigation.
VIEWING EXITED (code)  The process has exited and the viewer is showing retained logs.
```

The viewer observes terminal size changes in real time and redraws to the current dimensions without clearing the whole screen when avoidable.

## Follow behavior

For active runs, dynamic logs follow by default.

Following means the viewport is pinned to the bottom/live edge and new visible rows auto-scroll into view.

Follow is paused by vertical navigation away from the live edge:

- Arrow Up once
- PageUp
- mouse wheel up
- Ctrl-Home
- any command that moves the viewport away from the live edge

When follow is paused, state changes from `FOLLOWING ACTIVE` to `VIEWING ACTIVE`.

While in `VIEWING ACTIVE`:

- new log lines continue to be ingested and indexed;
- filters/exclusions still apply;
- the viewport must not jump to the bottom;
- the user can review old output, filter, and exclude noise without being pulled back to live output.

Follow is controlled only by vertical live-edge position. Horizontal scroll does not pause follow by itself.

## Resuming follow

`Ctrl-End` jumps to the live edge. If the process is active, it resumes follow immediately and changes status to `FOLLOWING ACTIVE`.

`PageDown` may resume follow only when the next page down lands at the end of the available visible output. If fewer than one page of visible rows remain below the current viewport, `PageDown` moves to the final page and resumes follow for an active process.

## Selection model

The viewer has a visible selector/highlight.

The selected item is a log record, displayed at the terminal row where the first physical row of that record appears.

When wrapping is enabled, one selected log record may occupy multiple terminal rows. The highlight should make the selected record clear without confusing physical wrapped rows with separate records.

When wrapping is toggled, the same log record remains selected. The terminal row may change due to layout, but selection identity does not.

When `Space` excludes lines similar to the selected record and that selected record disappears:

1. keep the same visual terminal-row intent where possible;
2. select the next visible record underneath that row;
3. if no visible record exists underneath, select the nearest previous visible record.

Do not yank the viewport to the tail or to the top after exclusion.

## Vertical navigation

- `PageUp` moves one page upward and places the selector bar at the top of the screen.
- `PageDown` moves one page downward and places the selector bar at the bottom of the screen.
- Arrow Up/Down move the selector within the visible viewport.
- When the selector reaches the top/bottom edge, additional arrow movement scrolls the viewport in that direction.
- `Ctrl-Home` jumps to the beginning/top of visible logs.
- `Ctrl-End` jumps to the end/live edge.

## Horizontal navigation and wrapping

- `Ctrl-W` toggles line wrapping.
- When wrapping is enabled, long records wrap within terminal width.
- When wrapping is disabled, records remain one physical row and horizontal scrolling is enabled.
- `Home` moves horizontally to column zero of the selected record.
- `End` jumps horizontally to the length of the longest visible record on screen.
- Left/Right move horizontally by one column.
- Shift-Left/Shift-Right move horizontally by one column when the terminal reports those keys distinctly.
- Ctrl-Left/Ctrl-Right move horizontally by ten columns.

Horizontal movement does not pause follow by itself.

## Timestamp display

Timestamp display has two independent state variables:

```text
timestamp mode: none | time | date
timezone mode:  local | UTC
```

Keybindings:

```text
Ctrl-T    cycle timestamp mode: none -> time -> date -> none
Ctrl-U    toggle timestamp timezone presentation: local <-> UTC
```

No timezone offset toggle. Operators either want local time or UTC; showing `-0400`/offset noise is not needed in the normal viewer.

Examples:

```text
none:       message
time local: 14:32:08 message
time UTC:   18:32:08Z message
date local: 2026-06-29 14:32:08 message
date UTC:   2026-06-29 18:32:08Z message
```

Timestamp presentation does not change stored log records, indexing, filtering, similarity exclusion, ordering, or plain `hold logs` output.

Do not use `Ctrl-Z` for timezone or timestamp behavior; it conventionally suspends foreground terminal jobs.

## Source controls

Captured records should know their source/stream through the durable log index metadata:

```text
stdout
stderr
stdin
pty/other
```

Source visibility keybindings:

```text
Ctrl-1    toggle stdout records
Ctrl-2    toggle stderr records
Ctrl-3    toggle stdin records
Ctrl-4    toggle pty/other records
Ctrl-Y    toggle source column display
```

`Ctrl-S` is mnemonic for source, but it conflicts with terminal software flow-control history. Use `Ctrl-Y` as the low-risk default unless raw-mode/IXON behavior is explicitly tested and a deliberate decision is made to capture `Ctrl-S`.

When source column is enabled, use compact labels:

```text
OUT │ message
ERR │ warning
IN  │ input
PTY │ interactive output
```

Source filters should update from metadata, not by searching record text.

## Filtering and exclusion semantics

Typing printable characters opens or updates the live filter field in the header.

Filtering works against the current non-excluded universe of records. Noise exclusion is destructive until reset:

- `Space` removes records similar to the selected record from the active viewer universe.
- Later typed filters operate only over records that have not been excluded.
- Backspace relaxes the typed filter, but excluded records stay excluded.
- `Ctrl-R` clears typed filter and all similarity/noise exclusions, restoring all records.

Conceptually the active viewer owns one visibility bitmap:

```text
visible_bitmap[record_no] = present or not present
```

Supporting state may include typed filter text, exclusion examples, source mask, chunk/index state, and cached decoded records. The user-facing rule remains simple: records are either present in the current viewer result or not; reset restores them.

No character counter. No static count of selected/excluded examples. If a count is not live, useful, and necessary, it does not belong in the default chrome.

## Center-out search/filtering

Filtering and exclusion must be viewport-first.

Use the currently selected/anchored record as `R`. Discovery priority is center-out:

```text
R
R+1
R-1
R+2
R-2
R+3
R-3
...
```

Rendering remains log order, always.

The goal is that filtering feels immediate. In normal operation there should be no `SEARCHING` or match-counter noise. If the design is fast enough, the user should just see results update as they type.

If the file is enormous or storage is slow, the engine may continue scanning/indexing in cooperative slices, but it must not pretend a match does not exist just because an early budget expired. It should remember continuation positions and continue until the viewport is satisfied, the user changes the query/anchor, or both file boundaries are exhausted.

## Indexing model

### Hold-captured logs

Persistent sidecar indexes are most valuable for live captured logs because Hold owns the capture path and can record metadata as bytes are written.

Target storage for captured logs:

```text
<run>.log      raw/plain captured bytes
<run>.log.idx  durable fixed-width record map
```

The sidecar is more than a disposable cache: it contains timestamps and stream/source metadata that cannot be reconstructed from a plain text log after the fact.

### Arbitrary text files

For a future generic text-file viewer, do not write surprise `.idx` files next to arbitrary user logs by default.

For:

```sh
hold view /path/to/logfile
```

use a memory-resident dynamic index:

1. open/stat the file;
2. build enough index around the initial viewport to render immediately;
3. continue indexing in memory cooperatively;
4. reprioritize around the user's current anchor whenever the user navigates or filters;
5. do not persist an index beside the file unless an explicit cache option is added later.

For pipe input:

```sh
command | hold view -
```

Hold owns the capture. It may timestamp and store the stream in Hold-managed private storage using the same raw-log plus sidecar-index shape.

## Sidecar index v1 target

The proposed durable Hold-captured log sidecar format:

```c
/*
 * Hold local log sidecar index v1
 *
 * Raw log file:
 *   run.log
 *
 * Sidecar index:
 *   run.log.idx
 *
 * One index entry corresponds to one retained log record.
 *
 * Entry size:
 *   16 bytes
 *
 * Entry layout:
 *   pos_len   = 44-bit raw log byte offset + 20-bit record length
 *   time_meta = 48-bit timestamp delta in microseconds + 16-bit metadata
 *
 * Timestamp:
 *   actual_time_us = header.base_unix_us + entry.time_delta_us
 *
 * Capacity:
 *   44-bit offset       = 16 TiB raw log file
 *   20-bit len_minus_1  = 1 MiB max record length
 *   48-bit delta_us     = about 8.92 years from base timestamp
 *   16-bit meta         = stream/newline/continuation/truncation/reserved flags
 */

#include <stdint.h>

#define HOLD_LOGIDX_MAGIC "HLOGIDX"
#define HOLD_LOGIDX_VERSION 1
#define HOLD_LOGIDX_ENTRY_SIZE 16

#define HOLD_LOGIDX_OFFSET_BITS 44
#define HOLD_LOGIDX_LEN_BITS    20
#define HOLD_LOGIDX_TIME_BITS   48
#define HOLD_LOGIDX_META_BITS   16

#define HOLD_LOGIDX_OFFSET_MASK ((1ULL << HOLD_LOGIDX_OFFSET_BITS) - 1ULL)
#define HOLD_LOGIDX_LEN_MASK    ((1ULL << HOLD_LOGIDX_LEN_BITS) - 1ULL)
#define HOLD_LOGIDX_TIME_MASK   ((1ULL << HOLD_LOGIDX_TIME_BITS) - 1ULL)
#define HOLD_LOGIDX_META_MASK   ((1ULL << HOLD_LOGIDX_META_BITS) - 1ULL)

/* All multi-byte fields are little-endian on disk. */
#define HOLD_LOGIDX_F_LITTLE_ENDIAN 0x00000001u
#define HOLD_LOGIDX_F_DIRTY         0x00000002u

/* Low bits of meta are source; higher bits are record-state flags. */
#define HOLD_LOGIDX_META_STREAM_MASK   0x0007u
#define HOLD_LOGIDX_META_STREAM_STDOUT 0x0000u
#define HOLD_LOGIDX_META_STREAM_STDERR 0x0001u
#define HOLD_LOGIDX_META_STREAM_STDIN  0x0002u
#define HOLD_LOGIDX_META_STREAM_PTY    0x0003u

#define HOLD_LOGIDX_META_NO_NEWLINE    0x0008u
#define HOLD_LOGIDX_META_CONTINUATION  0x0010u
#define HOLD_LOGIDX_META_TRUNCATED     0x0020u
#define HOLD_LOGIDX_META_RESERVED_MASK 0xffc0u

struct hold_logidx_header_v1 {
    char     magic[8];        /* "HLOGIDX" plus NUL */
    uint16_t version;         /* 1 */
    uint16_t header_size;     /* sizeof(struct hold_logidx_header_v1) */
    uint16_t entry_size;      /* 16 */
    uint16_t reserved16;

    uint32_t flags;           /* endian/options/dirty */
    uint32_t reserved32;

    uint64_t base_unix_us;    /* timestamp base in Unix epoch microseconds */
    uint64_t entry_count;     /* number of valid entries */

    uint64_t raw_size_bytes;  /* raw log size this index was built against */
    uint64_t raw_mtime_ns;    /* sanity check against raw log mtime */

    uint64_t reserved[4];
};

struct hold_logidx_entry_v1 {
    uint64_t pos_len;    /* low 44: offset, high 20: len_minus_1 */
    uint64_t time_meta;  /* low 48: delta_us, high 16: meta */
};
```

Packing helpers:

```c
static inline uint64_t
hold_logidx_pack_pos_len(uint64_t offset, uint32_t len)
{
    return (offset & HOLD_LOGIDX_OFFSET_MASK) |
           (((uint64_t)(len - 1U) & HOLD_LOGIDX_LEN_MASK)
            << HOLD_LOGIDX_OFFSET_BITS);
}

static inline uint64_t
hold_logidx_pack_time_meta(uint64_t delta_us, uint32_t meta)
{
    return (delta_us & HOLD_LOGIDX_TIME_MASK) |
           (((uint64_t)meta & HOLD_LOGIDX_META_MASK)
            << HOLD_LOGIDX_TIME_BITS);
}

static inline uint64_t hold_logidx_offset(uint64_t pos_len)
{
    return pos_len & HOLD_LOGIDX_OFFSET_MASK;
}

static inline uint32_t hold_logidx_len(uint64_t pos_len)
{
    return (uint32_t)(((pos_len >> HOLD_LOGIDX_OFFSET_BITS)
                      & HOLD_LOGIDX_LEN_MASK) + 1U);
}

static inline uint64_t hold_logidx_time_delta_us(uint64_t time_meta)
{
    return time_meta & HOLD_LOGIDX_TIME_MASK;
}

static inline uint32_t hold_logidx_meta(uint64_t time_meta)
{
    return (uint32_t)((time_meta >> HOLD_LOGIDX_TIME_BITS)
                     & HOLD_LOGIDX_META_MASK);
}

static inline uint64_t
hold_logidx_timestamp_us(const struct hold_logidx_header_v1 *h,
                         const struct hold_logidx_entry_v1 *e)
{
    return h->base_unix_us + hold_logidx_time_delta_us(e->time_meta);
}
```

Plain-English contract:

```text
Raw log:
  stores captured bytes.

Index header:
  stores format/version, base timestamp, entry count, and raw-file sanity info.

Each index entry:
  offset       where the record starts in the raw log
  length       how many bytes belong to the record
  delta_us     capture timestamp relative to header.base_unix_us
  meta         source and record-state bits

Viewer random access:
  entry_offset = header.header_size + record_no * 16
  pread(raw_log_fd, buf, len, offset)
```

No JSON is required for the hot path. JSON can remain an export/compatibility format if needed, but the target dynamic viewer design is raw/plain log plus sidecar record map.

### Sidecar write/recovery rules

The writer should append in this order:

1. append raw log bytes;
2. append index entry;
3. periodically update header `entry_count`, `raw_size_bytes`, and dirty/clean state.

On open after crash:

- validate magic/version/header/entry size;
- compute physical entries from sidecar file size;
- use the smaller safe count when header and physical size disagree;
- verify each used entry does not point past the current raw log size;
- recover or rebuild only what is safe.

Zero-length records should be skipped. Records larger than 1 MiB should be split with `CONTINUATION` and/or marked `TRUNCATED`, depending on the final capture policy.

## Acceptance checklist

- `hold logs`, `hold logs -f`, and `hold logs -n` stay plain/script-friendly by default.
- `hold logs --dynamic` and `hold logs --dyn` are the explicit full-screen entrypoints.
- 80x24 polished layout exists with stable header/footer.
- View mode is right-justified in the upper-right corner.
- `Ctrl-H Help` is right-justified in the lower-right corner.
- The raw log path is not shown in default chrome.
- Header shows typed filter text while filtering.
- Footer center cluster shows only operational modes: timestamp mode, local/UTC, source mask, wrapping.
- No default match counter, character counter, exclusion counter, or permanent search indicator.
- Active dynamic views start at live edge with `FOLLOWING ACTIVE`.
- Vertical navigation away from live edge pauses follow without stopping ingestion/indexing.
- `Ctrl-End` resumes follow for active runs.
- Horizontal scrolling/wrapping never pause follow by themselves.
- `Ctrl-T` cycles timestamp mode; `Ctrl-U` toggles local/UTC presentation.
- `Ctrl-1`/`Ctrl-2`/`Ctrl-3`/`Ctrl-4` toggle source visibility.
- `Ctrl-Y` toggles source column display.
- `Ctrl-Z` remains reserved for terminal job-control behavior.
- Filtering uses center-out discovery around the selected record while rendering in log order.
- Space exclusion removes similar records from the active universe until `Ctrl-R` reset.
- Backspace relaxes typed filter without unexcluding noise.
- Selection remains tied to the same log record across wrap toggles.
- If exclusion removes the selected record, selection resolves to the next row underneath or nearest previous row.
- Hold-captured logs have a durable sidecar index/record map before the dynamic viewer relies on fast source/timestamp/record-number operations.
- Arbitrary text file viewing, if added, uses memory-only indexing by default and does not create surprise sidecars next to user files.

## Implementation next steps

1. Finish and release/verify 0.4.0 first; do not expand the current large commit with viewer redesign code.
2. Keep this document as the source of truth for the before-0.5 viewer work.
3. Split viewer work into a separate branch/commit series:
   - log sidecar format and writer integration;
   - internal viewer source/index abstraction;
   - polished chrome/header/footer pass;
   - center-out filter/bitmap engine;
   - source/timestamp controls;
   - follow/navigation/exclusion acceptance tests.
4. Preserve current plain `hold logs` behavior and add `--dynamic` only when the redesigned viewer is ready.
