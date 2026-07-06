#include "hold/config.h"
#include "hold/core.h"
#include "hold/log_viewer.h"

#define VIEWER_FILTER_MAX 255
#define VIEWER_OFFSET_HISTORY_MAX 1024
#define VIEWER_SCAN_BYTES_PER_ROW (64u * 1024u)

enum viewer_scan_mode {
    VIEWER_SCAN_FORWARD = 0,
    VIEWER_SCAN_BACKWARD
};

enum viewer_key {
    VIEWER_KEY_NONE = 0,
    VIEWER_KEY_QUIT,
    VIEWER_KEY_SUSPEND,
    VIEWER_KEY_UP,
    VIEWER_KEY_DOWN,
    VIEWER_KEY_PAGE_UP,
    VIEWER_KEY_PAGE_DOWN,
    VIEWER_KEY_TOP,
    VIEWER_KEY_BOTTOM,
    VIEWER_KEY_BACKSPACE,
    VIEWER_KEY_TOGGLE,
    VIEWER_KEY_HELP,
    VIEWER_KEY_RESET,
    VIEWER_KEY_TS_CYCLE,
    VIEWER_KEY_TZ_TOGGLE,
    VIEWER_KEY_WRAP_TOGGLE,
    VIEWER_KEY_SOURCE_COL,
    VIEWER_KEY_LINE_NUMBERS,
    VIEWER_KEY_JUMP,
    VIEWER_KEY_WHEEL_UP,
    VIEWER_KEY_WHEEL_DOWN,
    VIEWER_KEY_PRINTABLE
};

struct viewer_row {
    char *line;
    off_t offset;
};

