#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/term.h"
#include "hold/console.h"
#include "hold/access.h"

/* runtime/start: launch orchestration — hashed-id reservation (via the store
 * reserve API), name generation, recipe capture, transactional record+index
 * write, redial reuse, restart supervision, docker-run exit parity. */

static volatile sig_atomic_t g_restart_stop = 0;

static void free_launch_and_observed_argv(char **launch_argv, char **observed_argv, int argc) {
    hold_free_argv_alloc(launch_argv, argc);
    hold_free_argv_alloc(observed_argv, argc);
}

static int enter_privileged_exec_cwd(void) {
    struct stat st;
    if (lstat("/", &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        errno = EPERM;
        return -1;
    }
    return chdir("/");
}

static void kill_supervisor_if_distinct(pid_t supervisor_pid, pid_t target_pid) {
    if (supervisor_pid > 1 && supervisor_pid != target_pid) {
        kill(supervisor_pid, SIGKILL);
        int st = 0;
        while (waitpid(supervisor_pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
}

/* ---- child environment ---- */

extern char **environ;

static int env_entry_key(const char *entry, char key[256]) {
    const char *eq = entry ? strchr(entry, '=') : NULL;
    if (!entry || !*entry || !eq || eq == entry || (size_t)(eq - entry) >= 256) {
        errno = EINVAL;
        return -1;
    }
    memcpy(key, entry, (size_t)(eq - entry));
    key[eq - entry] = '\0';
    return 0;
}

static int clear_process_environment(void) {
#if defined(__GLIBC__)
    return clearenv() == 0 ? 0 : -1;
#else
    char key[256];
    while (environ && environ[0]) {
        if (env_entry_key(environ[0], key) != 0) return -1;
        if (unsetenv(key) != 0) return -1;
    }
    return 0;
#endif
}

static int apply_child_env(int envc, char **env, bool clean_base) {
    if (clean_base) {
        if (clear_process_environment() != 0) return -1;
        if (setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0) return -1;
    }
    for (int i = 0; i < envc; i++) {
        const char *entry = env ? env[i] : NULL;
        char key[256];
        if (env_entry_key(entry, key) != 0) return -1;
        if (setenv(key, strchr(entry, '=') + 1, 1) != 0) return -1;
    }
    return 0;
}

/* ---- the child-side handshake and session preamble ---- */

/* THE errno-write handshake tail: any pre-exec failure rides the handshake
 * pipe as an errno, then _exit(127) — never a return into the caller. */
static _Noreturn void child_fail_errno(int handshake_fd) {
    int e = errno;
    (void)hold_write_all(handshake_fd, &e, sizeof(e));
    _exit(127);
}

/* THE spawn preamble, shared by the direct target and the restart
 * supervisor: new session, optionally pinned privileged cwd, child env,
 * optionally /dev/null stdin. */
static void child_session_setup(int handshake_fd,
                                bool pin_privileged_cwd,
                                int envc, char **env, bool clean_base,
                                bool null_stdin) {
    if (setsid() < 0) child_fail_errno(handshake_fd);
    if (pin_privileged_cwd && enter_privileged_exec_cwd() != 0) child_fail_errno(handshake_fd);
    if (apply_child_env(envc, env, clean_base) != 0) child_fail_errno(handshake_fd);
    if (null_stdin) {
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) child_fail_errno(handshake_fd);
        if (nullfd > STDERR_FILENO) close(nullfd);
    }
}

/* ---- restart policy ---- */

static void handle_restart_signal(int signo) {
    (void)signo;
    g_restart_stop = 1;
}

static bool restart_policy_is_enabled(const char *policy) {
    return policy && *policy && strcmp(policy, "no") != 0;
}

static bool restart_status_failed(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status) != 0;
    return WIFSIGNALED(status);
}

static bool restart_should_run_again(const char *policy, int status, int failures) {
    if (!policy || !*policy || strcmp(policy, "no") == 0) return false;
    if (strcmp(policy, "always") == 0 || strcmp(policy, "unless-stopped") == 0) return true;
    if (!strncmp(policy, "on-failure", 10) && (policy[10] == '\0' || policy[10] == ':')) {
        if (!restart_status_failed(status)) return false;
        if (policy[10] == ':' && policy[11]) {
            /* on-failure:N caps retries; an unparsable N means no cap. */
            char *end = NULL;
            long max_retries = strtol(policy + 11, &end, 10);
            if (end && !*end && max_retries >= 0 && max_retries <= INT_MAX && failures > max_retries) return false;
        }
        return true;
    }
    return false;
}

static void sleep_restart_delay(int seconds) {
    if (seconds <= 0) return;
    struct timespec sl = {.tv_sec = seconds, .tv_nsec = 0};
    while (!g_restart_stop && nanosleep(&sl, &sl) != 0 && errno == EINTR) {
        continue;
    }
}

/* Exit persistence: rc 1 (record purged after load) is FINAL — never
 * resurrect a call the user just removed. Only rc -1 retries (covers a
 * fast exit before the parent has written the record). */
static void mark_finished_with_retry(const struct hold_store *store, const char *id, int status) {
    for (int i = 0; i < 50; i++) {
        int rc = hold_mark_run_finished(store, id, status);
        if (rc == 0 || rc == 1) break;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
}

static void run_restart_supervisor(const struct hold_store *store,
                                   const char *id,
                                   int handshake_fd,
                                   bool interactive_stdin,
                                   int envc,
                                   char **env,
                                   bool clean_child_env,
                                   const char *resolved_exec_path,
                                   char **launch_argv,
                                   const char *restart_policy,
                                   int restart_delay_seconds,
                                   bool pin_privileged_cwd) {
    child_session_setup(handshake_fd, pin_privileged_cwd, envc, env, clean_child_env, !interactive_stdin);
    struct sigaction sa = {0};
    sa.sa_handler = handle_restart_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    close(handshake_fd);
    int failures = 0;
    int status = 0;
    bool have_status = false;
    while (!g_restart_stop) {
        pid_t worker = fork();
        if (worker < 0) {
            fprintf(stderr, "hold: restart supervisor failed to fork: %s\n", strerror(errno));
            mark_finished_with_retry(store, id, 127 << 8);
            _exit(127);
        }
        if (worker == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            execv(resolved_exec_path, launch_argv);
            fprintf(stderr, "hold: cannot restart '%s': %s\n", resolved_exec_path, strerror(errno));
            _exit(127);
        }
        while (waitpid(worker, &status, 0) < 0) {
            if (errno == EINTR) {
                if (g_restart_stop) kill(worker, SIGTERM);
                continue;
            }
            status = 127 << 8;
            break;
        }
        have_status = true;
        if (g_restart_stop) break;
        if (restart_status_failed(status)) failures++;
        else failures = 0;
        if (!restart_should_run_again(restart_policy, status, failures)) break;
        fprintf(stderr, "hold: restarting %s after exit status %d\n", resolved_exec_path, status);
        sleep_restart_delay(restart_delay_seconds);
    }
    if (have_status) mark_finished_with_retry(store, id, status);
    _exit(0);
}

/* Out-of-process pipe logger: drains the target's stdout/stderr pipes into
 * the indexed log. Closes both read ends in the launcher either way. */
static int spawn_log_capture(int stdout_fd, int stderr_fd, const char *log_path) {
    pid_t logger = fork();
    if (logger != 0) {
        int saved = errno;
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        errno = saved;
        return logger < 0 ? -1 : 0;
    }
    hold_close_stdio_to_devnull();
    int logfd = hold_open_append_no_symlink(log_path);
    if (logfd < 0) _exit(0);
    int idxfd = hold_open_log_index_fd(log_path, logfd);
    int fds[2] = {stdout_fd, stderr_fd};
    static const char *const streams[2] = {"stdout", "stderr"};
    while (fds[0] >= 0 || fds[1] >= 0) {
        struct pollfd pfds[2];
        int slot[2] = {-1, -1};
        int nfds = 0;
        for (int i = 0; i < 2; i++) {
            if (fds[i] < 0) continue;
            slot[i] = nfds;
            pfds[nfds].fd = fds[i];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (poll(pfds, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char buf[4096];
        for (int i = 0; i < 2; i++) {
            if (slot[i] < 0 || !(pfds[slot[i]].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
            ssize_t n = read(fds[i], buf, sizeof(buf));
            if (n > 0) {
                (void)hold_write_indexed_log_bytes_fd(logfd, idxfd, streams[i], buf, (size_t)n);
            } else {
                close(fds[i]);
                fds[i] = -1;
            }
        }
    }
    if (idxfd >= 0) close(idxfd);
    close(logfd);
    _exit(0);
}

/* docker run parity: a foreground launch exits with the held process's exit
 * status once it ends. An interrupted tail leaves the call held: exit 0. */
static int foreground_exit_status(const struct hold_store *store, const char *id) {
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    for (int i = 0; i < 50; i++) {
        struct hold_run_record r = {0};
        char path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) break;
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        bool marked = r.has_exit_code;
        int code = r.exit_code;
        hold_free_run_record(&r);
        if (st == STATE_RUNNING) return 0;
        if (marked) return code;
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
    fprintf(stderr, "hold: exit status unknown for call %.12s\n", id);
    return 1;
}

static void spawn_auto_remove_watcher(const struct hold_store *store, const struct hold_run_record *record) {
    if (!store || !record || !hold_valid_id(record->id)) {
        return;
    }
    pid_t watcher = fork();
    if (watcher != 0) {
        return;
    }
    hold_close_stdio_to_devnull();
    struct hold_store watch_store = *store;
    struct hold_run_record watch_record = *record;
    for (;;) {
        char boot[128];
        const char *boot_id = hold_boot_id_or_null(boot);
        enum run_state st = hold_eval_state(&watch_record, boot_id);
        if (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE) {
            bool removed = false;
            hold_prune_one_run(&watch_store, watch_record.id, boot_id, true, &removed);
            _exit(0);
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 200 * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
    }
}

/* ---- sudo-home store routing ---- */

bool hold_start_target_is_within_invoking_home(const struct hold_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv) {
    /* Only an explicit start target (bare `hold <cmd>` or `hold start <cmd>`)
     * is inspected; other owned verbs never route stores by argv. */
    if (argc <= 0 || !argv || !argv[0] || !*argv[0] ||
        (owned && !(command && strcmp(command, "start") == 0))) {
        return false;
    }
    const char *target = argv[0];

    const char *home = NULL;
    if (inv && inv->euid_root && inv->have_sudo_user && inv->invoking_home[0]) {
        home = inv->invoking_home;
    } else {
        home = getenv("HOME");
    }
    if (!home || !*home) {
        return false;
    }

    char resolved[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(target, resolved, sizeof(resolved)) != 0) {
        return false;
    }
    if (hold_path_is_within_dir(resolved, home)) {
        return true;
    }

    char cwd[HOLD_PATH_MAX] = {0};
    if (!getcwd(cwd, sizeof(cwd))) {
        cwd[0] = '\0';
    }
    for (int i = 1; i < argc; i++) {
        const char *arg = argv ? argv[i] : NULL;
        if (!arg || !*arg) {
            continue;
        }
        if (arg[0] == '-') {
            const char *eq = strchr(arg, '=');
            if (!eq || !eq[1]) {
                continue;
            }
            arg = eq + 1;
        }
        char path[HOLD_PATH_MAX];
        if (hold_resolve_existing_path_from_cwd(arg, cwd[0] ? cwd : NULL, path, sizeof(path)) == 0 &&
            hold_path_is_within_dir(path, home)) {
            return true;
        }
    }
    return false;
}

/* ---- the one launch unwind ---- */

/* Every launch failure rolls back through here: close what is open, kill and
 * reap the spawned group, then remove creation material (record, reserve,
 * owned log, console socket) and free the argv copies. Preserves errno. */
struct launch_undo {
    const struct hold_store *store;
    const char *id;           /* reserve to abort once reserved */
    bool reserved;
    const char *log_path;
    bool unlink_log;          /* owns_new_log, once launch material exists */
    const char *console_sock; /* unlinked when nonempty */
    const char *record_path;  /* unlinked when nonempty */
    int *fds;                 /* fd slots to close; -1 = already closed */
    int nfds;
    pid_t reap_pid;           /* rollback: pid to waitpid */
    pid_t group;              /* rollback: SIGKILL -group when > 0 */
    pid_t supervisor;
    char **launch_argv;
    char **observed_argv;
    int argc;
};

static void launch_undo(struct launch_undo *u) {
    int saved = errno;
    for (int i = 0; i < u->nfds; i++) {
        if (u->fds[i] >= 0) {
            close(u->fds[i]);
            u->fds[i] = -1;
        }
    }
    if (u->group > 0) {
        hold_rollback_spawned_group(u->reap_pid, u->group);
        kill_supervisor_if_distinct(u->supervisor, u->group);
    }
    if (u->record_path && u->record_path[0]) unlink(u->record_path);
    if (u->reserved) hold_abort_run_reservation(u->store, u->id);
    if (u->unlink_log && u->log_path[0]) unlink(u->log_path);
    if (u->console_sock[0]) unlink(u->console_sock);
    free_launch_and_observed_argv(u->launch_argv, u->observed_argv, u->argc);
    u->launch_argv = NULL;
    u->observed_argv = NULL;
    errno = saved;
}

static _Noreturn void launch_die(struct launch_undo *u, const char *msg) {
    launch_undo(u);
    hold_die_errno(msg);
}

static void copy_field(struct launch_undo *u, char *dst, size_t n, const char *src, const char *msg) {
    if (hold_checked_snprintf(dst, n, "%s", src) != 0) launch_die(u, msg);
}

/* ---- launch orchestration ---- */

int hold_perform_start_options(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_start_options *opts) {
    if (!opts) {
        hold_usage();
        return 5;
    }
    bool tail = opts->tail;
    bool console_mode = opts->console_mode;
    bool interactive_stdin = opts->interactive_stdin;
    int argc = opts->argc;
    char **argv = opts->argv;
    int envc = opts->envc;
    char **env = opts->env;
    const char *restart_policy = opts->restart_policy;
    const char *existing_id = opts->existing_id;
    if (argc <= 0 || !argv || !argv[0] ||
        envc < 0 || (envc > 0 && !env) ||
        opts->restart_delay_seconds < 0) {
        hold_usage();
        return 5;
    }
    bool restart_enabled = restart_policy_is_enabled(restart_policy);
    if (restart_enabled && console_mode) {
        fprintf(stderr, "hold: error: --restart with --tty/-t is not supported yet\n");
        return 5;
    }

    char resolved_exec_path[HOLD_PATH_MAX];
    const char *path_to_resolve = (opts->exec_path && *opts->exec_path) ? opts->exec_path : argv[0];
    if (hold_resolve_binary_path(path_to_resolve, resolved_exec_path, sizeof(resolved_exec_path)) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "hold: cannot start '%s': command not found\n", argv[0]);
        } else {
            fprintf(stderr, "hold: cannot start '%s': %s\n", argv[0], strerror(errno));
        }
        return 1;
    }

    char id[ID_STR_LEN] = {0}, log_path[HOLD_PATH_MAX] = {0}, console_sock[HOLD_PATH_MAX] = {0};
    char run_name[ALIAS_MAX_LEN + 1] = {0}, boot_id[128] = {0};
    /* Handshake, stdout, stderr, and target-pid pipe slots; adjacent R/W
     * pairs so hold_cloexec_pipe fills them in place and the unwind closes
     * whatever is still open. */
    enum { HS_R, HS_W, OUT_R, OUT_W, ERR_R, ERR_W, TP_R, TP_W, IO_N };
    int io[IO_N] = {-1, -1, -1, -1, -1, -1, -1, -1};
    struct launch_undo u = {
        .store = store,
        .id = id,
        .log_path = log_path,
        .console_sock = console_sock,
        .fds = io,
        .nfds = IO_N,
        .argc = argc,
    };

    char observed_cwd[HOLD_PATH_MAX] = {0};
    if (!getcwd(observed_cwd, sizeof(observed_cwd))) {
        observed_cwd[0] = '\0';
    }
    if (hold_copy_argv(&u.observed_argv, argc, argv) != 0) {
        hold_die_errno("hold: failed to prepare observed argv");
    }
    if (hold_copy_argv(&u.launch_argv, argc, argv) != 0) {
        launch_die(&u, "hold: failed to prepare argv");
    }
    char **observed_argv = u.observed_argv;
    char **launch_argv = u.launch_argv;
    free(launch_argv[0]);
    launch_argv[0] = strdup(resolved_exec_path);
    if (!launch_argv[0]) {
        launch_die(&u, "hold: failed to prepare argv");
    }
    if (hold_normalize_existing_argv_paths_from_cwd(launch_argv, argc, 1, observed_cwd[0] ? observed_cwd : NULL) != 0) {
        launch_die(&u, "hold: failed to normalize argv paths");
    }

    struct timespec start_ts;
    clock_gettime(CLOCK_REALTIME, &start_ts);
    int64_t start_unix_ns = (int64_t)start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec;
    bool has_boot = hold_current_boot_id(boot_id, sizeof(boot_id));
    bool restarting_existing = existing_id && *existing_id;
    bool owns_new_log = !restarting_existing;

    if (restarting_existing) {
        if (!hold_valid_id(existing_id) || !opts->existing_log_path || !*opts->existing_log_path) {
            errno = EINVAL;
            launch_die(&u, "hold: invalid restart record");
        }
        copy_field(&u, id, sizeof(id), existing_id, "hold: restart metadata too long");
        copy_field(&u, log_path, sizeof(log_path), opts->existing_log_path, "hold: restart metadata too long");
        if (opts->existing_run_name && *opts->existing_run_name) {
            copy_field(&u, run_name, sizeof(run_name), opts->existing_run_name, "hold: call name too long");
        }
    } else {
        if (hold_reserve_run_id(store, resolved_exec_path, argc, launch_argv,
                                observed_cwd[0] ? observed_cwd : NULL,
                                start_unix_ns, id) != 0) {
            launch_die(&u, "hold: failed to generate id");
        }
        u.reserved = true;
        int name_rc = hold_generate_run_name_for_id(store, id, opts->run_name, run_name);
        if (name_rc != 0) {
            launch_undo(&u);
            return name_rc;
        }
        if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
            launch_die(&u, "hold: log path too long");
        }
    }
    if (console_mode && hold_format_console_sock_path(store, id, console_sock, sizeof(console_sock)) != 0) {
        console_sock[0] = '\0';
        launch_die(&u, "hold: console socket path too long");
    }
    int log_preflight_fd = hold_open_append_no_symlink(log_path);
    if (log_preflight_fd < 0) {
        launch_die(&u, "hold: failed to open log");
    }
    close(log_preflight_fd);

    uid_t console_owner_uid = geteuid();
    bool console_have_allowed_peer_uid = inv && inv->euid_root && inv->have_sudo_user && inv->invoking_uid != console_owner_uid;
    uid_t console_allowed_peer_uid = console_have_allowed_peer_uid ? inv->invoking_uid : (uid_t)0;
    bool pin_privileged_cwd = inv && inv->euid_root && inv->have_sudo_user;
    bool clean_child_env = inv && inv->euid_root && store && store->kind == STORE_SYSTEM_MANAGED;

    /* Capture the invoking terminal size up front so the console broker can size
     * its PTY before the child execs. The broker itself runs with /dev/null stdio
     * and cannot query a terminal, so the size must be sampled here in the parent
     * while the real controlling terminal is still on our stdin/stdout. */
    unsigned short console_init_rows = 0, console_init_cols = 0;
    if (console_mode) {
        struct winsize start_ws;
        memset(&start_ws, 0, sizeof(start_ws));
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &start_ws) == 0 ||
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &start_ws) == 0) {
            console_init_rows = start_ws.ws_row;
            console_init_cols = start_ws.ws_col;
        }
    }

    if (hold_cloexec_pipe(io + HS_R) != 0) {
        launch_die(&u, "hold: pipe failed");
    }
    if (!console_mode &&
        (hold_cloexec_pipe(io + OUT_R) != 0 || hold_cloexec_pipe(io + ERR_R) != 0)) {
        launch_die(&u, "hold: pipe failed");
    }
    if (!restart_enabled && hold_cloexec_pipe(io + TP_R) != 0) {
        launch_die(&u, "hold: pipe failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        launch_die(&u, "hold: fork failed");
    }
    if (pid == 0) {
        close(io[HS_R]);
        if (io[OUT_R] >= 0) close(io[OUT_R]);
        if (io[ERR_R] >= 0) close(io[ERR_R]);
        if (io[TP_R] >= 0) close(io[TP_R]);
        if (restart_enabled) {
            if (dup2(io[OUT_W], STDOUT_FILENO) < 0 || dup2(io[ERR_W], STDERR_FILENO) < 0) {
                child_fail_errno(io[HS_W]);
            }
            if (io[OUT_W] > STDERR_FILENO) close(io[OUT_W]);
            if (io[ERR_W] > STDERR_FILENO) close(io[ERR_W]);
            run_restart_supervisor(store, id, io[HS_W], interactive_stdin,
                                   envc, env, clean_child_env,
                                   resolved_exec_path, launch_argv,
                                   restart_policy, opts->restart_delay_seconds,
                                   pin_privileged_cwd);
        }
        if (!console_mode) {
            /* Double-fork: this middle process becomes the waiter; the record
             * pid/pgid/sid are the TARGET's. */
            pid_t target = fork();
            if (target < 0) {
                child_fail_errno(io[HS_W]);
            }
            if (target != 0) {
                /* Write the target pid BEFORE closing the handshake pipe, so
                 * the parent never sees exec-success without a pid to read. */
                if (io[TP_W] >= 0) {
                    (void)hold_write_all(io[TP_W], &target, sizeof(target));
                    close(io[TP_W]);
                }
                if (io[OUT_W] >= 0) close(io[OUT_W]);
                if (io[ERR_W] >= 0) close(io[ERR_W]);
                close(io[HS_W]);
                hold_close_stdio_to_devnull();
                signal(SIGTERM, SIG_IGN);
                signal(SIGINT, SIG_IGN);
                signal(SIGHUP, SIG_IGN);
                int status = 0;
                while (waitpid(target, &status, 0) < 0) {
                    if (errno == EINTR) continue;
                    status = 127 << 8;
                    break;
                }
                mark_finished_with_retry(store, id, status);
                if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
                if (WIFSIGNALED(status)) _exit(128 + WTERMSIG(status));
                _exit(255);
            }
            if (io[TP_W] >= 0) close(io[TP_W]);
        }
        child_session_setup(io[HS_W], pin_privileged_cwd, envc, env, clean_child_env,
                            !console_mode && !interactive_stdin);
        if (console_mode) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd < 0 ||
                dup2(nullfd, STDIN_FILENO) < 0 ||
                dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(nullfd, STDERR_FILENO) < 0) {
                child_fail_errno(io[HS_W]);
            }
            if (nullfd > STDERR_FILENO) {
                close(nullfd);
            }
            hold_run_console_broker(io[HS_W], io[TP_W], store, id, log_path,
                                      console_sock, console_owner_uid,
                                      console_have_allowed_peer_uid,
                                      console_allowed_peer_uid,
                                      argc, launch_argv, resolved_exec_path,
                                      console_init_rows, console_init_cols);
            _exit(127);
        }
        if (dup2(io[OUT_W], STDOUT_FILENO) < 0 || dup2(io[ERR_W], STDERR_FILENO) < 0) {
            child_fail_errno(io[HS_W]);
        }
        if (io[OUT_W] > STDERR_FILENO) close(io[OUT_W]);
        if (io[ERR_W] > STDERR_FILENO) close(io[ERR_W]);
        execv(resolved_exec_path, launch_argv);
        child_fail_errno(io[HS_W]);
    }

    pid_t supervisor_pid = pid;
    u.reap_pid = pid;
    u.group = pid;
    u.supervisor = pid;
    u.unlink_log = owns_new_log;
    close(io[HS_W]);
    io[HS_W] = -1;
    for (int w = OUT_W; w < IO_N; w += 2) {
        if (io[w] >= 0) {
            close(io[w]);
            io[w] = -1;
        }
    }
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(io[HS_R], &child_errno);
    int handshake_errno = errno;
    close(io[HS_R]);
    io[HS_R] = -1;
    if (handshake < 0) {
        errno = handshake_errno;
        launch_die(&u, "hold: exec handshake failed");
    }
    if (handshake > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
        if (child_errno == ENOENT) {
            fprintf(stderr, "hold: cannot start '%s': command not found\n", launch_argv[0]);
        } else {
            fprintf(stderr, "hold: cannot start '%s': %s\n", launch_argv[0], strerror(child_errno));
        }
        u.group = 0; /* the child exited and is reaped — nothing to roll back */
        launch_undo(&u);
        return 1;
    }
    if (io[TP_R] >= 0) {
        pid_t target_pid = -1;
        ssize_t n = read(io[TP_R], &target_pid, sizeof(target_pid));
        close(io[TP_R]);
        io[TP_R] = -1;
        if (n != (ssize_t)sizeof(target_pid) || target_pid <= 1) {
            errno = EIO;
            launch_die(&u, "hold: failed to read target pid");
        }
        pid = target_pid;
        u.reap_pid = pid;
        u.group = pid;
    }

    if (!console_mode) {
        int logger_rc = spawn_log_capture(io[OUT_R], io[ERR_R], log_path);
        io[OUT_R] = -1;
        io[ERR_R] = -1;
        if (logger_rc != 0) {
            u.reap_pid = supervisor_pid;
            launch_die(&u, "hold: logger fork failed");
        }
    }

    struct hold_run_record r;
    hold_record_init_running(&r, id, log_path, pid, pid, pid, start_unix_ns,
                             opts->existing_created_unix_ns > 0 ? opts->existing_created_unix_ns : start_unix_ns);
    if (opts->existing_created_at && *opts->existing_created_at) {
        copy_field(&u, r.created_at, sizeof(r.created_at), opts->existing_created_at, "hold: created timestamp too long");
    }
    if (store->kind == STORE_SYSTEM_MANAGED) {
        r.has_invocation = true;
        bool via_sudo = inv && inv->have_sudo_user;
        r.invoked_by_uid = via_sudo ? inv->invoking_uid : 0;
        r.invoked_by_gid = via_sudo ? inv->invoking_gid : 0;
        r.invoked_via_sudo = via_sudo;
        copy_field(&u, r.invoked_by_user, sizeof(r.invoked_by_user),
                   via_sudo ? inv->invoking_user : "root", "hold: invoking user too long");
    }
    if (run_name[0]) {
        r.has_name = true;
        copy_field(&u, r.name, sizeof(r.name), run_name, "hold: call name too long");
    }
    if (console_sock[0]) {
        r.has_console = true;
        copy_field(&u, r.console_sock, sizeof(r.console_sock), console_sock, "hold: console socket path too long");
    }
    r.recipe.mode_interactive = interactive_stdin;
    r.recipe.mode_tty = console_mode;
    r.recipe.mode_detach = !tail && !console_mode;
    r.recipe.allow_multi = false;
    if (envc > 0 && hold_copy_argv(&r.recipe.env, envc, env) != 0) {
        launch_die(&u, "hold: failed to copy env metadata");
    }
    r.recipe.envc = envc;
    if (restart_enabled) {
        copy_field(&u, r.recipe.restart_policy, sizeof(r.recipe.restart_policy), restart_policy, "hold: restart policy too long");
        r.recipe.has_restart_policy = true;
    }
    r.recipe.restart_delay_seconds = opts->restart_delay_seconds;
    r.recipe.has_restart_delay = opts->restart_delay_seconds > 0;
    r.has_boot = has_boot;
    if (r.has_boot) {
        snprintf(r.boot_id, sizeof(r.boot_id), "%s", boot_id);
    }
    hold_read_proc_stat_tokens(pid, NULL, &r.proc_starttime_ticks);
    hold_read_proc_exe(pid, &r.exe_dev, &r.exe_ino);
    if (hold_format_argv_human(r.cmdline, sizeof(r.cmdline), argc, launch_argv) != 0) {
        snprintf(r.cmdline, sizeof(r.cmdline), "?");
    }
    r.has_observed = true;
    copy_field(&u, r.observed_exe, sizeof(r.observed_exe), resolved_exec_path, "hold: observed executable path too long");
    copy_field(&u, r.observed_cwd, sizeof(r.observed_cwd), observed_cwd, "hold: observed cwd too long");
    r.observed_argc = argc;
    r.observed_argv = observed_argv;

    char record_path[HOLD_PATH_MAX] = {0};
    if (getenv("HOLD_TEST_FAIL_RECORD_WRITE")) {
        errno = EIO;
        launch_die(&u, "hold: failed to write record");
    }
    if (hold_write_record_atomic(store->record_dir, &r, argc, launch_argv, record_path, sizeof(record_path)) != 0) {
        launch_die(&u, "hold: failed to write record");
    }
    if (!restarting_existing) {
        u.record_path = record_path;
    }

    if (store->kind == STORE_USER_LOCAL && inv && inv->euid_root && inv->have_sudo_user) {
        /* sudo-routed user-local launch: hand the artifacts back. */
        int chown_rc = 0;
        if (record_path[0] &&
            lchown(record_path, inv->invoking_uid, inv->invoking_gid) != 0) {
            chown_rc = -1;
        }
        if (lchown(log_path, inv->invoking_uid, inv->invoking_gid) != 0) {
            chown_rc = -1;
        }
        if (console_sock[0] && hold_path_exists(console_sock) &&
            lchown(console_sock, inv->invoking_uid, inv->invoking_gid) != 0) {
            chown_rc = -1;
        }
        if (chown_rc != 0) {
            if (errno == 0) errno = EIO;
            launch_die(&u, "hold: failed to set user-local ownership");
        }
    }
    if (store->kind == STORE_SYSTEM_MANAGED) {
        int public_rc = 0;
        if (getenv("HOLD_TEST_FAIL_PUBLIC_INDEX_WRITE")) {
            errno = EIO;
            public_rc = -1;
        } else if (hold_write_public_index_atomic(store, &r, NULL) != 0) {
            public_rc = -1;
        }
        if (public_rc != 0) {
            int saved = errno ? errno : EIO;
            char public_path[HOLD_PATH_MAX];
            if (hold_checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                unlink(public_path);
            }
            errno = saved;
            launch_die(&u, "hold: failed to write public index");
        }
    }

    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r.id, display_id);
    if (inv->docker_run) {
        /* Docker parity: detach prints the full ID alone; foreground prints nothing. */
        if (!tail) {
            printf("%s\n", r.id);
        }
    } else {
        printf("%s\n", display_id);
        hold_sig_note(inv,
                 "hold  started  %s   %s\n"
                 "         log      %s\n"
                 "         tail     hold tail %s\n"
                 "%s%s%s"
                 "         stop     hold stop %s\n",
                 display_id,
                 r.cmdline[0] ? r.cmdline : "?",
                 r.log_path,
                 display_id,
                 r.has_console ? "         console  hold console " : "",
                 r.has_console ? display_id : "",
                 r.has_console ? "\n" : "",
                 display_id);
    }
    fflush(stdout);

    if (tail) {
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        int tail_rc = 0;
        if (console_mode) {
            tail_rc = hold_run_native_console(r.console_sock, r.log_path, r.id,
                                              r.has_name && r.name[0] ? r.name : NULL);
        } else {
            tail_rc = hold_tail_log_until_exit(&r, false, true);
            if (tail_rc == 0) tail_rc = foreground_exit_status(store, r.id);
        }
        if (opts->auto_remove) {
            char boot[128];
            const char *watch_boot = hold_boot_id_or_null(boot);
            bool removed = false;
            int prune_rc = hold_prune_one_run(store, r.id, watch_boot, true, &removed);
            if (tail_rc == 0 && prune_rc != 0) tail_rc = prune_rc;
        }
        hold_free_argv_alloc(r.recipe.env, r.recipe.envc);
        return tail_rc;
    }
    if (opts->auto_remove) {
        spawn_auto_remove_watcher(store, &r);
    }
    hold_free_argv_alloc(r.recipe.env, r.recipe.envc);
    free_launch_and_observed_argv(launch_argv, observed_argv, argc);
    return 0;
}

int hold_ensure_start_store_for_command(const struct hold_invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct hold_store *store) {
    bool wants_system_store = (inv && inv->euid_root) || requested_system;

    if (wants_system_store &&
        hold_start_target_is_within_invoking_home(inv, owned, command, argc, argv)) {
        if (inv && inv->euid_root && inv->have_sudo_user) {
            return hold_ensure_invoking_user_store(inv, store);
        }
        return hold_ensure_user_store_for_current_user(store);
    }

    if (wants_system_store) {
        return hold_ensure_system_store(store);
    }
    return hold_ensure_user_store_for_current_user(store);
}
