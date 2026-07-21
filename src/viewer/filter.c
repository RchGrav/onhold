#include "hold/config.h"
#include "hold/log_viewer.h"
#include "hold/core.h"

#define VIEWER_READ_CHUNK 4096
#define VIEWER_DEFAULT_VISIBLE 50
#define VIEWER_DEFAULT_MATCH_RING 256
#define VIEWER_DEFAULT_SCAN_BUDGET (1024u * 1024u)
#define VIEWER_MAX_TERMS 64
#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

struct term_profile {
    uint32_t terms[VIEWER_MAX_TERMS];
    size_t count;
};

struct filter_state {
    const struct hold_log_filter_options *opts;
    struct term_profile examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t example_count;
    struct term_profile exclude_examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t exclude_example_count;
};

void hold_log_filter_options_init(struct hold_log_filter_options *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->similar_threshold = 0.45;
    opts->visible_capacity = VIEWER_DEFAULT_VISIBLE;
    opts->match_ring_capacity = VIEWER_DEFAULT_MATCH_RING;
    opts->max_results = VIEWER_DEFAULT_VISIBLE;
}

void hold_log_filter_result_free(struct hold_log_filter_result *result) {
    if (!result) return;
    if (result->lines) {
        for (size_t i = 0; i < result->line_count; i++) free(result->lines[i]);
    }
    free(result->lines);
    free(result->line_offsets);
    free(result->match_offsets);
    memset(result, 0, sizeof(*result));
}

static uint32_t fnv1a_lower_token(const char *s, size_t n) {
    uint32_t h = FNV_OFFSET;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)tolower((unsigned char)s[i]);
        h *= FNV_PRIME;
    }
    return h;
}

static bool profile_has(const struct term_profile *profile, uint32_t h) {
    for (size_t i = 0; i < profile->count; i++) {
        if (profile->terms[i] == h) return true;
    }
    return false;
}

static void profile_add(struct term_profile *profile, uint32_t h) {
    if (profile->count >= VIEWER_MAX_TERMS || profile_has(profile, h)) return;
    profile->terms[profile->count++] = h;
}

static void build_profile(const char *line, struct term_profile *profile) {
    memset(profile, 0, sizeof(*profile));
    if (!line) return;
    const char *p = line;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t n = (size_t)(p - start);
        if (n >= 2) profile_add(profile, fnv1a_lower_token(start, n));
    }
}

static double dice_similarity(const struct term_profile *a, const struct term_profile *b) {
    if (a->count == 0 || b->count == 0) return 0.0;
    size_t common = 0;
    for (size_t i = 0; i < a->count; i++) {
        if (profile_has(b, a->terms[i])) common++;
    }
    return (2.0 * (double)common) / (double)(a->count + b->count);
}

static bool line_matches_profiles(const struct term_profile *examples, size_t count, double threshold, const char *line) {
    if (count == 0) return false;
    struct term_profile line_profile;
    build_profile(line, &line_profile);
    for (size_t i = 0; i < count; i++) {
        if (dice_similarity(&line_profile, &examples[i]) >= threshold) return true;
    }
    return false;
}

static bool line_matches(const struct filter_state *state, const char *line) {
    bool has_literal = state->opts->literal && *state->opts->literal;
    bool has_similarity = state->example_count > 0;
    bool included = !has_literal && !has_similarity;
    if (has_literal && strstr(line, state->opts->literal)) included = true;
    if (has_similarity && line_matches_profiles(state->examples, state->example_count, state->opts->similar_threshold, line)) included = true;
    if (!included) return false;
    if (state->exclude_example_count > 0 &&
        line_matches_profiles(state->exclude_examples, state->exclude_example_count, state->opts->similar_threshold, line)) {
        return false;
    }
    return true;
}

static bool source_visible(const struct hold_log_filter_options *opts, off_t line_offset) {
    if (opts->source_mask == 0 || !opts->idx_map) return true;
    const struct hold_logidx_record *rec = hold_logidx_map_find(opts->idx_map, line_offset);
    if (!rec) return true; /* unknown source stays visible; never fake metadata */
    unsigned bit;
    switch (hold_logidx_record_stream(rec->meta)) {
        case HOLD_LOG_STREAM_STDERR: bit = HOLD_LOG_SRC_STDERR; break;
        case HOLD_LOG_STREAM_STDIN:  bit = HOLD_LOG_SRC_STDIN; break;
        case HOLD_LOG_STREAM_PTY:    bit = HOLD_LOG_SRC_PTY; break;
        default:                     bit = HOLD_LOG_SRC_STDOUT; break;
    }
    return (opts->source_mask & bit) != 0;
}

