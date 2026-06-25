#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

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

static const char *explicit_start_argv0(bool owned, const char *command, int argc, char **argv);
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
                                            int argc,
                                            char **argv);
static void spawn_auto_remove_watcher(const struct hold_store *store, const struct hold_run_record *record);

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
    return hold_path_is_within_dir(resolved, home);
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
    return hold_perform_start_with_env_options(inv, store, tail, console_mode, false, argc, argv, exec_path, run_alias, envc, env);
}

int hold_perform_start_with_env_options(const struct hold_invocation *inv,
                                          const struct hold_store *store,
                                          bool tail,
                                          bool console_mode,
                                          bool auto_remove,
                                          int argc,
                                          char **argv,
                                          const char *exec_path,
                                          const char *run_alias,
                                          int envc,
                                          char **env) {
    if (argc <= 0 || !argv || !argv[0] || envc < 0 || (envc > 0 && !env)) {
        hold_usage();
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

    char **launch_argv = NULL;
    if (hold_copy_argv(&launch_argv, argc, argv) != 0) {
        hold_die_errno("hold: failed to prepare argv");
    }
    free(launch_argv[0]);
    launch_argv[0] = strdup(resolved_exec_path);
    if (!launch_argv[0]) {
        hold_die_errno("hold: failed to prepare argv");
    }

    char id[16], log_path[HOLD_PATH_MAX], reserve_path[HOLD_PATH_MAX], console_sock[HOLD_PATH_MAX], boot_id[128] = {0};
    console_sock[0] = '\0';
    bool has_boot = hold_current_boot_id(boot_id, sizeof(boot_id));
    struct hold_store system_hint;
    struct hold_store invoking_user_store;
    const struct hold_store *avoid_public_store = NULL;
    const struct hold_store *avoid_user_store = NULL;

    if (store->kind == STORE_USER_LOCAL) {
        if (hold_init_system_store(&system_hint) == 0) {
            avoid_public_store = &system_hint;
        }
    } else if (inv && inv->have_sudo_user && inv->invoking_home[0]) {
        if (hold_init_user_store_from_home(inv->invoking_home, &invoking_user_store) == 0) {
            avoid_user_store = &invoking_user_store;
        }
    }

    if (hold_gen_id_for_store(store, avoid_public_store, avoid_user_store, id, sizeof(id)) != 0) {
        hold_free_argv_alloc(launch_argv, argc);
        hold_die_errno("hold: failed to generate id");
    }
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
        hold_free_argv_alloc(launch_argv, argc);
        hold_die_errno("hold: log path too long");
    }
    if (hold_checked_snprintf(reserve_path, sizeof(reserve_path), "%s/.%s.reserve", store->record_dir, id) != 0) {
        hold_free_argv_alloc(launch_argv, argc);
        hold_die_errno("hold: reserve path too long");
    }
    if (console_mode && hold_format_console_sock_path(store, id, console_sock, sizeof(console_sock)) != 0) {
        hold_free_argv_alloc(launch_argv, argc);
        hold_die_errno("hold: console socket path too long");
    }
    uid_t console_owner_uid = geteuid();
    bool console_have_allowed_peer_uid = inv && inv->euid_root && inv->have_sudo_user && inv->invoking_uid != console_owner_uid;
    uid_t console_allowed_peer_uid = console_have_allowed_peer_uid ? inv->invoking_uid : (uid_t)0;

    int pipefd[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(pipefd, O_CLOEXEC) != 0)
#endif
    {
        if (pipe(pipefd) != 0) {
            int saved = errno;
            unlink(reserve_path);
            hold_free_argv_alloc(launch_argv, argc);
            errno = saved;
            hold_die_errno("hold: pipe failed");
        }
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(pipefd[0]);
            close(pipefd[1]);
            unlink(reserve_path);
            hold_free_argv_alloc(launch_argv, argc);
            errno = saved;
            hold_die_errno("hold: pipe setup failed");
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(reserve_path);
        hold_free_argv_alloc(launch_argv, argc);
        errno = saved;
        hold_die_errno("hold: fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
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
                                      log_path,
                                      console_sock,
                                      console_owner_uid,
                                      console_have_allowed_peer_uid,
                                      console_allowed_peer_uid,
                                      argc,
                                      launch_argv,
                                      resolved_exec_path);
            _exit(127);
        }
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (nullfd > 2) {
            close(nullfd);
        }

        int lfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (lfd < 0 || dup2(lfd, STDOUT_FILENO) < 0 || dup2(lfd, STDERR_FILENO) < 0) {
            int e = errno;
            hold_write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (lfd > 2) {
            close(lfd);
        }
        execv(resolved_exec_path, launch_argv);
        int e = errno;
        hold_write_all(pipefd[1], &e, sizeof(e));
        _exit(127);
    }

    close(pipefd[1]);
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(pipefd[0], &child_errno);
    int handshake_errno = errno;
    close(pipefd[0]);
    if (handshake < 0) {
        hold_rollback_spawned_group(pid, pid);
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        hold_free_argv_alloc(launch_argv, argc);
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
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        hold_free_argv_alloc(launch_argv, argc);
        return 1;
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
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    r.start_unix_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    hold_format_rfc3339_utc_from_ns(r.start_unix_ns, r.started_at, sizeof(r.started_at));
    r.has_started_at = true;
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
    if (console_sock[0]) {
        r.has_console = true;
        if (hold_checked_snprintf(r.console_sock, sizeof(r.console_sock), "%s", console_sock) != 0) {
            hold_die_errno("hold: console socket path too long");
        }
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
                if (record_path[0]) {
                    unlink(record_path);
                }
                unlink(log_path);
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink(reserve_path);
                hold_free_argv_alloc(launch_argv, argc);
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
                if (record_path[0]) {
                    unlink(record_path);
                }
                unlink(log_path);
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink(reserve_path);
                hold_free_argv_alloc(launch_argv, argc);
                char public_path[HOLD_PATH_MAX];
                if (hold_checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                    unlink(public_path);
                }
                errno = saved;
                hold_die_errno("hold: failed to write public index");
            }
        }
        printf("%s\n", r.id);
        hold_sig_note(inv,
                 "hold  started  %s   %s\n"
                 "         log      %s\n"
                 "         tail     hold tail %s\n"
                 "%s%s%s"
                 "         stop     hold stop %s\n",
                 r.id,
                 r.cmdline[0] ? r.cmdline : "?",
                 r.log_path,
                 r.id,
                 r.has_console ? "         console  hold console " : "",
                 r.has_console ? r.id : "",
                 r.has_console ? "\n" : "",
                 r.id);
        fflush(stdout);

        if (tail) {
            hold_free_argv_alloc(launch_argv, argc);
            int tail_rc = hold_tail_log_until_exit(&r, false, true);
            if (auto_remove) {
                char boot[128] = {0};
                bool have_boot = hold_current_boot_id(boot, sizeof(boot));
                bool removed = false;
                int prune_rc = hold_prune_one_run(store, r.id, have_boot ? boot : NULL, true, &removed);
                if (tail_rc == 0 && prune_rc != 0) return prune_rc;
            }
            return tail_rc;
        }
        if (auto_remove) {
            spawn_auto_remove_watcher(store, &r);
        }
        hold_free_argv_alloc(launch_argv, argc);
        return 0;
    }
    {
        int saved = errno;
        hold_rollback_spawned_group(pid, pid);
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        hold_free_argv_alloc(launch_argv, argc);
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
                               const char *token_atom,
                               const char *hash,
                               bool multi,
                               int multi_count) {
    char token[8 + ALIAS_MAX_LEN + 1 + PROFILE_HASH_STR_LEN];
    char count_buf[32];
    char *canon[7];
    int n = 0;
    canon[n++] = "start";
    if (tail) {
        canon[n++] = "--tail";
    }
    if (console_mode) {
        canon[n++] = "--console";
    }
    if (hash && hold_valid_alias(token_atom) && hold_valid_profile_hash(hash)) {
        canon[n++] = "00000000";
        canon[n++] = (char *)token_atom;
        canon[n++] = (char *)hash;
        return hold_elevate_with_sudo_canonical(program, n, canon);
    } else if (hold_checked_snprintf(token, sizeof(token), "system:%s", token_atom) != 0) {
        return 3;
    }
    canon[n++] = token;
    if (multi) {
        canon[n++] = "--multi";
        if (multi_count != 1) {
            snprintf(count_buf, sizeof(count_buf), "%d", multi_count);
            canon[n++] = count_buf;
        }
    }
    return hold_elevate_with_sudo_canonical(program, n, canon);
}

