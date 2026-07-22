#include "hold/config.h"
#include "hold/core.h"
#include "hold/log_viewer.h"

#define VIEWER_FILTER_MAX 255
#define VIEWER_OFFSET_HISTORY_MAX 1024
#define VIEWER_SCAN_BYTES_PER_ROW (64u * 1024u)
#define VIEWER_SCAN_BUDGET_FLOOR (1024u * 1024u)

enum viewer_scan_mode { VIEWER_SCAN_FORWARD = 0, VIEWER_SCAN_BACKWARD };

/* Playback rate ladder (spec): 1x -> 2x -> 3x -> 4x -> 8x -> 16x. */
static const unsigned viewer_play_rates[] = {1, 2, 3, 4, 8, 16};
#define VIEWER_PLAY_RUNGS (sizeof(viewer_play_rates) / sizeof(viewer_play_rates[0]))
#define VIEWER_OSD_MS 2000
#define VIEWER_OSD_CELLS 7

enum viewer_key {
    VIEWER_KEY_NONE = 0, VIEWER_KEY_QUIT, VIEWER_KEY_SUSPEND,
    VIEWER_KEY_UP, VIEWER_KEY_DOWN, VIEWER_KEY_PAGE_UP, VIEWER_KEY_PAGE_DOWN,
    VIEWER_KEY_TOP, VIEWER_KEY_BOTTOM, VIEWER_KEY_BACKSPACE, VIEWER_KEY_TOGGLE,
    VIEWER_KEY_HELP, VIEWER_KEY_RESET, VIEWER_KEY_TS_CYCLE, VIEWER_KEY_TZ_TOGGLE,
    VIEWER_KEY_WRAP_TOGGLE, VIEWER_KEY_SOURCE_COL, VIEWER_KEY_LINE_NUMBERS,
    VIEWER_KEY_JUMP, VIEWER_KEY_WHEEL_UP, VIEWER_KEY_WHEEL_DOWN,
    VIEWER_KEY_CTRL_P, VIEWER_KEY_PRINTABLE
};

struct viewer_row { char *line; off_t offset; };

struct viewer_state {
    int fd;
    const char *title;
    bool debug_stats, colors, follow, follow_exited;
    hold_log_viewer_running_fn is_running;
    hold_log_viewer_exit_code_fn get_exit_code;
    void *running_userdata;
    struct hold_log_viewer_context context;
    struct hold_log_filter_options base_opts;
    char filter[VIEWER_FILTER_MAX + 1];
    char *examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t example_count;
    bool help_open, line_numbers, jump_active;
    char jump_buf[12];
    /* Presentation-only view controls (never change stored records or filtering). */
    enum hold_ts_mode ts_mode;
    int ts_zone; /* 0 local, 1 UTC, 2 elapsed since capture start (monotonic display) */
    bool wrap, source_column;
    /* Playback transport (docs/future/playback.md): a mode of the viewer, one
     * set of physics with browsing. play_pos is the head — how many index
     * records time has revealed; the page renders anchored at its byte edge. */
    bool play_mode, play_paused;
    int play_dir;         /* +1 forward, -1 rewind */
    int play_rung;        /* index into viewer_play_rates */
    size_t play_pos;      /* records revealed so far */
    uint64_t play_due_ms; /* monotonic deadline of the next head step */
    char osd[24];         /* transport indicator (UTF-8), upper-right corner */
    size_t osd_glyphs;
    uint64_t osd_until_ms;
    unsigned source_mask; /* 0 == all sources visible */
    struct hold_logidx_map idx_map;
    bool idx_loaded;
    off_t idx_loaded_raw_size;
    /* Content kind (playback spec): pty-tagged records make this a terminal
     * recording — screen physics (transport, timestamps, seek), no line
     * controls, and repaint by replay-from-nearest-clear. */
    bool screen_kind;
    /* Attached-console session being drained while the viewer runs; a second
     * Ctrl-P double-tap returns to the console (500 ms pairing window). */
    bool drain_enabled;
    int drain_fd;
    uint64_t ctrl_p_last_ms;
    bool proc_active, has_exit_code;
    int exit_code;
    off_t start_offset;
    off_t history[VIEWER_OFFSET_HISTORY_MAX];
    size_t history_count;
    enum viewer_scan_mode scan_mode;
    bool cache_valid, cache_scan_limited, cache_reached_eof;
    bool at_live_edge, at_oldest_edge, newer_available;
    struct viewer_row *visible;
    size_t visible_count, visible_capacity;
    off_t prev_offset, next_offset, tail_anchor;
    off_t newer_scan_offset, newer_floor_offset;
    bool local_scan_limit_active, cache_local_limited;
    off_t local_scan_limit_end;
    size_t cache_bytes_read, cache_lines_scanned, cache_match_count;
    size_t scan_generation, selected;
    bool select_resolve;
    off_t select_offset;
    size_t rows, cols;
};

/*
 * The TTY viewer keeps exactly one rendered page in `visible`.
 *
 * cache_valid means that page was filtered from `start_offset` using
 * `scan_mode`; `prev_offset` and `next_offset` are the directional anchors
 * produced by that scan. Cursor movement inside `visible` must not invalidate
 * the cache. PgUp/PgDn are the operations that deliberately move the anchor and
 * request a new bounded scan.
 *
 * In follow mode, `at_live_edge` means the next refill is anchored at EOF.
 * `tail_anchor` is the newest file size the viewer has observed.
 * `newer_floor_offset` is the file size at the moment the user left the live
 * edge; only matches at or beyond that floor count as "newer below".
 * `newer_scan_offset` is the newest appended byte offset examined for the
 * browsed-away notification. When the user browses away, each follow tick scans
 * a bounded appended slice and advances `newer_scan_offset`; sparse matches in
 * large bursts are therefore deferred, not skipped.
 *
 * A budget-limited page is a slice, not an answer: while the viewport is
 * unfilled and this page's boundary is unexplored, idle poll ticks resume the
 * scan from `next_offset` (forward) or `prev_offset` (backward) until the
 * page fills or the boundary is exhausted. The scan budget bounds per-tick
 * latency, never total coverage.
 */

struct raw_terminal { struct termios original; bool active; };

static int viewer_write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static int viewer_puts(const char *s) {
    return viewer_write_all(STDOUT_FILENO, s, strlen(s));
}

static int raw_terminal_enter(struct raw_terminal *raw) {
    memset(raw, 0, sizeof(*raw));
    if (tcgetattr(STDIN_FILENO, &raw->original) != 0) return -1;
    struct termios t = raw->original;
    /* ISIG off: Ctrl-C reaches the viewer as a byte and quits through the
     * cleanup path, and Ctrl-Z suspends deliberately (terminal restored)
     * instead of stopping the group inside the alternate screen. */
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN | ISIG);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= (tcflag_t)~(OPOST);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return -1;
    raw->active = true;
    /* Alternate screen, hidden cursor, SGR mouse buttons (wheel scrolling). */
    if (viewer_puts("\033[?1049h\033[?25l\033[?1006h\033[?1000h") != 0) return -1;
    return 0;
}

static void raw_terminal_leave(struct raw_terminal *raw) {
    viewer_puts("\033[?1000l\033[?1006l\033[?25h\033[?1049l");
    if (raw->active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw->original);
        raw->active = false;
    }
}