static char *dup_line(const char *line, size_t n) {
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, line, n);
    copy[n] = '\0';
    return copy;
}

static int push_visible(struct hold_log_filter_result *result,
                        const struct hold_log_filter_options *opts,
                        const char *line,
                        size_t n,
                        off_t line_offset) {
    if (result->line_count >= opts->visible_capacity || result->line_count >= opts->max_results) return 0;
    char *copy = dup_line(line, n);
    if (!copy) return -1;
    result->lines[result->line_count] = copy;
    result->line_offsets[result->line_count] = line_offset;
    result->line_count++;
    return 0;
}

static void push_match_offset(struct hold_log_filter_result *result,
                              const struct hold_log_filter_options *opts,
                              off_t off) {
    if (opts->match_ring_capacity == 0) return;
    size_t pos;
    if (result->match_ring_count < opts->match_ring_capacity) {
        pos = (result->match_ring_start + result->match_ring_count) % opts->match_ring_capacity;
        result->match_ring_count++;
    } else {
        pos = result->match_ring_start;
        result->match_ring_start = (result->match_ring_start + 1) % opts->match_ring_capacity;
    }
    result->match_offsets[pos] = off;
}

static int ensure_result_storage(const struct hold_log_filter_options *opts,
                                 struct hold_log_filter_result *result) {
    result->lines = calloc(opts->visible_capacity ? opts->visible_capacity : 1, sizeof(char *));
    if (!result->lines) return -1;
    result->line_offsets = calloc(opts->visible_capacity ? opts->visible_capacity : 1, sizeof(off_t));
    if (!result->line_offsets) return -1;
    if (opts->match_ring_capacity > 0) {
        result->match_offsets = calloc(opts->match_ring_capacity, sizeof(off_t));
        if (!result->match_offsets) return -1;
    }
    return 0;
}

static int prepare_state(const struct hold_log_filter_options *opts, struct filter_state *state) {
    memset(state, 0, sizeof(*state));
    state->opts = opts;
    if (opts->similar_example_count > HOLD_LOG_VIEWER_MAX_EXAMPLES) {
        errno = EINVAL;
        return -1;
    }
    if (opts->exclude_example_count > HOLD_LOG_VIEWER_MAX_EXAMPLES) {
        errno = EINVAL;
        return -1;
    }
    state->example_count = opts->similar_example_count;
    for (size_t i = 0; i < state->example_count; i++) build_profile(opts->similar_examples[i], &state->examples[i]);
    state->exclude_example_count = opts->exclude_example_count;
    for (size_t i = 0; i < state->exclude_example_count; i++) build_profile(opts->exclude_examples[i], &state->exclude_examples[i]);
    return 0;
}

static int consume_line(struct hold_log_filter_result *result,
                        const struct hold_log_filter_options *opts,
                        const struct filter_state *state,
                        const char *line,
                        size_t n,
                        off_t line_offset,
                        bool *done) {
    result->lines_scanned++;
    if (!source_visible(opts, line_offset)) {
        return 0;
    }
    if (!line_matches(state, line)) {
        return 0;
    }
    result->match_count++;
    push_match_offset(result, opts, line_offset);
    if (push_visible(result, opts, line, n, line_offset) != 0) {
        return -1;
    }
    if (result->line_count >= opts->max_results || result->line_count >= opts->visible_capacity) *done = true;
    return 0;
}