static int hold_perform_profile_start_options(const struct hold_invocation *inv,
                                                const struct hold_store *store,
                                                bool tail,
                                                bool console_mode,
                                                bool auto_remove,
                                                const char *hash,
                                                const char *alias) {
    struct hold_profile p;
    if (hold_load_profile_by_hash(store, hash, &p) != 0) {
        fprintf(stderr, "hold: error: profile %s is unavailable\n", hash);
        return 5;
    }
    int rc = hold_perform_start_with_env_options(inv, store, tail, console_mode, auto_remove, p.argc, p.argv, p.binary_path, alias, p.envc, p.env);
    hold_free_profile(&p);
    return rc;
}


int hold_perform_profile_start(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 bool tail,
                                 bool console_mode,
                                 const char *hash,
                                 const char *alias) {
    return hold_perform_profile_start_options(inv, store, tail, console_mode, false, hash, alias);
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
    return hold_cmd_start_action_options(inv, user_store, system_store, program, fallback_store, tail, console_mode, false, multi, multi_count, argc, argv);
}

int hold_cmd_start_action_options(const struct hold_invocation *inv,
                                    const struct hold_store *user_store,
                                    const struct hold_store *system_store,
                                    const char *program,
                                    const struct hold_store *fallback_store,
                                    bool tail,
                                    bool console_mode,
                                    bool auto_remove,
                                    bool multi,
                                    int multi_count,
                                    int argc,
                                    char **argv) {
    if (argc == 1) {
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
                                                   target.has_alias ? target.alias : target.hash,
                                                   target.has_alias ? target.hash : NULL,
                                                   false,
                                                   1);
                    if (start_rc != 0) {
                        break;
                    }
                }
            } else {
                if (target.has_alias && !multi) {
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
                        start_rc = hold_perform_start_with_env_options(inv,
                                                 &target.store,
                                                 tail,
                                                 console_mode,
                                                 auto_remove,
                                                 target.recipe.argc,
                                                 target.recipe.argv,
                                                 target.recipe.binary_path,
                                                 target.has_alias ? target.alias : NULL,
                                                 target.recipe.envc,
                                                 target.recipe.env);
                    } else {
                        start_rc = hold_perform_profile_start_options(inv,
                                                         &target.store,
                                                         tail,
                                                         console_mode,
                                                         auto_remove,
                                                         target.hash,
                                                         target.has_alias ? target.alias : NULL);
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
    return perform_explicit_start_options(inv, fallback_store, tail, console_mode, auto_remove, argc, argv);
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
        return hold_perform_start_with_env_options(inv, store, tail, console_mode, auto_remove, 3, shell_argv, NULL, NULL, 0, NULL);
    }
    return hold_perform_start_with_env_options(inv, store, tail, console_mode, auto_remove, argc, argv, NULL, NULL, 0, NULL);
}
