#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"
#include "hold/log_viewer.h"

static volatile sig_atomic_t g_tail_interrupted = 0;

enum signal_validation_state { SIGNAL_TARGET_RUNNING, SIGNAL_TARGET_EXITED };

static void handle_tail_sigint(int signo);
static int do_print_signal_command(const struct hold_store *store, const char *id, int sig);
static int validate_signal_target(const char *id,
                                  const struct hold_run_record *r,
                                  bool require_live,
                                  enum signal_validation_state *state_out);

static void handle_tail_sigint(int signo) {
    (void)signo;
    g_tail_interrupted = 1;
}

struct decoded_log_stream {
    char *line;
    size_t len;
    size_t cap;
};

static void decoded_log_stream_free(struct decoded_log_stream *s) {
    if (!s) return;
    free(s->line);
    s->line = NULL;
    s->len = 0;
    s->cap = 0;
}

static int decoded_log_stream_emit(struct decoded_log_stream *s) {
    if (!s || s->len == 0) return 0;
    char *line = malloc(s->len + 1);
    if (!line) return -1;
    memcpy(line, s->line, s->len);
    line[s->len] = '\0';
    char *decoded = NULL;
    int rc = hold_decode_json_log_line(line, &decoded);
    free(line);
    if (rc < 0) {
        free(decoded);
        return -1;
    }
    size_t n = strlen(decoded);
    int write_rc = n == 0 ? 0 : hold_write_all(STDOUT_FILENO, decoded, n);
    free(decoded);
    s->len = 0;
    return write_rc;
}

static int decoded_log_stream_write(struct decoded_log_stream *s, const char *buf, size_t n) {
    if (!s || (!buf && n > 0)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (s->len + 1 >= s->cap) {
            size_t next_cap = s->cap ? s->cap * 2 : 4096;
            while (next_cap <= s->len + 1) next_cap *= 2;
            char *next = realloc(s->line, next_cap);
            if (!next) return -1;
            s->line = next;
            s->cap = next_cap;
        }
        s->line[s->len++] = buf[i];
        if (buf[i] == '\n' && decoded_log_stream_emit(s) != 0) {
            return -1;
        }
    }
    return 0;
}

static int decoded_log_stream_flush(struct decoded_log_stream *s) {
    return decoded_log_stream_emit(s);
}

static int validate_signal_target(const char *id,
                                  const struct hold_run_record *r,
                                  bool require_live,
                                  enum signal_validation_state *state_out) {
    if (state_out) {
        *state_out = SIGNAL_TARGET_RUNNING;
    }
    if (r->pgid <= 1) {
        fprintf(stderr, "hold: error: invalid pgid %ld in record file\n", (long)r->pgid);
        return 5;
    }
    if (r->sid <= 0) {
        fprintf(stderr, "hold: error: invalid sid %ld in record file\n", (long)r->sid);
        return 5;
    }

    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    if (r->has_boot && have_boot && strcmp(r->boot_id, boot) != 0) {
        fprintf(stderr, "hold: error: call %s is stale and cannot be signaled\n", id);
        return 2;
    }

    bool have_identity_token = r->proc_starttime_ticks != 0 || (r->exe_dev != 0 && r->exe_ino != 0);
    char leader_state = 0;
    uint64_t leader_starttime = 0;
    bool have_leader_stat = hold_read_proc_stat_tokens(r->pid, &leader_state, &leader_starttime) == 0;
    if (have_leader_stat && leader_state != 'Z') {
        if (r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks) {
            fprintf(stderr, "hold: error: call %s process identity differs from the record and cannot be signaled\n", id);
            return 2;
        }
        if (r->proc_starttime_ticks == 0 && r->exe_dev != 0 && r->exe_ino != 0) {
            uint64_t exe_dev = 0;
            uint64_t exe_ino = 0;
            if (hold_read_proc_exe(r->pid, &exe_dev, &exe_ino) == 0 &&
                (exe_dev != r->exe_dev || exe_ino != r->exe_ino)) {
                fprintf(stderr, "hold: error: call %s process identity differs from the record and cannot be signaled\n", id);
                return 2;
            }
        }
        pid_t current_pgid = getpgid(r->pid);
        pid_t current_sid = getsid(r->pid);
        if ((current_pgid >= 0 && current_pgid != r->pgid) ||
            (current_sid >= 0 && current_sid != r->sid)) {
            fprintf(stderr, "hold: error: call %s process group/session differs from the record and cannot be signaled\n", id);
            return 2;
        }
        if (current_pgid >= 0 && current_sid >= 0) {
            if (!have_identity_token) {
                fprintf(stderr, "hold: error: call %s has no recorded process identity token and cannot be signaled\n", id);
                return 2;
            }
            return 0;
        }
    }
    if (have_leader_stat && r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks) {
        fprintf(stderr, "hold: error: call %s process identity differs from the record and cannot be signaled\n", id);
        return 2;
    }

    enum group_liveness gl = hold_group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_LIVE) {
        if (!have_identity_token) {
            fprintf(stderr, "hold: error: call %s has no recorded process identity token and cannot be signaled\n", id);
            return 2;
        }
        return 0;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        if (require_live) {
            fprintf(stderr, "hold: error: call %s is not running and cannot be printed as a signal command\n", id);
            return 2;
        }
        if (state_out) {
            *state_out = SIGNAL_TARGET_EXITED;
        }
        return 0;
    }

    fprintf(stderr, "hold: error: call %s could not be tied to its recorded process group/session and cannot be signaled\n", id);
    return 2;
}