int hold_log_filter_fd(int fd,
                         const struct hold_log_filter_options *in_opts,
                         struct hold_log_filter_result *result) {
    if (fd < 0 || !in_opts || !result) {
        errno = EINVAL;
        return -1;
    }

    struct hold_log_filter_options opts = *in_opts;
    if (opts.visible_capacity == 0) opts.visible_capacity = VIEWER_DEFAULT_VISIBLE;
    if (opts.max_results == 0 || opts.max_results > opts.visible_capacity) opts.max_results = opts.visible_capacity;
    if (opts.similar_threshold <= 0.0) opts.similar_threshold = 0.45;

    memset(result, 0, sizeof(*result));
    if (ensure_result_storage(&opts, result) != 0) {
        hold_log_filter_result_free(result);
        return -1;
    }

    struct filter_state state;
    if (prepare_state(&opts, &state) != 0) {
        hold_log_filter_result_free(result);
        return -1;
    }

    char read_buf[VIEWER_READ_CHUNK];
    char *line = NULL;
    size_t line_cap = 0;
    size_t line_len = 0;
    off_t line_offset = lseek(fd, 0, SEEK_CUR);
    if (line_offset < 0) line_offset = 0;
    off_t next_offset = line_offset;
    bool done = false;

    while (!done) {
        size_t read_cap = sizeof(read_buf);
        if (opts.scan_byte_budget > 0) {
            if (result->bytes_read >= opts.scan_byte_budget) {
                result->scan_limited = true;
                break;
            }
            size_t remaining_budget = opts.scan_byte_budget - result->bytes_read;
            if (remaining_budget < read_cap) read_cap = remaining_budget;
        }
        ssize_t nr = read(fd, read_buf, read_cap);
        if (nr == 0) {
            result->reached_eof = true;
            if (line_len > 0) {
                if (line_len + 1 > line_cap) {
                    char *grown = realloc(line, line_len + 1);
                    if (!grown) goto oom;
                    line = grown;
                    line_cap = line_len + 1;
                }
                line[line_len] = '\0';
                if (consume_line(result, &opts, &state, line, line_len, line_offset, &done) != 0) goto oom;
                result->next_offset = next_offset;
            }
            break;
        }
        if (nr < 0) {
            if (errno == EINTR) continue;
            free(line);
            hold_log_filter_result_free(result);
            return -1;
        }
        result->bytes_read += (size_t)nr;
        for (ssize_t i = 0; i < nr; i++) {
            char c = read_buf[i];
            if (line_len + 2 > line_cap) {
                size_t new_cap = line_cap ? line_cap * 2 : 256;
                while (new_cap < line_len + 2) new_cap *= 2;
                char *grown = realloc(line, new_cap);
                if (!grown) goto oom;
                line = grown;
                line_cap = new_cap;
            }
            line[line_len++] = c;
            next_offset++;
            if (c == '\n') {
                line[line_len] = '\0';
                if (consume_line(result, &opts, &state, line, line_len, line_offset, &done) != 0) goto oom;
                line_len = 0;
                line_offset = next_offset;
                result->next_offset = next_offset;
                if (done) break;
            }
        }
        if (opts.scan_byte_budget > 0 && result->bytes_read >= opts.scan_byte_budget && !done) {
            result->scan_limited = true;
            if (next_offset > result->next_offset) result->next_offset = next_offset;
            break;
        }
    }
    if (result->next_offset == 0) result->next_offset = next_offset;
    free(line);
    return 0;

oom:
    free(line);
    hold_log_filter_result_free(result);
    errno = ENOMEM;
    return -1;
}

