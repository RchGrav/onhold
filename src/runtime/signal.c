#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"
#include "sigmund/log_viewer.h"

static volatile sig_atomic_t g_tail_interrupted = 0;

enum signal_validation_state { SIGNAL_TARGET_RUNNING, SIGNAL_TARGET_EXITED };

static void handle_tail_sigint(int signo);
static int do_print_signal_command(const struct sigmund_store *store, const char *id, int sig);
static int validate_signal_target(const char *id,
                                  const struct sigmund_run_record *r,
                                  bool require_live,
                                  enum signal_validation_state *state_out);

static void handle_tail_sigint(int signo) {
    (void)signo;
    g_tail_interrupted = 1;
}

static int validate_signal_target(const char *id,
                                  const struct sigmund_run_record *r,
                                  bool require_live,
                                  enum signal_validation_state *state_out) {
    if (state_out) {
        *state_out = SIGNAL_TARGET_RUNNING;
    }
    if (r->pgid <= 1) {
        fprintf(stderr, "sigmund: error: invalid pgid %ld in record file\n", (long)r->pgid);
        return 5;
    }
    if (r->sid <= 0) {
        fprintf(stderr, "sigmund: error: invalid sid %ld in record file\n", (long)r->sid);
        return 5;
    }

    char boot[128] = {0};
    if (!r->has_boot || !sigmund_current_boot_id(boot, sizeof(boot))) {
        fprintf(stderr, "sigmund: error: run %s could not be validated for signaling (missing boot_id)\n", id);
        return 2;
    }
    if (strcmp(r->boot_id, boot) != 0) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }

    char leader_state = 0;
    uint64_t leader_starttime = 0;
    bool have_leader_stat = sigmund_read_proc_stat_tokens(r->pid, &leader_state, &leader_starttime) == 0;
    if (have_leader_stat && leader_state != 'Z') {
        if (r->proc_starttime_ticks == 0 || leader_starttime != r->proc_starttime_ticks) {
            fprintf(stderr, "sigmund: error: run %s process identity differs from the record and cannot be signaled\n", id);
            return 2;
        }
        pid_t current_pgid = getpgid(r->pid);
        pid_t current_sid = getsid(r->pid);
        if (current_pgid != r->pgid || current_sid != r->sid) {
            fprintf(stderr, "sigmund: error: run %s process group/session differs from the record and cannot be signaled\n", id);
            return 2;
        }
        return 0;
    }
    if (have_leader_stat && r->proc_starttime_ticks != 0 && leader_starttime != r->proc_starttime_ticks) {
        fprintf(stderr, "sigmund: error: run %s process identity differs from the record and cannot be signaled\n", id);
        return 2;
    }

    enum group_liveness gl = sigmund_group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_LIVE) {
        return 0;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        if (require_live) {
            fprintf(stderr, "sigmund: error: run %s is not running and cannot be printed as a signal command\n", id);
            return 2;
        }
        if (state_out) {
            *state_out = SIGNAL_TARGET_EXITED;
        }
        return 0;
    }

    fprintf(stderr, "sigmund: error: run %s could not be tied to its recorded process group/session and cannot be signaled\n", id);
    return 2;
}

int sigmund_tail_log_until_exit(const struct sigmund_run_record *r, bool from_end, bool follow_until_exit) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        sigmund_die_errno("sigmund: failed to open log for tail");
    }

    char boot[128] = {0};
    bool have_boot = r->has_boot && sigmund_current_boot_id(boot, sizeof(boot));
    if (from_end) {
        lseek(fd, 0, SEEK_END);
    }

    struct sigaction sa = {0}, old_sa = {0};
    sa.sa_handler = handle_tail_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);
    g_tail_interrupted = 0;

    char buf[4096];
    int sleep_polls = 0;
    while (!g_tail_interrupted) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (sigmund_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
                close(fd);
                sigaction(SIGINT, &old_sa, NULL);
                sigmund_die_errno("sigmund: failed writing tailed output");
            }
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            sigaction(SIGINT, &old_sa, NULL);
            sigmund_die_errno("sigmund: failed while tailing log");
        }
        if (!follow_until_exit) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
        sleep_polls++;
        if (sleep_polls % 10 == 0) {
            enum run_state st = sigmund_eval_state(r, have_boot ? boot : NULL);
            if (st != STATE_RUNNING) {
                break;
            }
        }
    }

    close(fd);
    sigaction(SIGINT, &old_sa, NULL);
    return 0;
}

