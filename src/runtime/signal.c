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
#include "hold/observe.h"

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

    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    if (r->has_boot && boot_id && strcmp(r->boot_id, boot_id) != 0) {
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

    /* Leader gone, group live: there is nothing left to compare the recorded
     * identity against, and no atomic verify-and-signal exists for a process
     * group. Matching the recorded pgid+sid within the same boot is the
     * deliberate best effort SPEC.md states; the zombie-leader test pins it. */
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
    const char *boot_id = r->has_boot ? hold_boot_id_or_null(boot) : NULL;
    if (from_end) {
        lseek(fd, 0, SEEK_END);
    }

    struct sigaction sa = {0}, old_sa = {0};
    sa.sa_handler = handle_tail_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);
    g_tail_interrupted = 0;

    /* Logs are raw captured bytes (the pre-0.5 JSON-lines encoding is no
     * longer decoded); tailing is a straight copy to stdout. */
    char buf[4096];
    int sleep_polls = 0;
    while (!g_tail_interrupted) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
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
            enum run_state st = hold_eval_state(r, boot_id);
            if (st != STATE_RUNNING) {
                break;
            }
        }
    }

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
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
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

/* Docker --tail semantics: the newest N records. Anchor at EOF and widen the
 * scan window until N lines are in view or the window covers the whole log;
 * the engine's byte-budget semantics stay untouched. */
static int view_backward_last_n(int fd,
                                const struct hold_log_filter_options *opts,
                                bool debug_stats,
                                off_t *resume_out) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) return -1;
    size_t budget = 1u << 20;
    struct hold_log_filter_result result;
    for (;;) {
        if (hold_log_filter_backward_fd(fd, opts, end, budget, &result) != 0) return -1;
        if (result.line_count >= opts->max_results || (off_t)budget >= end) break;
        hold_log_filter_result_free(&result);
        budget *= 4;
    }
    int rc = print_view_result(&result, debug_stats);
    if (resume_out) *resume_out = result.line_count > 0 ? result.next_offset : end;
    hold_log_filter_result_free(&result);
    return rc;
}

/* Plain output without a limit dumps the whole log, batch by batch, and
 * keeps the single debug-stats stderr line shape. */
static int view_forward_full(int fd, const struct hold_log_filter_options *opts, bool debug_stats) {
    struct hold_log_filter_result result;
    size_t bytes = 0, scanned = 0, matches = 0, visible = 0;
    bool eof = false;
    off_t off = 0;
    for (;;) {
        if (lseek(fd, off, SEEK_SET) < 0) return -1;
        if (hold_log_filter_fd(fd, opts, &result) != 0) return -1;
        bytes += result.bytes_read;
        scanned += result.lines_scanned;
        eof = result.reached_eof;
        if (result.line_count == 0) {
            hold_log_filter_result_free(&result);
            break;
        }
        matches += result.match_count;
        visible += result.line_count;
        int rc = print_view_result(&result, false);
        off = result.next_offset;
        hold_log_filter_result_free(&result);
        if (rc != 0) return rc;
    }
    if (debug_stats) {
        fprintf(stderr,
                "hold logs viewer: bytes_read=%zu lines_scanned=%zu matches=%zu visible=%zu eof=%s\n",
                bytes, scanned, matches, visible, eof ? "yes" : "no");
    }
    return 0;
}

struct interactive_view_liveness {
    const struct hold_run_record *record;
    const char *record_dir;
    char boot[128];
    bool have_boot;
};

static bool interactive_view_run_is_running(void *userdata) {
    struct interactive_view_liveness *live = userdata;
    if (!live || !live->record) return false;
    return hold_eval_state(live->record, live->have_boot ? live->boot : NULL) == STATE_RUNNING;
}

static bool interactive_view_exit_code(void *userdata, int *code_out) {
    struct interactive_view_liveness *live = userdata;
    if (!live || !live->record || !live->record_dir) return false;
    struct hold_run_record cur;
    char cur_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(live->record_dir, live->record->id, &cur, cur_path, sizeof(cur_path)) != 0) {
        return false;
    }
    bool have = cur.has_exit_code;
    if (have && code_out) *code_out = cur.exit_code;
    hold_free_run_record(&cur);
    return have;
}

