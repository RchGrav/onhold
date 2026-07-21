#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"
#if defined(__linux__)
#include <sys/syscall.h>
#endif

static volatile sig_atomic_t g_restart_stop = 0;

static const char *explicit_start_argv0(bool owned, const char *command, int argc, char **argv);
static int apply_child_env(int envc, char **env, bool clean_base);
static int perform_start_with_metadata_name_options_internal(const struct hold_invocation *inv,
                                                             const struct hold_store *store,
                                                             const struct hold_start_options *opts);

static int reserve_hashed_run_id(const struct hold_store *store,
                                 const char *resolved_exec_path,
                                 int argc,
                                 char **argv,
                                 const char *cwd,
                                 int64_t start_unix_ns,
                                 char out[ID_STR_LEN]);
static int spawn_log_capture(int stdout_fd,
                             int stderr_fd,
                             const char *log_path);
static void close_stdio_to_devnull(void);
static int enter_privileged_exec_cwd(void);
static void spawn_auto_remove_watcher(const struct hold_store *store, const struct hold_run_record *record);

static void free_launch_and_observed_argv(char **launch_argv, char **observed_argv, int argc) {
    hold_free_argv_alloc(launch_argv, argc);
    hold_free_argv_alloc(observed_argv, argc);
}

static void unlink_if_nonempty(const char *path) {
    if (path && *path) {
        unlink(path);
    }
}