int hold_tail_log_until_exit(const struct hold_run_record *r, bool from_end, bool follow_until_exit) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        hold_die_errno("hold: failed to open log for tail");
    }

    char boot[128] = {0};
    bool have_boot = r->has_boot && hold_current_boot_id(boot, sizeof(boot));
    if (from_end) {
        lseek(fd, 0, SEEK_END);
    }

    struct sigaction sa = {0}, old_sa = {0};
    sa.sa_handler = handle_tail_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);
    g_tail_interrupted = 0;

    char buf[4096];
    struct decoded_log_stream decoder = {0};
    int sleep_polls = 0;
    while (!g_tail_interrupted) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (decoded_log_stream_write(&decoder, buf, (size_t)n) != 0) {
                decoded_log_stream_free(&decoder);
                close(fd);
                sigaction(SIGINT, &old_sa, NULL);
                hold_die_errno("hold: failed writing tailed output");
            }
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            decoded_log_stream_free(&decoder);
            close(fd);
            sigaction(SIGINT, &old_sa, NULL);
            hold_die_errno("hold: failed while tailing log");
        }
        if (!follow_until_exit) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
        sleep_polls++;
        if (sleep_polls % 10 == 0) {
            enum run_state st = hold_eval_state(r, have_boot ? boot : NULL);
            if (st != STATE_RUNNING) {
                break;
            }
        }
    }

    if (decoded_log_stream_flush(&decoder) != 0) {
        decoded_log_stream_free(&decoder);
        close(fd);
        sigaction(SIGINT, &old_sa, NULL);
        hold_die_errno("hold: failed writing tailed output");
    }
    decoded_log_stream_free(&decoder);
    close(fd);
    sigaction(SIGINT, &old_sa, NULL);
    return 0;
}

int hold_do_signal_action(const struct hold_store *store, const char *id, int sig, bool graceful, bool *already_done) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (already_done) {
        *already_done = false;
    }
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum signal_validation_state signal_state = SIGNAL_TARGET_RUNNING;
    int validation_rc = validate_signal_target(id, &r, false, &signal_state);
    if (validation_rc != 0) {
        hold_free_run_record(&r);
        return validation_rc;
    }
    if (signal_state == SIGNAL_TARGET_EXITED) {
        if (already_done) {
            *already_done = true;
        }
        hold_free_run_record(&r);
        return 0;
    }

    if (kill(-r.pgid, sig) != 0) {
        if (errno == EPERM) {
            hold_free_run_record(&r);
            return 3;
        }
        if (errno == ESRCH) {
            if (already_done) {
                *already_done = true;
            }
            hold_free_run_record(&r);
            return 0;
        }
        hold_free_run_record(&r);
        return 4;
    }

    if (graceful) {
        if (hold_wait_target_group_gone(&r, STOP_TIMEOUT_MS)) {
            hold_report_session_escapees(&r);
            hold_free_run_record(&r);
            return 0;
        }
        signal_state = SIGNAL_TARGET_RUNNING;
        validation_rc = validate_signal_target(id, &r, false, &signal_state);
        if (validation_rc != 0) {
            hold_free_run_record(&r);
            return validation_rc;
        }
        if (signal_state == SIGNAL_TARGET_EXITED) {
            if (already_done) {
                *already_done = true;
            }
            hold_free_run_record(&r);
            return 0;
        }
        if (kill(-r.pgid, SIGKILL) != 0 && errno != ESRCH) {
            if (errno == EPERM) {
                hold_free_run_record(&r);
                return 3;
            }
            hold_free_run_record(&r);
            return 4;
        }
        if (hold_wait_target_group_gone(&r, 1000)) {
            hold_report_session_escapees(&r);
            hold_free_run_record(&r);
            return 0;
        }
        hold_free_run_record(&r);
        return 4;
    }
    hold_report_session_escapees(&r);
    hold_free_run_record(&r);
    return 0;
}

