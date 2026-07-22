#pragma once
#ifndef HOLD_LOG_VIEWER_H
#define HOLD_LOG_VIEWER_H

#include "hold/config.h"

#define HOLD_LOG_VIEWER_MAX_EXAMPLES 8

/* Source visibility bits. A mask of 0 means "all sources visible" so
 * zero-initialised options keep every record (no metadata lookup needed). */
#define HOLD_LOG_SRC_STDOUT 0x1u
#define HOLD_LOG_SRC_STDERR 0x2u
#define HOLD_LOG_SRC_STDIN 0x4u
#define HOLD_LOG_SRC_PTY 0x8u
#define HOLD_LOG_SRC_ALL 0u

struct hold_logidx_map; /* from hold/core.h */

struct hold_log_filter_options {
    const char *literal;
    const char *similar_examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t similar_example_count;
    const char *exclude_examples[HOLD_LOG_VIEWER_MAX_EXAMPLES];
    size_t exclude_example_count;
    double similar_threshold;
    size_t visible_capacity;
    size_t match_ring_capacity;
    size_t max_results;
    size_t scan_byte_budget;
    /* Optional sidecar-backed source filtering. When source_mask is nonzero,
     * records whose stream bit is clear are skipped using idx_map metadata
     * keyed by byte offset; records with no index entry are kept. */
    const struct hold_logidx_map *idx_map;
    unsigned source_mask;
};

struct hold_log_filter_result {
    char **lines;
    off_t *line_offsets;
    off_t *match_offsets;
    size_t line_count;
    size_t match_count;
    size_t match_ring_count;
    size_t match_ring_start;
    size_t bytes_read;
    size_t lines_scanned;
    off_t prev_offset;
    off_t next_offset;
    bool reached_eof;
    bool scan_limited;
};

typedef bool (*hold_log_viewer_running_fn)(void *userdata);

/* Fills *code_out with the exit status of a call that has finished; returns
 * false while the call is still running or the status is unknown. */
typedef bool (*hold_log_viewer_exit_code_fn)(void *userdata, int *code_out);

struct hold_log_viewer_follow {
    bool enabled;
    hold_log_viewer_running_fn is_running;
    hold_log_viewer_exit_code_fn exit_code;
    void *userdata;
    /* Attached-console time-travel (docs/future/playback.md): the broker
     * keeps fanning the live stream out to the attach socket while the
     * viewer owns the terminal; those bytes are already in the indexed log
     * the viewer renders from, so the socket copy is drained and discarded
     * each idle tick to keep the never-released session from backing up.
     * EOF on the drain fd means the console ended. */
    bool drain_enabled;
    int drain_fd;
};

struct hold_log_viewer_context {
    const char *run_id;
    const char *name;    /* the call's name: the header's preferred label */
    const char *command;
    const char *log_path;
    bool active;         /* process running when the viewer opened */
    bool has_exit_code;
    int exit_code;
    bool replay;         /* open in playback mode, from the start of time */
    bool attached;       /* entered from an attached console via the Ctrl-P
                            double-tap: another double-tap returns to it */
};

void hold_log_filter_options_init(struct hold_log_filter_options *opts);
void hold_log_filter_result_free(struct hold_log_filter_result *result);
int hold_log_filter_fd(int fd,
                         const struct hold_log_filter_options *opts,
                         struct hold_log_filter_result *result);
int hold_log_filter_backward_fd(int fd,
                                  const struct hold_log_filter_options *opts,
                                  off_t anchor_offset,
                                  size_t byte_budget,
                                  struct hold_log_filter_result *result);
int hold_log_viewer_tty_fd(int fd,
                             const char *title,
                             const struct hold_log_filter_options *opts,
                             const struct hold_log_viewer_follow *follow,
                             const struct hold_log_viewer_context *context,
                             bool debug_stats);

/* Screen-recording repaint: replay-from-nearest-clear (decision 5,
 * docs/future/playback.md). Scans a bounded window back from `end` for the
 * last screen-clearing escape (erase-display, alt-screen switch, RIS) and
 * emits a clear plus the recorded bytes after it, raw, onto out_fd —
 * reconstructing the screen as of byte `end`. Alt-screen switches and RIS
 * inside the emitted span are rewritten to plain clears so a replayed
 * recording can never yank the live terminal out of the caller's screen.
 * With no marker in the window: from_start re-emits the whole log at max
 * speed (returns 0), otherwise nothing is emitted and 1 is returned so the
 * caller can fall back to line-shaped output. -1 on I/O failure. */
int hold_log_screen_repaint(int log_fd, off_t end, bool from_start, int out_fd);

#endif /* HOLD_LOG_VIEWER_H */