static int read_byte(unsigned char *out) {
    while (1) {
        ssize_t n = read(STDIN_FILENO, out, 1);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

static bool byte_ready(int timeout_ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    int ready;
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

/* Every printable byte belongs to the filter, so the only quit keys are ones
 * nobody can type into a filter term: lone Esc, Ctrl-C, and Ctrl-D — the pty
 * EOF byte, which is also what `script` and friends send when input ends. */
static const struct { unsigned char byte; enum viewer_key key; } viewer_ctrl_keys[] = {
    {3, VIEWER_KEY_QUIT},        {4, VIEWER_KEY_QUIT},           /* Ctrl-C, Ctrl-D */
    {26, VIEWER_KEY_SUSPEND},    {7, VIEWER_KEY_JUMP},           /* Ctrl-Z, Ctrl-G */
    {8, VIEWER_KEY_HELP},        {12, VIEWER_KEY_LINE_NUMBERS},  /* Ctrl-H, Ctrl-L */
    {18, VIEWER_KEY_RESET},      {20, VIEWER_KEY_TS_CYCLE},      /* Ctrl-R, Ctrl-T */
    {21, VIEWER_KEY_TZ_TOGGLE},  {23, VIEWER_KEY_WRAP_TOGGLE},   /* Ctrl-U, Ctrl-W */
    {25, VIEWER_KEY_SOURCE_COL}, {127, VIEWER_KEY_BACKSPACE},
    {6, VIEWER_KEY_PAGE_DOWN},   {2, VIEWER_KEY_PAGE_UP},        /* Ctrl-F, Ctrl-B */
    {16, VIEWER_KEY_CTRL_P},                                     /* attach return */
};

/* Arrow/Home/End final bytes shared by the SS3 (ESC O) and CSI (ESC [) forms. */
static enum viewer_key arrow_final_key(unsigned char f) {
    if (f == 'A') return VIEWER_KEY_UP;
    if (f == 'B') return VIEWER_KEY_DOWN;
    if (f == 'H') return VIEWER_KEY_TOP;
    if (f == 'F') return VIEWER_KEY_BOTTOM;
    return VIEWER_KEY_NONE;
}

static enum viewer_key read_key(unsigned char *printable, int timeout_ms) {
    unsigned char c = 0;
    *printable = 0;
    if (timeout_ms >= 0 && !byte_ready(timeout_ms)) return VIEWER_KEY_NONE;
    if (read_byte(&c) != 0) return VIEWER_KEY_QUIT;
    if (c == ' ' || c == '\r' || c == '\n') return c == ' ' ? VIEWER_KEY_TOGGLE : VIEWER_KEY_DOWN;
    for (size_t i = 0; i < sizeof(viewer_ctrl_keys) / sizeof(viewer_ctrl_keys[0]); i++) {
        if (viewer_ctrl_keys[i].byte == c) return viewer_ctrl_keys[i].key;
    }
    if (c == 27) {
        /* A lone Esc is the quit key; an Esc with bytes right behind it is
         * the start of an arrow/page/home/end sequence. */
        if (!byte_ready(50)) return VIEWER_KEY_QUIT;
        unsigned char a = 0;
        if (read_byte(&a) != 0) return VIEWER_KEY_QUIT;
        if (a == 'O') {
            unsigned char b = 0;
            if (read_byte(&b) != 0) return VIEWER_KEY_QUIT;
            return arrow_final_key(b);
        }
        /* Esc followed by anything that is not a recognized sequence keeps
         * Esc's meaning: quit. (This also covers Esc immediately chased by
         * the pty EOF byte when piped input closes.) */
        if (a != '[') return VIEWER_KEY_QUIT;
        unsigned char seq[32];
        size_t n = 0;
        while (n + 1 < sizeof(seq)) {
            if (read_byte(&seq[n]) != 0) return VIEWER_KEY_QUIT;
            unsigned char ch = seq[n++];
            if (ch >= 0x40 && ch <= 0x7e) break;
        }
        if (n == 0) return VIEWER_KEY_NONE;
        unsigned char final = seq[n - 1];
        /* SGR mouse report: ESC [ < Pb ; Px ; Py M|m. Wheel notches carry
         * bit 64; everything else (clicks, drags, releases) is ignored. */
        if (seq[0] == '<') {
            if (final != 'M') return VIEWER_KEY_NONE;
            long btn = 0;
            for (size_t i = 1; i + 1 < n && isdigit(seq[i]); i++) btn = btn * 10 + (seq[i] - '0');
            if (btn & 64) return (btn & 1) ? VIEWER_KEY_WHEEL_DOWN : VIEWER_KEY_WHEEL_UP;
            return VIEWER_KEY_NONE;
        }
        if (final != '~') return arrow_final_key(final);
        {
            long param = 0;
            for (size_t i = 0; i + 1 < n && isdigit(seq[i]); i++) param = param * 10 + (long)(seq[i] - '0');
            if (param == 5) return VIEWER_KEY_PAGE_UP;
            if (param == 6) return VIEWER_KEY_PAGE_DOWN;
            if (param == 1 || param == 7) return VIEWER_KEY_TOP;
            if (param == 4 || param == 8) return VIEWER_KEY_BOTTOM;
        }
        return VIEWER_KEY_NONE;
    }
    if (isprint(c)) {
        *printable = c;
        return VIEWER_KEY_PRINTABLE;
    }
    return VIEWER_KEY_NONE;
}

static void free_examples(struct viewer_state *state) {
    for (size_t i = 0; i < state->example_count; i++) free(state->examples[i]);
    memset(state->examples, 0, sizeof(state->examples));
    state->example_count = 0;
}

static void cache_clear(struct viewer_state *state) {
    if (state->visible) for (size_t i = 0; i < state->visible_count; i++) free(state->visible[i].line);
    state->visible_count = 0;
    state->prev_offset = state->next_offset = state->start_offset;
    state->cache_scan_limited = state->cache_reached_eof = state->cache_local_limited = false;
    state->cache_bytes_read = state->cache_lines_scanned = state->cache_match_count = 0;
    state->at_oldest_edge = false;
}

static void cache_free(struct viewer_state *state) {
    cache_clear(state);
    free(state->visible);
    state->visible = NULL;
    state->visible_capacity = 0;
    state->cache_valid = false;
}

static int cache_ensure_capacity(struct viewer_state *state, size_t cap) {
    if (state->visible_capacity >= cap) return 0;
    struct viewer_row *grown = realloc(state->visible, cap * sizeof(*state->visible));
    if (!grown) return -1;
    memset(grown + state->visible_capacity, 0, (cap - state->visible_capacity) * sizeof(*grown));
    state->visible = grown;
    state->visible_capacity = cap;
    return 0;
}

/* Copy scanner rows into the page cache. prepend inserts them above row 0
 * (backward continuation: older rows arrive above) and unwinds fully on a
 * copy failure; appended rows are owned as they land. */
static int cache_append_rows(struct viewer_state *state, const struct hold_log_filter_result *r, bool prepend) {
    size_t add = r->line_count;
    if (add == 0) return 0;
    if (cache_ensure_capacity(state, state->visible_count + add) != 0) return -1;
    if (prepend) memmove(state->visible + add, state->visible, state->visible_count * sizeof(*state->visible));
    for (size_t i = 0; i < add; i++) {
        char *copy = strdup(r->lines[i]);
        if (!copy) {
            if (!prepend) return -1;
            for (size_t j = 0; j < i; j++) free(state->visible[j].line);
            memmove(state->visible, state->visible + add, state->visible_count * sizeof(*state->visible));
            return -1;
        }
        size_t at = prepend ? i : state->visible_count++;
        state->visible[at].line = copy;
        state->visible[at].offset = r->line_offsets[i];
    }
    if (prepend) state->visible_count += add;
    return 0;
}

static int cache_load_result(struct viewer_state *state, const struct hold_log_filter_result *result, size_t cap) {
    if (cache_ensure_capacity(state, cap ? cap : 1) != 0) return -1;
    cache_clear(state);
    if (cache_append_rows(state, result, false) != 0) {
        cache_clear(state);
        return -1;
    }
    state->prev_offset = result->prev_offset;
    state->next_offset = result->next_offset;
    state->cache_scan_limited = result->scan_limited;
    state->cache_reached_eof = result->reached_eof;
    state->cache_bytes_read = result->bytes_read;
    state->cache_lines_scanned = result->lines_scanned;
    state->cache_match_count = result->match_count;
    state->at_oldest_edge = state->scan_mode == VIEWER_SCAN_BACKWARD
        ? (!result->scan_limited && result->match_count <= result->line_count)
        : state->start_offset == 0;
    state->cache_valid = true;
    state->scan_generation++;
    if (state->selected >= state->visible_count && state->visible_count > 0) state->selected = state->visible_count - 1;
    if (state->visible_count == 0) state->selected = 0;
    /*
     * WO-3 (spec:177-190): selection is record identity, not a row number.
     * When an operation asked for a record by offset, resolve it against the
     * fresh page: the record itself if still visible, else the record
     * underneath it, else the nearest previous one.
     */
    if (state->select_resolve) {
        state->select_resolve = false;
        if (state->visible_count > 0) {
            size_t pick = state->visible_count - 1;
            for (size_t i = 0; i < state->visible_count; i++) {
                if (state->visible[i].offset >= state->select_offset) { pick = i; break; }
            }
            state->selected = pick;
        }
    }
    return 0;
}

static void cache_invalidate(struct viewer_state *state) {
    state->cache_valid = false;
}

/* Ask the next refill to keep the cursor on this record (see the resolution
 * rules in cache_load_result). */
static void select_record_after_refill(struct viewer_state *state, off_t offset) {
    state->select_resolve = true;
    state->select_offset = offset;
}

static void configure_filter_opts(const struct viewer_state *state, struct hold_log_filter_options *opts);
static void enter_browsing_mode(struct viewer_state *state, bool stabilize_visible_page);
static void clear_local_scan_limit(struct viewer_state *state);
static void jump_to_newest_page(struct viewer_state *state);

static bool refresh_terminal_size(struct viewer_state *state) {
    size_t old_rows = state->rows, old_cols = state->cols;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        state->rows = ws.ws_row;
        state->cols = ws.ws_col;
    } else { state->rows = 24; state->cols = 80; }
    if (old_rows && (old_rows != state->rows || old_cols != state->cols)) {
        /* A relayout must not move the cursor off its record (spec:177). */
        if (state->selected < state->visible_count) {
            select_record_after_refill(state, state->visible[state->selected].offset);
        }
        cache_invalidate(state);
        return true;
    }
    return false;
}

static off_t current_page_anchor(const struct viewer_state *state) {
    if (state->visible_count > 0) return state->visible[0].offset;
    return state->start_offset;
}

static void reset_filter_navigation(struct viewer_state *state) {
    bool preserve_browsed_page = state->follow && !state->at_live_edge;
    off_t browsed_anchor = preserve_browsed_page ? current_page_anchor(state) : 0;
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->at_live_edge = false;
    state->newer_available = false;
    clear_local_scan_limit(state);
    if (state->follow) {
        off_t end = lseek(state->fd, 0, SEEK_END);
        if (end >= 0) state->tail_anchor = end;
        state->newer_scan_offset = state->newer_floor_offset = state->tail_anchor;
        if (preserve_browsed_page) {
            state->start_offset = browsed_anchor < 0 ? 0 : browsed_anchor;
            if (state->next_offset > state->start_offset) {
                state->local_scan_limit_active = true;
                state->local_scan_limit_end = state->next_offset;
            }
        } else {
            if (end >= 0) state->start_offset = end;
            state->scan_mode = VIEWER_SCAN_BACKWARD;
            state->at_live_edge = true;
        }
    } else {
        state->start_offset = state->tail_anchor = 0;
        state->newer_scan_offset = state->newer_floor_offset = 0;
    }
    state->history_count = 0;
    /* The page anchor survives the filter edit, so the selection does too:
     * the same record stays selected, or resolves per spec:179-185. */
    if (preserve_browsed_page && state->selected < state->visible_count) {
        select_record_after_refill(state, state->visible[state->selected].offset);
    }
    state->selected = 0;
    cache_invalidate(state);
}

/*
 * Rows available for log content. The polished chrome pins a one-line header
 * plus a one-line footer; a filter row appears between them only while a
 * filter is typed. The --debug-stats harness keeps the legacy single header
 * line so its frozen navigation assertions stay byte-stable.
 */
static size_t viewer_body_rows(const struct viewer_state *state) {
    size_t chrome = state->debug_stats ? 2 : ((state->filter[0] || state->jump_active) ? 3 : 2);
    return state->rows > chrome ? state->rows - chrome : 1;
}

/* Single-hit probe options: find one match, keep the ring honest. */
static void configure_probe_opts(const struct viewer_state *state, struct hold_log_filter_options *opts) {
    configure_filter_opts(state, opts);
    opts->visible_capacity = 1;
    opts->max_results = 1;
    opts->match_ring_capacity = 1;
}

static size_t scan_budget_floor(const struct viewer_state *state) {
    size_t budget = viewer_body_rows(state) * VIEWER_SCAN_BYTES_PER_ROW;
    return budget < VIEWER_SCAN_BUDGET_FLOOR ? VIEWER_SCAN_BUDGET_FLOOR : budget;
}

static int appended_range_has_match(struct viewer_state *state, off_t start, off_t end, bool *has_match, off_t *scanned_to) {
    *has_match = false;
    *scanned_to = start;
    if (end <= start) return 0;
    if (lseek(state->fd, start, SEEK_SET) < 0) return -1;

    struct hold_log_filter_options opts;
    configure_probe_opts(state, &opts);
    opts.scan_byte_budget = scan_budget_floor(state);
    off_t appended_bytes = end - start;
    if (appended_bytes > 0 && (uintmax_t)appended_bytes < (uintmax_t)opts.scan_byte_budget) opts.scan_byte_budget = (size_t)appended_bytes;

    struct hold_log_filter_result result;
    if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
    *has_match = result.match_count > 0;
    *scanned_to = result.next_offset > start ? result.next_offset : start;
    if (*scanned_to > end) *scanned_to = end;
    hold_log_filter_result_free(&result);
    return 0;
}

static int handle_follow_tick(struct viewer_state *state) {
    bool changed = false;
    off_t end = lseek(state->fd, 0, SEEK_END);
    if (end >= 0 && end > state->tail_anchor) {
        state->tail_anchor = end;
        if (state->at_live_edge) {
            state->start_offset = state->newer_scan_offset = state->newer_floor_offset = end;
            state->newer_available = false;
            cache_invalidate(state);
            changed = true;
        }
    }
    if (end >= 0 && !state->at_live_edge && !state->newer_available && state->newer_scan_offset < state->tail_anchor) {
        off_t scanned_to = state->newer_scan_offset;
        bool has_match = false;
        if (appended_range_has_match(state, state->newer_scan_offset, state->tail_anchor, &has_match, &scanned_to) != 0) {
            return -1;
        }
        if (scanned_to > state->newer_scan_offset) state->newer_scan_offset = scanned_to;
        if (has_match) {
            state->newer_available = true;
            changed = true;
        }
    }
    if (state->follow && state->cache_reached_eof && state->is_running && !state->is_running(state->running_userdata)) {
        state->follow = false;
        state->follow_exited = true;
        state->proc_active = false;
        int code = 0;
        if (state->get_exit_code && state->get_exit_code(state->running_userdata, &code)) {
            state->exit_code = code;
            state->has_exit_code = true;
        }
        cache_invalidate(state);
        changed = true;
    }
    return changed ? 1 : 0;
}

/* The exit message must never lie: before quitting, do one final check for
 * matches that appeared below while the operator was browsed away. */
static int mark_newer_before_quit(struct viewer_state *state) {
    if (!state->follow || state->at_live_edge || state->newer_available) return 0;
    off_t end = lseek(state->fd, 0, SEEK_END);
    if (end < 0) return -1;
    if (end > state->tail_anchor) state->tail_anchor = end;
    if (state->newer_scan_offset >= state->tail_anchor) {
        struct hold_log_filter_options opts;
        configure_probe_opts(state, &opts);
        struct hold_log_filter_result tail_result;
        if (hold_log_filter_backward_fd(state->fd, &opts, state->tail_anchor, VIEWER_SCAN_BUDGET_FLOOR, &tail_result) != 0) return -1;
        bool has_tail_match = tail_result.line_count > 0 && tail_result.line_offsets[0] >= state->newer_floor_offset;
        hold_log_filter_result_free(&tail_result);
        if (!has_tail_match) return 0;
        state->newer_available = true;
        return 1;
    }
    off_t scanned_to = state->newer_scan_offset;
    bool has_match = false;
    if (appended_range_has_match(state, state->newer_scan_offset, state->tail_anchor, &has_match, &scanned_to) != 0) return -1;
    if (scanned_to > state->newer_scan_offset) state->newer_scan_offset = scanned_to;
    if (!has_match) return 0;
    state->newer_available = true;
    return 1;
}

static int toggle_example(struct viewer_state *state, const char *line) {
    if (!line) return 0;
    /* Space edits the line the operator is looking at: pin the rendered page
     * first so the exclusion does not re-anchor at EOF ("looping" back down). */
    enter_browsing_mode(state, true);
    /* spec:179-185 — after the exclusion lands, the selection resolves to
     * this record, else the record underneath it, else nearest previous. */
    off_t selected_record = state->selected < state->visible_count
                                ? state->visible[state->selected].offset
                                : (off_t)-1;
    for (size_t i = 0; i < state->example_count; i++) {
        if (state->examples[i] && strcmp(state->examples[i], line) == 0) {
            free(state->examples[i]);
            for (size_t j = i + 1; j < state->example_count; j++) state->examples[j - 1] = state->examples[j];
            state->examples[--state->example_count] = NULL;
            reset_filter_navigation(state);
            if (selected_record >= 0) select_record_after_refill(state, selected_record);
            return 0;
        }
    }
    if (state->example_count >= HOLD_LOG_VIEWER_MAX_EXAMPLES) return 0;
    char *copy = strdup(line);
    if (!copy) return -1;
    state->examples[state->example_count++] = copy;
    reset_filter_navigation(state);
    if (selected_record >= 0) select_record_after_refill(state, selected_record);
    return 0;
}

static void configure_filter_opts(const struct viewer_state *state, struct hold_log_filter_options *opts) {
    *opts = state->base_opts;
    opts->literal = state->filter[0] ? state->filter : NULL;
    opts->similar_example_count = 0;
    opts->exclude_example_count = state->example_count;
    for (size_t i = 0; i < opts->exclude_example_count; i++) opts->exclude_examples[i] = state->examples[i];
    size_t visible_rows = viewer_body_rows(state);
    opts->visible_capacity = visible_rows;
    opts->max_results = visible_rows;
    if (opts->match_ring_capacity < visible_rows * 4) opts->match_ring_capacity = visible_rows * 4;
    opts->idx_map = state->idx_loaded ? &state->idx_map : NULL;
    opts->source_mask = state->source_mask;
}

/*
 * Load (or reload) the HLOGIDX sidecar so timestamp and source metadata track a
 * growing followed log. Absence of a sidecar is not an error: the viewer simply
 * renders without timestamps/source and the mode cluster stays honest.
 */
static void viewer_reload_idx(struct viewer_state *state) {
    if (!state->context.log_path || !*state->context.log_path) return;
    off_t size = lseek(state->fd, 0, SEEK_END);
    if (state->idx_loaded && size >= 0 && size == state->idx_loaded_raw_size) return;
    struct hold_logidx_map fresh;
    if (hold_logidx_map_load(state->context.log_path, &fresh) != 0) return;
    if (state->idx_loaded) hold_logidx_map_free(&state->idx_map);
    state->idx_map = fresh;
    state->idx_loaded = true;
    state->idx_loaded_raw_size = size;
    /* The stream tag decides what the viewer is looking at (playback spec):
     * any pty-tagged record makes the log a terminal recording. Sticky —
     * a session that goes TUI mid-capture switches to screen physics. */
    if (!state->screen_kind) {
        for (size_t i = state->idx_map.count; i > 0; i--) {
            if (hold_logidx_record_stream(state->idx_map.records[i - 1].meta) == HOLD_LOG_STREAM_PTY) {
                state->screen_kind = true;
                break;
            }
        }
    }
}

/* ---- screen-recording repaint (docs/future/playback.md, decision 5) ------ */

#define SCREEN_CLEAR_SCAN_WINDOW (256u * 1024u)

/* The screen-clearing markers a repaint can anchor on. Alt-screen switches
 * and RIS are also the sequences that must never reach the live terminal
 * raw (they would yank it out of the caller's alt screen), so the same
 * table drives both the anchor search and the emit-time rewrite. `safe`
 * marks sequences that may be emitted as-is. */
static const struct { const char *seq; size_t len; bool safe; } screen_markers[] = {
    {"\033[2J", 4, true},
    {"\033[?1049h", 8, false}, {"\033[?1049l", 8, false},
    {"\033[?1047h", 8, false}, {"\033[?1047l", 8, false},
    {"\033[?47h", 6, false},   {"\033[?47l", 6, false},
    {"\033c", 2, false},
};
#define SCREEN_MARKER_MAX_LEN 8

static int screen_marker_at(const unsigned char *buf, size_t n, size_t i) {
    if (buf[i] != 0x1b) return -1;
    for (size_t m = 0; m < sizeof(screen_markers) / sizeof(screen_markers[0]); m++) {
        size_t len = screen_markers[m].len;
        if (i + len <= n && memcmp(buf + i, screen_markers[m].seq, len) == 0) return (int)m;
    }
    return -1;
}

/* Emits log bytes [start, end) raw, rewriting unsafe screen switches to
 * plain clears. Chunked with a small carry so markers straddling chunk
 * boundaries are still seen. */
static int screen_emit_span(int log_fd, off_t start, off_t end, int out_fd) {
    unsigned char buf[16384];
    size_t have = 0;
    off_t off = start;
    for (;;) {
        while (have < sizeof(buf) && off < end) {
            ssize_t nr = pread(log_fd, buf + have, sizeof(buf) - have, off);
            if (nr < 0 && errno == EINTR) continue;
            if (nr < 0) return -1;
            if (nr == 0) { end = off; break; } /* index past a torn tail */
            have += (size_t)nr;
            off += nr;
        }
        if (have == 0) return 0;
        bool final = off >= end;
        /* Hold back a tail that could hide a split marker, unless final. */
        size_t scan_limit = final ? have : have - (SCREEN_MARKER_MAX_LEN - 1);
        size_t i = 0, emitted = 0;
        while (i < scan_limit) {
            if (buf[i] != 0x1b) { i++; continue; }
            int m = screen_marker_at(buf, have, i);
            if (m < 0 || screen_markers[m].safe) { i++; continue; }
            if (i > emitted && viewer_write_all(out_fd, (const char *)buf + emitted, i - emitted) != 0) return -1;
            if (viewer_write_all(out_fd, "\033[2J\033[H", 7) != 0) return -1;
            i += screen_markers[m].len;
            emitted = i;
        }
        if (i > emitted && viewer_write_all(out_fd, (const char *)buf + emitted, i - emitted) != 0) return -1;
        memmove(buf, buf + i, have - i);
        have -= i;
        if (final) {
            if (have > 0 && viewer_write_all(out_fd, (const char *)buf, have) != 0) return -1;
            return 0;
        }
    }
}

int hold_log_screen_repaint(int log_fd, off_t end, bool from_start, int out_fd) {
    if (end < 0) return -1;
    off_t win_start = end > (off_t)SCREEN_CLEAR_SCAN_WINDOW ? end - (off_t)SCREEN_CLEAR_SCAN_WINDOW : 0;
    off_t anchor = -1;
    size_t win_len = (size_t)(end - win_start);
    if (win_len > 0) {
        unsigned char *win = malloc(win_len);
        if (!win) return -1;
        size_t got = 0;
        while (got < win_len) {
            ssize_t nr = pread(log_fd, win + got, win_len - got, win_start + (off_t)got);
            if (nr < 0 && errno == EINTR) continue;
            if (nr <= 0) break;
            got += (size_t)nr;
        }
        for (size_t i = 0; i < got; i++) {
            int m = screen_marker_at(win, got, i);
            if (m >= 0) anchor = win_start + (off_t)(i + screen_markers[m].len);
        }
        free(win);
    }
    if (anchor < 0) {
        if (!from_start) return 1;
        anchor = 0; /* no clear within bounded distance: re-emit from start */
    }
    if (viewer_write_all(out_fd, "\033[2J\033[H", 7) != 0) return -1;
    return screen_emit_span(log_fd, anchor, end, out_fd);
}

/* ---- playback transport (docs/future/playback.md) ----------------------- */

static uint64_t viewer_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Byte offset of the playback head: everything before it has been revealed. */
static off_t playback_edge_offset(const struct viewer_state *state) {
    if (!state->idx_loaded || state->play_pos == 0) return 0;
    size_t i = state->play_pos <= state->idx_map.count ? state->play_pos : state->idx_map.count;
    const struct hold_logidx_record *rec = &state->idx_map.records[i - 1];
    return rec->offset + (off_t)rec->len;
}

/* Re-anchor the rendered page at the playback head, like a live tail whose
 * edge is the head instead of EOF. */
static void playback_anchor(struct viewer_state *state) {
    state->scan_mode = VIEWER_SCAN_BACKWARD;
    state->start_offset = playback_edge_offset(state);
    state->at_live_edge = false;
    state->at_oldest_edge = false;
    state->history_count = 0;
    state->selected = 0;
    clear_local_scan_limit(state);
    cache_invalidate(state);
}

/* Flash the mode-change OSD (spec, verbatim): PLAY / PAUSED, or chevrons
 * whose count is ladder position + 1 so the glyph itself reads as speed. */
static void playback_osd_show(struct viewer_state *state) {
    if (state->play_paused) {
        snprintf(state->osd, sizeof(state->osd), "PAUSED");
        state->osd_glyphs = 6;
    } else if (state->play_rung == 0 && state->play_dir > 0) {
        snprintf(state->osd, sizeof(state->osd), "PLAY");
        state->osd_glyphs = 4;
    } else {
        const char *glyph = state->play_dir > 0 ? "\xe2\x96\xb6" : "\xe2\x97\x80"; /* U+25B6 / U+25C0 */
        size_t nglyphs = (size_t)state->play_rung + 1;
        for (size_t i = 0; i < nglyphs; i++) memcpy(state->osd + i * 3, glyph, 3);
        state->osd[nglyphs * 3] = '\0';
        state->osd_glyphs = nglyphs;
    }
    state->osd_until_ms = viewer_now_ms() + VIEWER_OSD_MS;
}

static void playback_enter(struct viewer_state *state, size_t pos, int dir, bool paused) {
    state->play_mode = true;
    state->play_paused = paused;
    state->play_dir = dir;
    state->play_rung = 0;
    state->play_pos = pos;
    state->play_due_ms = viewer_now_ms();
    state->newer_available = false;
    state->newer_scan_offset = state->newer_floor_offset = state->tail_anchor;
    playback_osd_show(state);
    playback_anchor(state);
}

/* Esc/q leave playback into browsing; reaching the live edge of a followed
 * log resumes the tail (spec: scrub-to-live-edge resumes the tail). */
static void playback_leave(struct viewer_state *state, bool to_tail) {
    state->play_mode = false;
    state->play_paused = false;
    state->osd[0] = '\0';
    state->osd_until_ms = 0;
    if (to_tail && state->follow) {
        jump_to_newest_page(state);
        return;
    }
    cache_invalidate(state); /* repaint as a browse page: the cursor appears */
}

/* Advance the head on its schedule: sleep the recorded deltas, scaled by the
 * rate ladder. Returns true when the frame must repaint. */
static bool playback_tick(struct viewer_state *state) {
    if (!state->play_mode || state->play_paused || !state->idx_loaded) return false;
    uint64_t now = viewer_now_ms();
    if (now < state->play_due_ms) return false;
    unsigned rate = viewer_play_rates[state->play_rung];
    bool changed = false, moved = false;
    for (int guard = 4096; guard > 0 && now >= state->play_due_ms; guard--) {
        if (state->play_dir > 0) {
            if (state->play_pos >= state->idx_map.count) {
                viewer_reload_idx(state); /* a live log may have grown */
                if (state->play_pos >= state->idx_map.count) {
                    if (state->follow) {
                        playback_leave(state, true);
                        return true;
                    }
                    state->play_paused = true; /* end of a finished log */
                    playback_osd_show(state);
                    changed = true;
                    break;
                }
            }
            state->play_pos++;
        } else {
            if (state->play_pos == 0) {
                state->play_paused = true; /* rewound to the start of time */
                playback_osd_show(state);
                changed = true;
                break;
            }
            state->play_pos--;
        }
        moved = true;
        /* The next boundary to cross is between records p-1 and p; sleep the
         * recorded delta between them, scaled by the rate. */
        size_t p = state->play_pos;
        if (state->play_dir > 0 ? p < state->idx_map.count : p > 0) {
            uint64_t a = state->idx_map.records[p - 1].ts_us;
            uint64_t b = state->idx_map.records[p].ts_us;
            state->play_due_ms += b > a ? (b - a) / 1000ULL / rate : 0;
        }
    }
    if (moved) {
        playback_anchor(state);
        changed = true;
    }
    return changed;
}

/* Space: stop / resume at 1x. From the live tail it freezes the recorded
 * edge (playback mode, paused). */
static void transport_space(struct viewer_state *state) {
    if (!state->play_mode) {
        viewer_reload_idx(state);
        if (!state->idx_loaded) return;
        playback_enter(state, state->idx_map.count, +1, true);
        return;
    }
    if (state->play_paused) {
        state->play_paused = false;
        state->play_dir = +1;
        state->play_rung = 0; /* Space always resumes at 1x */
        state->play_due_ms = viewer_now_ms();
    } else {
        state->play_paused = true;
    }
    playback_osd_show(state);
}

/* `.` / `,`: multistep FF / RW — repeat presses step the rate ladder up; a
 * direction change starts back at 1x. From the live tail `,` starts
 * rewinding; `.` has nothing ahead of real time. */
static void transport_rate(struct viewer_state *state, int dir) {
    if (!state->play_mode) {
        if (dir > 0) return;
        viewer_reload_idx(state);
        if (!state->idx_loaded) return;
        playback_enter(state, state->idx_map.count, -1, false);
        return;
    }
    if (state->play_paused || state->play_dir != dir) {
        state->play_paused = false;
        state->play_dir = dir;
        state->play_rung = 0;
        state->play_due_ms = viewer_now_ms();
    } else if ((size_t)state->play_rung + 1 < VIEWER_PLAY_RUNGS) {
        state->play_rung++;
    }
    playback_osd_show(state);
}

/* Re-anchor forward at `off` and rescan; the shared tail of both backward
 * refill restarts (empty backward page, oldest backward page). */
static int refill_forward_from(struct viewer_state *state, struct hold_log_filter_options *opts,
                               size_t scan_budget, off_t off, struct hold_log_filter_result *result) {
    state->start_offset = off;
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->history_count = 0;
    state->at_live_edge = false;
    hold_log_filter_result_free(result);
    opts->scan_byte_budget = scan_budget;
    /* A pinned byte range (browsed-page preserve, playback head) caps forward
     * scans on every refill path, not only the plain forward branch. */
    if (state->local_scan_limit_active && state->local_scan_limit_end > off) {
        off_t cap = state->local_scan_limit_end - off;
        if ((uintmax_t)cap < (uintmax_t)opts->scan_byte_budget) opts->scan_byte_budget = (size_t)cap;
    }
    if (lseek(state->fd, off, SEEK_SET) < 0) return -1;
    return hold_log_filter_fd(state->fd, opts, result);
}

static int refill_cache(struct viewer_state *state) {
    viewer_reload_idx(state);
    if (state->play_mode) {
        /* The playback head is the visible end of time: no scan on any refill
         * path may reveal bytes past it. */
        state->local_scan_limit_active = true;
        state->local_scan_limit_end = playback_edge_offset(state);
    }
    struct hold_log_filter_options opts;
    configure_filter_opts(state, &opts);
    size_t visible_rows = viewer_body_rows(state);
    size_t scan_budget = visible_rows * VIEWER_SCAN_BYTES_PER_ROW;
    bool local_capped = false;
    struct hold_log_filter_result result;
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        if (state->follow && state->at_live_edge) {
            off_t end = lseek(state->fd, 0, SEEK_END);
            if (end >= 0) {
                state->start_offset = state->tail_anchor = end;
                state->newer_scan_offset = state->newer_floor_offset = end;
            }
        }
        if (hold_log_filter_backward_fd(state->fd, &opts, state->start_offset, scan_budget, &result) != 0) return -1;
        bool oldest_backward_page = !result.scan_limited && result.match_count <= result.line_count;
        if (state->start_offset > 0 && !result.scan_limited && result.line_count == 0) {
            /* Nothing above the anchor matched: the page lives at the top. */
            if (refill_forward_from(state, &opts, scan_budget, 0, &result) != 0) return -1;
        } else if (!state->at_live_edge && oldest_backward_page && result.line_count > 0) {
            /* The backward window hit the oldest match: pin the page under it
             * so repeated PageUp is idempotent. */
            if (refill_forward_from(state, &opts, scan_budget, result.line_offsets[0], &result) != 0) return -1;
        }
    } else {
        opts.scan_byte_budget = scan_budget;
        if (state->local_scan_limit_active && state->local_scan_limit_end > state->start_offset) {
            off_t local_budget = state->local_scan_limit_end - state->start_offset;
            if (local_budget > 0 && (uintmax_t)local_budget < (uintmax_t)opts.scan_byte_budget) {
                opts.scan_byte_budget = (size_t)local_budget;
                local_capped = true;
            }
        }
        if (lseek(state->fd, state->start_offset, SEEK_SET) < 0) return -1;
        if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
    }
    clear_local_scan_limit(state);
    int rc = cache_load_result(state, &result, visible_rows);
    hold_log_filter_result_free(&result);
    /* Playback pages are deliberately truncated at the head: continuation
     * ticks must never scan past it. */
    if (rc == 0) state->cache_local_limited = local_capped || state->play_mode;
    return rc;
}

/*
 * WO-2: the engine must never pretend a match does not exist because a budget
 * expired. A page whose scan was budget-limited keeps scanning in idle-tick
 * slices until the viewport is satisfied or the page's file boundary is
 * exhausted. Pages deliberately pinned to a byte range (cache_local_limited)
 * stay as scanned: their truncation is intent, not a budget accident.
 */
static bool continuation_pending(const struct viewer_state *state) {
    /* A budget-limited page keeps scanning; an unfilled forward page that
     * reached EOF keeps watching, because EOF is not a permanent fact on a
     * live log (the re-probe is a zero-byte read until the file grows). */
    return state->cache_valid && !state->cache_local_limited &&
           state->visible_count < viewer_body_rows(state) &&
           (state->cache_scan_limited ||
            (state->scan_mode == VIEWER_SCAN_FORWARD && state->cache_reached_eof));
}

static int continue_scan_tick(struct viewer_state *state) {
    size_t room = viewer_body_rows(state) - state->visible_count;
    struct hold_log_filter_options opts;
    configure_filter_opts(state, &opts);
    opts.visible_capacity = room;
    opts.max_results = room;
    size_t budget = scan_budget_floor(state);
    bool was_limited = state->cache_scan_limited;
    bool changed = false;
    struct hold_log_filter_result result;
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        off_t anchor = state->prev_offset;
        if (anchor <= 0) { state->cache_scan_limited = false; return 1; }
        if (hold_log_filter_backward_fd(state->fd, &opts, anchor, budget, &result) != 0) return -1;
        size_t add = result.line_count;
        if (cache_append_rows(state, &result, true) != 0) { hold_log_filter_result_free(&result); return -1; }
        if (add > 0) {
            /* Older rows arrive above: the cursor stays on the same record. */
            if (state->visible_count > add && !(state->follow && state->at_live_edge)) state->selected += add;
            changed = true;
        }
        state->prev_offset = result.prev_offset;
        state->cache_scan_limited = result.scan_limited;
        if (!result.scan_limited && result.match_count <= result.line_count) state->at_oldest_edge = true;
    } else {
        if (lseek(state->fd, state->next_offset, SEEK_SET) < 0) return -1;
        opts.scan_byte_budget = budget;
        if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
        if (cache_append_rows(state, &result, false) != 0) { hold_log_filter_result_free(&result); return -1; }
        if (result.line_count > 0) changed = true;
        if (result.next_offset > state->next_offset) state->next_offset = result.next_offset;
        state->cache_scan_limited = result.scan_limited;
        state->cache_reached_eof = result.reached_eof;
    }
    state->cache_bytes_read += result.bytes_read;
    state->cache_lines_scanned += result.lines_scanned;
    state->cache_match_count += result.match_count;
    hold_log_filter_result_free(&result);
    if (was_limited && !state->cache_scan_limited) changed = true;
    return changed ? 1 : 0;
}