int hold_log_filter_backward_fd(int fd,
                                  const struct hold_log_filter_options *in_opts,
                                  off_t anchor_offset,
                                  size_t byte_budget,
                                  struct hold_log_filter_result *result) {
    if (fd < 0 || !in_opts || !result) {
        errno = EINVAL;
        return -1;
    }

    struct hold_log_filter_options opts = *in_opts;
    if (opts.visible_capacity == 0) opts.visible_capacity = VIEWER_DEFAULT_VISIBLE;
    if (opts.max_results == 0 || opts.max_results > opts.visible_capacity) opts.max_results = opts.visible_capacity;
    if (opts.similar_threshold <= 0.0) opts.similar_threshold = 0.45;
    if (byte_budget == 0) byte_budget = VIEWER_DEFAULT_SCAN_BUDGET;

    memset(result, 0, sizeof(*result));
    if (ensure_result_storage(&opts, result) != 0) {
        hold_log_filter_result_free(result);
        return -1;
    }

    struct filter_state state;
    if (prepare_state(&opts, &state) != 0) {
        hold_log_filter_result_free(result);
        return -1;
    }

    off_t file_end = lseek(fd, 0, SEEK_END);
    if (file_end < 0) {
        hold_log_filter_result_free(result);
        return -1;
    }
    if (anchor_offset < 0 || anchor_offset > file_end) anchor_offset = file_end;
    result->reached_eof = anchor_offset >= file_end;
    result->next_offset = anchor_offset;
    result->prev_offset = anchor_offset;
    if (anchor_offset == 0) return 0;

    off_t start = 0;
    if ((uintmax_t)anchor_offset > (uintmax_t)byte_budget) {
        start = anchor_offset - (off_t)byte_budget;
        result->scan_limited = true;
    }
    size_t span = (size_t)(anchor_offset - start);
    char *buf = malloc(span + 1);
    if (!buf) {
        hold_log_filter_result_free(result);
        errno = ENOMEM;
        return -1;
    }
    if (lseek(fd, start, SEEK_SET) < 0) {
        free(buf);
        hold_log_filter_result_free(result);
        return -1;
    }
    size_t got = 0;
    while (got < span) {
        ssize_t nr = read(fd, buf + got, span - got);
        if (nr < 0) {
            if (errno == EINTR) continue;
            free(buf);
            hold_log_filter_result_free(result);
            return -1;
        }
        if (nr == 0) break;
        got += (size_t)nr;
    }
    buf[got] = '\0';
    result->bytes_read = got;

    size_t parse = 0;
    if (start > 0) {
        while (parse < got && buf[parse] != '\n') parse++;
        if (parse < got) parse++;
    }

    char **ring_lines = calloc(opts.visible_capacity ? opts.visible_capacity : 1, sizeof(char *));
    off_t *ring_offsets = calloc(opts.visible_capacity ? opts.visible_capacity : 1, sizeof(off_t));
    off_t *ring_next_offsets = calloc(opts.visible_capacity ? opts.visible_capacity : 1, sizeof(off_t));
    if (!ring_lines || !ring_offsets || !ring_next_offsets) {
        free(ring_lines);
        free(ring_offsets);
        free(ring_next_offsets);
        free(buf);
        hold_log_filter_result_free(result);
        errno = ENOMEM;
        return -1;
    }
    size_t ring_start = 0, ring_count = 0;
    size_t line_start = parse;
    for (size_t i = parse; i <= got; i++) {
        if (i < got && buf[i] != '\n') continue;
        size_t line_len = i < got ? i + 1 - line_start : i - line_start;
        off_t line_off = start + (off_t)line_start;
        off_t line_next_off = start + (off_t)(i < got ? i + 1 : i);
        if (line_off >= anchor_offset) break;
        if (line_len > 0) {
            result->lines_scanned++;
            char saved = buf[line_start + line_len];
            buf[line_start + line_len] = '\0';
            const char *visible = buf + line_start;
            size_t visible_len = line_len;
            bool matches = source_visible(&opts, line_off) && line_matches(&state, visible);
            buf[line_start + line_len] = saved;
            if (matches) {
                result->match_count++;
                push_match_offset(result, &opts, line_off);
                char *copy = dup_line(visible, visible_len);
                if (!copy) {
                    free(buf);
                    for (size_t j = 0; j < ring_count; j++) {
                        size_t pos = (ring_start + j) % opts.visible_capacity;
                        free(ring_lines[pos]);
                    }
                    free(ring_lines);
                    free(ring_offsets);
                    free(ring_next_offsets);
                    hold_log_filter_result_free(result);
                    errno = ENOMEM;
                    return -1;
                }
                size_t cap = opts.visible_capacity ? opts.visible_capacity : 1;
                if (ring_count < cap) {
                    size_t pos = (ring_start + ring_count) % cap;
                    ring_lines[pos] = copy;
                    ring_offsets[pos] = line_off;
                    ring_next_offsets[pos] = line_next_off;
                    ring_count++;
                } else {
                    free(ring_lines[ring_start]);
                    ring_lines[ring_start] = copy;
                    ring_offsets[ring_start] = line_off;
                    ring_next_offsets[ring_start] = line_next_off;
                    ring_start = (ring_start + 1) % cap;
                }
            }
        }
        line_start = i + 1;
    }

    size_t out_count = ring_count;
    if (out_count > opts.max_results) out_count = opts.max_results;
    size_t skip = ring_count - out_count;
    for (size_t j = 0; j < out_count; j++) {
        size_t pos = (ring_start + skip + j) % (opts.visible_capacity ? opts.visible_capacity : 1);
        result->lines[j] = ring_lines[pos];
        result->line_offsets[j] = ring_offsets[pos];
        ring_lines[pos] = NULL;
        result->line_count++;
    }
    if (result->line_count > 0) {
        result->prev_offset = result->line_offsets[0];
        size_t last = result->line_count - 1;
        size_t last_pos = (ring_start + skip + last) % (opts.visible_capacity ? opts.visible_capacity : 1);
        result->next_offset = ring_next_offsets[last_pos];
    } else {
        result->prev_offset = start;
    }

    for (size_t j = 0; j < ring_count; j++) {
        size_t pos = (ring_start + j) % (opts.visible_capacity ? opts.visible_capacity : 1);
        free(ring_lines[pos]);
    }
    free(ring_lines);
    free(ring_offsets);
    free(ring_next_offsets);
    free(buf);
    return 0;
}