int sigmund_do_signal_action(const struct sigmund_store *store, const char *id, int sig, bool graceful, bool *already_done) {
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (already_done) {
        *already_done = false;
    }
    if (sigmund_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum signal_validation_state signal_state = SIGNAL_TARGET_RUNNING;
    int validation_rc = validate_signal_target(id, &r, false, &signal_state);
    if (validation_rc != 0) {
        return validation_rc;
    }
    if (signal_state == SIGNAL_TARGET_EXITED) {
        if (already_done) {
            *already_done = true;
        }
        return 0;
    }

    if (kill(-r.pgid, sig) != 0) {
        if (errno == EPERM) {
            return 3;
        }
        if (errno == ESRCH) {
            if (already_done) {
                *already_done = true;
            }
            return 0;
        }
        return 4;
    }

    if (graceful) {
        if (sigmund_wait_target_group_gone(&r, STOP_TIMEOUT_MS)) {
            sigmund_report_session_escapees(&r);
            return 0;
        }
        if (kill(-r.pgid, SIGKILL) != 0 && errno != ESRCH) {
            if (errno == EPERM) {
                return 3;
            }
            return 4;
        }
        if (sigmund_wait_target_group_gone(&r, 1000)) {
            sigmund_report_session_escapees(&r);
            return 0;
        }
        return 4;
    }
    sigmund_report_session_escapees(&r);
    return 0;
}

static int do_print_signal_command(const struct sigmund_store *store, const char *id, int sig) {
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum signal_validation_state signal_state = SIGNAL_TARGET_RUNNING;
    int validation_rc = validate_signal_target(id, &r, true, &signal_state);
    if (validation_rc != 0) {
        return validation_rc;
    }
    printf("kill -%s -- -%ld\n", sig == SIGKILL ? "KILL" : "TERM", (long)r.pgid);
    return 0;
}

int sigmund_cmd_signal_action(const struct sigmund_invocation *inv,
                             const struct sigmund_store *user_store,
                             const struct sigmund_store *system_store,
                             const char *program,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd) {
    if (argc <= 0) {
        fprintf(stderr, "usage: sigmund %s <target>...\n", command);
        return 5;
    }
    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    for (int i = 0; i < argc; i++) {
        struct sigmund_resolved_target *one = NULL;
        int none = 0;
        int rc = sigmund_resolve_action_token(inv, user_store, system_store, command, argv[i], all, &one, &none);
        if (rc != 0) {
            free(one);
            free(targets);
            return rc;
        }
        if (none > 0) {
            struct sigmund_resolved_target *next = realloc(targets, (size_t)(ntargets + none) * sizeof(*targets));
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
        sigmund_sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }
    bool need_elevation = false;
    for (int i = 0; i < ntargets; i++) {
        need_elevation = need_elevation || targets[i].needs_elevation;
    }
    if (need_elevation) {
        int rc = sigmund_elevate_with_sudo_targets(program, command, NULL, targets, ntargets, all, print_cmd);
        free(targets);
        return rc;
    }
    int worst = 0;
    for (int i = 0; i < ntargets; i++) {
        bool already_done = false;
        int rc = print_cmd ? do_print_signal_command(&targets[i].store, targets[i].id, sig)
                           : sigmund_do_signal_action(&targets[i].store, targets[i].id, sig, graceful, &already_done);
        if (!print_cmd && rc == 0) {
            if (already_done) {
                sigmund_sig_note(inv, "sigmund: %s already exited\n", targets[i].id);
            } else {
                sigmund_sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", targets[i].id);
            }
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    free(targets);
    return worst;
}

int sigmund_cmd_tail_action(const struct sigmund_invocation *inv,
                           const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *program,
                           const char *id_token) {
    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = sigmund_resolve_action_token(inv, user_store, system_store, "tail", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sigmund_sig_note(inv, "sigmund: nothing to tail\n");
        return 0;
    }
    struct sigmund_resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = sigmund_elevate_with_sudo_targets(program, "tail", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
    rc = sigmund_tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
    free(targets);
    return rc;
}

int sigmund_cmd_dump_action(const struct sigmund_invocation *inv,
                           const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *program,
                           const char *id_token) {
    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = sigmund_resolve_action_token(inv, user_store, system_store, "dump", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sigmund_sig_note(inv, "sigmund: nothing to dump\n");
        return 0;
    }
    struct sigmund_resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = sigmund_elevate_with_sudo_targets(program, "dump", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        free(targets);
        return 5;
    }
    int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        free(targets);
        sigmund_die_errno("sigmund: failed to open log for dump");
    }
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            free(targets);
            sigmund_die_errno("sigmund: failed while dumping log");
        }
        if (sigmund_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            close(fd);
            free(targets);
            sigmund_die_errno("sigmund: failed writing dumped output");
        }
    }
    close(fd);
    free(targets);
    return 0;
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

static int print_view_result(const struct sigmund_log_filter_result *result, bool debug_stats) {
    for (size_t i = 0; i < result->line_count; i++) {
        size_t n = strlen(result->lines[i]);
        if (sigmund_write_all(STDOUT_FILENO, result->lines[i], n) != 0) {
            sigmund_die_errno("sigmund: failed writing viewed output");
        }
        if (n == 0 || result->lines[i][n - 1] != '\n') {
            if (sigmund_write_all(STDOUT_FILENO, "\n", 1) != 0) {
                sigmund_die_errno("sigmund: failed writing viewed output");
            }
        }
    }
    if (debug_stats) {
        fprintf(stderr,
                "mund view: bytes_read=%zu lines_scanned=%zu matches=%zu visible=%zu eof=%s\n",
                result->bytes_read,
                result->lines_scanned,
                result->match_count,
                result->line_count,
                result->reached_eof ? "yes" : "no");
    }
    return 0;
}

static int stream_view_result(const struct sigmund_log_filter_result *result) {
    return print_view_result(result, false);
}

struct interactive_view_liveness {
    const struct sigmund_run_record *record;
    char boot[128];
    bool have_boot;
};

static bool interactive_view_run_is_running(void *userdata) {
    struct interactive_view_liveness *live = userdata;
    if (!live || !live->record) return false;
    return sigmund_eval_state(live->record, live->have_boot ? live->boot : NULL) == STATE_RUNNING;
}

static int stream_view_follow_until_exit(int fd,
                                         const struct sigmund_run_record *r,
                                         const struct sigmund_log_filter_options *opts,
                                         bool from_end,
                                         bool follow_until_exit,
                                         bool debug_stats) {
    char boot[128] = {0};
    bool have_boot = r->has_boot && sigmund_current_boot_id(boot, sizeof(boot));
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
        struct sigmund_log_filter_result result;
        if (sigmund_log_filter_fd(fd, opts, &result) != 0) {
            sigaction(SIGINT, &old_sa, NULL);
            sigmund_die_errno("sigmund: failed while filtering followed log");
        }
        if (result.line_count > 0) {
            if (stream_view_result(&result) != 0) rc = -1;
            if (result.next_offset > 0) lseek(fd, result.next_offset, SEEK_SET);
            sigmund_log_filter_result_free(&result);
            if (rc != 0) break;
            continue;
        }
        bool reached_eof = result.reached_eof;
        sigmund_log_filter_result_free(&result);
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
            enum run_state st = sigmund_eval_state(r, have_boot ? boot : NULL);
            if (st != STATE_RUNNING) {
                struct sigmund_log_filter_result final_result;
                if (sigmund_log_filter_fd(fd, opts, &final_result) == 0) {
                    if (final_result.line_count > 0) rc = stream_view_result(&final_result);
                    if (debug_stats) {
                        fprintf(stderr,
                                "mund view follow: bytes_read=%zu lines_scanned=%zu matches=%zu visible=%zu eof=%s\n",
                                final_result.bytes_read,
                                final_result.lines_scanned,
                                final_result.match_count,
                                final_result.line_count,
                                final_result.reached_eof ? "yes" : "no");
                    }
                    sigmund_log_filter_result_free(&final_result);
                }
                break;
            }
        }
    }
    sigaction(SIGINT, &old_sa, NULL);
    if (rc != 0) sigmund_die_errno("sigmund: failed writing followed view output");
    return 0;
}

static int elevate_view_target(const char *program,
                               const struct sigmund_resolved_target *target,
                               const struct sigmund_log_filter_options *opts,
                               bool debug_stats,
                               bool interactive,
                               bool follow) {
    const char *prefix = target->scope == RESOLVE_SYSTEM_MANAGED ? "system:" : "user:";
    char token[sizeof("system:") + sizeof(target->id)];
    snprintf(token, sizeof(token), "%s%s", prefix, target->id);

    int canonical_argc = 2 + (opts->literal ? 2 : 0) + (int)(opts->similar_example_count * 2) +
                         (opts->max_results ? 2 : 0) + (debug_stats ? 1 : 0) + (interactive ? 1 : 0) +
                         (follow ? 1 : 0);
    char **canon = calloc((size_t)canonical_argc, sizeof(char *));
    char limit_buf[32];
    if (!canon) return 3;

    int n = 0;
    canon[n++] = "view";
    canon[n++] = token;
    if (opts->literal) {
        canon[n++] = "--filter";
        canon[n++] = (char *)opts->literal;
    }
    for (size_t i = 0; i < opts->similar_example_count; i++) {
        canon[n++] = "--similar";
        canon[n++] = (char *)opts->similar_examples[i];
    }
    if (opts->max_results) {
        snprintf(limit_buf, sizeof(limit_buf), "%zu", opts->max_results);
        canon[n++] = "--limit";
        canon[n++] = limit_buf;
    }
    if (debug_stats) {
        canon[n++] = "--debug-stats";
    }
    if (interactive) {
        canon[n++] = "--interactive";
    }
    if (follow) {
        canon[n++] = "--follow";
    }
    int rc = sigmund_elevate_with_sudo_canonical(program, n, canon);
    free(canon);
    return rc;
}

int sigmund_cmd_view_action(const struct sigmund_invocation *inv,
                           const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *program,
                           int argc,
                           char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--follow|-f] [--plain|--interactive] [--debug-stats]\n");
        return 5;
    }

    struct sigmund_log_filter_options opts;
    sigmund_log_filter_options_init(&opts);
    bool debug_stats = false;
    bool force_plain = false;
    bool force_interactive = false;
    bool follow = false;
    const char *target_token = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--filter")) {
            if (++i >= argc) {
                fprintf(stderr, "usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--follow|-f] [--plain|--interactive] [--debug-stats]\n");
                return 5;
            }
            opts.literal = argv[i];
        } else if (!strcmp(argv[i], "--similar")) {
            if (++i >= argc || opts.similar_example_count >= SIGMUND_LOG_VIEWER_MAX_EXAMPLES) {
                fprintf(stderr, "usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--follow|-f] [--plain|--interactive] [--debug-stats]\n");
                return 5;
            }
            opts.similar_examples[opts.similar_example_count++] = argv[i];
        } else if (!strcmp(argv[i], "--limit")) {
            if (++i >= argc || !parse_view_limit(argv[i], &opts.max_results)) {
                fprintf(stderr, "sigmund: error: invalid --limit\n");
                return 5;
            }
            opts.visible_capacity = opts.max_results;
        } else if (!strcmp(argv[i], "--debug-stats")) {
            debug_stats = true;
        } else if (!strcmp(argv[i], "--plain")) {
            force_plain = true;
        } else if (!strcmp(argv[i], "--interactive")) {
            force_interactive = true;
        } else if (!strcmp(argv[i], "--follow") || !strcmp(argv[i], "-f")) {
            follow = true;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--follow|-f] [--plain|--interactive] [--debug-stats]\n");
            return 0;
        } else if (!target_token) {
            target_token = argv[i];
        } else {
            fprintf(stderr, "usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--follow|-f] [--plain|--interactive] [--debug-stats]\n");
            return 5;
        }
    }
    if (force_plain && force_interactive) {
        fprintf(stderr, "sigmund: error: --plain and --interactive cannot be combined\n");
        return 5;
    }
    if (!target_token) {
        fprintf(stderr, "usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--follow|-f] [--plain|--interactive] [--debug-stats]\n");
        return 5;
    }

    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = sigmund_resolve_action_token(inv, user_store, system_store, "view", target_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sigmund_sig_note(inv, "sigmund: nothing to view\n");
        return 0;
    }
    struct sigmund_resolved_target target = targets[0];
    bool interactive = force_interactive || (!force_plain && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
    if (force_interactive && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr, "sigmund: error: --interactive requires a TTY\n");
        free(targets);
        return 5;
    }
    if (target.needs_elevation) {
        rc = elevate_view_target(program, &target, &opts, debug_stats, interactive, follow);
        free(targets);
        return rc;
    }
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        free(targets);
        return 5;
    }
    int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        free(targets);
        sigmund_die_errno("sigmund: failed to open log for view");
    }
    if (interactive) {
        struct sigmund_log_viewer_follow follow_opts = {0};
        struct interactive_view_liveness live = {0};
        if (follow) {
            live.record = &r;
            live.have_boot = sigmund_current_boot_id(live.boot, sizeof(live.boot));
            follow_opts.enabled = true;
            follow_opts.is_running = interactive_view_run_is_running;
            follow_opts.userdata = &live;
            if (sigmund_eval_state(&r, live.have_boot ? live.boot : NULL) == STATE_RUNNING) {
                lseek(fd, 0, SEEK_END);
            }
        }
        rc = sigmund_log_viewer_tty_fd(fd, target_token, &opts, follow ? &follow_opts : NULL, debug_stats);
        if (rc != 0) {
            close(fd);
            free(targets);
            sigmund_die_errno("sigmund: failed while viewing log");
        }
    } else if (follow) {
        char boot[128] = {0};
        bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
        enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
        rc = stream_view_follow_until_exit(fd, &r, &opts, st == STATE_RUNNING, st == STATE_RUNNING, debug_stats);
    } else {
        struct sigmund_log_filter_result result;
        if (sigmund_log_filter_fd(fd, &opts, &result) != 0) {
            close(fd);
            free(targets);
            sigmund_die_errno("sigmund: failed while filtering log");
        }
        rc = print_view_result(&result, debug_stats);
        sigmund_log_filter_result_free(&result);
    }
    close(fd);
    free(targets);
    return rc;
}
