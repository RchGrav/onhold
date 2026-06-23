#include "sigmund/config.h"
#include "sigmund/log_viewer.h"

#define VIEWER_FILTER_MAX 255
#define VIEWER_OFFSET_HISTORY_MAX 1024

enum viewer_key {
    VIEWER_KEY_NONE = 0,
    VIEWER_KEY_QUIT,
    VIEWER_KEY_UP,
    VIEWER_KEY_DOWN,
    VIEWER_KEY_PAGE_UP,
    VIEWER_KEY_PAGE_DOWN,
    VIEWER_KEY_BACKSPACE,
    VIEWER_KEY_TOGGLE,
    VIEWER_KEY_PRINTABLE
};

struct viewer_state {
    int fd;
    const char *title;
    bool debug_stats;
    bool follow;
    bool follow_exited;
    sigmund_log_viewer_running_fn is_running;
    void *running_userdata;
    struct sigmund_log_filter_options base_opts;
    char filter[VIEWER_FILTER_MAX + 1];
    char *examples[SIGMUND_LOG_VIEWER_MAX_EXAMPLES];
    size_t example_count;
    off_t start_offset;
    off_t history[VIEWER_OFFSET_HISTORY_MAX];
    size_t history_count;
    size_t selected;
    size_t rows;
    size_t cols;
};

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
    if (c == ' ' || c == '\r' || c == '\n') return c == ' ' ? VIEWER_KEY_TOGGLE : VIEWER_KEY_DOWN;
    if (c == 127 || c == 8) return VIEWER_KEY_BACKSPACE;
    if (c == 'j') return VIEWER_KEY_DOWN;
    if (c == 'k') return VIEWER_KEY_UP;
    if (c == 'f' || c == 6) return VIEWER_KEY_PAGE_DOWN;
    if (c == 'b' || c == 2) return VIEWER_KEY_PAGE_UP;
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

static void reset_filter_navigation(struct viewer_state *state) {
    if (!state->follow) state->start_offset = 0;
    state->history_count = 0;
    state->selected = 0;
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
    if (state->example_count >= SIGMUND_LOG_VIEWER_MAX_EXAMPLES) return 0;
    char *copy = strdup(line);
    if (!copy) return -1;
    state->examples[state->example_count++] = copy;
    reset_filter_navigation(state);
    return 0;
}

