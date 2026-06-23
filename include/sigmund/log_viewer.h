#pragma once
#ifndef SIGMUND_LOG_VIEWER_H
#define SIGMUND_LOG_VIEWER_H

#include "sigmund/config.h"

#define SIGMUND_LOG_VIEWER_MAX_EXAMPLES 8

struct sigmund_log_filter_options {
    const char *literal;
    const char *similar_examples[SIGMUND_LOG_VIEWER_MAX_EXAMPLES];
    size_t similar_example_count;
    double similar_threshold;
    size_t visible_capacity;
    size_t match_ring_capacity;
    size_t max_results;
};

struct sigmund_log_filter_result {
    char **lines;
    off_t *line_offsets;
    off_t *match_offsets;
    size_t line_count;
    size_t match_count;
    size_t match_ring_count;
    size_t match_ring_start;
    size_t bytes_read;
    size_t lines_scanned;
    off_t next_offset;
    bool reached_eof;
};

void sigmund_log_filter_options_init(struct sigmund_log_filter_options *opts);
void sigmund_log_filter_result_free(struct sigmund_log_filter_result *result);
int sigmund_log_filter_fd(int fd,
                         const struct sigmund_log_filter_options *opts,
                         struct sigmund_log_filter_result *result);
int sigmund_log_viewer_tty_fd(int fd,
                             const char *title,
                             const struct sigmund_log_filter_options *opts,
                             bool debug_stats);

#endif /* SIGMUND_LOG_VIEWER_H */