struct viewer_state {
    int fd;
    const char *title;
    bool debug_stats;
    bool colors;
    bool follow;
    bool follow_exited;
    hold_log_viewer_running_fn is_running;
    hold_log_viewer_exit_code_fn get_exit_code;
    void *running_userdata;
    struct hold_log_viewer_context context;
    struct hold_log_filter_options base_opts;
    char filter[VIEWER_FILTER_MAX + 1];
    char *examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t example_count;
    bool help_open;
    bool line_numbers;
    bool jump_active;
    char jump_buf[12];
    /* Presentation-only view controls (never change stored records or filtering). */
    enum hold_ts_mode ts_mode;
    bool ts_utc;
    bool wrap;
    bool source_column;
    unsigned source_mask; /* 0 == all sources visible */
    struct hold_logidx_map idx_map;
    bool idx_loaded;
    off_t idx_loaded_raw_size;
    bool proc_active;
    bool has_exit_code;
    int exit_code;
    off_t start_offset;
    off_t history[VIEWER_OFFSET_HISTORY_MAX];
    size_t history_count;
    enum viewer_scan_mode scan_mode;
    bool cache_valid;
    bool cache_scan_limited;
    bool cache_reached_eof;
    bool at_live_edge;
    bool at_oldest_edge;
    bool newer_available;
    struct viewer_row *visible;
    size_t visible_count;
    size_t visible_capacity;
    off_t prev_offset;
    off_t next_offset;
    off_t tail_anchor;
    off_t newer_scan_offset;
    off_t newer_floor_offset;
    bool local_scan_limit_active;
    off_t local_scan_limit_end;
    size_t cache_bytes_read;
    size_t cache_lines_scanned;
    size_t cache_match_count;
    size_t scan_generation;
    size_t selected;
    size_t rows;
    size_t cols;
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
 */

struct raw_terminal {
    struct termios original;
    bool active;
};

static int viewer_write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static int viewer_puts(const char *s) {
    return viewer_write_all(STDOUT_FILENO, s, strlen(s));
}

static int terminal_size(size_t *rows, size_t *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
    *rows = 24;
    *cols = 80;
    return 0;
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
    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
        .revents = 0,
    };
    int ready;
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

static enum viewer_key read_key(unsigned char *printable, int timeout_ms) {
    unsigned char c = 0;
    *printable = 0;
    if (timeout_ms >= 0 && !byte_ready(timeout_ms)) return VIEWER_KEY_NONE;
    if (read_byte(&c) != 0) return VIEWER_KEY_QUIT;
    /* Every printable byte belongs to the filter, so the only quit keys are
     * ones nobody can type into a filter term: Esc (below), Ctrl-C, and
     * Ctrl-D — the pty EOF byte, which is also what `script` and friends
     * send when their input ends. */
    if (c == 3 || c == 4) return VIEWER_KEY_QUIT;
    if (c == 26) return VIEWER_KEY_SUSPEND;     /* Ctrl-Z */
    if (c == 7) return VIEWER_KEY_JUMP;         /* Ctrl-G: go to line */
    if (c == 8) return VIEWER_KEY_HELP;
    if (c == 12) return VIEWER_KEY_LINE_NUMBERS; /* Ctrl-L */
    if (c == 18) return VIEWER_KEY_RESET;
    if (c == 20) return VIEWER_KEY_TS_CYCLE;    /* Ctrl-T */
    if (c == 21) return VIEWER_KEY_TZ_TOGGLE;   /* Ctrl-U */
    if (c == 23) return VIEWER_KEY_WRAP_TOGGLE; /* Ctrl-W */
    if (c == 25) return VIEWER_KEY_SOURCE_COL;  /* Ctrl-Y */
    if (c == ' ' || c == '\r' || c == '\n') return c == ' ' ? VIEWER_KEY_TOGGLE : VIEWER_KEY_DOWN;
    if (c == 127) return VIEWER_KEY_BACKSPACE;
    if (c == 6) return VIEWER_KEY_PAGE_DOWN;
    if (c == 2) return VIEWER_KEY_PAGE_UP;
    if (c == 27) {
        /* A lone Esc is the quit key; an Esc with bytes right behind it is
         * the start of an arrow/page/home/end sequence. */
        if (!byte_ready(50)) return VIEWER_KEY_QUIT;
        unsigned char a = 0;
        if (read_byte(&a) != 0) return VIEWER_KEY_QUIT;
        if (a == 'O') {
            unsigned char b = 0;
            if (read_byte(&b) != 0) return VIEWER_KEY_QUIT;
            if (b == 'A') return VIEWER_KEY_UP;
            if (b == 'B') return VIEWER_KEY_DOWN;
            if (b == 'H') return VIEWER_KEY_TOP;
            if (b == 'F') return VIEWER_KEY_BOTTOM;
            return VIEWER_KEY_NONE;
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
        if (final == 'A') return VIEWER_KEY_UP;
        if (final == 'B') return VIEWER_KEY_DOWN;
        if (final == 'H') return VIEWER_KEY_TOP;
        if (final == 'F') return VIEWER_KEY_BOTTOM;
        if (final == '~') {
            long param = 0;
            bool have_param = false;
            for (size_t i = 0; i + 1 < n; i++) {
                if (isdigit(seq[i])) {
                    have_param = true;
                    param = param * 10 + (long)(seq[i] - '0');
                    continue;
                }
                break;
            }
            if (have_param) {
                if (param == 5) return VIEWER_KEY_PAGE_UP;
                if (param == 6) return VIEWER_KEY_PAGE_DOWN;
                if (param == 1 || param == 7) return VIEWER_KEY_TOP;
                if (param == 4 || param == 8) return VIEWER_KEY_BOTTOM;
            }
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
    if (state->visible) {
        for (size_t i = 0; i < state->visible_count; i++) free(state->visible[i].line);
    }
    state->visible_count = 0;
    state->prev_offset = state->start_offset;
    state->next_offset = state->start_offset;
    state->cache_scan_limited = false;
    state->cache_reached_eof = false;
    state->cache_bytes_read = 0;
    state->cache_lines_scanned = 0;
    state->cache_match_count = 0;
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

static int cache_load_result(struct viewer_state *state, const struct hold_log_filter_result *result, size_t cap) {
    if (cache_ensure_capacity(state, cap ? cap : 1) != 0) return -1;
    cache_clear(state);
    for (size_t i = 0; i < result->line_count; i++) {
        char *copy = strdup(result->lines[i]);
        if (!copy) {
            cache_clear(state);
            return -1;
        }
        state->visible[state->visible_count].line = copy;
        state->visible[state->visible_count].offset = result->line_offsets[i];
        state->visible_count++;
    }
    state->prev_offset = result->prev_offset;
    state->next_offset = result->next_offset;
    state->cache_scan_limited = result->scan_limited;
    state->cache_reached_eof = result->reached_eof;
    state->cache_bytes_read = result->bytes_read;
    state->cache_lines_scanned = result->lines_scanned;
    state->cache_match_count = result->match_count;
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        state->at_oldest_edge = !result->scan_limited && result->match_count <= result->line_count;
    } else {
        state->at_oldest_edge = state->start_offset == 0;
    }
    state->cache_valid = true;
    state->scan_generation++;
    if (state->selected >= state->visible_count && state->visible_count > 0) state->selected = state->visible_count - 1;
    if (state->visible_count == 0) state->selected = 0;
    return 0;
}

static void cache_invalidate(struct viewer_state *state) {
    state->cache_valid = false;
}

static void configure_filter_opts(const struct viewer_state *state, struct hold_log_filter_options *opts);
static void enter_browsing_mode(struct viewer_state *state, bool stabilize_visible_page);
static void clear_local_scan_limit(struct viewer_state *state);
static void jump_to_newest_page(struct viewer_state *state);

static bool refresh_terminal_size(struct viewer_state *state) {
    size_t old_rows = state->rows, old_cols = state->cols;
    terminal_size(&state->rows, &state->cols);
    if (old_rows && (old_rows != state->rows || old_cols != state->cols)) {
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
    if (state->follow) {
        off_t end = lseek(state->fd, 0, SEEK_END);
        if (end >= 0) state->tail_anchor = end;
        if (preserve_browsed_page) {
            if (browsed_anchor < 0) browsed_anchor = 0;
            state->start_offset = browsed_anchor;
            state->newer_scan_offset = state->tail_anchor;
            state->newer_floor_offset = state->tail_anchor;
            state->scan_mode = VIEWER_SCAN_FORWARD;
            state->at_live_edge = false;
            if (state->next_offset > browsed_anchor) {
                state->local_scan_limit_active = true;
                state->local_scan_limit_end = state->next_offset;
            }
        } else {
            if (end >= 0) state->start_offset = end;
            state->newer_scan_offset = state->tail_anchor;
            state->newer_floor_offset = state->tail_anchor;
            state->scan_mode = VIEWER_SCAN_BACKWARD;
            state->at_live_edge = true;
            state->local_scan_limit_active = false;
            state->local_scan_limit_end = 0;
        }
        state->newer_available = false;
    } else {
        state->start_offset = 0;
        state->tail_anchor = 0;
        state->newer_scan_offset = 0;
        state->newer_floor_offset = 0;
        state->scan_mode = VIEWER_SCAN_FORWARD;
        state->at_live_edge = false;
        state->newer_available = false;
        state->local_scan_limit_active = false;
        state->local_scan_limit_end = 0;
    }
    state->history_count = 0;
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

static int appended_range_has_match(struct viewer_state *state, off_t start, off_t end, bool *has_match, off_t *scanned_to) {
    *has_match = false;
    *scanned_to = start;
    if (end <= start) return 0;
    if (lseek(state->fd, start, SEEK_SET) < 0) return -1;

    struct hold_log_filter_options opts;
    configure_filter_opts(state, &opts);
    opts.visible_capacity = 1;
    opts.max_results = 1;
    opts.match_ring_capacity = 1;

    size_t visible_rows = viewer_body_rows(state);
    opts.scan_byte_budget = visible_rows * VIEWER_SCAN_BYTES_PER_ROW;
    if (opts.scan_byte_budget < 1024u * 1024u) opts.scan_byte_budget = 1024u * 1024u;
    off_t appended_bytes = end - start;
    if (appended_bytes > 0 && (uintmax_t)appended_bytes < (uintmax_t)opts.scan_byte_budget) {
        opts.scan_byte_budget = (size_t)appended_bytes;
    }

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
            state->start_offset = end;
            state->newer_scan_offset = end;
            state->newer_floor_offset = end;
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
        if (state->get_exit_code) {
            int code = 0;
            if (state->get_exit_code(state->running_userdata, &code)) {
                state->exit_code = code;
                state->has_exit_code = true;
            }
        }
        cache_invalidate(state);
        changed = true;
    }
    return changed ? 1 : 0;
}

static int mark_newer_before_quit(struct viewer_state *state) {
    if (!state->follow || state->at_live_edge || state->newer_available) return 0;
    off_t end = lseek(state->fd, 0, SEEK_END);
    if (end < 0) return -1;
    if (end > state->tail_anchor) state->tail_anchor = end;
    if (state->newer_scan_offset >= state->tail_anchor) {
        struct hold_log_filter_options opts;
        configure_filter_opts(state, &opts);
        opts.visible_capacity = 1;
        opts.max_results = 1;
        opts.match_ring_capacity = 1;
        struct hold_log_filter_result tail_result;
        if (hold_log_filter_backward_fd(state->fd, &opts, state->tail_anchor, 1024u * 1024u, &tail_result) != 0) return -1;
        bool has_tail_match = tail_result.line_count > 0 && tail_result.line_offsets[0] >= state->newer_floor_offset;
        hold_log_filter_result_free(&tail_result);
        if (has_tail_match) {
            state->newer_available = true;
            return 1;
        }
        return 0;
    }
    off_t scanned_to = state->newer_scan_offset;
    bool has_match = false;
    if (appended_range_has_match(state, state->newer_scan_offset, state->tail_anchor, &has_match, &scanned_to) != 0) return -1;
    if (scanned_to > state->newer_scan_offset) state->newer_scan_offset = scanned_to;
    if (has_match) {
        state->newer_available = true;
        return 1;
    }
    return 0;
}

static bool same_line(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int toggle_example(struct viewer_state *state, const char *line) {
    if (!line) return 0;
    /*
     * Space is an editing operation on the line the operator is currently
     * looking at.  If the viewer is still technically at the live edge, first
     * pin the rendered page so applying the exclusion does not immediately
     * re-anchor at EOF and look like it "looped" back to the bottom.
     */
    enter_browsing_mode(state, true);
    for (size_t i = 0; i < state->example_count; i++) {
        if (same_line(state->examples[i], line)) {
            free(state->examples[i]);
            for (size_t j = i + 1; j < state->example_count; j++) state->examples[j - 1] = state->examples[j];
            state->example_count--;
            state->examples[state->example_count] = NULL;
            reset_filter_navigation(state);
            return 0;
        }
    }
    if (state->example_count >= HOLD_LOG_VIEWER_MAX_EXAMPLES) return 0;
    char *copy = strdup(line);
    if (!copy) return -1;
    state->examples[state->example_count++] = copy;
    reset_filter_navigation(state);
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
}

static void write_sanitized_line(const char *line, size_t width) {
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)line; *p && *p != '\n' && used < width; p++) {
        char out = (isprint(*p) || *p == '\t') ? (char)*p : ' ';
        viewer_write_all(STDOUT_FILENO, &out, 1);
        used++;
    }
    viewer_puts("\033[K");
}

static int refill_cache(struct viewer_state *state) {
    viewer_reload_idx(state);
    struct hold_log_filter_options opts;
    configure_filter_opts(state, &opts);
    size_t visible_rows = viewer_body_rows(state);
    size_t scan_budget = visible_rows * VIEWER_SCAN_BYTES_PER_ROW;
    struct hold_log_filter_result result;
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        if (state->follow && state->at_live_edge) {
            off_t end = lseek(state->fd, 0, SEEK_END);
            if (end >= 0) {
                state->start_offset = end;
                state->tail_anchor = end;
                state->newer_scan_offset = end;
                state->newer_floor_offset = end;
            }
        }
        if (hold_log_filter_backward_fd(state->fd, &opts, state->start_offset, scan_budget, &result) != 0) return -1;
        bool oldest_backward_page = !result.scan_limited && result.match_count <= result.line_count;
        if (state->start_offset > 0 && !result.scan_limited && result.line_count == 0) {
            state->start_offset = 0;
            state->scan_mode = VIEWER_SCAN_FORWARD;
            state->history_count = 0;
            state->at_live_edge = false;
            hold_log_filter_result_free(&result);
            memset(&result, 0, sizeof(result));
            opts.scan_byte_budget = scan_budget;
            if (lseek(state->fd, 0, SEEK_SET) < 0) return -1;
            if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
        } else if (!state->at_live_edge && oldest_backward_page && result.line_count > 0) {
            off_t oldest_visible = result.line_offsets[0];
            state->start_offset = oldest_visible;
            state->scan_mode = VIEWER_SCAN_FORWARD;
            state->history_count = 0;
            state->at_live_edge = false;
            hold_log_filter_result_free(&result);
            memset(&result, 0, sizeof(result));
            opts.scan_byte_budget = scan_budget;
            if (lseek(state->fd, state->start_offset, SEEK_SET) < 0) return -1;
            if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
        }
    } else {
        opts.scan_byte_budget = scan_budget;
        if (state->local_scan_limit_active && state->local_scan_limit_end > state->start_offset) {
            off_t local_budget = state->local_scan_limit_end - state->start_offset;
            if (local_budget > 0 && (uintmax_t)local_budget < (uintmax_t)opts.scan_byte_budget) {
                opts.scan_byte_budget = (size_t)local_budget;
            }
        }
        if (lseek(state->fd, state->start_offset, SEEK_SET) < 0) return -1;
        if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
    }
    clear_local_scan_limit(state);
    int rc = cache_load_result(state, &result, visible_rows);
    hold_log_filter_result_free(&result);
    return rc;
}

static const char *viewer_run_label(const struct viewer_state *state) {
    if (state->context.run_id && *state->context.run_id) return state->context.run_id;
    return state->title && *state->title ? state->title : "-";
}

static void put_bar_text(char *bar, size_t width, size_t pos, const char *text) {
    if (!bar || !text || pos >= width) return;
    size_t room = width - pos;
    size_t n = strlen(text);
    if (n <= room) {
        memcpy(bar + pos, text, n);
        return;
    }
    if (room >= 4) {
        memcpy(bar + pos, text, room - 3);
        memcpy(bar + pos + room - 3, "...", 3);
    } else {
        memcpy(bar + pos, text, room);
    }
}

static int viewer_put_capped(const char *s, size_t max) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < max; p++, n++) {
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
        size_t tlen = hold_logidx_format_time(rec->ts_us, state->ts_mode, state->ts_utc, ts, sizeof(ts));
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
    static const char *lines[] = {
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
        "Ctrl-U      UTC or local timestamps",
        "Ctrl-W      Wrap long lines",
        "Ctrl-Y      Source column",
        "Wheel       Scroll (spin faster, move faster)",
        "Esc         Quit",
    };
    size_t n = sizeof(lines) / sizeof(lines[0]);
    size_t width = state->cols ? state->cols : 80;
    size_t blockw = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(lines[i]);
        if (len > blockw) blockw = len;
    }
    size_t left = width > blockw ? (width - blockw) / 2 : 0;
    size_t top = body_rows > n ? (body_rows - n) / 2 : 0;
    char pad[256];
    if (left >= sizeof(pad)) left = sizeof(pad) - 1;
    memset(pad, ' ', left);
    pad[left] = '\0';
    size_t used = 0;
    for (; used < top; used++) {
        if (viewer_puts("\033[K\r\n") != 0) return -1;
    }
    for (size_t i = 0; i < n && used < body_rows; i++, used++) {
        if (viewer_puts(pad) != 0) return -1;
        if (viewer_put_capped(lines[i], width > left ? width - left : 0) != 0) return -1;
        if (viewer_puts("\033[K\r\n") != 0) return -1;
    }
    for (; used < body_rows; used++) {
        if (viewer_puts("\033[K\r\n") != 0) return -1;
    }
    return 0;
}

static int render_body_polished(struct viewer_state *state, size_t body_rows) {
    if (state->help_open) return render_help_overlay(state, body_rows);
    size_t width = state->cols ? state->cols : 80;
    size_t used = 0;
    /* While pinned to the live tail there is no cursor: the operator is
     * watching the stream, not pointing at a line. The selection appears
     * when a key first pulls the view into browsing. */
    bool show_cursor = !(state->follow && state->at_live_edge);
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
            size_t off = 0;
            while (off < dlen && used < body_rows) {
                size_t seg = dlen - off;
                if (seg > width) seg = width;
                rc = emit_display_row(disp + off, seg, width, sel, sgr);
                if (rc != 0) break;
                off += seg;
                used++;
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
    bool following = state->follow && state->at_live_edge && state->proc_active;
    if (following) {
        snprintf(out, n, "FOLLOWING ACTIVE");
    } else if (state->proc_active) {
        snprintf(out, n, "VIEWING ACTIVE");
    } else if (state->has_exit_code) {
        snprintf(out, n, "VIEWING EXITED (%d)", state->exit_code);
    } else {
        snprintf(out, n, "VIEWING EXITED");
    }
}

static int render_header_polished(const struct viewer_state *state) {
    size_t width = state->cols ? state->cols : 80;
    char *bar = malloc(width + 1);
    if (!bar) return -1;
    memset(bar, ' ', width);
    bar[width] = '\0';
    char idbuf[ID_DISPLAY_HEX_LEN + 1];
    /* Names are the human handle; the short id already lives in the footer. */
    const char *name = state->context.name && *state->context.name
                           ? state->context.name
                           : (state->context.run_id && *state->context.run_id
                                  ? hold_run_id_display(state->context.run_id, idbuf)
                                  : viewer_run_label(state));
    char left[128];
    snprintf(left, sizeof(left), "hold logs: %s", name);
    put_bar_text(bar, width, 0, left);
    char status[48];
    viewer_status_text(state, status, sizeof(status));
    size_t slen = strlen(status);
    if (slen < width) put_bar_text(bar, width, width - slen, status);
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
            if (viewer_puts(move) != 0) return -1;
            if (viewer_puts(sgr) != 0) return -1;
            if (viewer_puts(status) != 0) return -1;
            if (viewer_puts("\033[0m") != 0) return -1;
        }
    }
    if (viewer_puts("\033[K\r\n") != 0) return -1;
    /* The filter/jump row exists only while one is active; an empty chrome
     * line teaches nothing and costs a row of log. */
    if (state->jump_active) {
        if (viewer_puts("\033[7mgo to line: ") != 0) return -1;
        size_t room = width > 13 ? width - 13 : 0;
        if (viewer_put_capped(state->jump_buf, room) != 0) return -1;
        if (viewer_puts("\033[0m\033[K\r\n") != 0) return -1;
    } else if (state->filter[0]) {
        if (viewer_puts("\033[7mfilter: ") != 0) return -1;
        size_t room = width > 8 ? width - 8 : 0;
        if (viewer_put_capped(state->filter, room) != 0) return -1;
        if (viewer_puts("\033[0m\033[K\r\n") != 0) return -1;
    }
    return 0;
}

