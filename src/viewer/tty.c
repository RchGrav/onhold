#include "hold/config.h"
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
    VIEWER_KEY_UP,
    VIEWER_KEY_DOWN,
    VIEWER_KEY_PAGE_UP,
    VIEWER_KEY_PAGE_DOWN,
    VIEWER_KEY_BACKSPACE,
    VIEWER_KEY_TOGGLE,
    VIEWER_KEY_HELP,
    VIEWER_KEY_INFO,
    VIEWER_KEY_RESET,
    VIEWER_KEY_PRINTABLE
};

enum viewer_overlay {
    VIEWER_OVERLAY_NONE = 0,
    VIEWER_OVERLAY_HELP,
    VIEWER_OVERLAY_INFO
};

struct viewer_row {
    char *line;
    off_t offset;
};

struct viewer_state {
    int fd;
    const char *title;
    bool debug_stats;
    bool follow;
    bool follow_exited;
    hold_log_viewer_running_fn is_running;
    void *running_userdata;
    struct hold_log_viewer_context context;
    struct hold_log_filter_options base_opts;
    char filter[VIEWER_FILTER_MAX + 1];
    char *examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t example_count;
    enum viewer_overlay overlay;
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
 * `tail_anchor` is the newest file size the viewer has observed, while
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
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= (tcflag_t)~(OPOST);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return -1;
    raw->active = true;
    if (viewer_puts("\033[?1049h\033[?25l") != 0) return -1;
    return 0;
}