static int do_print_signal_command(const struct hold_store *store, const char *id, int sig) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum signal_validation_state signal_state = SIGNAL_TARGET_RUNNING;
    int validation_rc = validate_signal_target(id, &r, true, &signal_state);
    if (validation_rc != 0) {
        hold_free_run_record(&r);
        return validation_rc;
    }
    printf("kill -%s -- -%ld\n", sig == SIGKILL ? "KILL" : "TERM", (long)r.pgid);
    hold_free_run_record(&r);
    return 0;
}

int hold_cmd_signal_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd) {
    if (argc <= 0) {
        fprintf(stderr, "usage: hold %s <target>...\n", command);
        return 5;
    }
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    for (int i = 0; i < argc; i++) {
        struct hold_resolved_target *one = NULL;
        int none = 0;
        int rc = hold_resolve_action_token(inv, user_store, system_store, command, argv[i], all, &one, &none);
        if (rc != 0) {
            free(one);
            free(targets);
            return rc;
        }
        if (none > 0) {
            struct hold_resolved_target *next = realloc(targets, (size_t)(ntargets + none) * sizeof(*targets));
            if (!next) {
                free(one);
                free(targets);
                return 3;
            }
            targets = next;
            memcpy(targets + ntargets, one, (size_t)none * sizeof(*targets));
            ntargets += none;
        }
        free(one);
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to %s\n", command);
        return 0;
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            int rc = hold_report_requires_root(targets[i].id);
            free(targets);
            return rc;
        }
    }
    int worst = 0;
    for (int i = 0; i < ntargets; i++) {
        bool already_done = false;
        int rc = print_cmd ? do_print_signal_command(&targets[i].store, targets[i].id, sig)
                           : hold_do_signal_action(&targets[i].store, targets[i].id, sig, graceful, &already_done);
        if (!print_cmd && rc == 0) {
            if (already_done) {
                hold_sig_note(inv, "hold: %s already exited\n", targets[i].id);
            } else {
                hold_sig_note(inv, "hold: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", targets[i].id);
            }
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    free(targets);
    return worst;
}

int hold_cmd_tail_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "tail", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to tail\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "hold: record has no log path: %s\n", target.id);
        hold_free_run_record(&r);
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
    rc = hold_tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}

static bool parse_view_limit(const char *s, size_t *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < 1 || v > 10000) return false;
    *out = (size_t)v;
    return true;
}

static int print_view_result(const struct hold_log_filter_result *result, bool debug_stats) {
    for (size_t i = 0; i < result->line_count; i++) {
        size_t n = strlen(result->lines[i]);
        if (hold_write_all(STDOUT_FILENO, result->lines[i], n) != 0) {
            hold_die_errno("hold: failed writing viewed output");
        }
        if (n == 0 || result->lines[i][n - 1] != '\n') {
            if (hold_write_all(STDOUT_FILENO, "\n", 1) != 0) {
                hold_die_errno("hold: failed writing viewed output");
            }
        }
    }
    if (debug_stats) {
        fprintf(stderr,
                "hold logs viewer: bytes_read=%zu lines_scanned=%zu matches=%zu visible=%zu eof=%s\n",
                result->bytes_read,
                result->lines_scanned,
                result->match_count,
                result->line_count,
                result->reached_eof ? "yes" : "no");
    }
    return 0;
}