static void configure_filter_opts(const struct viewer_state *state, struct sigmund_log_filter_options *opts) {
    *opts = state->base_opts;
    opts->literal = state->filter[0] ? state->filter : NULL;
    opts->similar_example_count = state->example_count;
    for (size_t i = 0; i < state->example_count; i++) opts->similar_examples[i] = state->examples[i];
    size_t visible_rows = state->rows > 4 ? state->rows - 4 : 1;
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

static int render(struct viewer_state *state, struct sigmund_log_filter_result *result) {
    terminal_size(&state->rows, &state->cols);
    if (lseek(state->fd, state->start_offset, SEEK_SET) < 0) return -1;

    struct sigmund_log_filter_options opts;
    configure_filter_opts(state, &opts);
    if (sigmund_log_filter_fd(state->fd, &opts, result) != 0) return -1;
    if (state->selected >= result->line_count && result->line_count > 0) state->selected = result->line_count - 1;
    if (result->line_count == 0) state->selected = 0;

    char header[512];
    snprintf(header,
             sizeof(header),
             "\033[H\033[2Jmund view %s%s | filter: %s%s | similar: %zu | q quit\033[K\r\n",
             state->title ? state->title : "",
             state->follow ? " [follow]" : (state->follow_exited ? " [exited]" : ""),
             state->filter[0] ? state->filter : "(type to filter)",
             result->reached_eof ? " | EOF" : "",
             state->example_count);
    if (viewer_puts(header) != 0) return -1;

    size_t body_rows = state->rows > 4 ? state->rows - 4 : 1;
    size_t line_width = state->cols > 4 ? state->cols - 4 : state->cols;
    for (size_t i = 0; i < body_rows; i++) {
        if (i < result->line_count) {
            if (i == state->selected) viewer_puts("\033[7m");
            viewer_puts("  ");
            write_sanitized_line(result->lines[i], line_width);
            if (i == state->selected) viewer_puts("\033[0m");
        } else {
            viewer_puts("~\033[K");
        }
        viewer_puts("\r\n");
    }

    char footer[512];
    if (state->debug_stats) {
        snprintf(footer,
                 sizeof(footer),
                 "offset=%lld next=%lld scanned=%zu bytes=%zu matches=%zu%s | arrows/j/k PgUp/PgDn space=similar backspace=filter\033[K",
                 (long long)state->start_offset,
                 (long long)result->next_offset,
                 result->lines_scanned,
                 result->bytes_read,
                 result->match_count,
                 state->follow ? " follow=on" : (state->follow_exited ? " follow=exited" : ""));
    } else {
        snprintf(footer,
                 sizeof(footer),
                 "arrows/j/k move | PgUp/PgDn page | type filters | Backspace clears | Space toggles similarity%s\033[K",
                 state->follow ? " | Follow refresh on" : (state->follow_exited ? " | Run exited" : ""));
    }
    return viewer_puts(footer);
}

static void push_history(struct viewer_state *state, off_t off) {
    if (state->history_count < VIEWER_OFFSET_HISTORY_MAX) {
        state->history[state->history_count++] = off;
        return;
    }
    memmove(state->history, state->history + 1, sizeof(state->history[0]) * (VIEWER_OFFSET_HISTORY_MAX - 1));
    state->history[VIEWER_OFFSET_HISTORY_MAX - 1] = off;
}

static void page_down(struct viewer_state *state, const struct sigmund_log_filter_result *result) {
    if (result->line_count == 0 || result->next_offset <= state->start_offset) return;
    push_history(state, state->start_offset);
    state->start_offset = result->next_offset;
    state->selected = 0;
}

static void page_up(struct viewer_state *state) {
    if (state->history_count == 0) {
        state->start_offset = 0;
        state->selected = 0;
        return;
    }
    state->start_offset = state->history[--state->history_count];
    state->selected = 0;
}

int sigmund_log_viewer_tty_fd(int fd,
                             const char *title,
                             const struct sigmund_log_filter_options *opts,
                             const struct sigmund_log_viewer_follow *follow,
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
    state.base_opts = *opts;
    if (opts->literal) {
        snprintf(state.filter, sizeof(state.filter), "%s", opts->literal);
    }

    struct raw_terminal raw;
    if (raw_terminal_enter(&raw) != 0) return -1;
    int rc = 0;
    while (1) {
        struct sigmund_log_filter_result result;
        if (render(&state, &result) != 0) {
            rc = -1;
            break;
        }
        unsigned char printable = 0;
        enum viewer_key key = read_key(&printable, state.follow ? 250 : -1);
        if (key == VIEWER_KEY_NONE && state.follow) {
            if (result.reached_eof && state.is_running && !state.is_running(state.running_userdata)) {
                state.follow = false;
                state.follow_exited = true;
                sigmund_log_filter_result_free(&result);
                continue;
            }
            if (result.line_count > 0 && !result.reached_eof) {
                page_down(&state, &result);
            }
            sigmund_log_filter_result_free(&result);
            continue;
        }
        if (key == VIEWER_KEY_QUIT) {
            sigmund_log_filter_result_free(&result);
            break;
        }
        if (key == VIEWER_KEY_DOWN) {
            if (state.selected + 1 < result.line_count) state.selected++;
            else page_down(&state, &result);
        } else if (key == VIEWER_KEY_UP) {
            if (state.selected > 0) state.selected--;
            else page_up(&state);
        } else if (key == VIEWER_KEY_PAGE_DOWN) {
            page_down(&state, &result);
        } else if (key == VIEWER_KEY_PAGE_UP) {
            page_up(&state);
        } else if (key == VIEWER_KEY_BACKSPACE) {
            size_t n = strlen(state.filter);
            if (n > 0) {
                state.filter[n - 1] = '\0';
                reset_filter_navigation(&state);
            }
        } else if (key == VIEWER_KEY_PRINTABLE) {
            size_t n = strlen(state.filter);
            if (n < VIEWER_FILTER_MAX) {
                state.filter[n] = (char)printable;
                state.filter[n + 1] = '\0';
                reset_filter_navigation(&state);
            }
        } else if (key == VIEWER_KEY_TOGGLE) {
            const char *line = state.selected < result.line_count ? result.lines[state.selected] : NULL;
            if (toggle_example(&state, line) != 0) rc = -1;
        }
        sigmund_log_filter_result_free(&result);
        if (rc != 0) break;
    }
    raw_terminal_leave(&raw);
    free_examples(&state);
    return rc;
}
