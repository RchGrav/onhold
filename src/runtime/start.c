#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"
#include "hold/names/adjectives.h"
#include "hold/names/nouns.h"
#if defined(__linux__)
#include <syslog.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

struct start_profile_target {
    struct hold_store store;
    char hash[PROFILE_HASH_STR_LEN];
    char alias[ALIAS_MAX_LEN + 1];
    struct hold_profile recipe;
    bool has_hash;
    bool has_alias;
    bool has_recipe;
    bool needs_elevation;
};

static volatile sig_atomic_t g_restart_stop = 0;

static const char *explicit_start_argv0(bool owned, const char *command, int argc, char **argv);
static int apply_child_env(int envc, char **env);
static int apply_child_capabilities(int cap_addc, char **cap_add, int cap_dropc, char **cap_drop);
static int private_start_hash_for_token(const struct hold_store *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN],
                                        bool *matched);
static int private_start_recipe_for_token(const struct hold_store *store,
                                          const char *token,
                                          struct hold_profile *recipe,
                                          bool *matched);
static void free_start_profile_target(struct start_profile_target *target);
static void start_target_set_alias(struct start_profile_target *target, const char *alias);
static int start_target_set_recipe(struct start_profile_target *target,
                                   const struct hold_store *store,
                                   const char *alias);
static int start_target_set_hash(struct start_profile_target *target,
                                 const struct hold_store *store,
                                 const char *hash,
                                 const char *alias,
                                 bool needs_elevation);
static int count_running_alias(const struct hold_store *store, const char *alias, size_t *count_out);
static int reconnect_running_console_alias(const struct hold_invocation *inv,
                                           const struct hold_store *user_store,
                                           const struct hold_store *system_store,
                                           const char *program,
                                           const struct hold_store *store,
                                           const char *alias,
                                           bool *handled);
static int resolve_start_profile_target(const struct hold_invocation *inv,
                                        const struct hold_store *current_user_store,
                                        const struct hold_store *system_store,
                                        const char *token,
                                        struct start_profile_target *out);
static int perform_explicit_start_options(const struct hold_invocation *inv,
                                            const struct hold_store *store,
                                            bool tail,
                                            bool console_mode,
                                            bool auto_remove,
                                            bool interactive_stdin,
                                            const char *restart_policy,
                                            int restart_delay_seconds,
                                            int argc,
                                            char **argv);
static int perform_start_with_metadata_name_options_internal(const struct hold_invocation *inv,
                                                             const struct hold_store *store,
                                                             bool tail,
                                                             bool console_mode,
                                                             bool auto_remove,
                                                             bool interactive_stdin,
                                                             int argc,
                                                             char **argv,
                                                             const char *exec_path,
                                                             const char *run_alias,
                                                             const char *requested_run_name,
                                                             int envc,
                                                             char **env,
                                                             int portc,
                                                             char **ports,
                                                             int volumec,
                                                             char **volumes,
                                                             const char *restart_policy,
                                                             int restart_delay_seconds,
                                                             const char *log_destination,
                                                             int cap_addc,
                                                             char **cap_add,
                                                             int cap_dropc,
                                                             char **cap_drop,
                                                             const char *existing_id,
                                                             const char *existing_log_path,
                                                             const char *existing_run_name,
                                                             int64_t existing_created_unix_ns,
                                                             const char *existing_created_at);
static int run_name_exists_in_store(const struct hold_store *store, const char *name, const char *ignore_id);
static int find_restart_record(const struct hold_store *store, const char *token, char out[ID_STR_LEN]);
static int restart_existing_run(const struct hold_invocation *inv,
                                const struct hold_store *store,
                                bool tail,
                                bool console_mode,
                                bool auto_remove,
                                bool interactive_stdin,
                                const char *restart_policy,
                                int restart_delay_seconds,
                                const char *id);
static int generate_run_name_for_id(const struct hold_store *store, const char *id, const char *requested, char out[ALIAS_MAX_LEN + 1]);
static int reserve_hashed_run_id(const struct hold_store *store,
                                 const char *profile_alias,
                                 const char *resolved_exec_path,
                                 int argc,
                                 char **argv,
                                 const char *cwd,
                                 int64_t start_unix_ns,
                                 char out[ID_STR_LEN]);
static void spawn_log_capture(int stdout_fd,
                              int stderr_fd,
                              const char *log_path,
                              const char *log_destination,
                              const char *id,
                              const char *name,
                              const char *profile,
                              const char *cmdline);
static void close_stdio_to_devnull(void);
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

static int run_name_exists_in_store(const struct hold_store *store, const char *name, const char *ignore_id) {
    if (!store || !hold_valid_alias(name)) return 0;
    DIR *d = opendir(store->record_dir);
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) continue;
        if (ignore_id && strcmp(file_id, ignore_id) == 0) continue;
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) continue;
        struct hold_run_record r;
        memset(&r, 0, sizeof(r));
        if (hold_load_record(path, &r) == 0 && r.has_name && strcmp(r.name, name) == 0) {
            hold_free_run_record(&r);
            closedir(d);
            return 1;
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    return 0;
}