static int stream_view_result(const struct hold_log_filter_result *result) {
    return print_view_result(result, false);
}

struct interactive_view_liveness {
    const struct hold_run_record *record;
    char boot[128];
    bool have_boot;
};

static bool interactive_view_run_is_running(void *userdata) {
    struct interactive_view_liveness *live = userdata;
    if (!live || !live->record) return false;
    return hold_eval_state(live->record, live->have_boot ? live->boot : NULL) == STATE_RUNNING;
}

static int stream_view_follow_until_exit(int fd,
                                         const struct hold_run_record *r,
                                         const struct hold_log_filter_options *opts,
                                         bool from_end,
                                         bool follow_until_exit,
                                         bool debug_stats) {
    char boot[128] = {0};
    bool have_boot = r->has_boot && hold_current_boot_id(boot, sizeof(boot));
    if (from_end) {
        lseek(fd, 0, SEEK_END);
    }

    struct sigaction sa = {0}, old_sa = {0};
    sa.sa_handler = handle_tail_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);
    g_tail_interrupted = 0;

    int sleep_polls = 0;
    int rc = 0;
    while (!g_tail_interrupted) {
        struct hold_log_filter_result result;
        if (hold_log_filter_fd(fd, opts, &result) != 0) {
            sigaction(SIGINT, &old_sa, NULL);
            hold_die_errno("hold: failed while filtering followed log");
        }
        if (result.line_count > 0) {
            if (stream_view_result(&result) != 0) rc = -1;
            if (result.next_offset > 0) lseek(fd, result.next_offset, SEEK_SET);
            hold_log_filter_result_free(&result);
            if (rc != 0) break;
            continue;
        }
        bool reached_eof = result.reached_eof;
        hold_log_filter_result_free(&result);
        if (!follow_until_exit) {
            break;
        }
        if (!reached_eof) {
            continue;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
        sleep_polls++;
        if (sleep_polls % 10 == 0) {
            enum run_state st = hold_eval_state(r, have_boot ? boot : NULL);
            if (st != STATE_RUNNING) {
                struct hold_log_filter_result final_result;
                if (hold_log_filter_fd(fd, opts, &final_result) == 0) {
                    if (final_result.line_count > 0) rc = stream_view_result(&final_result);
                    if (debug_stats) {
                        fprintf(stderr,
                                "hold logs follow viewer: bytes_read=%zu lines_scanned=%zu matches=%zu visible=%zu eof=%s\n",
                                final_result.bytes_read,
                                final_result.lines_scanned,
                                final_result.match_count,
                                final_result.line_count,
                                final_result.reached_eof ? "yes" : "no");
                    }
                    hold_log_filter_result_free(&final_result);
                }
                break;
            }
        }
    }
    sigaction(SIGINT, &old_sa, NULL);
    if (rc != 0) hold_die_errno("hold: failed writing followed log output");
    return 0;
}

static int write_file_as_json_array_object(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (hold_write_all(STDOUT_FILENO, "[\n", 2) != 0) {
        close(fd);
        return -1;
    }
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            close(fd);
            return -1;
        }
    }
    if (hold_write_all(STDOUT_FILENO, "\n]\n", 3) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int hold_cmd_inspect_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to inspect\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (write_file_as_json_array_object(path) != 0) {
        hold_free_run_record(&r);
        free(targets);
        targets = NULL;
        hold_die_errno("hold: failed to inspect run record");
    }
    hold_free_run_record(&r);
    free(targets);
    return 0;
}