static int open_log_append_no_symlink(const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    size_t dir_len = (size_t)(slash - path);
    if (dir_len >= HOLD_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char dir[HOLD_PATH_MAX];
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    int fd = openat(dirfd, slash + 1, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    int saved = errno;
    close(dirfd);
    if (fd < 0) {
        errno = saved;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
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

static int restart_policy_max_retries(const char *policy) {
    const char *colon = policy ? strchr(policy, ':') : NULL;
    if (!colon || !colon[1]) return -1;
    char *end = NULL;
    long n = strtol(colon + 1, &end, 10);
    if (!end || *end || n < 0 || n > INT_MAX) return -2;
    return (int)n;
}

static bool restart_should_run_again(const char *policy, int status, int failures) {
    if (!policy || !*policy || strcmp(policy, "no") == 0) return false;
    if (strcmp(policy, "always") == 0 || strcmp(policy, "unless-stopped") == 0) return true;
    if (!strncmp(policy, "on-failure", 10) && (policy[10] == '\0' || policy[10] == ':')) {
        if (!restart_status_failed(status)) return false;
        int max_retries = restart_policy_max_retries(policy);
        if (max_retries >= 0 && failures > max_retries) return false;
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

static bool run_id_material_exists(const struct hold_store *store, const char *id) {
    char path[HOLD_PATH_MAX];
    return (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && hold_path_exists(path)) ||
           (hold_checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && hold_path_exists(path)) ||
           (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0 && hold_path_exists(path)) ||
           (store->console_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && hold_path_exists(path)) ||
           (store->public_dir[0] &&
            hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && hold_path_exists(path));
}

static void hash_field(struct sha256_ctx *ctx, const char *key, const char *value) {
    hold_sha256_update_nul_field(ctx, key);
    hold_sha256_update_nul_field(ctx, value ? value : "-");
}

static void compute_run_hash(const char *resolved_exec_path,
                             int argc,
                             char **argv,
                             const char *cwd,
                             int64_t start_unix_ns,
                             unsigned long counter,
                             char out[ID_STR_LEN]) {
    struct sha256_ctx ctx;
    unsigned char digest[32];
    char buf[64];
    hold_sha256_init(&ctx);
    hash_field(&ctx, "version", "hold-run-v1");
    hash_field(&ctx, "exe", resolved_exec_path);
    hash_field(&ctx, "cwd", cwd && *cwd ? cwd : "-");
    snprintf(buf, sizeof(buf), "%" PRId64, start_unix_ns);
    hash_field(&ctx, "timestamp_ns", buf);
    snprintf(buf, sizeof(buf), "%ld", (long)getpid());
    hash_field(&ctx, "launcher_pid", buf);
    snprintf(buf, sizeof(buf), "%d", argc);
    hash_field(&ctx, "argc", buf);
    for (int i = 0; i < argc; i++) {
        snprintf(buf, sizeof(buf), "argv[%d]", i);
        hash_field(&ctx, buf, argv && argv[i] ? argv[i] : "");
    }
    snprintf(buf, sizeof(buf), "%lu", counter);
    hash_field(&ctx, "counter", buf);
    hold_sha256_final(&ctx, digest);
    hold_hex_encode(digest, sizeof(digest), out, ID_STR_LEN);
}

static int reserve_hashed_run_id(const struct hold_store *store,
                                 const char *resolved_exec_path,
                                 int argc,
                                 char **argv,
                                 const char *cwd,
                                 int64_t start_unix_ns,
                                 char out[ID_STR_LEN]) {
    char reserve[HOLD_PATH_MAX];
    for (unsigned long counter = 0; counter < 1024; counter++) {
        compute_run_hash(resolved_exec_path, argc, argv, cwd, start_unix_ns, counter, out);
        if (!hold_valid_id(out) || run_id_material_exists(store, out)) continue;
        if (hold_checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", store->record_dir, out) != 0) return -1;
        int fd = open(reserve, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

static void logger_write_bytes(int logfd,
                               int idxfd,
                               const char *stream,
                               const char *buf,
                               size_t n) {
    (void)hold_write_indexed_log_bytes_fd(logfd, idxfd, stream, buf, n);
}

static int spawn_log_capture(int stdout_fd,
                             int stderr_fd,
                             const char *log_path) {
    pid_t logger = fork();
    if (logger < 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return -1;
    }
    if (logger > 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return 0;
    }
    close_stdio_to_devnull();
    int logfd = open_log_append_no_symlink(log_path);
    if (logfd < 0) _exit(0);
    int idxfd = hold_open_log_index_fd(log_path, logfd);
    while (stdout_fd >= 0 || stderr_fd >= 0) {
        struct pollfd pfds[2];
        int nfds = 0;
        int stdout_slot = -1;
        int stderr_slot = -1;
        if (stdout_fd >= 0) {
            stdout_slot = nfds;
            pfds[nfds].fd = stdout_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (stderr_fd >= 0) {
            stderr_slot = nfds;
            pfds[nfds].fd = stderr_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (nfds == 0) break;
        int sr = poll(pfds, (nfds_t)nfds, -1);
        if (sr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char buf[4096];
        if (stdout_slot >= 0 && (pfds[stdout_slot].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            ssize_t n = read(stdout_fd, buf, sizeof(buf));
            if (n > 0) logger_write_bytes(logfd, idxfd, "stdout", buf, (size_t)n);
            else { close(stdout_fd); stdout_fd = -1; }
        }
        if (stderr_slot >= 0 && (pfds[stderr_slot].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            ssize_t n = read(stderr_fd, buf, sizeof(buf));
            if (n > 0) logger_write_bytes(logfd, idxfd, "stderr", buf, (size_t)n);
            else { close(stderr_fd); stderr_fd = -1; }
        }
    }
    if (idxfd >= 0) close(idxfd);
    close(logfd);
    _exit(0);
}

static void run_restart_supervisor(int handshake_fd,
                                   const char *log_path,
                                   bool interactive_stdin,
                                   int envc,
                                   char **env,
                                   bool clean_child_env,
                                   const char *resolved_exec_path,
                                   char **launch_argv,
                                   const char *restart_policy,
                                   int restart_delay_seconds,
                                   bool pin_privileged_cwd) {
    (void)log_path;
    if (setsid() < 0) {
        int e = errno;
        hold_write_all(handshake_fd, &e, sizeof(e));
        _exit(127);
    }
    if (pin_privileged_cwd && enter_privileged_exec_cwd() != 0) {
        int e = errno;
        hold_write_all(handshake_fd, &e, sizeof(e));
        _exit(127);
    }
    if (apply_child_env(envc, env, clean_child_env) != 0) {
        int e = errno;
        hold_write_all(handshake_fd, &e, sizeof(e));
        _exit(127);
    }
    if (!interactive_stdin) {
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) {
            int e = errno;
            hold_write_all(handshake_fd, &e, sizeof(e));
            _exit(127);
        }
        if (nullfd > 2) close(nullfd);
    }
    struct sigaction sa = {0};
    sa.sa_handler = handle_restart_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    close(handshake_fd);
    int failures = 0;
    while (!g_restart_stop) {
        pid_t worker = fork();
        if (worker < 0) {
            fprintf(stderr, "hold: restart supervisor failed to fork: %s\n", strerror(errno));
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
        int status = 0;
        while (waitpid(worker, &status, 0) < 0) {
            if (errno == EINTR) {
                if (g_restart_stop) kill(worker, SIGTERM);
                continue;
            }
            status = 127 << 8;
            break;
        }
        if (g_restart_stop) break;
        if (restart_status_failed(status)) failures++;
        else failures = 0;
        if (!restart_should_run_again(restart_policy, status, failures)) break;
        fprintf(stderr, "hold: restarting %s after exit status %d\n", resolved_exec_path, status);
        sleep_restart_delay(restart_delay_seconds);
    }
    _exit(0);
}

static const char *explicit_start_argv0(bool owned, const char *command, int argc, char **argv) {
    if (argc <= 0 || !argv || !argv[0]) {
        return NULL;
    }
    if (!owned) {
        return argv[0];
    }
    if (command && strcmp(command, "start") == 0) {
        return argv[0];
    }
    return NULL;
}

extern char **environ;

static int clear_process_environment(void) {
#if defined(__GLIBC__)
    if (clearenv() != 0) return -1;
    return 0;
#else
    while (environ && environ[0]) {
        const char *entry = environ[0];
        const char *eq = strchr(entry, '=');
        if (!eq || eq == entry) {
            errno = EINVAL;
            return -1;
        }
        size_t key_len = (size_t)(eq - entry);
        char key[256];
        if (key_len >= sizeof(key)) {
            errno = EINVAL;
            return -1;
        }
        memcpy(key, entry, key_len);
        key[key_len] = '\0';
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
        const char *eq = entry ? strchr(entry, '=') : NULL;
        if (!entry || !*entry || !eq || eq == entry) {
            errno = EINVAL;
            return -1;
        }
        size_t key_len = (size_t)(eq - entry);
        char key[256];
        if (key_len >= sizeof(key)) {
            errno = EINVAL;
            return -1;
        }
        memcpy(key, entry, key_len);
        key[key_len] = '\0';
        if (setenv(key, eq + 1, 1) != 0) return -1;
    }
    return 0;
}


static void close_stdio_to_devnull(void) {
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        return;
    }
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
}

static void spawn_auto_remove_watcher(const struct hold_store *store, const struct hold_run_record *record) {
    if (!store || !record || !hold_valid_id(record->id)) {
        return;
    }
    pid_t watcher = fork();
    if (watcher != 0) {
        return;
    }
    close_stdio_to_devnull();
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


bool hold_start_target_is_within_invoking_home(const struct hold_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv) {
    const char *target = explicit_start_argv0(owned, command, argc, argv);
    if (!target || !*target) {
        return false;
    }

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

int hold_perform_start_options(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_start_options *opts) {
    if (!opts) {
        hold_usage();
        return 5;
    }
    return perform_start_with_metadata_name_options_internal(inv, store, opts);
}

static int perform_start_with_metadata_name_options_internal(const struct hold_invocation *inv,
                                                             const struct hold_store *store,
                                                             const struct hold_start_options *opts) {
    bool tail = opts->tail;
    bool console_mode = opts->console_mode;
    bool auto_remove = opts->auto_remove;
    bool interactive_stdin = opts->interactive_stdin;
    int argc = opts->argc;
    char **argv = opts->argv;
    const char *exec_path = opts->exec_path;
    const char *requested_run_name = opts->run_name;
    int envc = opts->envc;
    char **env = opts->env;
    const char *restart_policy = opts->restart_policy;
    int restart_delay_seconds = opts->restart_delay_seconds;
    const char *existing_id = opts->existing_id;
    const char *existing_log_path = opts->existing_log_path;
    const char *existing_run_name = opts->existing_run_name;
    int64_t existing_created_unix_ns = opts->existing_created_unix_ns;
    const char *existing_created_at = opts->existing_created_at;
    if (argc <= 0 || !argv || !argv[0] ||
        envc < 0 || (envc > 0 && !env) ||
        restart_delay_seconds < 0) {
        hold_usage();
        return 5;
    }
    bool restart_enabled = restart_policy_is_enabled(restart_policy);
    if (restart_enabled && console_mode) {
        fprintf(stderr, "hold: error: --restart with --tty/-t is not supported yet\n");
        return 5;
    }

    char resolved_exec_path[HOLD_PATH_MAX];
    const char *path_to_resolve = (exec_path && *exec_path) ? exec_path : argv[0];
    if (hold_resolve_binary_path(path_to_resolve, resolved_exec_path, sizeof(resolved_exec_path)) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "hold: cannot start '%s': command not found\n", argv[0]);
        } else {
            fprintf(stderr, "hold: cannot start '%s': %s\n", argv[0], strerror(errno));
        }
        return 1;
    }

    char observed_cwd[HOLD_PATH_MAX] = {0};
    if (!getcwd(observed_cwd, sizeof(observed_cwd))) {
        observed_cwd[0] = '\0';
    }
    char **observed_argv = NULL;
    if (hold_copy_argv(&observed_argv, argc, argv) != 0) {
        hold_die_errno("hold: failed to prepare observed argv");
    }

    char **launch_argv = NULL;
    if (hold_copy_argv(&launch_argv, argc, argv) != 0) {
        hold_free_argv_alloc(observed_argv, argc);
        hold_die_errno("hold: failed to prepare argv");
    }
    free(launch_argv[0]);
    launch_argv[0] = strdup(resolved_exec_path);
    if (!launch_argv[0]) {
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        hold_die_errno("hold: failed to prepare argv");
    }
    if (hold_normalize_existing_argv_paths_from_cwd(launch_argv, argc, 1, observed_cwd[0] ? observed_cwd : NULL) != 0) {
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        hold_die_errno("hold: failed to normalize argv paths");
    }

    struct timespec start_ts;
    clock_gettime(CLOCK_REALTIME, &start_ts);
    int64_t start_unix_ns = (int64_t)start_ts.tv_sec * 1000000000LL + start_ts.tv_nsec;

    char id[ID_STR_LEN], log_path[HOLD_PATH_MAX], reserve_path[HOLD_PATH_MAX] = {0}, console_sock[HOLD_PATH_MAX], boot_id[128] = {0};
    char run_name[ALIAS_MAX_LEN + 1] = {0};
    console_sock[0] = '\0';
    bool has_boot = hold_current_boot_id(boot_id, sizeof(boot_id));
    bool restarting_existing = existing_id && *existing_id;
    bool owns_new_log = !restarting_existing;

    if (restarting_existing) {
        if (!hold_valid_id(existing_id) || !existing_log_path || !*existing_log_path) {
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            errno = EINVAL;
            hold_die_errno("hold: invalid restart record");
        }
        if (hold_checked_snprintf(id, sizeof(id), "%s", existing_id) != 0 ||
            hold_checked_snprintf(log_path, sizeof(log_path), "%s", existing_log_path) != 0) {
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            hold_die_errno("hold: restart metadata too long");
        }
        if (existing_run_name && *existing_run_name) {
            if (hold_checked_snprintf(run_name, sizeof(run_name), "%s", existing_run_name) != 0) {
                free_launch_and_observed_argv(launch_argv, observed_argv, argc);
                hold_die_errno("hold: call name too long");
            }
        }
    } else {
        if (reserve_hashed_run_id(store,
                                  resolved_exec_path,
                                  argc,
                                  launch_argv,
                                  observed_cwd[0] ? observed_cwd : NULL,
                                  start_unix_ns,
                                  id) != 0) {
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            hold_die_errno("hold: failed to generate id");
        }
        int name_rc = hold_generate_run_name_for_id(store, id, requested_run_name, run_name);
        if (name_rc != 0) {
            if (hold_checked_snprintf(reserve_path, sizeof(reserve_path), "%s/.%s.reserve", store->record_dir, id) == 0) {
                unlink_if_nonempty(reserve_path);
            }
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            return name_rc;
        }
        if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            hold_die_errno("hold: log path too long");
        }
        if (hold_checked_snprintf(reserve_path, sizeof(reserve_path), "%s/.%s.reserve", store->record_dir, id) != 0) {
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            hold_die_errno("hold: reserve path too long");
        }
    }
    if (console_mode && hold_format_console_sock_path(store, id, console_sock, sizeof(console_sock)) != 0) {
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        hold_die_errno("hold: console socket path too long");
    }
    int log_preflight_fd = open_log_append_no_symlink(log_path);
    if (log_preflight_fd < 0) {
        int saved = errno;
        unlink_if_nonempty(reserve_path);
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        errno = saved;
        hold_die_errno("hold: failed to open log");
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

    int pipefd[2];
    int target_pid_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(pipefd, O_CLOEXEC) != 0)
#endif
    {
        if (pipe(pipefd) != 0) {
            int saved = errno;
            unlink_if_nonempty(reserve_path);
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            errno = saved;
            hold_die_errno("hold: pipe failed");
        }
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(pipefd[0]);
            close(pipefd[1]);
            unlink_if_nonempty(reserve_path);
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            errno = saved;
            hold_die_errno("hold: pipe setup failed");
        }
    }
    if (!console_mode) {
#if defined(__linux__) && defined(O_CLOEXEC)
        if (pipe2(stdout_pipe, O_CLOEXEC) != 0)
#endif
        {
            if (pipe(stdout_pipe) != 0) {
                int saved = errno;
                close(pipefd[0]);
                close(pipefd[1]);
                unlink_if_nonempty(reserve_path);
                free_launch_and_observed_argv(launch_argv, observed_argv, argc);
                errno = saved;
                hold_die_errno("hold: stdout pipe failed");
            }
            (void)fcntl(stdout_pipe[0], F_SETFD, FD_CLOEXEC);
            (void)fcntl(stdout_pipe[1], F_SETFD, FD_CLOEXEC);
        }
#if defined(__linux__) && defined(O_CLOEXEC)
        if (pipe2(stderr_pipe, O_CLOEXEC) != 0)
#endif
        {
            if (pipe(stderr_pipe) != 0) {
                int saved = errno;
                close(pipefd[0]);
                close(pipefd[1]);
                close(stdout_pipe[0]);
                close(stdout_pipe[1]);
                unlink_if_nonempty(reserve_path);
                free_launch_and_observed_argv(launch_argv, observed_argv, argc);
                errno = saved;
                hold_die_errno("hold: stderr pipe failed");
            }
            (void)fcntl(stderr_pipe[0], F_SETFD, FD_CLOEXEC);
            (void)fcntl(stderr_pipe[1], F_SETFD, FD_CLOEXEC);
        }
    }
    if (!console_mode && !restart_enabled) {
#if defined(__linux__) && defined(O_CLOEXEC)
        if (pipe2(target_pid_pipe, O_CLOEXEC) != 0)
#endif
        {
            if (pipe(target_pid_pipe) != 0) {
                int saved = errno;
                close(pipefd[0]);
                close(pipefd[1]);
                if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
                if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
                if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
                if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
                unlink_if_nonempty(reserve_path);
                free_launch_and_observed_argv(launch_argv, observed_argv, argc);
                errno = saved;
                hold_die_errno("hold: target pid pipe failed");
            }
            (void)fcntl(target_pid_pipe[0], F_SETFD, FD_CLOEXEC);
            (void)fcntl(target_pid_pipe[1], F_SETFD, FD_CLOEXEC);
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        if (target_pid_pipe[0] >= 0) close(target_pid_pipe[0]);
        if (target_pid_pipe[1] >= 0) close(target_pid_pipe[1]);
        unlink_if_nonempty(reserve_path);
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        errno = saved;
        hold_die_errno("hold: fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (target_pid_pipe[0] >= 0) close(target_pid_pipe[0]);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (restart_enabled) {
            if (!console_mode) {
                if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
                    int e = errno;
                    hold_write_all(pipefd[1], &e, sizeof(e));
                    _exit(127);
                }
                if (stdout_pipe[1] > STDERR_FILENO) close(stdout_pipe[1]);
                if (stderr_pipe[1] > STDERR_FILENO) close(stderr_pipe[1]);
            }
            run_restart_supervisor(pipefd[1],
                                   log_path,
                                   interactive_stdin,
                                   envc,
                                   env,
                                   clean_child_env,
                                   resolved_exec_path,
                                   launch_argv,
                                   restart_policy,
                                   restart_delay_seconds,
                                   pin_privileged_cwd);
        }
        if (!console_mode) {
            pid_t target = fork();
            if (target < 0) {
                int e = errno;
                hold_write_all(pipefd[1], &e, sizeof(e));
                _exit(127);
            }
            if (target != 0) {
                if (target_pid_pipe[1] >= 0) {
                    (void)hold_write_all(target_pid_pipe[1], &target, sizeof(target));
                    close(target_pid_pipe[1]);
                }
                if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
                if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
                close(pipefd[1]);
                close_stdio_to_devnull();
                signal(SIGTERM, SIG_IGN);
                signal(SIGINT, SIG_IGN);
                signal(SIGHUP, SIG_IGN);
                int status = 0;
                while (waitpid(target, &status, 0) < 0) {
                    if (errno == EINTR) continue;
                    status = 127 << 8;
                    break;
                }
                for (int i = 0; i < 50; i++) {
                    int mark_rc = hold_mark_run_finished(store, id, status);
                    if (mark_rc == 0) break;
                    /* rc 1: the record was purged after we loaded it — that
                     * removal is final, do not resurrect it. rc -1 (e.g. the
                     * parent has not written the record yet on a fast exit)
                     * stays retryable. */
                    if (mark_rc == 1) break;
                    struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
                    while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
                        continue;
                    }
                }
                if (WIFEXITED(status)) _exit(WEXITSTATUS(status));
                if (WIFSIGNALED(status)) _exit(128 + WTERMSIG(status));
                _exit(255);
            }
            if (target_pid_pipe[1] >= 0) close(target_pid_pipe[1]);
        }
        if (setsid() < 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (pin_privileged_cwd && enter_privileged_exec_cwd() != 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (apply_child_env(envc, env, clean_child_env) != 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (console_mode) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd < 0 ||
                dup2(nullfd, STDIN_FILENO) < 0 ||
                dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(nullfd, STDERR_FILENO) < 0) {
                int e = errno;
                hold_write_all(pipefd[1], &e, sizeof(e));
                _exit(127);
            }
            if (nullfd > STDERR_FILENO) {
                close(nullfd);
            }
            hold_run_console_broker(pipefd[1],
                                      store,
                                      id,
                                      log_path,
                                      console_sock,
                                      console_owner_uid,
                                      console_have_allowed_peer_uid,
                                      console_allowed_peer_uid,
                                      argc,
                                      launch_argv,
                                      resolved_exec_path,
                                      console_init_rows,
                                      console_init_cols);
            _exit(127);
        }
        if (!interactive_stdin) {
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) {
                int e = errno;
                hold_write_all(pipefd[1], &e, sizeof(e));
                _exit(127);
            }
            if (nullfd > 2) {
                close(nullfd);
            }
        }

        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (stdout_pipe[1] > STDERR_FILENO) close(stdout_pipe[1]);
        if (stderr_pipe[1] > STDERR_FILENO) close(stderr_pipe[1]);
        execv(resolved_exec_path, launch_argv);
        int e = errno;
        hold_write_all(pipefd[1], &e, sizeof(e));
        _exit(127);
    }

    pid_t supervisor_pid = pid;
    close(pipefd[1]);
    if (target_pid_pipe[1] >= 0) close(target_pid_pipe[1]);
    if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(pipefd[0], &child_errno);
    int handshake_errno = errno;
    close(pipefd[0]);
    if (handshake < 0) {
        hold_rollback_spawned_group(pid, pid);
        unlink_if_nonempty(reserve_path);
        if (owns_new_log) unlink(log_path);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (target_pid_pipe[0] >= 0) close(target_pid_pipe[0]);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        errno = handshake_errno;
        hold_die_errno("hold: exec handshake failed");
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
        unlink_if_nonempty(reserve_path);
        if (owns_new_log) unlink(log_path);
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (target_pid_pipe[0] >= 0) close(target_pid_pipe[0]);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        return 1;
    }
    if (target_pid_pipe[0] >= 0) {
        pid_t target_pid = -1;
        ssize_t n = read(target_pid_pipe[0], &target_pid, sizeof(target_pid));
        close(target_pid_pipe[0]);
        target_pid_pipe[0] = -1;
        if (n != (ssize_t)sizeof(target_pid) || target_pid <= 1) {
            hold_rollback_spawned_group(pid, pid);
            kill_supervisor_if_distinct(supervisor_pid, pid);
            unlink_if_nonempty(reserve_path);
            if (owns_new_log) unlink(log_path);
            if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
            if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
            if (console_sock[0]) {
                unlink(console_sock);
            }
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            errno = EIO;
            hold_die_errno("hold: failed to read target pid");
        }
        pid = target_pid;
    }

    if (!console_mode) {
        if (spawn_log_capture(stdout_pipe[0],
                              stderr_pipe[0],
                              log_path) != 0) {
            int saved = errno;
            stdout_pipe[0] = -1;
            stderr_pipe[0] = -1;
            hold_rollback_spawned_group(supervisor_pid, pid);
            kill_supervisor_if_distinct(supervisor_pid, pid);
            unlink_if_nonempty(reserve_path);
            if (owns_new_log) unlink(log_path);
            if (console_sock[0]) {
                unlink(console_sock);
            }
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            errno = saved;
            hold_die_errno("hold: logger fork failed");
        }
        stdout_pipe[0] = -1;
        stderr_pipe[0] = -1;
    }

    struct hold_run_record r = {0};
    r.version = 1;
    if (hold_checked_snprintf(r.id, sizeof(r.id), "%s", id) != 0) {
        hold_die_errno("hold: id too long");
    }
    if (hold_checked_snprintf(r.run_id, sizeof(r.run_id), "%s", id) != 0) {
        hold_die_errno("hold: id too long");
    }
    r.pid = pid;
    r.pgid = pid;
    r.sid = pid;
    r.start_unix_ns = start_unix_ns;
    r.created_unix_ns = existing_created_unix_ns > 0 ? existing_created_unix_ns : start_unix_ns;
    hold_format_rfc3339_utc_from_ns(r.start_unix_ns, r.started_at, sizeof(r.started_at));
    r.has_started_at = true;
    if (existing_created_at && *existing_created_at) {
        if (hold_checked_snprintf(r.created_at, sizeof(r.created_at), "%s", existing_created_at) != 0) {
            hold_die_errno("hold: created timestamp too long");
        }
    } else {
        hold_format_rfc3339_utc_from_ns(r.created_unix_ns, r.created_at, sizeof(r.created_at));
    }
    r.has_created_at = true;
    snprintf(r.state, sizeof(r.state), "running");
    r.has_state = true;
    r.uid = geteuid();
    r.gid = getegid();
    if (store->kind == STORE_SYSTEM_MANAGED) {
        r.has_invocation = true;
        if (inv && inv->have_sudo_user) {
            r.invoked_by_uid = inv->invoking_uid;
            r.invoked_by_gid = inv->invoking_gid;
            if (hold_checked_snprintf(r.invoked_by_user, sizeof(r.invoked_by_user), "%s", inv->invoking_user) != 0) {
                hold_die_errno("hold: invoking user too long");
            }
            r.invoked_via_sudo = true;
        } else {
            r.invoked_by_uid = 0;
            r.invoked_by_gid = 0;
            if (hold_checked_snprintf(r.invoked_by_user, sizeof(r.invoked_by_user), "%s", "root") != 0) {
                hold_die_errno("hold: invoking user too long");
            }
            r.invoked_via_sudo = false;
        }
    }
    if (run_name[0]) {
        r.has_name = true;
        if (hold_checked_snprintf(r.name, sizeof(r.name), "%s", run_name) != 0) {
            hold_die_errno("hold: call name too long");
        }
    }
    if (console_sock[0]) {
        r.has_console = true;
        if (hold_checked_snprintf(r.console_sock, sizeof(r.console_sock), "%s", console_sock) != 0) {
            hold_die_errno("hold: console socket path too long");
        }
    }
    r.recipe.mode_interactive = interactive_stdin;
    r.recipe.mode_tty = console_mode;
    r.recipe.mode_detach = !tail && !console_mode;
    r.recipe.allow_multi = false;
    if (envc > 0 && hold_copy_argv(&r.recipe.env, envc, env) != 0) {
        hold_die_errno("hold: failed to copy env metadata");
    }
    r.recipe.envc = envc;
    if (restart_enabled) {
        if (hold_checked_snprintf(r.recipe.restart_policy, sizeof(r.recipe.restart_policy), "%s", restart_policy) != 0) {
            hold_die_errno("hold: restart policy too long");
        }
        r.recipe.has_restart_policy = true;
    }
    r.recipe.restart_delay_seconds = restart_delay_seconds;
    r.recipe.has_restart_delay = restart_delay_seconds > 0;
    r.has_log = true;
    if (hold_checked_snprintf(r.log_path, sizeof(r.log_path), "%s", log_path) != 0) {
        hold_die_errno("hold: log path too long");
    }
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
    if (hold_checked_snprintf(r.observed_exe, sizeof(r.observed_exe), "%s", resolved_exec_path) != 0) {
        hold_die_errno("hold: observed executable path too long");
    }
    if (hold_checked_snprintf(r.observed_cwd, sizeof(r.observed_cwd), "%s", observed_cwd) != 0) {
        hold_die_errno("hold: observed cwd too long");
    }
    r.observed_argc = argc;
    r.observed_argv = observed_argv;

    char record_path[HOLD_PATH_MAX] = {0};
    bool chown_user_local_artifacts = store->kind == STORE_USER_LOCAL && inv && inv->euid_root && inv->have_sudo_user;
    if (getenv("HOLD_TEST_FAIL_RECORD_WRITE")) {
        errno = EIO;
    } else if (hold_write_record_atomic(store->record_dir, &r, argc, launch_argv, record_path, sizeof(record_path)) == 0) {
        if (chown_user_local_artifacts) {
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
                int saved = errno ? errno : EIO;
                hold_rollback_spawned_group(pid, pid);
                kill_supervisor_if_distinct(supervisor_pid, pid);
                if (record_path[0] && !restarting_existing) {
                    unlink(record_path);
                }
                if (owns_new_log) unlink(log_path);
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink_if_nonempty(reserve_path);
                free_launch_and_observed_argv(launch_argv, observed_argv, argc);
                errno = saved;
                hold_die_errno("hold: failed to set user-local ownership");
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
                int saved = errno;
                if (saved == 0) {
                    saved = EIO;
                }
                hold_rollback_spawned_group(pid, pid);
                kill_supervisor_if_distinct(supervisor_pid, pid);
                if (record_path[0] && !restarting_existing) {
                    unlink(record_path);
                }
                if (owns_new_log) unlink(log_path);
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink_if_nonempty(reserve_path);
                free_launch_and_observed_argv(launch_argv, observed_argv, argc);
                char public_path[HOLD_PATH_MAX];
                if (hold_checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                    unlink(public_path);
                }
                errno = saved;
                hold_die_errno("hold: failed to write public index");
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
                tail_rc = hold_run_native_console(r.console_sock);
            } else {
                tail_rc = hold_tail_log_until_exit(&r, false, true);
            }
            if (auto_remove) {
                char boot[128];
                const char *boot_id = hold_boot_id_or_null(boot);
                bool removed = false;
                int prune_rc = hold_prune_one_run(store, r.id, boot_id, true, &removed);
                if (tail_rc == 0 && prune_rc != 0) tail_rc = prune_rc;
            }
            hold_free_argv_alloc(r.recipe.env, r.recipe.envc);
            return tail_rc;
        }
        if (auto_remove) {
            spawn_auto_remove_watcher(store, &r);
        }
        hold_free_argv_alloc(r.recipe.env, r.recipe.envc);
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        return 0;
    }
    {
        int saved = errno;
        hold_rollback_spawned_group(pid, pid);
        kill_supervisor_if_distinct(supervisor_pid, pid);
        unlink_if_nonempty(reserve_path);
        if (owns_new_log) unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        free_launch_and_observed_argv(launch_argv, observed_argv, argc);
        errno = saved;
        hold_die_errno("hold: failed to write record");
    }
    return 1;
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