static int find_restart_record(const struct hold_store *store, const char *token, char out[ID_STR_LEN]) {
    if (!store || !token || !*token || !out) return 0;
    bool id_like = hold_valid_id_prefix(token);
    bool name_like = hold_valid_alias(token);
    if (!id_like && !name_like) return 0;
    DIR *d = opendir(store->record_dir);
    if (!d) return 0;
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) continue;
        bool hit = false;
        if (id_like && strncmp(file_id, token, strlen(token)) == 0) {
            hit = true;
        } else if (name_like) {
            char path[HOLD_PATH_MAX];
            if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) continue;
            struct hold_run_record r;
            memset(&r, 0, sizeof(r));
            if (hold_load_record(path, &r) == 0 && r.has_name && strcmp(r.name, token) == 0) {
                hit = true;
            }
            hold_free_run_record(&r);
        }
        if (hit) {
            matches++;
            if (hold_checked_snprintf(out, ID_STR_LEN, "%s", file_id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    if (matches == 0) return 0;
    if (matches > 1) {
        fprintf(stderr, "hold: error: restart target '%s' is ambiguous\n", token);
        return -2;
    }
    return 1;
}

static int restart_existing_run(const struct hold_invocation *inv,
                                const struct hold_store *store,
                                bool tail,
                                bool console_mode,
                                bool auto_remove,
                                bool interactive_stdin,
                                const char *restart_policy,
                                int restart_delay_seconds,
                                const char *id) {
    struct hold_run_record old;
    char record_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &old, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    enum run_state st = hold_eval_state(&old, have_boot ? boot : NULL);
    if (st == STATE_RUNNING) {
        char display[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(old.id, display);
        fprintf(stderr, "hold: error: run %s is already running\n", display);
        hold_free_run_record(&old);
        return 6;
    }
    if (!old.has_log || !old.log_path[0]) {
        fprintf(stderr, "hold: error: run %s has no retained log path\n", id);
        hold_free_run_record(&old);
        return 5;
    }
    char *j = NULL;
    char **argv = NULL;
    int argc = 0;
    int rc = 0;
    if (hold_read_owned_file_no_symlink(record_path, &j) != 0 ||
        hold_json_get_argv_alloc(j, &argv, &argc) != 0) {
        fprintf(stderr, "hold: error: failed to load restart argv for %s\n", id);
        rc = 5;
        goto out;
    }
    if (argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "hold: error: failed to load restart argv for %s\n", id);
        rc = 5;
        goto out;
    }
    rc = perform_start_with_metadata_name_options_internal(inv,
                                                           store,
                                                           tail,
                                                           console_mode,
                                                           auto_remove,
                                                           interactive_stdin,
                                                           argc,
                                                           argv,
                                                           argv[0],
                                                           old.has_alias ? old.alias : NULL,
                                                           NULL,
                                                           old.envc,
                                                           old.env,
                                                           old.portc,
                                                           old.ports,
                                                           old.volumec,
                                                           old.volumes,
                                                           restart_policy ? restart_policy : (old.has_restart_policy ? old.restart_policy : NULL),
                                                           restart_policy ? restart_delay_seconds : old.restart_delay_seconds,
                                                           old.has_log_destination ? old.log_destination : NULL,
                                                           old.cap_addc,
                                                           old.cap_add,
                                                           old.cap_dropc,
                                                           old.cap_drop,
                                                           old.id,
                                                           old.log_path,
                                                           old.has_name ? old.name : NULL,
                                                           old.created_unix_ns,
                                                           old.has_created_at ? old.created_at : NULL);
out:
    hold_free_argv_alloc(argv, argc);
    free(j);
    hold_free_run_record(&old);
    return rc;
}

static int generate_run_name_for_id(const struct hold_store *store, const char *id, const char *requested, char out[ALIAS_MAX_LEN + 1]) {
    if (requested && *requested) {
        if (!hold_valid_alias(requested)) {
            fprintf(stderr, "hold: error: invalid run name '%s'\n", requested);
            return 5;
        }
        if (run_name_exists_in_store(store, requested, NULL)) {
            fprintf(stderr, "hold: error: run name '%s' already exists\n", requested);
            return 5;
        }
        snprintf(out, ALIAS_MAX_LEN + 1, "%s", requested);
        return 0;
    }

    size_t adj_n = adjectives_count;
    size_t noun_n = nouns_count;
    size_t total = adj_n * noun_n;
    unsigned long long seed = 0;
    for (size_t i = 0; id && id[i] && i < 16; i++) {
        seed <<= 4;
        seed |= (unsigned long long)(id[i] <= '9' ? id[i] - '0' : id[i] - 'a' + 10);
    }
    for (size_t tries = 0; tries < total; tries++) {
        unsigned long long candidate = seed + tries;
        size_t adj_idx = (size_t)(candidate % adj_n);
        size_t noun_idx = (size_t)((candidate / adj_n) % noun_n);
        if (hold_checked_snprintf(out,
                                  ALIAS_MAX_LEN + 1,
                                  "%s_%s",
                                  adjectives[adj_idx],
                                  nouns[noun_idx]) != 0) {
            return 3;
        }
        if (!run_name_exists_in_store(store, out, NULL)) return 0;
    }
    char display[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(id, display);
    return hold_checked_snprintf(out, ALIAS_MAX_LEN + 1, "run_%s", display) == 0 ? 0 : 3;
}

static void hash_field(struct sha256_ctx *ctx, const char *key, const char *value) {
    hold_sha256_update_nul_field(ctx, key);
    hold_sha256_update_nul_field(ctx, value ? value : "-");
}

static void compute_run_hash(const char *profile_alias,
                             const char *resolved_exec_path,
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
    hash_field(&ctx, "profile", profile_alias && *profile_alias ? profile_alias : "-");
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
                                 const char *profile_alias,
                                 const char *resolved_exec_path,
                                 int argc,
                                 char **argv,
                                 const char *cwd,
                                 int64_t start_unix_ns,
                                 char out[ID_STR_LEN]) {
    char reserve[HOLD_PATH_MAX];
    for (unsigned long counter = 0; counter < 1024; counter++) {
        compute_run_hash(profile_alias, resolved_exec_path, argc, argv, cwd, start_unix_ns, counter, out);
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

static void mirror_syslog_line(const char *stream,
                               const char *id,
                               const char *name,
                               const char *profile,
                               const char *cmdline,
                               const char *buf,
                               size_t n) {
#if defined(__linux__)
    int priority = stream && strcmp(stream, "stderr") == 0 ? LOG_ERR : LOG_INFO;
    char display[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(id, display);
    char *line = strndup(buf ? buf : "", n);
    if (!line) return;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    openlog("hold", LOG_PID, LOG_USER);
    syslog(priority,
           "run=%s name=%s profile=%s stream=%s cmd=\"%s\" msg=\"%s\"",
           display[0] ? display : "-",
           name && *name ? name : "-",
           profile && *profile ? profile : "-",
           stream && *stream ? stream : "stdout",
           cmdline && *cmdline ? cmdline : "-",
           line);
    closelog();
    free(line);
#else
    (void)stream; (void)id; (void)name; (void)profile; (void)cmdline; (void)buf; (void)n;
#endif
}

static void logger_write_bytes(int logfd,
                               int idxfd,
                               const char *log_destination,
                               const char *stream,
                               const char *id,
                               const char *name,
                               const char *profile,
                               const char *cmdline,
                               const char *buf,
                               size_t n) {
    (void)hold_write_indexed_log_bytes_fd(logfd, idxfd, stream, buf, n);
    if (log_destination && strcmp(log_destination, "syslog") == 0) {
        size_t start = 0;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                mirror_syslog_line(stream, id, name, profile, cmdline, buf + start, i + 1 - start);
                start = i + 1;
            }
        }
        if (start < n) mirror_syslog_line(stream, id, name, profile, cmdline, buf + start, n - start);
    }
}

static void spawn_log_capture(int stdout_fd,
                              int stderr_fd,
                              const char *log_path,
                              const char *log_destination,
                              const char *id,
                              const char *name,
                              const char *profile,
                              const char *cmdline) {
    pid_t logger = fork();
    if (logger != 0) {
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        return;
    }
    close_stdio_to_devnull();
    int logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logfd < 0) _exit(0);
    int idxfd = hold_open_log_index_fd(log_path, logfd);
    while (stdout_fd >= 0 || stderr_fd >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (stdout_fd >= 0) { FD_SET(stdout_fd, &rfds); if (stdout_fd > maxfd) maxfd = stdout_fd; }
        if (stderr_fd >= 0) { FD_SET(stderr_fd, &rfds); if (stderr_fd > maxfd) maxfd = stderr_fd; }
        if (maxfd < 0) break;
        int sr = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (sr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char buf[4096];
        if (stdout_fd >= 0 && FD_ISSET(stdout_fd, &rfds)) {
            ssize_t n = read(stdout_fd, buf, sizeof(buf));
            if (n > 0) logger_write_bytes(logfd, idxfd, log_destination, "stdout", id, name, profile, cmdline, buf, (size_t)n);
            else { close(stdout_fd); stdout_fd = -1; }
        }
        if (stderr_fd >= 0 && FD_ISSET(stderr_fd, &rfds)) {
            ssize_t n = read(stderr_fd, buf, sizeof(buf));
            if (n > 0) logger_write_bytes(logfd, idxfd, log_destination, "stderr", id, name, profile, cmdline, buf, (size_t)n);
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
                                   const char *resolved_exec_path,
                                   char **launch_argv,
                                   int cap_addc,
                                   char **cap_add,
                                   int cap_dropc,
                                   char **cap_drop,
                                   const char *restart_policy,
                                   int restart_delay_seconds) {
    (void)log_path;
    if (setsid() < 0) {
        int e = errno;
        hold_write_all(handshake_fd, &e, sizeof(e));
        _exit(127);
    }
    if (apply_child_env(envc, env) != 0) {
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
            if (apply_child_capabilities(cap_addc, cap_add, cap_dropc, cap_drop) != 0) {
                fprintf(stderr, "hold: cannot apply restart capabilities for '%s': %s\n", resolved_exec_path, strerror(errno));
                _exit(127);
            }
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

static int apply_child_env(int envc, char **env) {
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

#if defined(__linux__) && defined(SYS_capget) && defined(SYS_capset)
struct hold_cap_name { const char *name; int value; };
static const struct hold_cap_name HOLD_CAP_NAMES[] = {
#ifdef CAP_CHOWN
    {"CHOWN", CAP_CHOWN},
#endif
#ifdef CAP_DAC_OVERRIDE
    {"DAC_OVERRIDE", CAP_DAC_OVERRIDE},
#endif
#ifdef CAP_DAC_READ_SEARCH
    {"DAC_READ_SEARCH", CAP_DAC_READ_SEARCH},
#endif
#ifdef CAP_FOWNER
    {"FOWNER", CAP_FOWNER},
#endif
#ifdef CAP_FSETID
    {"FSETID", CAP_FSETID},
#endif
#ifdef CAP_KILL
    {"KILL", CAP_KILL},
#endif
#ifdef CAP_SETGID
    {"SETGID", CAP_SETGID},
#endif
#ifdef CAP_SETUID
    {"SETUID", CAP_SETUID},
#endif
#ifdef CAP_SETPCAP
    {"SETPCAP", CAP_SETPCAP},
#endif
#ifdef CAP_LINUX_IMMUTABLE
    {"LINUX_IMMUTABLE", CAP_LINUX_IMMUTABLE},
#endif
#ifdef CAP_NET_BIND_SERVICE
    {"NET_BIND_SERVICE", CAP_NET_BIND_SERVICE},
#endif
#ifdef CAP_NET_BROADCAST
    {"NET_BROADCAST", CAP_NET_BROADCAST},
#endif
#ifdef CAP_NET_ADMIN
    {"NET_ADMIN", CAP_NET_ADMIN},
#endif
#ifdef CAP_NET_RAW
    {"NET_RAW", CAP_NET_RAW},
#endif
#ifdef CAP_IPC_LOCK
    {"IPC_LOCK", CAP_IPC_LOCK},
#endif
#ifdef CAP_IPC_OWNER
    {"IPC_OWNER", CAP_IPC_OWNER},
#endif
#ifdef CAP_SYS_MODULE
    {"SYS_MODULE", CAP_SYS_MODULE},
#endif
#ifdef CAP_SYS_RAWIO
    {"SYS_RAWIO", CAP_SYS_RAWIO},
#endif
#ifdef CAP_SYS_CHROOT
    {"SYS_CHROOT", CAP_SYS_CHROOT},
#endif
#ifdef CAP_SYS_PTRACE
    {"SYS_PTRACE", CAP_SYS_PTRACE},
#endif
#ifdef CAP_SYS_PACCT
    {"SYS_PACCT", CAP_SYS_PACCT},
#endif
#ifdef CAP_SYS_ADMIN
    {"SYS_ADMIN", CAP_SYS_ADMIN},
#endif
#ifdef CAP_SYS_BOOT
    {"SYS_BOOT", CAP_SYS_BOOT},
#endif
#ifdef CAP_SYS_NICE
    {"SYS_NICE", CAP_SYS_NICE},
#endif
#ifdef CAP_SYS_RESOURCE
    {"SYS_RESOURCE", CAP_SYS_RESOURCE},
#endif
#ifdef CAP_SYS_TIME
    {"SYS_TIME", CAP_SYS_TIME},
#endif
#ifdef CAP_SYS_TTY_CONFIG
    {"SYS_TTY_CONFIG", CAP_SYS_TTY_CONFIG},
#endif
#ifdef CAP_MKNOD
    {"MKNOD", CAP_MKNOD},
#endif
#ifdef CAP_LEASE
    {"LEASE", CAP_LEASE},
#endif
#ifdef CAP_AUDIT_WRITE
    {"AUDIT_WRITE", CAP_AUDIT_WRITE},
#endif
#ifdef CAP_AUDIT_CONTROL
    {"AUDIT_CONTROL", CAP_AUDIT_CONTROL},
#endif
#ifdef CAP_SETFCAP
    {"SETFCAP", CAP_SETFCAP},
#endif
#ifdef CAP_MAC_OVERRIDE
    {"MAC_OVERRIDE", CAP_MAC_OVERRIDE},
#endif
#ifdef CAP_MAC_ADMIN
    {"MAC_ADMIN", CAP_MAC_ADMIN},
#endif
#ifdef CAP_SYSLOG
    {"SYSLOG", CAP_SYSLOG},
#endif
#ifdef CAP_WAKE_ALARM
    {"WAKE_ALARM", CAP_WAKE_ALARM},
#endif
#ifdef CAP_BLOCK_SUSPEND
    {"BLOCK_SUSPEND", CAP_BLOCK_SUSPEND},
#endif
#ifdef CAP_AUDIT_READ
    {"AUDIT_READ", CAP_AUDIT_READ},
#endif
#ifdef CAP_PERFMON
    {"PERFMON", CAP_PERFMON},
#endif
#ifdef CAP_BPF
    {"BPF", CAP_BPF},
#endif
#ifdef CAP_CHECKPOINT_RESTORE
    {"CHECKPOINT_RESTORE", CAP_CHECKPOINT_RESTORE},
#endif
};

static int cap_last_cap_value(void) {
    long fallback = 40;
#ifdef CAP_LAST_CAP
    fallback = CAP_LAST_CAP;
#endif
    FILE *f = fopen("/proc/sys/kernel/cap_last_cap", "r");
    if (!f) return (int)fallback;
    long n = fallback;
    if (fscanf(f, "%ld", &n) != 1 || n < 0 || n > 63) n = fallback;
    fclose(f);
    return (int)n;
}

static int normalize_cap_name(const char *in, char out[64]) {
    if (!in || !*in) { errno = EINVAL; return -1; }
    const char *src = in;
    if (!strncasecmp(src, "CAP_", 4)) src += 4;
    size_t n = strlen(src);
    if (n == 0 || n >= 64) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '-') c = '_';
        if (!(isalnum(c) || c == '_')) { errno = EINVAL; return -1; }
        out[i] = (char)toupper(c);
    }
    out[n] = '\0';
    return 0;
}

static int cap_name_to_value(const char *name) {
    char norm[64];
    if (normalize_cap_name(name, norm) != 0) return -1;
    for (size_t i = 0; i < sizeof(HOLD_CAP_NAMES) / sizeof(HOLD_CAP_NAMES[0]); i++) {
        if (!strcmp(norm, HOLD_CAP_NAMES[i].name)) return HOLD_CAP_NAMES[i].value;
    }
    errno = EINVAL;
    return -1;
}

static void cap_set_bit(struct __user_cap_data_struct data[2], int cap, bool enabled) {
    unsigned idx = (unsigned)cap / 32U;
    __u32 mask = ((__u32)1U) << ((unsigned)cap % 32U);
    if (idx >= 2) return;
    if (enabled) {
        data[idx].effective |= mask;
        data[idx].permitted |= mask;
    } else {
        data[idx].effective &= ~mask;
        data[idx].permitted &= ~mask;
        data[idx].inheritable &= ~mask;
    }
}

static bool cap_permitted_bit_is_set(const struct __user_cap_data_struct data[2], int cap) {
    unsigned idx = (unsigned)cap / 32U;
    __u32 mask = ((__u32)1U) << ((unsigned)cap % 32U);
    return idx < 2 && (data[idx].permitted & mask) != 0;
}

static int apply_child_capabilities(int cap_addc, char **cap_add, int cap_dropc, char **cap_drop) {
    if (cap_addc <= 0 && cap_dropc <= 0) return 0;
    if ((cap_addc > 0 && !cap_add) || (cap_dropc > 0 && !cap_drop)) { errno = EINVAL; return -1; }
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    bool drop_all = false;
    bool drop_explicit[64];
    memset(&hdr, 0, sizeof(hdr));
    memset(data, 0, sizeof(data));
    memset(drop_explicit, 0, sizeof(drop_explicit));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    if (syscall(SYS_capget, &hdr, data) != 0) return -1;
    int last = cap_last_cap_value();
    for (int i = 0; i < cap_dropc; i++) {
        char norm[64];
        if (normalize_cap_name(cap_drop[i], norm) != 0) return -1;
        if (!strcmp(norm, "ALL")) {
            drop_all = true;
            for (int c = 0; c <= last && c < 64; c++) cap_set_bit(data, c, false);
            continue;
        }
        int c = cap_name_to_value(norm);
        if (c < 0 || c > last) return -1;
        cap_set_bit(data, c, false);
        if (c < 64) drop_explicit[c] = true;
    }
    for (int i = 0; i < cap_addc; i++) {
        char norm[64];
        if (normalize_cap_name(cap_add[i], norm) != 0) return -1;
        if (!strcmp(norm, "ALL")) {
            for (int c = 0; c <= last && c < 64; c++) cap_set_bit(data, c, true);
            continue;
        }
        int c = cap_name_to_value(norm);
        if (c < 0 || c > last) return -1;
        cap_set_bit(data, c, true);
    }
    for (int c = 0; c <= last && c < 64; c++) {
        if ((drop_all || drop_explicit[c]) && !cap_permitted_bit_is_set(data, c)) {
            (void)prctl(PR_CAPBSET_DROP, c, 0, 0, 0);
        }
    }
    return syscall(SYS_capset, &hdr, data) == 0 ? 0 : -1;
}
#else
static int apply_child_capabilities(int cap_addc, char **cap_add, int cap_dropc, char **cap_drop) {
    (void)cap_add; (void)cap_drop;
    if (cap_addc > 0 || cap_dropc > 0) { errno = ENOTSUP; return -1; }
    return 0;
}
#endif

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
        char boot[128] = {0};
        bool have_boot = hold_current_boot_id(boot, sizeof(boot));
        enum run_state st = hold_eval_state(&watch_record, have_boot ? boot : NULL);
        if (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE) {
            bool removed = false;
            hold_prune_one_run(&watch_store, watch_record.id, have_boot ? boot : NULL, true, &removed);
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

int hold_perform_start(const struct hold_invocation *inv,
                         const struct hold_store *store,
                         bool tail,
                         bool console_mode,
                         int argc,
                         char **argv,
                         const char *exec_path,
                         const char *run_alias) {
    return hold_perform_start_with_env(inv, store, tail, console_mode, argc, argv, exec_path, run_alias, 0, NULL);
}

int hold_perform_start_with_env(const struct hold_invocation *inv,
                                  const struct hold_store *store,
                                  bool tail,
                                  bool console_mode,
                                  int argc,
                                  char **argv,
                                  const char *exec_path,
                                  const char *run_alias,
                                  int envc,
                                  char **env) {
    return hold_perform_start_with_env_options(inv, store, tail, console_mode, false, false, argc, argv, exec_path, run_alias, envc, env);
}

int hold_perform_start_with_env_options(const struct hold_invocation *inv,
                                          const struct hold_store *store,
                                          bool tail,
                                          bool console_mode,
                                          bool auto_remove,
                                          bool interactive_stdin,
                                          int argc,
                                          char **argv,
                                          const char *exec_path,
                                          const char *run_alias,
                                          int envc,
                                          char **env) {
    return hold_perform_start_with_metadata_options(inv, store, tail, console_mode, auto_remove, interactive_stdin,
                                                    argc, argv, exec_path, run_alias, envc, env, 0, NULL, 0, NULL, NULL, 0);
}

int hold_perform_start_with_metadata_options(const struct hold_invocation *inv,
                                               const struct hold_store *store,
                                               bool tail,
                                               bool console_mode,
                                               bool auto_remove,
                                               bool interactive_stdin,
                                               int argc,
                                               char **argv,
                                               const char *exec_path,
                                               const char *run_alias,
                                               int envc,
                                               char **env,
                                               int portc,
                                               char **ports,
                                               int volumec,
                                               char **volumes,
                                               const char *restart_policy,
                                               int restart_delay_seconds) {
    return hold_perform_start_with_metadata_name_options(inv, store, tail, console_mode, auto_remove, interactive_stdin,
                                                         argc, argv, exec_path, run_alias, NULL, envc, env, portc, ports,
                                                         volumec, volumes, restart_policy, restart_delay_seconds, NULL);
}

int hold_perform_start_with_metadata_name_options(const struct hold_invocation *inv,
                                                   const struct hold_store *store,
                                                   bool tail,
                                                   bool console_mode,
                                                   bool auto_remove,
                                                   bool interactive_stdin,
                                                   int argc,
                                                   char **argv,
                                                   const char *exec_path,
                                                   const char *run_alias,
                                                   const char *requested_run_name,
                                                   int envc,
                                                   char **env,
                                                   int portc,
                                                   char **ports,
                                                   int volumec,
                                                   char **volumes,
                                                   const char *restart_policy,
                                                   int restart_delay_seconds,
                                                   const char *log_destination) {
    return perform_start_with_metadata_name_options_internal(inv,
                                                             store,
                                                             tail,
                                                             console_mode,
                                                             auto_remove,
                                                             interactive_stdin,
                                                             argc,
                                                             argv,
                                                             exec_path,
                                                             run_alias,
                                                             requested_run_name,
                                                             envc,
                                                             env,
                                                             portc,
                                                             ports,
                                                             volumec,
                                                             volumes,
                                                             restart_policy,
                                                             restart_delay_seconds,
                                                             log_destination,
                                                             0,
                                                             NULL,
                                                             0,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             0,
                                                             NULL);
}

int hold_perform_start_with_metadata_name_cap_options(const struct hold_invocation *inv,
                                                       const struct hold_store *store,
                                                       bool tail,
                                                       bool console_mode,
                                                       bool auto_remove,
                                                       bool interactive_stdin,
                                                       int argc,
                                                       char **argv,
                                                       const char *exec_path,
                                                       const char *run_alias,
                                                       const char *requested_run_name,
                                                       int envc,
                                                       char **env,
                                                       int portc,
                                                       char **ports,
                                                       int volumec,
                                                       char **volumes,
                                                       int cap_addc,
                                                       char **cap_add,
                                                       int cap_dropc,
                                                       char **cap_drop,
                                                       const char *restart_policy,
                                                       int restart_delay_seconds,
                                                       const char *log_destination) {
    return perform_start_with_metadata_name_options_internal(inv,
                                                             store,
                                                             tail,
                                                             console_mode,
                                                             auto_remove,
                                                             interactive_stdin,
                                                             argc,
                                                             argv,
                                                             exec_path,
                                                             run_alias,
                                                             requested_run_name,
                                                             envc,
                                                             env,
                                                             portc,
                                                             ports,
                                                             volumec,
                                                             volumes,
                                                             restart_policy,
                                                             restart_delay_seconds,
                                                             log_destination,
                                                             cap_addc,
                                                             cap_add,
                                                             cap_dropc,
                                                             cap_drop,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             0,
                                                             NULL);
}

static int perform_start_with_metadata_name_options_internal(const struct hold_invocation *inv,
                                                             const struct hold_store *store,
                                                             bool tail,
                                                             bool console_mode,
                                                             bool auto_remove,
                                                             bool interactive_stdin,
                                                             int argc,
                                                             char **argv,
                                                             const char *exec_path,
                                                             const char *run_alias,
                                                             const char *requested_run_name,
                                                             int envc,
                                                             char **env,
                                                             int portc,
                                                             char **ports,
                                                             int volumec,
                                                             char **volumes,
                                                             const char *restart_policy,
                                                             int restart_delay_seconds,
                                                             const char *log_destination,
                                                             int cap_addc,
                                                             char **cap_add,
                                                             int cap_dropc,
                                                             char **cap_drop,
                                                             const char *existing_id,
                                                             const char *existing_log_path,
                                                             const char *existing_run_name,
                                                             int64_t existing_created_unix_ns,
                                                             const char *existing_created_at) {
    if (argc <= 0 || !argv || !argv[0] ||
        envc < 0 || (envc > 0 && !env) ||
        portc < 0 || (portc > 0 && !ports) ||
        volumec < 0 || (volumec > 0 && !volumes) ||
        cap_addc < 0 || (cap_addc > 0 && !cap_add) ||
        cap_dropc < 0 || (cap_dropc > 0 && !cap_drop) ||
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
                hold_die_errno("hold: run name too long");
            }
        }
    } else {
        if (reserve_hashed_run_id(store,
                                  run_alias,
                                  resolved_exec_path,
                                  argc,
                                  launch_argv,
                                  observed_cwd[0] ? observed_cwd : NULL,
                                  start_unix_ns,
                                  id) != 0) {
            free_launch_and_observed_argv(launch_argv, observed_argv, argc);
            hold_die_errno("hold: failed to generate id");
        }
        int name_rc = generate_run_name_for_id(store, id, requested_run_name, run_name);
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
    uid_t console_owner_uid = geteuid();
    bool console_have_allowed_peer_uid = inv && inv->euid_root && inv->have_sudo_user && inv->invoking_uid != console_owner_uid;
    uid_t console_allowed_peer_uid = console_have_allowed_peer_uid ? inv->invoking_uid : (uid_t)0;

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
                                   resolved_exec_path,
                                   launch_argv,
                                   cap_addc,
                                   cap_add,
                                   cap_dropc,
                                   cap_drop,
                                   restart_policy,
                                   restart_delay_seconds);
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
                    if (hold_mark_run_finished(store, id, status) == 0) break;
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
        if (apply_child_env(envc, env) != 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (apply_child_capabilities(cap_addc, cap_add, cap_dropc, cap_drop) != 0) {
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

    char logger_cmdline[HOLD_PATH_MAX];
    if (hold_format_argv_human(logger_cmdline, sizeof(logger_cmdline), argc, launch_argv) != 0) {
        snprintf(logger_cmdline, sizeof(logger_cmdline), "?");
    }
    if (!console_mode) {
        spawn_log_capture(stdout_pipe[0],
                          stderr_pipe[0],
                          log_path,
                          log_destination,
                          id,
                          run_name,
                          run_alias,
                          logger_cmdline);
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
    if (run_alias && hold_valid_alias(run_alias)) {
        r.has_alias = true;
        if (hold_checked_snprintf(r.alias, sizeof(r.alias), "%s", run_alias) != 0) {
            hold_die_errno("hold: alias too long");
        }
    }
    if (run_name[0]) {
        r.has_name = true;
        if (hold_checked_snprintf(r.name, sizeof(r.name), "%s", run_name) != 0) {
            hold_die_errno("hold: run name too long");
        }
    }
    if (console_sock[0]) {
        r.has_console = true;
        if (hold_checked_snprintf(r.console_sock, sizeof(r.console_sock), "%s", console_sock) != 0) {
            hold_die_errno("hold: console socket path too long");
        }
    }
    r.has_stdio_config = true;
    r.attach_stdin = interactive_stdin || console_mode;
    r.attach_stdout = true;
    r.attach_stderr = true;
    r.tty = console_mode;
    r.open_stdin = interactive_stdin || console_mode;
    r.stdin_once = false;
    r.mode_interactive = interactive_stdin;
    r.mode_tty = console_mode;
    r.mode_detach = !tail && !console_mode;
    r.allow_multi = false;
    if (envc > 0 && hold_copy_argv(&r.env, envc, env) != 0) {
        hold_die_errno("hold: failed to copy env metadata");
    }
    r.envc = envc;
    if (portc > 0 && hold_copy_argv(&r.ports, portc, ports) != 0) {
        hold_die_errno("hold: failed to copy port metadata");
    }
    r.portc = portc;
    if (volumec > 0 && hold_copy_argv(&r.volumes, volumec, volumes) != 0) {
        hold_die_errno("hold: failed to copy volume metadata");
    }
    r.volumec = volumec;
    if (cap_addc > 0 && hold_copy_argv(&r.cap_add, cap_addc, cap_add) != 0) {
        hold_die_errno("hold: failed to copy capability add metadata");
    }
    r.cap_addc = cap_addc;
    if (cap_dropc > 0 && hold_copy_argv(&r.cap_drop, cap_dropc, cap_drop) != 0) {
        hold_die_errno("hold: failed to copy capability drop metadata");
    }
    r.cap_dropc = cap_dropc;
    if (restart_enabled) {
        if (hold_checked_snprintf(r.restart_policy, sizeof(r.restart_policy), "%s", restart_policy) != 0) {
            hold_die_errno("hold: restart policy too long");
        }
        r.has_restart_policy = true;
    }
    r.restart_delay_seconds = restart_delay_seconds;
    r.has_restart_delay = restart_delay_seconds > 0;
    if (log_destination && *log_destination) {
        if (hold_checked_snprintf(r.log_destination, sizeof(r.log_destination), "%s", log_destination) != 0) {
            hold_die_errno("hold: log destination too long");
        }
        r.has_log_destination = true;
    }
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
                chown(record_path, inv->invoking_uid, inv->invoking_gid) != 0) {
                chown_rc = -1;
            }
            if (chown(log_path, inv->invoking_uid, inv->invoking_gid) != 0) {
                chown_rc = -1;
            }
            if (console_sock[0] && hold_path_exists(console_sock) &&
                chown(console_sock, inv->invoking_uid, inv->invoking_gid) != 0) {
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
            } else if (hold_write_public_index_atomic(store, &r) != 0) {
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
                char boot[128] = {0};
                bool have_boot = hold_current_boot_id(boot, sizeof(boot));
                bool removed = false;
                int prune_rc = hold_prune_one_run(store, r.id, have_boot ? boot : NULL, true, &removed);
                if (tail_rc == 0 && prune_rc != 0) tail_rc = prune_rc;
            }
            hold_free_argv_alloc(r.env, r.envc);
            hold_free_argv_alloc(r.ports, r.portc);
            hold_free_argv_alloc(r.volumes, r.volumec);
            hold_free_argv_alloc(r.cap_add, r.cap_addc);
            hold_free_argv_alloc(r.cap_drop, r.cap_dropc);
            return tail_rc;
        }
        if (auto_remove) {
            spawn_auto_remove_watcher(store, &r);
        }
        hold_free_argv_alloc(r.env, r.envc);
        hold_free_argv_alloc(r.ports, r.portc);
        hold_free_argv_alloc(r.volumes, r.volumec);
        hold_free_argv_alloc(r.cap_add, r.cap_addc);
        hold_free_argv_alloc(r.cap_drop, r.cap_dropc);
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

static int private_start_hash_for_token(const struct hold_store *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN],
                                        bool *matched) {
    *matched = false;
    if (hold_valid_profile_hash(token)) {
        *matched = true;
        if (hold_profile_exists_in_store(store, token) != 0) {
            return -1;
        }
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (hold_valid_alias(token)) {
        if (hold_alias_lookup_hash(store, token, hash) != 0) {
            return 0;
        }
        *matched = true;
        if (hold_profile_exists_in_store(store, hash) != 0) {
            return -1;
        }
        return 1;
    }
    return 0;
}

static int private_start_recipe_for_token(const struct hold_store *store,
                                          const char *token,
                                          struct hold_profile *recipe,
                                          bool *matched) {
    *matched = false;
    memset(recipe, 0, sizeof(*recipe));
    if (!hold_valid_alias(token)) {
        return 0;
    }
    if (hold_alias_lookup_recipe(store, token, recipe) != 0) {
        return 0;
    }
    *matched = true;
    return 1;
}

static void free_start_profile_target(struct start_profile_target *target) {
    if (target && target->has_recipe) {
        hold_free_profile(&target->recipe);
        target->has_recipe = false;
    }
}

static void start_target_set_alias(struct start_profile_target *target, const char *alias) {
    if (hold_valid_alias(alias)) {
        if (hold_checked_snprintf(target->alias, sizeof(target->alias), "%s", alias) == 0) {
            target->has_alias = true;
        }
    }
}

static int start_target_set_recipe(struct start_profile_target *target,
                                   const struct hold_store *store,
                                   const char *alias) {
    target->store = *store;
    target->has_recipe = true;
    start_target_set_alias(target, alias);
    return 1;
}

static int start_target_set_hash(struct start_profile_target *target,
                                 const struct hold_store *store,
                                 const char *hash,
                                 const char *alias,
                                 bool needs_elevation) {
    if (!hold_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    target->store = *store;
    if (hold_checked_snprintf(target->hash, sizeof(target->hash), "%s", hash) != 0) {
        return -1;
    }
    target->has_hash = true;
    target->needs_elevation = needs_elevation;
    start_target_set_alias(target, alias);
    return 1;
}

static int count_running_alias(const struct hold_store *store, const char *alias, size_t *count_out) {
    struct alias_match_list matches;
    if (hold_collect_private_alias_matches(store, alias, "start", &matches) != 0) {
        return -1;
    }
    *count_out = matches.count;
    hold_free_alias_match_list(&matches);
    return 0;
}

static int reconnect_running_console_alias(const struct hold_invocation *inv,
                                           const struct hold_store *user_store,
                                           const struct hold_store *system_store,
                                           const char *program,
                                           const struct hold_store *store,
                                           const char *alias,
                                           bool *handled) {
    *handled = false;
    struct alias_match_list matches;
    if (hold_collect_private_alias_matches(store, alias, "console", &matches) != 0) {
        return -1;
    }
    if (matches.count == 0) {
        hold_free_alias_match_list(&matches);
        return 0;
    }
    *handled = true;
    if (matches.count == 1) {
        char id[sizeof(matches.items[0].id)];
        snprintf(id, sizeof(id), "%s", matches.items[0].id);
        hold_free_alias_match_list(&matches);
        return hold_cmd_console_action(inv, user_store, system_store, program, id);
    }
    hold_free_alias_match_list(&matches);
    return hold_cmd_console_action(inv, user_store, system_store, program, alias);
}

static int resolve_start_profile_target(const struct hold_invocation *inv,
                                        const struct hold_store *current_user_store,
                                        const struct hold_store *system_store,
                                        const char *token,
                                        struct start_profile_target *out) {
    memset(out, 0, sizeof(*out));
    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    bool cap_token = false;
    if (scope == ID_TOKEN_SYSTEM && hold_parse_alias_cap_atom(atom, cap_alias, cap_hash) == 0) {
        if (!inv->euid_root || hold_verify_system_alias_cap(system_store, cap_alias, cap_hash) != 0) {
            fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        atom = cap_alias;
        cap_token = true;
    }
    if (scope == ID_TOKEN_INVALID) {
        return 0;
    }
    if (!hold_valid_profile_hash(atom) && !hold_valid_alias(atom)) {
        return 0;
    }

    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "hold: error: user:%s requires sudo provenance\n", atom);
                return -1;
            }
            bool matched = false;
            int rc = private_start_recipe_for_token(&user_store, atom, &out->recipe, &matched);
            if (rc == 1) {
                return start_target_set_recipe(out, &user_store, atom);
            }
            rc = private_start_hash_for_token(&user_store, atom, out->hash, &matched);
            if (rc == 1) {
                return start_target_set_hash(out, &user_store, out->hash, hold_valid_alias(atom) ? atom : NULL, false);
            }
            if (rc < 0 && matched) {
                fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            return 0;
        }
        if (scope == ID_TOKEN_SYSTEM) {
            if (cap_token) {
                bool matched = false;
                int rc = private_start_hash_for_token(system_store, cap_hash, out->hash, &matched);
                if (rc == 1) {
                    return start_target_set_hash(out, system_store, out->hash, cap_alias, false);
                }
                fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            bool matched = false;
            int rc = private_start_hash_for_token(system_store, atom, out->hash, &matched);
            if (rc == 1) {
                return start_target_set_hash(out, system_store, out->hash, hold_valid_alias(atom) ? atom : NULL, false);
            }
            if (rc < 0 && matched) {
                fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            return 0;
        }

        bool matched = false;
        int rc = private_start_hash_for_token(system_store, atom, out->hash, &matched);
        if (rc == 1) {
            return start_target_set_hash(out, system_store, out->hash, hold_valid_alias(atom) ? atom : NULL, false);
        }
        if (rc < 0 && matched) {
            fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        if (inv->have_sudo_user) {
            struct hold_store user_store;
            if (hold_init_invoking_user_store(inv, &user_store) == 0) {
                matched = false;
                rc = private_start_recipe_for_token(&user_store, atom, &out->recipe, &matched);
                if (rc == 1) {
                    return start_target_set_recipe(out, &user_store, atom);
                }
                rc = private_start_hash_for_token(&user_store, atom, out->hash, &matched);
                if (rc == 1) {
                    return start_target_set_hash(out, &user_store, out->hash, hold_valid_alias(atom) ? atom : NULL, false);
                }
                if (rc < 0 && matched) {
                    fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
                    return -1;
                }
            }
        }
        return 0;
    }

    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        bool matched = false;
        int rc = private_start_recipe_for_token(current_user_store, atom, &out->recipe, &matched);
        if (rc == 1) {
            return start_target_set_recipe(out, current_user_store, atom);
        }
        rc = private_start_hash_for_token(current_user_store, atom, out->hash, &matched);
        if (rc == 1) {
            return start_target_set_hash(out, current_user_store, out->hash, hold_valid_alias(atom) ? atom : NULL, false);
        }
        if (rc < 0 && matched) {
            fprintf(stderr, "hold: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        if (scope == ID_TOKEN_USER) {
            return 0;
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        int rc = hold_resolve_public_profile_token(system_store, atom, out->hash);
        if (rc == 1) {
            return start_target_set_hash(out, system_store, out->hash, hold_valid_alias(atom) ? atom : NULL, true);
        }
    }
    return 0;
}

int hold_elevate_start_token(const char *program,
                               bool tail,
                               bool console_mode,
                               bool interactive_stdin,
                               const char *token_atom,
                               const char *hash,
                               bool multi,
                               int multi_count) {
    char token[8 + ALIAS_MAX_LEN + 1 + PROFILE_HASH_STR_LEN];
    char count_buf[32];
    char *canon[10];
    int n = 0;
    canon[n++] = "start";
    if (tail) {
        canon[n++] = "--tail";
    }
    if (console_mode) {
        canon[n++] = "--console";
    }
    if (interactive_stdin) {
        canon[n++] = "--interactive";
    }
    if (multi) {
        if (multi_count == 1) {
            canon[n++] = "--force";
        } else {
            canon[n++] = "--multi";
            snprintf(count_buf, sizeof(count_buf), "%d", multi_count);
            canon[n++] = count_buf;
        }
    }
    if (hash && hold_valid_alias(token_atom) && hold_valid_profile_hash(hash)) {
        canon[n++] = "000000000000";
        canon[n++] = (char *)token_atom;
        canon[n++] = (char *)hash;
        return hold_elevate_with_sudo_canonical(program, n, canon);
    } else if (hold_checked_snprintf(token, sizeof(token), "system:%s", token_atom) != 0) {
        return 3;
    }
    canon[n++] = token;
    return hold_elevate_with_sudo_canonical(program, n, canon);
}

static int hold_perform_profile_start_options(const struct hold_invocation *inv,
                                                const struct hold_store *store,
                                                bool tail,
                                                bool console_mode,
                                                bool auto_remove,
                                                bool interactive_stdin,
                                                const char *hash,
                                                const char *alias,
                                                const char *restart_policy,
                                                int restart_delay_seconds,
                                                const char *log_destination) {
    struct hold_profile p;
    if (hold_load_profile_by_hash(store, hash, &p) != 0) {
        fprintf(stderr, "hold: error: profile %s is unavailable\n", hash);
        return 5;
    }
    bool eff_console = console_mode || p.mode_tty;
    bool eff_interactive = interactive_stdin || p.mode_interactive;
    bool eff_tail = tail;
    if (!tail && p.mode_detach && !console_mode && !interactive_stdin) {
        eff_tail = false;
    }
    int rc = hold_perform_start_with_metadata_name_cap_options(inv, store, eff_tail, eff_console, auto_remove, eff_interactive,
                                                           p.argc, p.argv, p.binary_path, alias, NULL,
                                                           p.envc, p.env,
                                                           p.portc, p.ports,
                                                           p.volumec, p.volumes,
                                                           p.cap_addc, p.cap_add,
                                                           p.cap_dropc, p.cap_drop,
                                                           restart_policy ? restart_policy : (p.has_restart_policy ? p.restart_policy : NULL),
                                                           restart_policy ? restart_delay_seconds : p.restart_delay_seconds,
                                                           log_destination ? log_destination : (p.has_log_destination ? p.log_destination : NULL));
    hold_free_profile(&p);
    return rc;
}


int hold_perform_profile_start(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 bool tail,
                                 bool console_mode,
                                 const char *hash,
                                 const char *alias) {
    return hold_perform_profile_start_options(inv, store, tail, console_mode, false, false, hash, alias, NULL, 0, NULL);
}

int hold_cmd_start_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *program,
                            const struct hold_store *fallback_store,
                            bool tail,
                            bool console_mode,
                            bool multi,
                            int multi_count,
                            int argc,
                            char **argv) {
    return hold_cmd_start_action_options(inv, user_store, system_store, program, fallback_store, tail, console_mode, false, false, multi, multi_count, NULL, 0, argc, argv);
}

int hold_cmd_start_action_options(const struct hold_invocation *inv,
                                    const struct hold_store *user_store,
                                    const struct hold_store *system_store,
                                    const char *program,
                                    const struct hold_store *fallback_store,
                                    bool tail,
                                    bool console_mode,
                                    bool auto_remove,
                                    bool interactive_stdin,
                                    bool multi,
                                    int multi_count,
                                    const char *restart_policy,
                                    int restart_delay_seconds,
                                    int argc,
                                    char **argv) {
    return hold_cmd_start_action_name_options(inv, user_store, system_store, program, fallback_store, tail, console_mode,
                                              auto_remove, interactive_stdin, multi, multi_count, restart_policy,
                                              restart_delay_seconds, NULL, NULL, true, argc, argv);
}

int hold_cmd_start_action_name_options(const struct hold_invocation *inv,
                                        const struct hold_store *user_store,
                                        const struct hold_store *system_store,
                                        const char *program,
                                        const struct hold_store *fallback_store,
                                        bool tail,
                                        bool console_mode,
                                        bool auto_remove,
                                        bool interactive_stdin,
                                        bool multi,
                                        int multi_count,
                                        const char *restart_policy,
                                        int restart_delay_seconds,
                                        const char *requested_run_name,
                                        const char *log_destination,
                                        bool allow_existing_restart,
                                        int argc,
                                        char **argv) {
    if (argc == 1) {
        if (allow_existing_restart) {
            char restart_id[ID_STR_LEN];
            const struct hold_store *restart_store = NULL;
            int restart_match = 0;
            if (user_store && user_store->record_dir[0]) {
                restart_match = find_restart_record(user_store, argv[0], restart_id);
                if (restart_match == 1) restart_store = user_store;
            }
            if (restart_match == 0 && system_store && system_store->record_dir[0] &&
                (inv->euid_root || (fallback_store && fallback_store->kind == STORE_SYSTEM_MANAGED))) {
                restart_match = find_restart_record(system_store, argv[0], restart_id);
                if (restart_match == 1) restart_store = system_store;
            }
            if (restart_match == -2) {
                return 6;
            }
            if (restart_match < 0) {
                return 3;
            }
            if (restart_store) {
                if (multi) {
                    fprintf(stderr, "hold: error: --multi applies only to profile starts\n");
                    return 5;
                }
                if (requested_run_name) {
                    fprintf(stderr, "hold: error: --name cannot rename an existing run during start\n");
                    return 5;
                }
                return restart_existing_run(inv,
                                            restart_store,
                                            tail,
                                            console_mode,
                                            auto_remove,
                                            interactive_stdin,
                                            restart_policy,
                                            restart_delay_seconds,
                                            restart_id);
            }
        }
        struct start_profile_target target;
        int rc = resolve_start_profile_target(inv, user_store, system_store, argv[0], &target);
        if (rc < 0) {
            return 5;
        }
        if (rc == 1) {
            int start_rc;
            int starts = multi ? multi_count : 1;
            if (tail && starts > 1) {
                fprintf(stderr, "hold: error: --tail cannot follow multiple starts\n");
                free_start_profile_target(&target);
                return 5;
            }
            if (target.needs_elevation) {
                start_rc = 0;
                for (int i = 0; i < starts; i++) {
                    start_rc = hold_elevate_start_token(program,
                                                   tail,
                                                   console_mode,
                                                   interactive_stdin,
                                                   target.has_alias ? target.alias : target.hash,
                                                   target.has_alias ? target.hash : NULL,
                                                   false,
                                                   1);
                    if (start_rc != 0) {
                        break;
                    }
                }
            } else {
                if (console_mode && target.has_alias && !multi) {
                    bool reconnected = false;
                    int reconnect_rc = reconnect_running_console_alias(inv,
                                                                        user_store,
                                                                        system_store,
                                                                        program,
                                                                        &target.store,
                                                                        target.alias,
                                                                        &reconnected);
                    if (reconnect_rc != 0 || reconnected) {
                        free_start_profile_target(&target);
                        return reconnect_rc < 0 ? 3 : reconnect_rc;
                    }
                }
                bool profile_allows_multi = target.has_recipe && target.recipe.allow_multi;
                if (target.has_alias && !multi && !profile_allows_multi) {
                    size_t running = 0;
                    if (count_running_alias(&target.store, target.alias, &running) != 0) {
                        free_start_profile_target(&target);
                        return 3;
                    }
                    if (running > 0) {
                        fprintf(stderr,
                                "hold: error: profile '%s' already has a running process; use --force to start another\n",
                                target.alias);
                        free_start_profile_target(&target);
                        return 6;
                    }
                }
                start_rc = 0;
                for (int i = 0; i < starts; i++) {
                    if (target.has_recipe) {
                        bool eff_console = console_mode || target.recipe.mode_tty;
                        bool eff_interactive = interactive_stdin || target.recipe.mode_interactive;
                        bool eff_tail = tail;
                        if (!tail && target.recipe.mode_detach && !console_mode && !interactive_stdin) {
                            eff_tail = false;
                        }
                        start_rc = hold_perform_start_with_metadata_name_cap_options(inv,
                                                 &target.store,
                                                 eff_tail,
                                                 eff_console,
                                                 auto_remove,
                                                 eff_interactive,
                                                 target.recipe.argc,
                                                 target.recipe.argv,
                                                 target.recipe.binary_path,
                                                 target.has_alias ? target.alias : NULL,
                                                 requested_run_name,
                                                 target.recipe.envc,
                                                 target.recipe.env,
                                                 target.recipe.portc,
                                                 target.recipe.ports,
                                                 target.recipe.volumec,
                                                 target.recipe.volumes,
                                                 target.recipe.cap_addc,
                                                 target.recipe.cap_add,
                                                 target.recipe.cap_dropc,
                                                 target.recipe.cap_drop,
                                                 restart_policy ? restart_policy : (target.recipe.has_restart_policy ? target.recipe.restart_policy : NULL),
                                                 restart_policy ? restart_delay_seconds : target.recipe.restart_delay_seconds,
                                                 log_destination ? log_destination : (target.recipe.has_log_destination ? target.recipe.log_destination : NULL));
                    } else {
                        start_rc = hold_perform_profile_start_options(inv,
                                                         &target.store,
                                                         tail,
                                                         console_mode,
                                                         auto_remove,
                                                         interactive_stdin,
                                                         target.hash,
                                                         target.has_alias ? target.alias : NULL,
                                                         restart_policy,
                                                         restart_delay_seconds,
                                                         log_destination);
                    }
                    if (start_rc != 0) {
                        break;
                    }
                }
            }
            free_start_profile_target(&target);
            return start_rc;
        }
        free_start_profile_target(&target);
    }
    if (multi) {
        fprintf(stderr, "hold: error: --multi applies only to profile starts\n");
        return 5;
    }
    if (requested_run_name || log_destination) {
        if (argc <= 0) {
            fprintf(stderr, "usage: hold start <cmd> [args...]\n");
            return 5;
        }
        if (argc == 1) {
            char *shell_argv[4] = {"sh", "-c", argv[0], NULL};
            return hold_perform_start_with_metadata_name_options(inv,
                                                                  fallback_store,
                                                                  tail,
                                                                  console_mode,
                                                                  auto_remove,
                                                                  interactive_stdin,
                                                                  3,
                                                                  shell_argv,
                                                                  NULL,
                                                                  NULL,
                                                                  requested_run_name,
                                                                  0,
                                                                  NULL,
                                                                  0,
                                                                  NULL,
                                                                  0,
                                                                  NULL,
                                                                  restart_policy,
                                                                  restart_delay_seconds,
                                                                  log_destination);
        }
        return hold_perform_start_with_metadata_name_options(inv,
                                                              fallback_store,
                                                              tail,
                                                              console_mode,
                                                              auto_remove,
                                                              interactive_stdin,
                                                              argc,
                                                              argv,
                                                              NULL,
                                                              NULL,
                                                              requested_run_name,
                                                              0,
                                                              NULL,
                                                              0,
                                                              NULL,
                                                              0,
                                                              NULL,
                                                              restart_policy,
                                                              restart_delay_seconds,
                                                              log_destination);
    }
    return perform_explicit_start_options(inv, fallback_store, tail, console_mode, auto_remove, interactive_stdin, restart_policy, restart_delay_seconds, argc, argv);
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

static int perform_explicit_start_options(const struct hold_invocation *inv,
                                            const struct hold_store *store,
                                            bool tail,
                                            bool console_mode,
                                            bool auto_remove,
                                            bool interactive_stdin,
                                            const char *restart_policy,
                                            int restart_delay_seconds,
                                            int argc,
                                            char **argv) {
    if (argc <= 0) {
        fprintf(stderr, "usage: hold start <cmd> [args...]\n");
        return 5;
    }
    if (argc == 1) {
        char *shell_argv[4];
        shell_argv[0] = "sh";
        shell_argv[1] = "-c";
        shell_argv[2] = argv[0];
        shell_argv[3] = NULL;
        return hold_perform_start_with_metadata_options(inv, store, tail, console_mode, auto_remove, interactive_stdin, 3, shell_argv, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, restart_policy, restart_delay_seconds);
    }
    return hold_perform_start_with_metadata_options(inv, store, tail, console_mode, auto_remove, interactive_stdin, argc, argv, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, restart_policy, restart_delay_seconds);
}