int hold_cmd_view_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           int argc,
                           char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: hold logs <target> [--follow|-f] [--plain|--interactive]\n");
        return 5;
    }

    struct hold_log_filter_options opts;
    hold_log_filter_options_init(&opts);
    bool debug_stats = false;
    bool force_plain = false;
    bool force_interactive = false;
    bool follow = false;
    const char *target_token = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--filter")) {
            if (++i >= argc) {
                fprintf(stderr, "usage: hold logs <target> [--follow|-f] [--plain|--interactive]\n");
                return 5;
            }
            opts.literal = argv[i];
        } else if (!strcmp(argv[i], "--similar")) {
            if (++i >= argc || opts.similar_example_count >= HOLD_LOG_VIEWER_MAX_EXAMPLES) {
                fprintf(stderr, "usage: hold logs <target> [--follow|-f] [--plain|--interactive]\n");
                return 5;
            }
            opts.similar_examples[opts.similar_example_count++] = argv[i];
        } else if (!strcmp(argv[i], "--limit") || !strcmp(argv[i], "--tail") || !strcmp(argv[i], "-n")) {
            if (++i >= argc || !parse_view_limit(argv[i], &opts.max_results)) {
                fprintf(stderr, "hold: error: invalid log line limit\n");
                return 5;
            }
            opts.visible_capacity = opts.max_results;
        } else if (!strncmp(argv[i], "--tail=", 7)) {
            if (!parse_view_limit(argv[i] + 7, &opts.max_results)) {
                fprintf(stderr, "hold: error: invalid log line limit\n");
                return 5;
            }
            opts.visible_capacity = opts.max_results;
        } else if (!strcmp(argv[i], "--debug-stats")) {
            debug_stats = true;
        } else if (!strcmp(argv[i], "--plain") || !strcmp(argv[i], "--print") || !strcmp(argv[i], "-p")) {
            force_plain = true;
        } else if (!strcmp(argv[i], "--interactive")) {
            force_interactive = true;
        } else if (!strcmp(argv[i], "--follow") || !strcmp(argv[i], "-f")) {
            follow = true;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: hold logs <target> [--follow|-f] [--plain|--interactive]\n");
            return 0;
        } else if (!target_token) {
            target_token = argv[i];
        } else {
            fprintf(stderr, "usage: hold logs <target> [--follow|-f] [--plain|--interactive]\n");
            return 5;
        }
    }
    if (force_plain && force_interactive) {
        fprintf(stderr, "hold: error: --plain and --interactive cannot be combined\n");
        return 5;
    }
    if (!target_token) {
        fprintf(stderr, "usage: hold logs <target> [--follow|-f] [--plain|--interactive]\n");
        return 5;
    }

    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "view", target_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to log\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    bool interactive = force_interactive || (!force_plain && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
    if (force_interactive && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr, "hold: error: --interactive requires a TTY\n");
        free(targets);
        return 5;
    }
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "hold: record has no log path: %s\n", target.id);
        hold_free_run_record(&r);
        free(targets);
        return 5;
    }
    int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        hold_free_run_record(&r);
        free(targets);
        hold_die_errno("hold: failed to open log");
    }
    if (interactive) {
        struct hold_log_viewer_follow follow_opts = {0};
        struct interactive_view_liveness live = {0};
        if (follow) {
            live.record = &r;
            live.have_boot = hold_current_boot_id(live.boot, sizeof(live.boot));
            follow_opts.enabled = true;
            follow_opts.is_running = interactive_view_run_is_running;
            follow_opts.userdata = &live;
            if (hold_eval_state(&r, live.have_boot ? live.boot : NULL) == STATE_RUNNING) {
                lseek(fd, 0, SEEK_END);
            }
        }
        struct hold_log_viewer_context viewer_context = {
            .run_id = r.id[0] ? r.id : target.id,
            .profile = r.has_alias ? r.alias : NULL,
            .command = r.cmdline,
            .log_path = r.log_path,
        };
        rc = hold_log_viewer_tty_fd(fd, target_token, &opts, follow ? &follow_opts : NULL, &viewer_context, debug_stats);
        if (rc != 0) {
            close(fd);
            hold_free_run_record(&r);
            free(targets);
            hold_die_errno("hold: failed while viewing log");
        }
    } else if (follow) {
        char boot[128] = {0};
        bool have_boot = hold_current_boot_id(boot, sizeof(boot));
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        rc = stream_view_follow_until_exit(fd, &r, &opts, st == STATE_RUNNING, st == STATE_RUNNING, debug_stats);
    } else {
        struct hold_log_filter_result result;
        if (hold_log_filter_fd(fd, &opts, &result) != 0) {
            close(fd);
            hold_free_run_record(&r);
            free(targets);
            hold_die_errno("hold: failed while filtering log");
        }
        rc = print_view_result(&result, debug_stats);
        hold_log_filter_result_free(&result);
    }
    close(fd);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}