static const char *viewer_run_label(const struct viewer_state *state) {
    if (state->context.run_id && *state->context.run_id) return state->context.run_id;
    return state->title && *state->title ? state->title : "-";
}

static void put_bar_text(char *bar, size_t width, size_t pos, const char *text) {
    if (!bar || !text || pos >= width) return;
    size_t room = width - pos;
    size_t n = strlen(text);
    if (n <= room) { memcpy(bar + pos, text, n); return; }
    if (room >= 4) {
        memcpy(bar + pos, text, room - 3);
        memcpy(bar + pos + room - 3, "...", 3);
    } else {
        memcpy(bar + pos, text, room);
    }
}

/* A chrome bar: left text at column 0, right text flush to the edge. */
static char *make_bar(size_t width, const char *left, const char *right) {
    char *bar = malloc(width + 1);
    if (!bar) return NULL;
    memset(bar, ' ', width);
    bar[width] = '\0';
    put_bar_text(bar, width, 0, left);
    size_t rlen = strlen(right);
    if (rlen < width) put_bar_text(bar, width, width - rlen, right);
    return bar;
}

/* Sanitized bounded emit: printable bytes and tabs pass, the rest blank;
 * stops at the record's newline. */
static int viewer_put_capped(const char *s, size_t max) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && *p != '\n' && n < max; p++, n++) {
        char out = (isprint(*p) || *p == '\t') ? (char)*p : ' ';
        if (viewer_write_all(STDOUT_FILENO, &out, 1) != 0) return -1;
    }
    return 0;
}

