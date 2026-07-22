#include "hold/config.h"
#include "hold/core.h"
#include "hold/term.h"

/* THE PTY-master pump. Every reader of a hold-owned PTY master drains it
 * through this one step so the capture path (indexed log first, then caller
 * fan-out) and the end-of-target semantics live in exactly one place.
 *
 * End-of-target: a PTY master reports the last slave closing either as EOF
 * or as read() failing with EIO (Linux). EIO is checked only when the read
 * actually failed — never against a stale errno from a successful read. */
ssize_t hold_term_pump_master(int master, int logfd, int logidxfd,
                              struct hold_term_tui_detect *tui,
                              char *buf, size_t n) {
    ssize_t r = read(master, buf, n);
    if (r > 0) {
        const char *stream = "stdout";
        if (tui) {
            hold_term_tui_feed(tui, buf, (size_t)r);
            if (tui->tui) stream = "pty";
        }
        (void)hold_write_indexed_log_bytes_fd(logfd, logidxfd, stream, buf, (size_t)r);
        return r;
    }
    if (r == 0 || (r < 0 && errno == EIO)) {
        return 0;
    }
    return -1;
}

/* ---- ANSI TUI detection (docs/future/playback.md) ------------------------
 *
 * A tiny resumable scanner for the two escape families only a screen-taking
 * program emits: alt-screen switches (CSI ? 47/1047/1049 h) and explicit
 * cursor addressing (CSI row ; col H|f, both parameters present). SGR
 * colors, \r progress bars, erase-to-EOL, and bare \033[H all fall through,
 * so a plain log can never misclassify (best-effort in the other direction
 * is the accepted trade). */

enum { TUI_GROUND = 0, TUI_ESC, TUI_CSI, TUI_PRIVATE, TUI_PARAM1, TUI_PARAM2 };

void hold_term_tui_feed(struct hold_term_tui_detect *d, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    if (d->tui) return;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        bool digit = c >= '0' && c <= '9';
        switch (d->st) {
        case TUI_ESC:
            if (c == '[') {
                d->st = TUI_CSI;
                continue;
            }
            break;
        case TUI_CSI:
            if (c == '?') {
                d->st = TUI_PRIVATE;
                d->param = 0;
                d->saw_digit = false;
                continue;
            }
            if (digit) {
                d->st = TUI_PARAM1;
                continue;
            }
            break;
        case TUI_PRIVATE:
            if (digit) {
                if (d->param < 100000) d->param = d->param * 10 + (unsigned)(c - '0');
                d->saw_digit = true;
                continue;
            }
            if (c == 'h' && d->saw_digit &&
                (d->param == 47 || d->param == 1047 || d->param == 1049)) {
                d->tui = true;
                return;
            }
            break;
        case TUI_PARAM1:
            if (digit) continue;
            if (c == ';') {
                d->st = TUI_PARAM2;
                d->saw_digit = false;
                continue;
            }
            break;
        case TUI_PARAM2:
            if (digit) {
                d->saw_digit = true;
                continue;
            }
            if ((c == 'H' || c == 'f') && d->saw_digit) {
                d->tui = true;
                return;
            }
            break;
        default:
            break;
        }
        d->st = c == 0x1b ? TUI_ESC : TUI_GROUND;
    }
}

/* ---- THE detach-key FSM, shared by console attach and the hold-on shell ----
 *
 * pending is always a strict prefix of a chord, so feeding a byte either
 * extends the prefix (arming the flush deadline), completes a chord, or
 * unwinds: the byte that broke the match is popped, the surviving prefix is
 * released through the sink, and the byte is re-fed against the empty state.
 * Two chords may share a prefix (Ctrl-P Ctrl-Q detach beside the Ctrl-P
 * Ctrl-P time-travel double-tap): the pending bytes track both until one
 * completes or both stop matching. */

static int64_t term_now_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}

void hold_term_detach_init(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys) {
    memset(d, 0, sizeof(*d));
    if (!keys || nkeys == 0 || nkeys > HOLD_TERM_DETACH_MAX_KEYS) return;
    memcpy(d->keys, keys, nkeys);
    d->nkeys = nkeys;
}

void hold_term_detach_set_alt(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys) {
    d->alt_nkeys = 0;
    if (!keys || nkeys == 0 || nkeys > HOLD_TERM_DETACH_MAX_KEYS) return;
    memcpy(d->alt_keys, keys, nkeys);
    d->alt_nkeys = nkeys;
}

/* Does pending match the first len bytes of this chord? Reports completion. */
static bool chord_prefix(const unsigned char *keys, size_t nkeys,
                         const unsigned char *pending, size_t len, bool *complete) {
    if (nkeys == 0 || len > nkeys || memcmp(keys, pending, len) != 0) return false;
    if (len == nkeys) *complete = true;
    return true;
}

int hold_term_detach_flush(struct hold_term_detach *d, hold_term_detach_sink sink, void *ctx) {
    if (d->pending_len == 0) return 0;
    size_t n = d->pending_len;
    d->pending_len = 0;
    return sink(ctx, d->pending, n) != 0 ? -1 : 0;
}

int hold_term_detach_feed(struct hold_term_detach *d, unsigned char c,
                          hold_term_detach_sink sink, void *ctx, int *completed) {
    *completed = 0;
    if (d->nkeys == 0 && d->alt_nkeys == 0) return sink(ctx, &c, 1) != 0 ? -1 : 0;
    for (;;) {
        if (d->pending_len == 0 &&
            !(d->nkeys > 0 && c == d->keys[0]) &&
            !(d->alt_nkeys > 0 && c == d->alt_keys[0])) {
            return sink(ctx, &c, 1) != 0 ? -1 : 0;
        }
        if (d->pending_len >= HOLD_TERM_DETACH_MAX_KEYS) {
            if (hold_term_detach_flush(d, sink, ctx) != 0) return -1;
            continue;
        }
        d->pending[d->pending_len++] = c;
        bool primary_done = false, alt_done = false;
        bool primary_live = chord_prefix(d->keys, d->nkeys, d->pending, d->pending_len, &primary_done);
        bool alt_live = chord_prefix(d->alt_keys, d->alt_nkeys, d->pending, d->pending_len, &alt_done);
        if (primary_done || alt_done) {
            d->pending_len = 0;
            *completed = primary_done ? 1 : 2;
            return 0;
        }
        if (primary_live || alt_live) {
            d->deadline_usec = term_now_usec() + HOLD_TERM_DETACH_FLUSH_USEC;
            return 0;
        }
        d->pending_len--;
        if (hold_term_detach_flush(d, sink, ctx) != 0) return -1;
        /* re-feed c against the now-empty pending state */
    }
}

int hold_term_detach_timeout_ms(const struct hold_term_detach *d) {
    if (d->pending_len == 0) return -1;
    int64_t remaining = d->deadline_usec - term_now_usec();
    if (remaining <= 0) return 0;
    return (int)((remaining + 999) / 1000);
}