static int render_footer_polished(const struct viewer_state *state) {
    size_t width = state->cols ? state->cols : 80;
    char *bar = malloc(width + 1);
    if (!bar) return -1;
    memset(bar, ' ', width);
    bar[width] = '\0';
    /* Left: the short id. Right: the one discoverability hint. The screen
     * already shows whether timestamps, wrap, or the source column are on;
     * the footer does not narrate the visible. */
    char id[ID_DISPLAY_HEX_LEN + 1];
    const char *idtext = state->context.run_id && *state->context.run_id
                             ? hold_run_id_display(state->context.run_id, id)
                             : viewer_run_label(state);
    char leftbuf[ID_DISPLAY_HEX_LEN + 4];
    snprintf(leftbuf, sizeof(leftbuf), " %s", idtext);
    put_bar_text(bar, width, 0, leftbuf);
    const char *right = state->help_open ? "any key returns " : "Ctrl-H Help   Esc Quit ";
    size_t rlen = strlen(right);
    if (rlen < width) put_bar_text(bar, width, width - rlen, right);
    int rc = viewer_puts("\033[7m");
    if (rc == 0) rc = viewer_write_all(STDOUT_FILENO, bar, width);
    if (rc == 0) rc = viewer_puts("\033[0m");
    free(bar);
    return rc;
}