static int stream_view_follow_until_exit(int fd,
                                         const struct hold_run_record *r,
                                         const struct hold_log_filter_options *opts,
                                         bool from_end,
                                         bool follow_until_exit,
                                         bool debug_stats) {
    char boot[128] = {0};
    const char *boot_id = r->has_boot ? hold_boot_id_or_null(boot) : NULL;
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
            enum run_state st = hold_eval_state(r, boot_id);
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

/* Build the neutral fd-target object for a live call's leader: where fds 0/1/2
 * actually point right now (a pipe, the pty, /dev/null, a redirected file).
 * Returns a heap string like "  \"Stdio\": {\n ... }" or NULL when the process
 * is gone or none of its fds could be read. Caller frees. */
static char *build_stdio_object(pid_t pid) {
    static const struct { int fd; const char *key; } slots[] = {
        {0, "Stdin"}, {1, "Stdout"}, {2, "Stderr"}
    };
    char targets[3][HOLD_PATH_MAX];
    bool any = false;
    for (size_t i = 0; i < 3; i++) {
        if (hold_proc_fd_target(pid, slots[i].fd, targets[i], sizeof(targets[i])) == 0) {
            any = true;
        } else {
            targets[i][0] = '\0';
        }
    }
    if (!any) {
        return NULL;
    }
    size_t cap = 4096;
    char *obj = malloc(cap);
    if (!obj) {
        return NULL;
    }
    FILE *f = fmemopen(obj, cap, "w");
    if (!f) {
        free(obj);
        return NULL;
    }
    fprintf(f, "  \"Stdio\": {\n");
    for (size_t i = 0; i < 3; i++) {
        fprintf(f, "    \"%s\": \"", slots[i].key);
        hold_json_escape(f, targets[i]);
        fprintf(f, "\"%s\n", i < 2 ? "," : "");
    }
    fprintf(f, "  }");
    if (fclose(f) != 0) {
        free(obj);
        return NULL;
    }
    return obj;
}

/* Print the record file as a one-object JSON array, splicing a live "Stdio"
 * key in before the object's closing brace. The record is a single top-level
 * object, so the last '}' in the file is its terminator. */
static int write_inspect_with_stdio(const char *path, const char *stdio_obj) {
    char *buf = NULL;
    if (hold_read_small_file(path, &buf) != 0 || !buf) {
        return -1;
    }
    char *close = strrchr(buf, '}');
    if (!close) {
        free(buf);
        return -1;
    }
    /* Trim whitespace before the closing brace so the spliced comma lands right
     * after the last existing value. */
    char *tail = close;
    while (tail > buf && (tail[-1] == '\n' || tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r')) {
        tail--;
    }
    *tail = '\0';
    int rc = 0;
    if (fputs("[\n", stdout) == EOF ||
        fputs(buf, stdout) == EOF ||
        fputs(",\n", stdout) == EOF ||
        fputs(stdio_obj, stdout) == EOF ||
        fputs("\n}\n]\n", stdout) == EOF) {
        rc = -1;
    }
    free(buf);
    return rc;
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
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
    /* Live fd targets are observable state, added only for a running call whose
     * leader still exists; ended calls fall back to the verbatim record. */
    char *stdio_obj = NULL;
    if (st == STATE_RUNNING && r.pid > 1) {
        stdio_obj = build_stdio_object(r.pid);
    }
    if (stdio_obj) {
        rc = write_inspect_with_stdio(path, stdio_obj);
        free(stdio_obj);
    } else {
        rc = write_file_as_json_array_object(path);
    }
    if (rc != 0) {
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
    bool have_limit = false;
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
            have_limit = true;
        } else if (!strncmp(argv[i], "--tail=", 7)) {
            if (!parse_view_limit(argv[i] + 7, &opts.max_results)) {
                fprintf(stderr, "hold: error: invalid log line limit\n");
                return 5;
            }
            opts.visible_capacity = opts.max_results;
            have_limit = true;
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
        live.record = &r;
        live.record_dir = target.store.record_dir;
        live.have_boot = hold_current_boot_id(live.boot, sizeof(live.boot));
        enum run_state view_state = hold_eval_state(&r, live.have_boot ? live.boot : NULL);
        /* The full-screen viewer follows live calls by default (the before-0.5
         * design: open at the newest output, FOLLOWING ACTIVE). Plain non-TTY
         * output keeps strict -f semantics for scripts. */
        if (follow || view_state == STATE_RUNNING) {
            follow_opts.enabled = true;
            follow_opts.is_running = interactive_view_run_is_running;
            follow_opts.exit_code = interactive_view_exit_code;
            follow_opts.userdata = &live;
            if (view_state == STATE_RUNNING) {
                lseek(fd, 0, SEEK_END);
            }
        }
        struct hold_log_viewer_context viewer_context = {
            .run_id = r.id[0] ? r.id : target.id,
            .name = r.has_name && r.name[0] ? r.name : NULL,
            .command = r.cmdline,
            .log_path = r.log_path,
            .active = view_state == STATE_RUNNING,
            .has_exit_code = r.has_exit_code,
            .exit_code = r.exit_code,
        };
        rc = hold_log_viewer_tty_fd(fd, target_token, &opts, follow_opts.enabled ? &follow_opts : NULL, &viewer_context, debug_stats);
        if (rc != 0) {
            close(fd);
            hold_free_run_record(&r);
            free(targets);
            hold_die_errno("hold: failed while viewing log");
        }
    } else if (follow) {
        char boot[128];
        const char *boot_id = hold_boot_id_or_null(boot);
        enum run_state st = hold_eval_state(&r, boot_id);
        if (have_limit) {
            off_t resume = 0;
            if (view_backward_last_n(fd, &opts, false, &resume) != 0 ||
                lseek(fd, resume, SEEK_SET) < 0) {
                close(fd);
                hold_free_run_record(&r);
                free(targets);
                hold_die_errno("hold: failed while filtering log");
            }
            rc = stream_view_follow_until_exit(fd, &r, &opts, false, st == STATE_RUNNING, debug_stats);
        } else {
            rc = stream_view_follow_until_exit(fd, &r, &opts, st == STATE_RUNNING, st == STATE_RUNNING, debug_stats);
        }
    } else if (have_limit) {
        if (view_backward_last_n(fd, &opts, debug_stats, NULL) != 0) {
            close(fd);
            hold_free_run_record(&r);
            free(targets);
            hold_die_errno("hold: failed while filtering log");
        }
        rc = 0;
    } else {
        if (view_forward_full(fd, &opts, debug_stats) != 0) {
            close(fd);
            hold_free_run_record(&r);
            free(targets);
            hold_die_errno("hold: failed while filtering log");
        }
        rc = 0;
    }
    close(fd);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}