/* Presentation-only prefix for one record: line number, timestamp, source
 * column. Bracketed fields mark record starts, which keeps wrapped
 * continuation rows visually distinct and filtered gaps visible in the
 * numbering. Emitted as printable ASCII so byte-width accounting stays exact. */
static size_t viewer_row_prefix(const struct viewer_state *state, off_t offset, char *out, size_t n) {
    if (n == 0) return 0;
    out[0] = '\0';
    size_t used = 0;
    const struct hold_logidx_record *rec =
        state->idx_loaded ? hold_logidx_map_find(&state->idx_map, offset) : NULL;
    if (state->line_numbers && rec && used + 12 < n) {
        size_t ordinal = (size_t)(rec - state->idx_map.records) + 1;
        int w = snprintf(out + used, n - used, "[%7zu] ", ordinal);
        if (w > 0 && (size_t)w < n - used) used += (size_t)w;
    }
    if (state->ts_mode != HOLD_TS_NONE && rec && used + 4 < n) {
        char ts[48];
        size_t tlen;
        if (state->ts_zone == 2) {
            /* Monotonic display: elapsed since the capture base, one Ctrl-U
             * family with local and UTC (playback spec). */
            uint64_t rel = rec->ts_us > state->idx_map.base_unix_us ? rec->ts_us - state->idx_map.base_unix_us : 0;
            int w = snprintf(ts, sizeof(ts), "+%02llu:%02llu:%02llu.%03llu",
                             (unsigned long long)(rel / 3600000000ULL),
                             (unsigned long long)(rel / 60000000ULL % 60ULL),
                             (unsigned long long)(rel / 1000000ULL % 60ULL),
                             (unsigned long long)(rel / 1000ULL % 1000ULL));
            tlen = w > 0 ? (size_t)w : 0;
        } else {
            tlen = hold_logidx_format_time(rec->ts_us, state->ts_mode, state->ts_zone == 1, ts, sizeof(ts));
        }
        while (tlen > 0 && ts[tlen - 1] == ' ') ts[--tlen] = '\0';
        if (tlen > 0) {
            int w = snprintf(out + used, n - used, "[%s] ", ts);
            if (w > 0 && (size_t)w < n - used) used += (size_t)w;
        }
    }
    if (state->source_column && used + 6 < n) {
        const char *label = "OUT | ";
        if (rec) {
            switch (hold_logidx_record_stream(rec->meta)) {
                case HOLD_LOG_STREAM_STDERR: label = "ERR | "; break;
                case HOLD_LOG_STREAM_STDIN:  label = "IN  | "; break;
                case HOLD_LOG_STREAM_PTY:    label = "PTY | "; break;
                default:                     label = "OUT | "; break;
            }
        }
        memcpy(out + used, label, 6);
        used += 6;
        out[used] = '\0';
    }
    return used;
}