static int render_polished(struct viewer_state *state) {
    if (render_header_polished(state) != 0) return -1;
    if (render_body_polished(state, viewer_body_rows(state)) != 0) return -1;
    if (render_footer_polished(state) != 0) return -1;
    return viewer_puts("\033[K\033[J");
}

/*
 * Legacy chrome for the --debug-stats regression harness. Its header string and
 * footer field layout are a frozen test contract; the product path is
 * render_polished. Keep this byte-stable.
 */
static int render_legacy(struct viewer_state *state) {
    char header[512];
    bool has_filters = state->filter[0] || state->example_count > 0;
    if (state->filter[0]) {
        snprintf(header,
                 sizeof(header),
                 "\033[?25l\033[Hfilter: %s%s%s%s%s\033[K\r\n",
                 state->filter,
                 state->example_count ? " | excluding similar" : "",
                 state->cache_scan_limited ? " | partial" : (state->cache_reached_eof ? " | EOF" : ""),
                 state->newer_available ? " | newer below" : "",
                 has_filters ? " | Ctrl-R reset" : "");
    } else {
        snprintf(header,
                 sizeof(header),
                 "\033[?25l\033[Hhold logs %s%s%s%s%s%s\033[K\r\n",
                 viewer_run_label(state),
                 state->follow ? (state->at_live_edge ? "  ● live" : "  browsing") : (state->follow_exited ? "  exited" : ""),
                 state->example_count ? "  excluding similar" : "",
                 state->cache_scan_limited ? " | partial" : (state->cache_reached_eof ? " | EOF" : ""),
                 state->newer_available ? " | newer below" : "",
                 has_filters ? " | Ctrl-R reset" : "");
    }
    if (viewer_puts(header) != 0) return -1;

    size_t body_rows = viewer_body_rows(state);
    size_t line_width = state->cols;
    for (size_t i = 0; i < body_rows; i++) {
        if (i < state->visible_count) {
            if (i == state->selected) viewer_puts("\033[7m");
            write_sanitized_line(state->visible[i].line, line_width);
            if (i == state->selected) viewer_puts("\033[0m");
        } else {
            viewer_puts("~\033[K");
        }
        viewer_puts("\r\n");
    }

    char footer[512];
    snprintf(footer,
             sizeof(footer),
             "scan_gen=%zu offset=%lld prev=%lld next=%lld scanned=%zu bytes=%zu matches=%zu excludes=%zu%s%s | Ctrl-H help Ctrl-R reset\033[K",
             state->scan_generation,
             (long long)state->start_offset,
             (long long)state->prev_offset,
             (long long)state->next_offset,
             state->cache_lines_scanned,
             state->cache_bytes_read,
             state->cache_match_count,
             state->example_count,
             state->follow ? (state->at_live_edge ? " follow=tail" : " follow=browsing") : (state->follow_exited ? " follow=exited" : ""),
             state->newer_available ? " newer=yes" : "");
    if (viewer_puts(footer) != 0) return -1;
    return viewer_puts("\033[K\033[J");
}

