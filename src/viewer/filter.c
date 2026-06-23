#include "sigmund/config.h"
#include "sigmund/log_viewer.h"

#define VIEWER_READ_CHUNK 4096
#define VIEWER_DEFAULT_VISIBLE 50
#define VIEWER_DEFAULT_MATCH_RING 256
#define VIEWER_MAX_TERMS 64
#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

struct term_profile {
    uint32_t terms[VIEWER_MAX_TERMS];
    size_t count;
};

struct filter_state {
    const struct sigmund_log_filter_options *opts;
    struct term_profile examples[SIGMUND_LOG_VIEWER_MAX_EXAMPLES];
    size_t example_count;
};

void sigmund_log_filter_options_init(struct sigmund_log_filter_options *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->similar_threshold = 0.45;
    opts->visible_capacity = VIEWER_DEFAULT_VISIBLE;
    opts->match_ring_capacity = VIEWER_DEFAULT_MATCH_RING;
    opts->max_results = VIEWER_DEFAULT_VISIBLE;
}

void sigmund_log_filter_result_free(struct sigmund_log_filter_result *result) {
    if (!result) return;
    if (result->lines) {
        for (size_t i = 0; i < result->line_count; i++) free(result->lines[i]);
    }
    free(result->lines);
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

static bool line_matches_similarity(const struct filter_state *state, const char *line) {
    if (state->example_count == 0) return false;
    struct term_profile line_profile;
    build_profile(line, &line_profile);
    for (size_t i = 0; i < state->example_count; i++) {
        if (dice_similarity(&line_profile, &state->examples[i]) >= state->opts->similar_threshold) return true;
    }
    return false;
}

static bool line_matches(const struct filter_state *state, const char *line) {
    bool has_literal = state->opts->literal && *state->opts->literal;
    bool has_similarity = state->example_count > 0;
    if (!has_literal && !has_similarity) return true;
    if (has_literal && strstr(line, state->opts->literal)) return true;
    if (has_similarity && line_matches_similarity(state, line)) return true;
    return false;
}

static char *dup_line(const char *line, size_t n) {
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, line, n);
    copy[n] = '\0';
    return copy;
}

static int push_visible(struct sigmund_log_filter_result *result,
                        const struct sigmund_log_filter_options *opts,
                        const char *line,
                        size_t n) {
    if (result->line_count >= opts->visible_capacity || result->line_count >= opts->max_results) return 0;
    char *copy = dup_line(line, n);
    if (!copy) return -1;
    result->lines[result->line_count++] = copy;
    return 0;
}

static void push_match_offset(struct sigmund_log_filter_result *result,
                              const struct sigmund_log_filter_options *opts,
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

static int ensure_result_storage(const struct sigmund_log_filter_options *opts,
                                 struct sigmund_log_filter_result *result) {
    result->lines = calloc(opts->visible_capacity ? opts->visible_capacity : 1, sizeof(char *));
    if (!result->lines) return -1;
    if (opts->match_ring_capacity > 0) {
        result->match_offsets = calloc(opts->match_ring_capacity, sizeof(off_t));
        if (!result->match_offsets) return -1;
    }
    return 0;
}

static int prepare_state(const struct sigmund_log_filter_options *opts, struct filter_state *state) {
    memset(state, 0, sizeof(*state));
    state->opts = opts;
    if (opts->similar_example_count > SIGMUND_LOG_VIEWER_MAX_EXAMPLES) {
        errno = EINVAL;
        return -1;
    }
    state->example_count = opts->similar_example_count;
    for (size_t i = 0; i < state->example_count; i++) build_profile(opts->similar_examples[i], &state->examples[i]);
    return 0;
}

static int consume_line(struct sigmund_log_filter_result *result,
                        const struct sigmund_log_filter_options *opts,
                        const struct filter_state *state,
                        const char *line,
                        size_t n,
                        off_t line_offset,
                        bool *done) {
    result->lines_scanned++;
    if (!line_matches(state, line)) return 0;
    result->match_count++;
    push_match_offset(result, opts, line_offset);
    if (push_visible(result, opts, line, n) != 0) return -1;
    if (result->line_count >= opts->max_results || result->line_count >= opts->visible_capacity) *done = true;
    return 0;
}

int sigmund_log_filter_fd(int fd,
                         const struct sigmund_log_filter_options *in_opts,
                         struct sigmund_log_filter_result *result) {
    if (fd < 0 || !in_opts || !result) {
        errno = EINVAL;
        return -1;
    }

    struct sigmund_log_filter_options opts = *in_opts;
    if (opts.visible_capacity == 0) opts.visible_capacity = VIEWER_DEFAULT_VISIBLE;
    if (opts.max_results == 0 || opts.max_results > opts.visible_capacity) opts.max_results = opts.visible_capacity;
    if (opts.similar_threshold <= 0.0) opts.similar_threshold = 0.45;

    memset(result, 0, sizeof(*result));
    if (ensure_result_storage(&opts, result) != 0) {
        sigmund_log_filter_result_free(result);
        return -1;
    }

    struct filter_state state;
    if (prepare_state(&opts, &state) != 0) {
        sigmund_log_filter_result_free(result);
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
        ssize_t nr = read(fd, read_buf, sizeof(read_buf));
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
            }
            break;
        }
        if (nr < 0) {
            if (errno == EINTR) continue;
            free(line);
            sigmund_log_filter_result_free(result);
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
                if (done) break;
            }
        }
    }
    free(line);
    return 0;

oom:
    free(line);
    sigmund_log_filter_result_free(result);
    errno = ENOMEM;
    return -1;
}