/* Builds the sanitized display text (prefix + record text) for one visible row. */
static char *build_display_line(const struct viewer_state *state, size_t i, size_t *out_len) {
    char prefix[96];
    size_t plen = viewer_row_prefix(state, state->visible[i].offset, prefix, sizeof(prefix));
    const char *line = state->visible[i].line;
    size_t linelen = 0;
    for (const unsigned char *p = (const unsigned char *)line; *p && *p != '\n'; p++) linelen++;
    char *buf = malloc(plen + linelen + 1);
    if (!buf) return NULL;
    memcpy(buf, prefix, plen);
    size_t k = plen;
    for (const unsigned char *p = (const unsigned char *)line; *p && *p != '\n'; p++) {
        buf[k++] = (isprint(*p) || *p == '\t') ? (char)*p : ' ';
    }
    buf[k] = '\0';
    *out_len = k;
    return buf;
}

static int emit_display_row(const char *seg, size_t seglen, size_t width, bool selected, const char *sgr) {
    if (sgr && viewer_puts(sgr) != 0) return -1;
    if (selected && viewer_puts("\033[7m") != 0) return -1;
    if (seglen > width) seglen = width;
    if (viewer_write_all(STDOUT_FILENO, seg, seglen) != 0) return -1;
    if (viewer_puts("\033[K") != 0) return -1;
    if ((selected || sgr) && viewer_puts("\033[0m") != 0) return -1;
    return viewer_puts("\r\n");
}

/* Row tint by recorded stream, when the sidecar knows it: stderr reads red. */
static const char *viewer_row_sgr(const struct viewer_state *state, off_t offset) {
    if (!state->colors || !state->idx_loaded) return NULL;
    const struct hold_logidx_record *rec = hold_logidx_map_find(&state->idx_map, offset);
    if (rec && hold_logidx_record_stream(rec->meta) == HOLD_LOG_STREAM_STDERR) return "\033[31m";
    return NULL;
}

/*
 * Centered key reference, drawn in the body while help is open. Any key
 * dismisses it. A footer one-liner cannot hold the whole key map at 80
 * columns without truncating itself mid-word.
 */
static int render_help_overlay(const struct viewer_state *state, size_t body_rows) {
    static const char *browse_lines[] = {
        "Type        Filter as you type",
        "Backspace   Relax the filter",
        "Space       Exclude lines like the selected line",
        "Ctrl-R      Reset all filters",
        "Up/Down     Move the selection",
        "PgUp/PgDn   Page through matches",
        "Home/End    Oldest page / live tail",
        "Ctrl-G      Go to line number",
        "Ctrl-L      Line numbers",
        "Ctrl-T      Timestamps: off, time, date",
        "Ctrl-U      Local, UTC, or elapsed timestamps",
        "Ctrl-W      Wrap long lines",
        "Ctrl-Y      Source column",
        "Wheel       Scroll (spin faster, move faster)",
        "Esc         Quit",
    };
    /* Playback mode shows only the keys that exist in it: transport plus the
     * shared display toggles (one set of physics). */
    static const char *play_lines[] = {
        "Space       Stop / resume at 1x",
        ".           Fast-forward: 1, 2, 3, 4, 8, 16x",
        ",           Rewind, same ladder",
        "Home/End    Start / live edge",
        "Ctrl-L      Line numbers",
        "Ctrl-T      Timestamps: off, time, date",
        "Ctrl-U      Local, UTC, or elapsed timestamps",
        "Ctrl-W      Wrap long lines",
        "Ctrl-Y      Source column",
        "Esc         Back to browsing",
    };
    /* A terminal recording is not lines (playback spec): its help shows only
     * transport, timestamps, and seek — line controls do not exist there. */
    static const char *screen_lines[] = {
        "Space       Stop / resume at 1x",
        ".           Fast-forward: 1, 2, 3, 4, 8, 16x",
        ",           Rewind, same ladder",
        "Home/End    Start of time / live edge",
        "Ctrl-T      Timestamps: off, time, date",
        "Ctrl-U      UTC or local timestamps",
        "Esc         Leave",
    };
    const char *const *lines = state->screen_kind ? screen_lines
                               : state->play_mode ? play_lines : browse_lines;
    size_t n = state->screen_kind ? sizeof(screen_lines) / sizeof(screen_lines[0])
               : state->play_mode ? sizeof(play_lines) / sizeof(play_lines[0])
                                  : sizeof(browse_lines) / sizeof(browse_lines[0]);
    size_t width = state->cols ? state->cols : 80;
    size_t blockw = 0;
    for (size_t i = 0; i < n; i++) if (strlen(lines[i]) > blockw) blockw = strlen(lines[i]);
    size_t left = width > blockw ? (width - blockw) / 2 : 0;
    size_t top = body_rows > n ? (body_rows - n) / 2 : 0;
    char pad[256];
    if (left >= sizeof(pad)) left = sizeof(pad) - 1;
    memset(pad, ' ', left);
    pad[left] = '\0';
    size_t used = 0;
    for (; used < top; used++) if (viewer_puts("\033[K\r\n") != 0) return -1;
    for (size_t i = 0; i < n && used < body_rows; i++, used++) {
        if (viewer_puts(pad) != 0) return -1;
        if (viewer_put_capped(lines[i], width > left ? width - left : 0) != 0) return -1;
        if (viewer_puts("\033[K\r\n") != 0) return -1;
    }
    for (; used < body_rows; used++) if (viewer_puts("\033[K\r\n") != 0) return -1;
    return 0;
}

static int render_body_polished(struct viewer_state *state, size_t body_rows) {
    if (state->help_open) return render_help_overlay(state, body_rows);
    size_t width = state->cols ? state->cols : 80;
    size_t used = 0;
    /* While pinned to the live tail there is no cursor: the operator is
     * watching the stream, not pointing at a line. The selection appears
     * when a key first pulls the view into browsing. Playback likewise: the
     * head is the point of attention, not a selected row. */
    bool show_cursor = !(state->follow && state->at_live_edge) && !state->play_mode;
    for (size_t i = 0; i < state->visible_count && used < body_rows; i++) {
        size_t dlen = 0;
        char *disp = build_display_line(state, i, &dlen);
        if (!disp) return -1;
        bool sel = show_cursor && (i == state->selected);
        const char *sgr = viewer_row_sgr(state, state->visible[i].offset);
        int rc = 0;
        if (!state->wrap || dlen <= width) {
            rc = emit_display_row(disp, dlen, width, sel, sgr);
            used++;
        } else {
            for (size_t off = 0; off < dlen && used < body_rows && rc == 0; used++) {
                size_t seg = dlen - off > width ? width : dlen - off;
                rc = emit_display_row(disp + off, seg, width, sel, sgr);
                off += seg;
            }
        }
        free(disp);
        if (rc != 0) return -1;
    }
    for (; used < body_rows; used++) {
        if (viewer_puts("~\033[K\r\n") != 0) return -1;
    }
    return 0;
}

static void viewer_status_text(const struct viewer_state *state, char *out, size_t n) {
    /* Honesty rule (playback spec): synthetic/recovered timing is labeled in
     * the chrome, never presented as recorded truth. A pty-tagged log is
     * announced as a terminal recording, and its head timestamp is the
     * screen kind's Ctrl-T display (a screen has no per-line prefix rows). */
    const char *rebuilt = state->idx_loaded && (state->idx_map.synthetic || state->idx_map.recovered)
                              ? "timing reconstructed | "
                              : "";
    char screen[80];
    screen[0] = '\0';
    if (state->screen_kind) {
        char ts[48];
        ts[0] = '\0';
        if (state->ts_mode != HOLD_TS_NONE && state->idx_loaded && state->idx_map.count > 0) {
            size_t head = state->play_mode
                              ? (state->play_pos > 0 ? state->play_pos : 1)
                              : state->idx_map.count;
            if (head > state->idx_map.count) head = state->idx_map.count;
            hold_logidx_format_time(state->idx_map.records[head - 1].ts_us, state->ts_mode,
                                    state->ts_zone == 1, ts, sizeof(ts));
        }
        snprintf(screen, sizeof(screen), "%sterminal recording | ", ts);
    }
    if (state->play_mode) snprintf(out, n, "%s%s%s", screen, rebuilt, state->play_paused ? "REPLAY PAUSED" : "REPLAY");
    else if (state->follow && state->at_live_edge && state->proc_active) snprintf(out, n, "%s%sFOLLOWING ACTIVE", screen, rebuilt);
    else if (state->proc_active) snprintf(out, n, "%s%sVIEWING ACTIVE", screen, rebuilt);
    else if (state->has_exit_code) snprintf(out, n, "%s%sVIEWING EXITED (%d)", screen, rebuilt, state->exit_code);
    else snprintf(out, n, "%s%sVIEWING EXITED", screen, rebuilt);
}