static int render(struct viewer_state *state) {
    refresh_terminal_size(state);
    if (!state->cache_valid && refill_cache(state) != 0) return -1;
    if (state->debug_stats) return render_legacy(state);
    return render_polished(state);
}

static void push_history(struct viewer_state *state, off_t off) {
    if (state->history_count < VIEWER_OFFSET_HISTORY_MAX) {
        state->history[state->history_count++] = off;
        return;
    }
    memmove(state->history, state->history + 1, sizeof(state->history[0]) * (VIEWER_OFFSET_HISTORY_MAX - 1));
    state->history[VIEWER_OFFSET_HISTORY_MAX - 1] = off;
}

static void page_down(struct viewer_state *state) {
    clear_local_scan_limit(state);
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        if (state->at_live_edge) {
            return;
        }
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
        state->selected = 0;
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
    state->selected = 0;
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
    state->newer_scan_offset = state->tail_anchor;
    state->newer_floor_offset = state->tail_anchor;
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
    state->start_offset = end;
    state->tail_anchor = end;
    state->newer_scan_offset = end;
    state->newer_floor_offset = end;
    state->at_live_edge = state->follow;
    state->at_oldest_edge = false;
    cache_invalidate(state);
}

/* Jump to a 1-based record ordinal from the sidecar index. Filters stay
 * active: the page starts at the first visible match at or after that line. */
