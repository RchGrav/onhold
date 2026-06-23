#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"

static volatile sig_atomic_t g_tail_interrupted = 0;

static void handle_tail_sigint(int signo);
static int do_print_signal_command(const struct sigmund_store *store, const char *id, int sig);

static void handle_tail_sigint(int signo) {
    (void)signo;
    g_tail_interrupted = 1;
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
    char path[SIGMUND_PATH_MAX], boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    if (already_done) {
        *already_done = false;
    }
    if (sigmund_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    if (r.pgid <= 1) {
        fprintf(stderr, "sigmund: error: invalid pgid %ld in record file\n", (long)r.pgid);
        return 5;
    }
    if (r.has_boot && have_boot && strcmp(r.boot_id, boot) != 0) {
        fprintf(stderr, "sigmund: error: run %s is stale (record boot_id differs from current boot)\n", id);
        return 2;
    }

    enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
    if (st == STATE_STALE) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }
    if (st == STATE_EXITED || st == STATE_FAILED) {
        if (already_done) {
            *already_done = true;
        }
        return 0;
    }
    if (st == STATE_UNKNOWN) {
        fprintf(stderr, "sigmund: error: run %s could not be validated and cannot be signaled\n", id);
        return 2;
    }

#ifdef SIGMUND_TESTING
    /* Force the signal-delivery failure branches so the exit-code contract
     * (3 = EPERM, 4 = delivery failed, 0 = already gone) is regression-tested
     * without needing to arrange a real kill() failure. */
    {
        const char *force_kill = getenv("SIGMUND_TEST_FORCE_KILL_RC");
        if (force_kill) {
            if (strcmp(force_kill, "eperm") == 0) {
                return 3;
            }
            if (strcmp(force_kill, "esrch") == 0) {
                if (already_done) {
                    *already_done = true;
                }
                return 0;
            }
            return 4;
        }
    }
#endif

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
    char path[SIGMUND_PATH_MAX], boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    if (sigmund_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    if (r.pgid <= 1) {
        fprintf(stderr, "sigmund: error: invalid pgid %ld in record file\n", (long)r.pgid);
        return 5;
    }
    enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
    if (r.has_boot && have_boot && strcmp(r.boot_id, boot) != 0) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }
    if (st == STATE_STALE) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }
    if (st == STATE_UNKNOWN) {
        fprintf(stderr, "sigmund: error: run %s could not be validated and cannot be signaled\n", id);
        return 2;
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