static int render_header_polished(const struct viewer_state *state) {
    size_t width = state->cols ? state->cols : 80;
    char idbuf[ID_DISPLAY_HEX_LEN + 1];
    /* Names are the human handle; the short id already lives in the footer. */
    const char *name = state->context.name && *state->context.name
                           ? state->context.name
                           : (state->context.run_id && *state->context.run_id
                                  ? hold_run_id_display(state->context.run_id, idbuf)
                                  : viewer_run_label(state));
    char left[128];
    snprintf(left, sizeof(left), "hold logs: %s", name);
    char status[80];
    viewer_status_text(state, status, sizeof(status));
    size_t slen = strlen(status);
    char *bar = make_bar(width, left, status);
    if (!bar) return -1;
    int rc = viewer_puts("\033[?25l\033[H");
    if (rc == 0 && state->colors) rc = viewer_puts("\033[1m");
    if (rc == 0) rc = viewer_write_all(STDOUT_FILENO, bar, width);
    if (rc == 0 && state->colors) rc = viewer_puts("\033[0m");
    free(bar);
    if (rc != 0) return -1;
    /* Recolor the status word in place: green while following the live
     * tail, yellow for a nonzero exit. The bar text stays byte-identical. */
    if (state->colors && slen < width) {
        const char *sgr = NULL;
        if (state->follow && state->at_live_edge && state->proc_active) sgr = "\033[1;32m";
        else if (state->has_exit_code && state->exit_code != 0) sgr = "\033[1;33m";
        if (sgr) {
            char move[32];
            snprintf(move, sizeof(move), "\033[1;%zuH", width - slen + 1);
            if (viewer_puts(move) != 0 || viewer_puts(sgr) != 0) return -1;
            if (viewer_puts(status) != 0 || viewer_puts("\033[0m") != 0) return -1;
        }
    }
    if (viewer_puts("\033[K\r\n") != 0) return -1;
    /* The filter/jump row exists only while one is active; an empty chrome
     * line teaches nothing and costs a row of log. */
    if (state->jump_active) {
        if (viewer_puts("\033[7mgo to line: ") != 0) return -1;
        if (viewer_put_capped(state->jump_buf, width > 13 ? width - 13 : 0) != 0) return -1;
        if (viewer_puts("\033[0m\033[K\r\n") != 0) return -1;
    } else if (state->filter[0]) {
        if (viewer_puts("\033[7mfilter: ") != 0) return -1;
        if (viewer_put_capped(state->filter, width > 8 ? width - 8 : 0) != 0) return -1;
        if (viewer_puts("\033[0m\033[K\r\n") != 0) return -1;
    }
    return 0;
}

static int render_footer_polished(const struct viewer_state *state) {
    size_t width = state->cols ? state->cols : 80;
    /* Left: the short id. Right: the one discoverability hint. The screen
     * already shows whether timestamps, wrap, or the source column are on;
     * the footer does not narrate the visible. */
    char id[ID_DISPLAY_HEX_LEN + 1];
    const char *idtext = state->context.run_id && *state->context.run_id
                             ? hold_run_id_display(state->context.run_id, id)
                             : viewer_run_label(state);
    char leftbuf[ID_DISPLAY_HEX_LEN + 4];
    snprintf(leftbuf, sizeof(leftbuf), " %s", idtext);
    char *bar = make_bar(width, leftbuf, state->help_open ? "any key returns " : "Ctrl-H Help   Esc Quit ");
    if (!bar) return -1;
    int rc = viewer_puts("\033[7m");
    if (rc == 0) rc = viewer_write_all(STDOUT_FILENO, bar, width);
    if (rc == 0) rc = viewer_puts("\033[0m");
    free(bar);
    return rc;
}

/* Mode-change OSD (playback spec, verbatim): blank 7 cells of the top two
 * rows at the upper right and center the indicator there; it shows for 2 s,
 * then the next repaint restores the blanked characters. */
static int render_osd(const struct viewer_state *state) {
    if (!state->osd[0]) return 0;
    size_t width = state->cols ? state->cols : 80;
    if (width < VIEWER_OSD_CELLS || state->osd_glyphs > VIEWER_OSD_CELLS) return 0;
    size_t col = width - VIEWER_OSD_CELLS + 1;
    char buf[96];
    snprintf(buf, sizeof(buf), "\033[0m\033[1;%zuH%*s", col, VIEWER_OSD_CELLS, "");
    if (viewer_puts(buf) != 0) return -1;
    int lead = (int)((VIEWER_OSD_CELLS - state->osd_glyphs) / 2);
    int trail = (int)(VIEWER_OSD_CELLS - state->osd_glyphs) - lead;
    snprintf(buf, sizeof(buf), "\033[2;%zuH%*s%s%*s", col, lead, "", state->osd, trail, "");
    return viewer_puts(buf);
}

static int render_polished(struct viewer_state *state) {
    if (render_header_polished(state) != 0) return -1;
    if (render_body_polished(state, viewer_body_rows(state)) != 0) return -1;
    if (render_footer_polished(state) != 0) return -1;
    if (viewer_puts("\033[K\033[J") != 0) return -1;
    return render_osd(state);
}

/*
 * Legacy chrome for the --debug-stats regression harness. Its header string and
 * footer field layout are a frozen test contract; the product path is
 * render_polished. Keep this byte-stable.
 */
static int render_legacy(struct viewer_state *state) {
    char header[512];
    bool has_filters = state->filter[0] || state->example_count > 0;
    const char *partial = state->cache_scan_limited ? " | partial" : (state->cache_reached_eof ? " | EOF" : "");
    const char *newer = state->newer_available ? " | newer below" : "";
    const char *reset = has_filters ? " | Ctrl-R reset" : "";
    if (state->filter[0]) {
        snprintf(header, sizeof(header), "\033[?25l\033[Hfilter: %s%s%s%s%s\033[K\r\n", state->filter,
                 state->example_count ? " | excluding similar" : "", partial, newer, reset);
    } else {
        snprintf(header, sizeof(header), "\033[?25l\033[Hhold logs %s%s%s%s%s%s\033[K\r\n", viewer_run_label(state),
                 state->follow ? (state->at_live_edge ? "  ● live" : "  browsing") : (state->follow_exited ? "  exited" : ""),
                 state->example_count ? "  excluding similar" : "", partial, newer, reset);
    }
    if (viewer_puts(header) != 0) return -1;

    size_t body_rows = viewer_body_rows(state);
    for (size_t i = 0; i < body_rows; i++) {
        if (i < state->visible_count) {
            if (i == state->selected) viewer_puts("\033[7m");
            viewer_put_capped(state->visible[i].line, state->cols);
            viewer_puts("\033[K");
            if (i == state->selected) viewer_puts("\033[0m");
        } else {
            viewer_puts("~\033[K");
        }
        viewer_puts("\r\n");
    }

    char footer[512];
    snprintf(footer, sizeof(footer),
             "scan_gen=%zu offset=%lld prev=%lld next=%lld scanned=%zu bytes=%zu matches=%zu excludes=%zu%s%s | Ctrl-H help Ctrl-R reset\033[K",
             state->scan_generation, (long long)state->start_offset, (long long)state->prev_offset,
             (long long)state->next_offset, state->cache_lines_scanned, state->cache_bytes_read,
             state->cache_match_count, state->example_count,
             state->follow ? (state->at_live_edge ? " follow=tail" : " follow=browsing") : (state->follow_exited ? " follow=exited" : ""),
             state->newer_available ? " newer=yes" : "");
    if (viewer_puts(footer) != 0) return -1;
    return viewer_puts("\033[K\033[J");
}

/* One frame of a terminal recording: the reconstructed screen (replay from
 * the nearest clear up to the playback head, or to EOF at the live edge),
 * with the viewer's chrome drawn over the top and bottom rows. */
static int render_screen_frame(struct viewer_state *state) {
    off_t end;
    if (state->play_mode) {
        end = playback_edge_offset(state);
    } else {
        end = lseek(state->fd, 0, SEEK_END);
        if (end < 0) end = 0;
    }
    if (viewer_puts("\033[?25l\033[0m") != 0) return -1;
    if (hold_log_screen_repaint(state->fd, end, true, STDOUT_FILENO) < 0) return -1;
    if (viewer_puts("\033[0m") != 0) return -1;
    if (render_header_polished(state) != 0) return -1;
    char move[32];
    snprintf(move, sizeof(move), "\033[%zu;1H", state->rows ? state->rows : 24);
    if (viewer_puts(move) != 0) return -1;
    if (render_footer_polished(state) != 0) return -1;
    if (viewer_puts("\033[?25l") != 0) return -1;
    return render_osd(state);
}

static int render(struct viewer_state *state) {
    refresh_terminal_size(state);
    if (state->screen_kind) {
        /* Screen physics: no line cache, no filter machinery — the raw
         * bytes themselves repaint the frame. */
        viewer_reload_idx(state);
        if (!state->help_open) return render_screen_frame(state);
        if (render_header_polished(state) != 0) return -1;
        if (render_help_overlay(state, viewer_body_rows(state)) != 0) return -1;
        if (render_footer_polished(state) != 0) return -1;
        if (viewer_puts("\033[K\033[J") != 0) return -1;
        return render_osd(state);
    }
    if (!state->cache_valid && refill_cache(state) != 0) return -1;
    if (state->debug_stats) return render_legacy(state);
    return render_polished(state);
}

/* Follow tick for a terminal recording at the live edge: no filter probes,
 * just growth (repaint) and process exit. */
static int screen_follow_tick(struct viewer_state *state) {
    bool changed = false;
    off_t end = lseek(state->fd, 0, SEEK_END);
    if (end >= 0 && end > state->tail_anchor) {
        state->tail_anchor = end;
        if (state->at_live_edge) changed = true;
    }
    if (state->follow && state->is_running && !state->is_running(state->running_userdata)) {
        state->follow = false;
        state->follow_exited = true;
        state->proc_active = false;
        int code = 0;
        if (state->get_exit_code && state->get_exit_code(state->running_userdata, &code)) {
            state->exit_code = code;
            state->has_exit_code = true;
        }
        changed = true;
    }
    return changed ? 1 : 0;
}

/* Attached-console drain (docs/future/playback.md): the never-released
 * session's live fan-out is read and discarded — the same bytes are already
 * in the indexed log being rendered. EOF means the console ended while
 * time-traveling. Returns true when the frame must repaint. */
static bool viewer_drain_console(struct viewer_state *state) {
    if (!state->drain_enabled || state->drain_fd < 0) return false;
    char buf[4096];
    for (int guard = 256; guard > 0; guard--) {
        struct pollfd pfd = {.fd = state->drain_fd, .events = POLLIN, .revents = 0};
        int pr = poll(&pfd, 1, 0);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR))) return false;
        ssize_t n = read(state->drain_fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        state->drain_enabled = false;
        state->follow = false;
        state->follow_exited = true;
        state->proc_active = false;
        cache_invalidate(state);
        return true;
    }
    return false;
}

static void push_history(struct viewer_state *state, off_t off) {
    if (state->history_count < VIEWER_OFFSET_HISTORY_MAX) { state->history[state->history_count++] = off; return; }
    memmove(state->history, state->history + 1, sizeof(state->history[0]) * (VIEWER_OFFSET_HISTORY_MAX - 1));
    state->history[VIEWER_OFFSET_HISTORY_MAX - 1] = off;
}