static void perform_jump(struct viewer_state *state) {
    state->jump_active = false;
    if (!state->jump_buf[0] || !state->idx_loaded || state->idx_map.count == 0) {
        state->jump_buf[0] = '\0';
        return;
    }
    unsigned long n = strtoul(state->jump_buf, NULL, 10);
    state->jump_buf[0] = '\0';
    if (n < 1) n = 1;
    if (n > state->idx_map.count) n = state->idx_map.count;
    if (state->follow && state->at_live_edge) enter_browsing_mode(state, false);
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->start_offset = state->idx_map.records[n - 1].offset;
    state->at_live_edge = false;
    state->at_oldest_edge = false;
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
        if (state->at_oldest_edge ||
            state->visible_count == 0 ||
            state->prev_offset <= 0 ||
            state->prev_offset >= state->start_offset) {
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
        if (state->start_offset != 0) {
            state->start_offset = 0;
            state->selected = 0;
            cache_invalidate(state);
        }
        return;
    }
    state->start_offset = state->history[--state->history_count];
    state->selected = 0;
    cache_invalidate(state);
}

static void handle_key_down(struct viewer_state *state) {
    if (state->follow && state->at_live_edge) return; /* nothing below the tail */
    if (state->selected + 1 < state->visible_count) {
        enter_browsing_mode(state, true);
        state->selected++;
    } else {
        page_down(state);
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
        page_up(state);
    }
}