static void raw_terminal_leave(struct raw_terminal *raw) {
    viewer_puts("\033[?25h\033[?1049l");
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

static enum viewer_key read_key(unsigned char *printable, int timeout_ms) {
    unsigned char c = 0;
    *printable = 0;
    if (timeout_ms >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int ready;
        do {
            ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) return VIEWER_KEY_NONE;
    }
    if (read_byte(&c) != 0) return VIEWER_KEY_QUIT;
    if (c == 3 || c == 4 || c == 'q' || c == 'Q') return VIEWER_KEY_QUIT;
    if (c == 8) return VIEWER_KEY_HELP;
    if (c == 9) return VIEWER_KEY_INFO;
    if (c == 18) return VIEWER_KEY_RESET;
    if (c == ' ' || c == '\r' || c == '\n') return c == ' ' ? VIEWER_KEY_TOGGLE : VIEWER_KEY_DOWN;
    if (c == 127) return VIEWER_KEY_BACKSPACE;
    if (c == 'j') return VIEWER_KEY_DOWN;
    if (c == 'k') return VIEWER_KEY_UP;
    if (c == 6) return VIEWER_KEY_PAGE_DOWN;
    if (c == 2) return VIEWER_KEY_PAGE_UP;
    if (c == 27) {
        unsigned char a = 0, b = 0, d = 0;
        if (read_byte(&a) != 0) return VIEWER_KEY_QUIT;
        if (a != '[') return VIEWER_KEY_QUIT;
        if (read_byte(&b) != 0) return VIEWER_KEY_QUIT;
        if (b == 'A') return VIEWER_KEY_UP;
        if (b == 'B') return VIEWER_KEY_DOWN;
        if (b == '5' || b == '6') {
            if (read_byte(&d) == 0 && d == '~') return b == '5' ? VIEWER_KEY_PAGE_UP : VIEWER_KEY_PAGE_DOWN;
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
    state->at_oldest_edge = !result->scan_limited && result->match_count <= result->line_count;
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

static bool refresh_terminal_size(struct viewer_state *state) {
    size_t old_rows = state->rows, old_cols = state->cols;
    terminal_size(&state->rows, &state->cols);
    if (old_rows && (old_rows != state->rows || old_cols != state->cols)) {
        cache_invalidate(state);
        return true;
    }
    return false;
}

static void reset_filter_navigation(struct viewer_state *state) {
    if (state->follow) {
        off_t end = lseek(state->fd, 0, SEEK_END);
        if (end >= 0) state->start_offset = end;
        state->tail_anchor = state->start_offset;
        state->newer_scan_offset = state->tail_anchor;
        state->scan_mode = VIEWER_SCAN_BACKWARD;
        state->at_live_edge = true;
        state->newer_available = false;
    } else {
        state->start_offset = 0;
        state->tail_anchor = 0;
        state->newer_scan_offset = 0;
        state->scan_mode = VIEWER_SCAN_FORWARD;
        state->at_live_edge = false;
        state->newer_available = false;
    }
    state->history_count = 0;
    state->selected = 0;
    cache_invalidate(state);
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

    size_t visible_rows = state->rows > 2 ? state->rows - 2 : 1;
    opts.scan_byte_budget = visible_rows * VIEWER_SCAN_BYTES_PER_ROW;
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
        cache_invalidate(state);
        changed = true;
    }
    return changed ? 1 : 0;
}

static bool same_line(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int toggle_example(struct viewer_state *state, const char *line) {
    if (!line) return 0;
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
    size_t visible_rows = state->rows > 2 ? state->rows - 2 : 1;
    opts->visible_capacity = visible_rows;
    opts->max_results = visible_rows;
    if (opts->match_ring_capacity < visible_rows * 4) opts->match_ring_capacity = visible_rows * 4;
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
    struct hold_log_filter_options opts;
    configure_filter_opts(state, &opts);
    size_t visible_rows = state->rows > 2 ? state->rows - 2 : 1;
    size_t scan_budget = visible_rows * VIEWER_SCAN_BYTES_PER_ROW;
    struct hold_log_filter_result result;
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        if (state->follow && state->at_live_edge) {
            off_t end = lseek(state->fd, 0, SEEK_END);
            if (end >= 0) {
                state->start_offset = end;
                state->tail_anchor = end;
                state->newer_scan_offset = end;
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
        if (lseek(state->fd, state->start_offset, SEEK_SET) < 0) return -1;
        if (hold_log_filter_fd(state->fd, &opts, &result) != 0) return -1;
    }
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

static int render_bottom_bar(const struct viewer_state *state) {
    size_t width = state->cols ? state->cols : 80;
    char *bar = malloc(width + 1);
    if (!bar) return -1;
    memset(bar, ' ', width);
    bar[width] = '\0';

    if (state->overlay == VIEWER_OVERLAY_HELP) {
        const char *help = " type filter | Space exclude similar | Backspace relax | arrows/Pg move | q quit | any key closes ";
        put_bar_text(bar, width, 0, help);
        int rc = viewer_puts("\033[7m");
        if (rc == 0) rc = viewer_write_all(STDOUT_FILENO, bar, width);
        if (rc == 0) rc = viewer_puts("\033[0m");
        free(bar);
        return rc;
    }

    char left[96];
    snprintf(left, sizeof(left), " %s ", viewer_run_label(state));
    put_bar_text(bar, width, 0, left);

    const char *profile = state->context.profile && *state->context.profile ? state->context.profile : "-";
    char center[ALIAS_MAX_LEN + 32];
    snprintf(center, sizeof(center), " profile: %s ", profile);
    size_t center_len = strlen(center);
    if (center_len + 2 < width) put_bar_text(bar, width, (width - center_len) / 2, center);

    const char *right = " HELP Ctrl-H ";
    size_t right_len = strlen(right);
    if (right_len < width) put_bar_text(bar, width, width - right_len, right);

    int rc = viewer_puts("\033[7m");
    if (rc == 0) rc = viewer_write_all(STDOUT_FILENO, bar, width);
    if (rc == 0) rc = viewer_puts("\033[0m");
    free(bar);
    return rc;
}

static int viewer_move(size_t row, size_t col) {
    char seq[64];
    snprintf(seq, sizeof(seq), "\033[%zu;%zuH", row, col);
    return viewer_puts(seq);
}

static int draw_modal_line(size_t row, size_t col, size_t inner_width, const char *text) {
    if (viewer_move(row, col) != 0) return -1;
    if (viewer_puts("│ ") != 0) return -1;
    size_t used = 0;
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p && used < inner_width; p++, used++) {
            char out = (isprint(*p) || *p == '\t') ? (char)*p : ' ';
            if (viewer_write_all(STDOUT_FILENO, &out, 1) != 0) return -1;
        }
    }
    while (used++ < inner_width) {
        if (viewer_puts(" ") != 0) return -1;
    }
    return viewer_puts(" │");
}

static int draw_modal_rule(size_t row, size_t col, size_t inner_width, bool top) {
    if (viewer_move(row, col) != 0) return -1;
    if (viewer_puts(top ? "┌" : "└") != 0) return -1;
    for (size_t i = 0; i < inner_width + 2; i++) {
        if (viewer_puts("─") != 0) return -1;
    }
    return viewer_puts(top ? "┐" : "┘");
}

static int render_overlay(const struct viewer_state *state) {
    if (state->overlay == VIEWER_OVERLAY_NONE) return 0;
    if (state->overlay == VIEWER_OVERLAY_HELP) return 0;
    size_t width = state->cols > 12 ? state->cols - 8 : state->cols;
    if (width > 72) width = 72;
    if (width < 32) width = 32;
    size_t inner = width - 4;
    size_t col = state->cols > width ? (state->cols - width) / 2 + 1 : 1;
    size_t row = state->rows > 9 ? (state->rows - 8) / 2 + 1 : 1;

    if (draw_modal_rule(row, col, inner, true) != 0) return -1;
    char line[160];
    snprintf(line, sizeof(line), "runid: %s", viewer_run_label(state));
    if (draw_modal_line(row + 1, col, inner, "Process information") != 0) return -1;
    if (draw_modal_line(row + 2, col, inner, line) != 0) return -1;
    snprintf(line, sizeof(line), "profile: %s", state->context.profile && *state->context.profile ? state->context.profile : "-");
    if (draw_modal_line(row + 3, col, inner, line) != 0) return -1;
    snprintf(line, sizeof(line), "filter: %.96s", state->filter[0] ? state->filter : "-");
    if (draw_modal_line(row + 4, col, inner, line) != 0) return -1;
    snprintf(line, sizeof(line), "command: %.96s", state->context.command && *state->context.command ? state->context.command : "-");
    if (draw_modal_line(row + 5, col, inner, line) != 0) return -1;
    if (draw_modal_line(row + 6, col, inner, "PRESS ANY KEY TO EXIT") != 0) return -1;
    return draw_modal_rule(row + 7, col, inner, false);
}

static int render(struct viewer_state *state) {
    refresh_terminal_size(state);
    if (!state->cache_valid && refill_cache(state) != 0) return -1;

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
                 state->follow ? "  ● live" : (state->follow_exited ? "  exited" : ""),
                 state->example_count ? "  excluding similar" : "",
                 state->cache_scan_limited ? " | partial" : (state->cache_reached_eof ? " | EOF" : ""),
                 state->newer_available ? " | newer below" : "",
                 has_filters ? " | Ctrl-R reset" : "");
    }
    if (viewer_puts(header) != 0) return -1;

    size_t body_rows = state->rows > 2 ? state->rows - 2 : 1;
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

    if (state->debug_stats) {
        char footer[512];
        snprintf(footer,
                 sizeof(footer),
                 "scan_gen=%zu offset=%lld prev=%lld next=%lld scanned=%zu bytes=%zu matches=%zu excludes=%zu%s%s | Ctrl-H help Ctrl-I info Ctrl-R reset\033[K",
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
    } else {
        if (render_bottom_bar(state) != 0) return -1;
    }
    if (render_overlay(state) != 0) return -1;
    return viewer_puts("\033[K\033[J");
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
    if (state->scan_mode == VIEWER_SCAN_BACKWARD) {
        if (state->history_count > 0) {
            state->start_offset = state->history[--state->history_count];
            state->at_live_edge = state->history_count == 0 && state->start_offset >= state->tail_anchor;
        } else {
            if (!state->at_live_edge && state->visible_count > 0 && state->next_offset > state->start_offset) {
                state->scan_mode = VIEWER_SCAN_FORWARD;
                state->start_offset = state->next_offset;
                state->at_live_edge = false;
            } else {
                off_t end = lseek(state->fd, 0, SEEK_END);
                if (end >= 0) {
                    state->start_offset = end;
                    state->tail_anchor = end;
                    state->newer_scan_offset = end;
                }
                state->at_live_edge = true;
                state->newer_available = false;
            }
        }
        state->selected = 0;
        cache_invalidate(state);
        return;
    }
    if (state->visible_count == 0 || state->next_offset <= state->start_offset) return;
    push_history(state, state->start_offset);
    state->start_offset = state->next_offset;
    state->selected = 0;
    cache_invalidate(state);
}

static void pin_to_oldest_visible_page(struct viewer_state *state) {
    state->at_live_edge = false;
    state->scan_mode = VIEWER_SCAN_FORWARD;
    state->history_count = 0;
    state->selected = 0;
    if (state->visible_count > 0) {
        state->start_offset = state->visible[0].offset;
    } else {
        state->start_offset = 0;
    }
    cache_invalidate(state);
}

static void enter_browsing_mode(struct viewer_state *state, bool stabilize_visible_page) {
    if (!state->follow || !state->at_live_edge) return;
    state->at_live_edge = false;
    state->newer_scan_offset = state->tail_anchor;
    if (stabilize_visible_page && state->cache_valid && state->visible_count > 0) {
        state->start_offset = state->visible[0].offset;
        state->scan_mode = VIEWER_SCAN_FORWARD;
        state->history_count = 0;
        cache_invalidate(state);
    }
}

static void page_up(struct viewer_state *state) {
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
    state.running_userdata = follow ? follow->userdata : NULL;
    if (context) state.context = *context;
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
        }
        state.scan_mode = VIEWER_SCAN_BACKWARD;
    }
    if (opts->literal) {
        snprintf(state.filter, sizeof(state.filter), "%s", opts->literal);
    }

    struct raw_terminal raw;
    if (raw_terminal_enter(&raw) != 0) return -1;
    int rc = 0;
    bool need_render = true;
    while (1) {
        if (need_render && render(&state) != 0) {
            rc = -1;
            break;
        }
        need_render = false;
        unsigned char printable = 0;
        enum viewer_key key = read_key(&printable, 250);
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
        if (state.overlay != VIEWER_OVERLAY_NONE) {
            state.overlay = VIEWER_OVERLAY_NONE;
            need_render = true;
            continue;
        }
        if (key == VIEWER_KEY_QUIT) {
            break;
        }
        if (key == VIEWER_KEY_DOWN) {
            if (state.selected + 1 < state.visible_count) {
                enter_browsing_mode(&state, true);
                state.selected++;
            } else {
                page_down(&state);
            }
            need_render = true;
        } else if (key == VIEWER_KEY_UP) {
            if (state.selected > 0) {
                enter_browsing_mode(&state, true);
                state.selected--;
            } else {
                page_up(&state);
            }
            need_render = true;
        } else if (key == VIEWER_KEY_PAGE_DOWN) {
            page_down(&state);
            need_render = true;
        } else if (key == VIEWER_KEY_PAGE_UP) {
            page_up(&state);
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
            const char *line = state.selected < state.visible_count ? state.visible[state.selected].line : NULL;
            if (toggle_example(&state, line) != 0) rc = -1;
            need_render = true;
        } else if (key == VIEWER_KEY_HELP) {
            state.overlay = VIEWER_OVERLAY_HELP;
            need_render = true;
        } else if (key == VIEWER_KEY_INFO) {
            state.overlay = VIEWER_OVERLAY_INFO;
            need_render = true;
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
    raw_terminal_leave(&raw);
    free_examples(&state);
    cache_free(&state);
    return rc;
}