static void page_down(struct viewer_state *state) {
    clear_local_scan_limit(state);
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        if (state->at_live_edge) return;
        if (state->visible_count == 0 || state->next_offset <= state->start_offset) return;
        /*
         * A manual PageDown after browsing older log content should advance to
         * the next real page, not replay the initial EOF anchor that was saved
         * when PageUp left the live tail.  Reusing that history entry is the
         * classic "snap back to bottom" loop: one PageUp followed by PageDown
         * jumps straight to the live edge instead of the adjacent newer page.
         */
        push_history(state, current_page_anchor(state));
        state->scan_mode = VIEWER_SCAN_FORWARD;
        state->start_offset = state->next_offset;
        state->at_live_edge = false;
        /* spec:190 — PageDown parks the selector on the bottom row; past any
         * refill count, cache_load_result clamps to the last row. */
        state->selected = viewer_body_rows(state);
        cache_invalidate(state);
        return;
    }
    if (state->visible_count == 0 || state->next_offset <= state->start_offset) return;
    /*
     * A forward scan that reached EOF is already showing the newest available
     * browsed page.  EOF itself is not a page.  Advancing to next_offset here
     * creates the apparent wrap/loop back to the bottom (or an empty tail
     * page) while the operator is trying to stay in manual navigation.
     *
     * The filter can stop exactly after filling the screen with the final real
     * line before it performs the read that would mark reached_eof.  Treat a
     * next anchor at-or-past the observed file end as EOF too, so repeated
     * PageDown stops on the last real page instead of moving to an empty
     * bottom page.
     *
     * When the call is still live, paging past the newest page has one honest
     * meaning left: return to the tail and keep following.
     */
    if (state->cache_reached_eof) {
        if (state->follow) jump_to_newest_page(state);
        return;
    }
    off_t end = state->follow ? state->tail_anchor : lseek(state->fd, 0, SEEK_END);
    if (end >= 0 && state->next_offset >= end) {
        if (state->follow) jump_to_newest_page(state);
        return;
    }
    push_history(state, state->start_offset);
    state->start_offset = state->next_offset;
    /* spec:190 — PageDown parks the selector on the bottom row (clamped). */
    state->selected = viewer_body_rows(state);
    cache_invalidate(state);
}

static void clear_local_scan_limit(struct viewer_state *state) {
    state->local_scan_limit_active = false;
    state->local_scan_limit_end = 0;
}

static void pin_to_oldest_visible_page(struct viewer_state *state) {
    state->at_live_edge = false;
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->history_count = 0;
    state->selected = 0;
    /*
     * The oldest page is a file boundary, not merely the first row currently
     * visible from the last backward scan.  Re-anchor at byte zero and refill
     * forward so repeated Up/PageUp at the top is idempotent and cannot reuse
     * any live-tail/history anchor that would feel like looping back down.
     * Literal/exclusion filters still start at byte zero; the filter engine
     * simply skips nonmatching lines until it finds the first visible match.
     */
    state->start_offset = 0;
    state->at_oldest_edge = true;
    clear_local_scan_limit(state);
    cache_invalidate(state);
}

static void enter_browsing_mode(struct viewer_state *state, bool stabilize_visible_page) {
    if (!state->follow || !state->at_live_edge) return;
    state->at_live_edge = false;
    state->newer_scan_offset = state->newer_floor_offset = state->tail_anchor;
    if (stabilize_visible_page && state->cache_valid && state->visible_count > 0) {
        state->start_offset = state->visible[0].offset;
        state->scan_mode = VIEWER_SCAN_FORWARD;
        state->history_count = 0;
        cache_invalidate(state);
    }
}

static void jump_to_newest_page(struct viewer_state *state) {
    clear_local_scan_limit(state);
    state->history_count = 0;
    state->selected = 0;
    state->newer_available = false;
    state->scan_mode = VIEWER_SCAN_BACKWARD;
    off_t end = lseek(state->fd, 0, SEEK_END);
    if (end < 0) end = state->tail_anchor;
    if (end < 0) end = 0;
    state->start_offset = state->tail_anchor = end;
    state->newer_scan_offset = state->newer_floor_offset = end;
    state->at_live_edge = state->follow;
    state->at_oldest_edge = false;
    cache_invalidate(state);
}

/* Jump to a 1-based record ordinal from the sidecar index. Filters stay
 * active: the page starts at the first visible match at or after that line. */
static void perform_jump(struct viewer_state *state) {
    state->jump_active = false;
    if (!state->jump_buf[0] || !state->idx_loaded || state->idx_map.count == 0) { state->jump_buf[0] = '\0'; return; }
    unsigned long n = strtoul(state->jump_buf, NULL, 10);
    state->jump_buf[0] = '\0';
    if (n < 1) n = 1;
    if (n > state->idx_map.count) n = state->idx_map.count;
    if (state->follow && state->at_live_edge) enter_browsing_mode(state, false);
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->start_offset = state->idx_map.records[n - 1].offset;
    state->at_live_edge = state->at_oldest_edge = false;
    state->history_count = 0;
    state->selected = 0;
    clear_local_scan_limit(state);
    cache_invalidate(state);
}

static void page_up(struct viewer_state *state) {
    clear_local_scan_limit(state);
    enter_browsing_mode(state, false);
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        state->at_live_edge = false;
        if (state->at_oldest_edge || state->visible_count == 0 ||
            state->prev_offset <= 0 || state->prev_offset >= state->start_offset) {
            pin_to_oldest_visible_page(state);
            return;
        }
        state->scan_mode = VIEWER_SCAN_BACKWARD;
        push_history(state, state->start_offset);
        state->start_offset = state->prev_offset;
        state->selected = 0;
        cache_invalidate(state);
        return;
    }
    if (state->history_count == 0) {
        if (state->start_offset != 0) { state->start_offset = 0; state->selected = 0; cache_invalidate(state); }
        return;
    }
    state->start_offset = state->history[--state->history_count];
    state->selected = 0;
    cache_invalidate(state);
}

/*
 * WO-3 (spec:192): an arrow at the screen edge scrolls the viewport one line
 * in that direction — never a page lurch. Both scrolls re-anchor the page one
 * record over and refill, so the ordinary refill/continuation machinery keeps
 * owning cache state. Only when the adjacent record is unknown (a sparse
 * filter whose next match sits beyond the probe budget) do they fall back to
 * the page operations, which already know how to hunt via idle-tick
 * continuation.
 */
static void scroll_line_down(struct viewer_state *state) {
    if (state->visible_count < 2) { page_down(state); return; }
    /* Rows below this page are still being discovered; let the ticks land. */
    if (continuation_pending(state)) return;
    if (state->cache_reached_eof) return; /* EOF is not a line */
    off_t end = state->follow ? state->tail_anchor : lseek(state->fd, 0, SEEK_END);
    if (end >= 0 && state->next_offset >= end) return;
    clear_local_scan_limit(state);
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->start_offset = state->visible[1].offset;
    state->at_live_edge = false;
    /* Past any refill count: cache_load_result clamps to the bottom row. */
    state->selected = viewer_body_rows(state);
    cache_invalidate(state);
}

static void scroll_line_up(struct viewer_state *state) {
    if (state->visible_count == 0) { page_up(state); return; }
    clear_local_scan_limit(state);
    enter_browsing_mode(state, false);
    if (state->at_oldest_edge || state->visible[0].offset <= 0) return;
    struct hold_log_filter_options opts;
    configure_probe_opts(state, &opts);
    struct hold_log_filter_result probe;
    if (hold_log_filter_backward_fd(state->fd, &opts, state->visible[0].offset, scan_budget_floor(state), &probe) != 0) return;
    bool found = probe.line_count > 0, limited = probe.scan_limited;
    off_t prev_record = found ? probe.line_offsets[0] : 0;
    hold_log_filter_result_free(&probe);
    if (!found) {
        if (limited) {
            page_up(state); /* sparse filter: the page refill + continuation own the hunt */
        } else {
            state->at_oldest_edge = true; /* whole span above scanned; nothing visible up there */
        }
        return;
    }
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->start_offset = prev_record;
    state->at_live_edge = false;
    state->selected = 0;
    cache_invalidate(state);
}

static void handle_key_down(struct viewer_state *state) {
    if (state->follow && state->at_live_edge) return; /* nothing below the tail */
    if (state->selected + 1 < state->visible_count) {
        enter_browsing_mode(state, true);
        state->selected++;
    } else {
        scroll_line_down(state);
    }
}

static void handle_key_up(struct viewer_state *state) {
    if (state->follow && state->at_live_edge && state->visible_count > 0) {
        /* Leaving the tail: the cursor appears on the bottom row, where the
         * operator was just looking. */
        enter_browsing_mode(state, true);
        state->selected = state->visible_count - 1;
    } else if (state->selected > 0) {
        enter_browsing_mode(state, true);
        state->selected--;
    } else {
        scroll_line_up(state);
    }
}