int hold_log_viewer_tty_fd(int fd,
                             const char *title,
                             const struct hold_log_filter_options *opts,
                             const struct hold_log_viewer_follow *follow,
                             const struct hold_log_viewer_context *context,
                             bool debug_stats) {
    if (fd < 0 || !opts || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }
    struct viewer_state state;
    memset(&state, 0, sizeof(state));
    state.fd = fd;
    state.title = title;
    state.debug_stats = debug_stats;
    state.follow = follow && follow->enabled;
    state.is_running = follow ? follow->is_running : NULL;
    state.get_exit_code = follow ? follow->exit_code : NULL;
    state.running_userdata = follow ? follow->userdata : NULL;
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
            state.start_offset = end;
            state.tail_anchor = end;
            state.newer_scan_offset = end;
            state.newer_floor_offset = end;
        }
        state.scan_mode = VIEWER_SCAN_BACKWARD;
    }
    if (opts->literal) {
        snprintf(state.filter, sizeof(state.filter), "%s", opts->literal);
    }
    const char *term = getenv("TERM");
    state.colors = !state.debug_stats && !getenv("NO_COLOR") && term && strcmp(term, "dumb") != 0;

    struct raw_terminal raw;
    if (raw_terminal_enter(&raw) != 0) return -1;
    int rc = 0;
    bool need_render = true;
    enum viewer_key pending = VIEWER_KEY_NONE;
    unsigned char pending_printable = 0;
    while (1) {
        if (need_render && render(&state) != 0) {
            rc = -1;
            break;
        }
        need_render = false;
        unsigned char printable = 0;
        enum viewer_key key;
        if (pending != VIEWER_KEY_NONE) {
            key = pending;
            printable = pending_printable;
            pending = VIEWER_KEY_NONE;
        } else {
            key = read_key(&printable, 250);
        }
        if (key == VIEWER_KEY_NONE) {
            if (refresh_terminal_size(&state)) need_render = true;
            if (state.follow) {
                int tick = handle_follow_tick(&state);
                if (tick < 0) {
                    rc = -1;
                    break;
                }
                if (tick > 0) need_render = true;
            }
            continue;
        }
        if (state.help_open) {
            state.help_open = false;
            need_render = true;
            continue;
        }
        if (state.jump_active) {
            /* Modal line-number entry: digits build the target, Enter jumps,
             * Esc or Ctrl-G cancels. Anything else cancels and falls through. */
            if (key == VIEWER_KEY_PRINTABLE && printable >= '0' && printable <= '9') {
                size_t n = strlen(state.jump_buf);
                if (n + 1 < sizeof(state.jump_buf)) {
                    state.jump_buf[n] = (char)printable;
                    state.jump_buf[n + 1] = '\0';
                }
                need_render = true;
                continue;
            }
            if (key == VIEWER_KEY_BACKSPACE) {
                size_t n = strlen(state.jump_buf);
                if (n > 0) state.jump_buf[n - 1] = '\0';
                need_render = true;
                continue;
            }
            if (key == VIEWER_KEY_DOWN) { /* Enter */
                perform_jump(&state);
                need_render = true;
                continue;
            }
            if (key == VIEWER_KEY_QUIT || key == VIEWER_KEY_JUMP) {
                state.jump_active = false;
                state.jump_buf[0] = '\0';
                need_render = true;
                continue;
            }
            state.jump_active = false;
            state.jump_buf[0] = '\0';
        }
        if (key == VIEWER_KEY_QUIT) {
            int quit_tick = mark_newer_before_quit(&state);
            if (quit_tick < 0) {
                rc = -1;
                break;
            }
            if (quit_tick > 0 && render(&state) != 0) {
                rc = -1;
            }
            break;
        }
        if (key == VIEWER_KEY_DOWN) {
            handle_key_down(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_UP) {
            handle_key_up(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_WHEEL_UP || key == VIEWER_KEY_WHEEL_DOWN) {
            /* Wheel scrolling is naturally speed-sensitive: a fast spin
             * queues many notches, so drain the identical ones and apply
             * them in one repaint (three rows per notch). */
            int steps = 3;
            while (byte_ready(0)) {
                unsigned char p2 = 0;
                enum viewer_key k2 = read_key(&p2, 0);
                if (k2 == key) {
                    steps += 3;
                    continue;
                }
                if (k2 != VIEWER_KEY_NONE) {
                    pending = k2;
                    pending_printable = p2;
                }
                break;
            }
            for (int s = 0; s < steps; s++) {
                if (key == VIEWER_KEY_WHEEL_UP) handle_key_up(&state);
                else handle_key_down(&state);
            }
            need_render = true;
        } else if (key == VIEWER_KEY_PAGE_DOWN) {
            page_down(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_PAGE_UP) {
            page_up(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_TOP) {
            pin_to_oldest_visible_page(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_BOTTOM) {
            jump_to_newest_page(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_BACKSPACE) {
            size_t n = strlen(state.filter);
            if (n > 0) {
                state.filter[n - 1] = '\0';
                reset_filter_navigation(&state);
                need_render = true;
            }
        } else if (key == VIEWER_KEY_PRINTABLE) {
            size_t n = strlen(state.filter);
            if (n < VIEWER_FILTER_MAX) {
                state.filter[n] = (char)printable;
                state.filter[n + 1] = '\0';
                reset_filter_navigation(&state);
                need_render = true;
            }
        } else if (key == VIEWER_KEY_TOGGLE) {
            if (state.follow && state.at_live_edge && state.visible_count > 0) {
                /* No cursor is visible at the live edge, so the first Space
                 * only summons it (bottom row); the next Space excludes. */
                enter_browsing_mode(&state, true);
                state.selected = state.visible_count - 1;
            } else {
                const char *line = state.selected < state.visible_count ? state.visible[state.selected].line : NULL;
                if (toggle_example(&state, line) != 0) rc = -1;
            }
            need_render = true;
        } else if (key == VIEWER_KEY_SUSPEND) {
            /* Honest Ctrl-Z: restore the terminal, stop like any job, and
             * repaint when the shell resumes us. */
            raw_terminal_leave(&raw);
            kill(0, SIGTSTP);
            if (raw_terminal_enter(&raw) != 0) {
                rc = -1;
                break;
            }
            cache_invalidate(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_HELP) {
            state.help_open = true;
            need_render = true;
        } else if (key == VIEWER_KEY_TS_CYCLE) {
            state.ts_mode = state.ts_mode == HOLD_TS_NONE ? HOLD_TS_TIME
                          : state.ts_mode == HOLD_TS_TIME ? HOLD_TS_DATE
                          : HOLD_TS_NONE;
            need_render = true;
        } else if (key == VIEWER_KEY_TZ_TOGGLE) {
            state.ts_utc = !state.ts_utc;
            need_render = true;
        } else if (key == VIEWER_KEY_WRAP_TOGGLE) {
            state.wrap = !state.wrap;
            need_render = true;
        } else if (key == VIEWER_KEY_SOURCE_COL) {
            state.source_column = !state.source_column;
            need_render = true;
        } else if (key == VIEWER_KEY_LINE_NUMBERS) {
            state.line_numbers = !state.line_numbers;
            need_render = true;
        } else if (key == VIEWER_KEY_JUMP) {
            if (state.idx_loaded && state.idx_map.count > 0) {
                state.jump_active = true;
                state.jump_buf[0] = '\0';
                need_render = true;
            }
        } else if (key == VIEWER_KEY_RESET) {
            if (state.filter[0] || state.example_count > 0) {
                state.filter[0] = '\0';
                free_examples(&state);
                reset_filter_navigation(&state);
                need_render = true;
            }
        }
        if (rc != 0) break;
    }
    if (rc == 0) {
        int final_tick = mark_newer_before_quit(&state);
        if (final_tick < 0) {
            rc = -1;
        } else if (final_tick > 0 && render(&state) != 0) {
            rc = -1;
        }
    }
    raw_terminal_leave(&raw);
    free_examples(&state);
    cache_free(&state);
    if (state.idx_loaded) hold_logidx_map_free(&state.idx_map);
    return rc;
}