int hold_log_viewer_tty_fd(int fd, const char *title, const struct hold_log_filter_options *opts,
                             const struct hold_log_viewer_follow *follow,
                             const struct hold_log_viewer_context *context, bool debug_stats) {
    if (fd < 0 || !opts || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { errno = ENOTTY; return -1; }
    struct viewer_state state;
    memset(&state, 0, sizeof(state));
    state.fd = fd;
    state.title = title;
    state.debug_stats = debug_stats;
    state.follow = follow && follow->enabled;
    state.is_running = follow ? follow->is_running : NULL;
    state.get_exit_code = follow ? follow->exit_code : NULL;
    state.running_userdata = follow ? follow->userdata : NULL;
    state.drain_enabled = follow && follow->drain_enabled;
    state.drain_fd = state.drain_enabled ? follow->drain_fd : -1;
    if (context) {
        state.context = *context;
        state.proc_active = context->active;
        state.has_exit_code = context->has_exit_code;
        state.exit_code = context->exit_code;
    }
    state.base_opts = *opts;
    off_t current = lseek(fd, 0, SEEK_CUR);
    state.start_offset = current >= 0 ? current : 0;
    state.tail_anchor = state.start_offset;
    state.at_live_edge = state.follow;
    if (state.follow) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end >= 0) {
            state.start_offset = state.tail_anchor = end;
            state.newer_scan_offset = state.newer_floor_offset = end;
        }
        state.scan_mode = VIEWER_SCAN_BACKWARD;
    }
    if (opts->literal) snprintf(state.filter, sizeof(state.filter), "%s", opts->literal);
    const char *term = getenv("TERM");
    state.colors = !state.debug_stats && !getenv("NO_COLOR") && term && strcmp(term, "dumb") != 0;
    /* The sidecar decides the content kind before the first frame renders
     * (playback spec: the stream tag decides what the viewer is looking at). */
    viewer_reload_idx(&state);
    if (state.context.replay) {
        /* --replay: play from the start of recorded time. An empty or
         * unindexable log just opens the ordinary viewer. */
        if (state.idx_loaded && state.idx_map.count > 0) playback_enter(&state, 0, +1, false);
    }

    struct raw_terminal raw;
    if (raw_terminal_enter(&raw) != 0) return -1;
    int rc = 0;
    bool need_render = true;
    enum viewer_key pending = VIEWER_KEY_NONE;
    unsigned char pending_printable = 0;
    while (1) {
        if (need_render && render(&state) != 0) { rc = -1; break; }
        need_render = false;
        unsigned char printable = 0;
        enum viewer_key key;
        if (pending != VIEWER_KEY_NONE) {
            key = pending;
            printable = pending_printable;
            pending = VIEWER_KEY_NONE;
        } else {
            /* A pending continuation shortens the idle tick: scanning keeps
             * its throughput while any keypress still lands instantly. A due
             * playback step or OSD expiry shortens it further. An attached
             * console being drained keeps the tick short so its fan-out
             * buffer never backs the broker up. */
            int timeout = continuation_pending(&state) ? 10 : (state.drain_enabled ? 50 : 250);
            uint64_t now = viewer_now_ms();
            if (state.play_mode && !state.play_paused) {
                if (state.play_due_ms <= now) timeout = 0;
                else if (state.play_due_ms - now < (uint64_t)timeout) timeout = (int)(state.play_due_ms - now);
            }
            if (state.osd_until_ms > now && state.osd_until_ms - now < (uint64_t)timeout)
                timeout = (int)(state.osd_until_ms - now);
            key = read_key(&printable, timeout);
        }
        if (key == VIEWER_KEY_NONE) {
            if (refresh_terminal_size(&state)) need_render = true;
            if (viewer_drain_console(&state)) need_render = true;
            if (state.osd_until_ms && viewer_now_ms() >= state.osd_until_ms) {
                state.osd[0] = '\0';
                state.osd_until_ms = 0;
                need_render = true; /* restore the blanked corner */
            }
            if (state.play_mode) {
                if (playback_tick(&state)) need_render = true;
            } else if (state.follow) {
                if (state.screen_kind) {
                    if (screen_follow_tick(&state)) need_render = true;
                } else {
                    int tick = handle_follow_tick(&state);
                    if (tick < 0) { rc = -1; break; }
                    if (tick > 0) need_render = true;
                }
            }
            if (continuation_pending(&state)) {
                int cont = continue_scan_tick(&state);
                if (cont < 0) { rc = -1; break; }
                if (cont > 0) need_render = true;
            }
            continue;
        }
        if (state.help_open) { state.help_open = false; need_render = true; continue; }
        if (state.jump_active) {
            /* Modal line-number entry: digits build the target, Enter jumps,
             * Esc or Ctrl-G cancels. Anything else cancels and falls through. */
            bool consumed = true;
            size_t n = strlen(state.jump_buf);
            if (key == VIEWER_KEY_PRINTABLE && printable >= '0' && printable <= '9') {
                if (n + 1 < sizeof(state.jump_buf)) { state.jump_buf[n] = (char)printable; state.jump_buf[n + 1] = '\0'; }
            } else if (key == VIEWER_KEY_BACKSPACE) {
                if (n > 0) state.jump_buf[n - 1] = '\0';
            } else if (key == VIEWER_KEY_DOWN) { /* Enter */
                perform_jump(&state);
            } else {
                state.jump_active = false;
                state.jump_buf[0] = '\0';
                consumed = key == VIEWER_KEY_QUIT || key == VIEWER_KEY_JUMP;
            }
            if (consumed) { need_render = true; continue; }
        }
        if (key == VIEWER_KEY_CTRL_P) {
            /* Entered from an attached console: a second Ctrl-P double-tap
             * returns to it (the transparent jump, both directions). A lone
             * Ctrl-P is nothing — the key is otherwise unassigned. */
            uint64_t now = viewer_now_ms();
            bool doubled = state.context.attached && state.ctrl_p_last_ms != 0 &&
                           now - state.ctrl_p_last_ms <= 500;
            state.ctrl_p_last_ms = now;
            if (doubled) break;
            continue;
        }
        state.ctrl_p_last_ms = 0;
        if (state.screen_kind) {
            /* Screen physics only (playback spec: two content kinds, two
             * control sets) — transport, timestamps, seek, help, quit. Line
             * controls do not exist on a screen. */
            if (key == VIEWER_KEY_PRINTABLE && printable == 'q') key = VIEWER_KEY_QUIT;
            if (key != VIEWER_KEY_QUIT && key != VIEWER_KEY_SUSPEND) {
                switch (key) {
                case VIEWER_KEY_TOGGLE: transport_space(&state); need_render = true; break;
                case VIEWER_KEY_PRINTABLE:
                    if (printable == '.') { transport_rate(&state, +1); need_render = true; }
                    else if (printable == ',') { transport_rate(&state, -1); need_render = true; }
                    break;
                case VIEWER_KEY_TOP:
                    /* Seek to the start of time, paused at the first frame. */
                    viewer_reload_idx(&state);
                    if (state.idx_loaded && state.idx_map.count > 0) {
                        if (!state.play_mode) {
                            playback_enter(&state, 0, +1, true);
                        } else {
                            state.play_pos = 0;
                            state.play_paused = true;
                            state.play_due_ms = viewer_now_ms();
                            playback_osd_show(&state);
                            playback_anchor(&state);
                        }
                        need_render = true;
                    }
                    break;
                case VIEWER_KEY_BOTTOM:
                    /* Scrub to the live edge; a followed console resumes. */
                    if (state.play_mode) {
                        if (state.follow) {
                            playback_leave(&state, true);
                        } else {
                            viewer_reload_idx(&state);
                            state.play_pos = state.idx_map.count;
                            state.play_paused = true;
                            playback_osd_show(&state);
                            playback_anchor(&state);
                        }
                        need_render = true;
                    }
                    break;
                case VIEWER_KEY_TS_CYCLE:
                    state.ts_mode = state.ts_mode == HOLD_TS_NONE ? HOLD_TS_TIME
                                  : state.ts_mode == HOLD_TS_TIME ? HOLD_TS_DATE
                                  : HOLD_TS_NONE;
                    need_render = true;
                    break;
                case VIEWER_KEY_TZ_TOGGLE: state.ts_zone = (state.ts_zone + 1) % 3; need_render = true; break;
                case VIEWER_KEY_HELP: state.help_open = true; need_render = true; break;
                default:
                    break;
                }
                continue;
            }
        }
        if (state.play_mode && !state.screen_kind) {
            /* Playback-mode keys: transport plus the shared display toggles.
             * Filtering and browse navigation are browse-mode keys; Esc/q
             * leave playback into browsing (playback spec key scoping). */
            switch (key) {
            case VIEWER_KEY_QUIT: playback_leave(&state, false); need_render = true; continue;
            case VIEWER_KEY_TOGGLE: transport_space(&state); need_render = true; continue;
            case VIEWER_KEY_PRINTABLE:
                if (printable == '.') { transport_rate(&state, +1); need_render = true; }
                else if (printable == ',') { transport_rate(&state, -1); need_render = true; }
                else if (printable == 'q') { playback_leave(&state, false); need_render = true; }
                continue;
            case VIEWER_KEY_TOP:
                state.play_pos = 0;
                state.play_due_ms = viewer_now_ms();
                playback_anchor(&state);
                need_render = true;
                continue;
            case VIEWER_KEY_BOTTOM:
                /* Scrub to the live edge: a followed log resumes the tail. */
                if (state.follow) {
                    playback_leave(&state, true);
                } else {
                    viewer_reload_idx(&state);
                    state.play_pos = state.idx_map.count;
                    state.play_paused = true;
                    playback_osd_show(&state);
                    playback_anchor(&state);
                }
                need_render = true;
                continue;
            case VIEWER_KEY_UP: case VIEWER_KEY_DOWN:
            case VIEWER_KEY_PAGE_UP: case VIEWER_KEY_PAGE_DOWN:
            case VIEWER_KEY_WHEEL_UP: case VIEWER_KEY_WHEEL_DOWN:
            case VIEWER_KEY_BACKSPACE: case VIEWER_KEY_RESET: case VIEWER_KEY_JUMP:
                continue;
            default:
                break;
            }
        } else if (state.follow && state.at_live_edge &&
                   (key == VIEWER_KEY_TOGGLE ||
                    (key == VIEWER_KEY_PRINTABLE && (printable == '.' || printable == ',')))) {
            /* Transport is live while tailing (playback spec): Space freezes
             * the recorded edge, `,` starts rewinding through the history. */
            if (key == VIEWER_KEY_TOGGLE) transport_space(&state);
            else transport_rate(&state, printable == '.' ? +1 : -1);
            need_render = true;
            continue;
        }
        if (key == VIEWER_KEY_QUIT) {
            int quit_tick = mark_newer_before_quit(&state);
            if (quit_tick < 0) { rc = -1; break; }
            if (quit_tick > 0 && render(&state) != 0) rc = -1;
            break;
        }
        switch (key) {
        case VIEWER_KEY_DOWN: handle_key_down(&state); need_render = true; break;
        case VIEWER_KEY_UP: handle_key_up(&state); need_render = true; break;
        case VIEWER_KEY_WHEEL_UP:
        case VIEWER_KEY_WHEEL_DOWN: {
            /* Wheel scrolling is naturally speed-sensitive: a fast spin
             * queues many notches, so drain the identical ones and apply
             * them in one repaint (three rows per notch). */
            int steps = 3;
            while (byte_ready(0)) {
                unsigned char p2 = 0;
                enum viewer_key k2 = read_key(&p2, 0);
                if (k2 == key) { steps += 3; continue; }
                if (k2 != VIEWER_KEY_NONE) { pending = k2; pending_printable = p2; }
                break;
            }
            for (int s = 0; s < steps; s++) {
                if (key == VIEWER_KEY_WHEEL_UP) handle_key_up(&state);
                else handle_key_down(&state);
            }
            need_render = true;
            break;
        }
        case VIEWER_KEY_PAGE_DOWN: page_down(&state); need_render = true; break;
        case VIEWER_KEY_PAGE_UP: page_up(&state); need_render = true; break;
        case VIEWER_KEY_TOP: pin_to_oldest_visible_page(&state); need_render = true; break;
        case VIEWER_KEY_BOTTOM: jump_to_newest_page(&state); need_render = true; break;
        case VIEWER_KEY_BACKSPACE: {
            size_t n = strlen(state.filter);
            if (n > 0) { state.filter[n - 1] = '\0'; reset_filter_navigation(&state); need_render = true; }
            break;
        }
        case VIEWER_KEY_PRINTABLE: {
            size_t n = strlen(state.filter);
            if (n < VIEWER_FILTER_MAX) {
                state.filter[n] = (char)printable;
                state.filter[n + 1] = '\0';
                reset_filter_navigation(&state);
                need_render = true;
            }
            break;
        }
        case VIEWER_KEY_TOGGLE: {
            /* Browse-mode Space stays zap-exclude (playback spec key scoping;
             * at the live edge Space is transport, handled above). */
            const char *line = state.selected < state.visible_count ? state.visible[state.selected].line : NULL;
            if (toggle_example(&state, line) != 0) rc = -1;
            need_render = true;
            break;
        }
        case VIEWER_KEY_SUSPEND:
            /* Honest Ctrl-Z: restore the terminal, stop like any job, and
             * repaint when the shell resumes us. */
            raw_terminal_leave(&raw);
            kill(0, SIGTSTP);
            if (raw_terminal_enter(&raw) != 0) { rc = -1; break; }
            cache_invalidate(&state);
            need_render = true;
            break;
        case VIEWER_KEY_HELP: state.help_open = true; need_render = true; break;
        case VIEWER_KEY_TS_CYCLE:
            state.ts_mode = state.ts_mode == HOLD_TS_NONE ? HOLD_TS_TIME
                          : state.ts_mode == HOLD_TS_TIME ? HOLD_TS_DATE
                          : HOLD_TS_NONE;
            need_render = true;
            break;
        case VIEWER_KEY_TZ_TOGGLE: state.ts_zone = (state.ts_zone + 1) % 3; need_render = true; break;
        case VIEWER_KEY_WRAP_TOGGLE: state.wrap = !state.wrap; need_render = true; break;
        case VIEWER_KEY_SOURCE_COL: state.source_column = !state.source_column; need_render = true; break;
        case VIEWER_KEY_LINE_NUMBERS: state.line_numbers = !state.line_numbers; need_render = true; break;
        case VIEWER_KEY_JUMP:
            if (state.idx_loaded && state.idx_map.count > 0) { state.jump_active = true; state.jump_buf[0] = '\0'; need_render = true; }
            break;
        case VIEWER_KEY_RESET:
            if (state.filter[0] || state.example_count > 0) {
                state.filter[0] = '\0';
                free_examples(&state);
                reset_filter_navigation(&state);
                need_render = true;
            }
            break;
        default:
            break;
        }
        if (rc != 0) break;
    }
    if (rc == 0) {
        int final_tick = mark_newer_before_quit(&state);
        if (final_tick < 0) rc = -1;
        else if (final_tick > 0 && render(&state) != 0) rc = -1;
    }
    raw_terminal_leave(&raw);
    free_examples(&state);
    cache_free(&state);
    if (state.idx_loaded) hold_logidx_map_free(&state.idx_map);
    return rc;
}
