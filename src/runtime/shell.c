#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/console.h"
#include "hold/console_internal.h"
#include "hold/observe.h"
#include "hold/term.h"

struct shell_raw_terminal {
    struct termios original;
    bool active;
};

/* Set when a `hold off` running inside this session SIGTERMs the proxy so the
 * relay loop can unwind cleanly (restore the terminal, hang up the shell). */
static volatile sig_atomic_t g_hold_on_off_requested = 0;
static void hold_on_off_signal_handler(int sig) {
    (void)sig;
    g_hold_on_off_requested = 1;
}

#if defined(__linux__)
static int read_proc_comm(pid_t pid, char *out, size_t n) {
    char path[64];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid) != 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = '\0';
    size_t len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return 0;
}
#endif

static const char *resolve_user_shell(void) {
    const char *shell = getenv("SHELL");
    if (shell && *shell) return shell;
    static char passwd_shell[HOLD_PATH_MAX];
    struct hold_passwd_entry pw;
    if (hold_lookup_passwd_by_uid(geteuid(), &pw) == 0 && pw.shell[0] &&
        hold_checked_snprintf(passwd_shell, sizeof(passwd_shell), "%s", pw.shell) == 0) {
        return passwd_shell;
    }
    return "/bin/sh";
}

static int enter_raw_if_tty(struct shell_raw_terminal *raw) {
    memset(raw, 0, sizeof(*raw));
    if (!isatty(STDIN_FILENO)) return 0;
    if (tcgetattr(STDIN_FILENO, &raw->original) != 0) return -1;
    struct termios t = raw->original;
    cfmakeraw(&t);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return -1;
    raw->active = true;
    return 0;
}

static void leave_raw(struct shell_raw_terminal *raw) {
    if (raw->active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw->original);
        raw->active = false;
    }
}

#if defined(__linux__)
static void shell_close_stdio_to_devnull(void) {
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) return;
    (void)dup2(fd, STDIN_FILENO);
    (void)dup2(fd, STDOUT_FILENO);
    (void)dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
}
#endif

/* The session shell rides the one spawn engine (term/spawn); if the user's
 * shell cannot be spawned the session falls back to /bin/sh, preserving the
 * old in-child exec ladder's behavior. */
static int spawn_shell_session(const char *shell,
                               unsigned short rows,
                               unsigned short cols,
                               int *master_out,
                               pid_t *child_out) {
    char *shell_argv[2] = {(char *)shell, NULL};
    struct hold_term_spawn spec = {
        .argv = shell_argv,
        .exec_path = shell,
        .rows = rows,
        .cols = cols,
    };
    if (hold_term_pty_spawn(&spec, master_out, child_out) == 0) {
        return 0;
    }
    char *sh_argv[2] = {"sh", NULL};
    struct hold_term_spawn fallback = {
        .argv = sh_argv,
        .exec_path = "/bin/sh",
        .rows = rows,
        .cols = cols,
    };
    return hold_term_pty_spawn(&fallback, master_out, child_out);
}

#if defined(__linux__)
static int find_process_in_pgid(pid_t pgid, pid_t *pid_out, pid_t *sid_out) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    pid_t best = 0, best_sid = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char *end = NULL;
        long pid_long = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0' || pid_long <= 0) continue;
        pid_t proc_pgid = 0, proc_sid = 0;
        char state = 0;
        if (hold_proc_read_ids((pid_t)pid_long, &proc_pgid, &proc_sid, &state) != 0) continue;
        if (proc_pgid != pgid || state == 'Z') continue;
        if ((pid_t)pid_long == pgid) {
            best = (pid_t)pid_long;
            best_sid = proc_sid;
            break;
        }
        if (best == 0 || pid_long < best) {
            best = (pid_t)pid_long;
            best_sid = proc_sid;
        }
    }
    closedir(d);
    if (best <= 0) {
        errno = ESRCH;
        return -1;
    }
    *pid_out = best;
    *sid_out = best_sid;
    return 0;
}

static int read_proc_cmdline(pid_t pid, char ***argv_out, int *argc_out) {
    char path[128], buf[65536];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid) != 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t nr;
    do {
        nr = read(fd, buf, sizeof(buf) - 1);
    } while (nr < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (nr <= 0) {
        errno = nr < 0 ? saved : EIO;
        return -1;
    }
    buf[nr] = '\0';
    int argc = 0;
    for (ssize_t i = 0; i < nr; i++) {
        if (buf[i] == '\0') argc++;
    }
    if (buf[nr - 1] != '\0') argc++;
    if (argc <= 0) {
        errno = EIO;
        return -1;
    }
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) return -1;
    ssize_t pos = 0;
    for (int i = 0; i < argc; i++) {
        ssize_t start = pos;
        while (pos < nr && buf[pos] != '\0') pos++;
        argv[i] = strndup(buf + start, (size_t)(pos - start));
        if (!argv[i]) {
            hold_free_argv_alloc(argv, i);
            return -1;
        }
        pos++;
    }
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static int read_proc_exe_path(pid_t pid, char *out, size_t n) {
    char path[128];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid) != 0) return -1;
    ssize_t nr = readlink(path, out, n - 1);
    if (nr < 0 || (size_t)nr >= n) return -1;
    out[nr] = '\0';
    return 0;
}

static int read_proc_cwd_path(pid_t pid, char *out, size_t n) {
    char path[128];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/cwd", (long)pid) != 0) return -1;
    ssize_t nr = readlink(path, out, n - 1);
    if (nr < 0 || (size_t)nr >= n) return -1;
    out[nr] = '\0';
    return 0;
}
#endif

#if defined(__linux__)
static void shell_background_logger(int master, const char *log_path, pid_t shell_pid, pid_t adopted_pgid, pid_t adopted_sid) {
    signal(SIGHUP, SIG_IGN);
    shell_close_stdio_to_devnull();
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) _exit(1);
    int idxfd = hold_open_log_index_fd(log_path, fd);
    while (1) {
        struct pollfd pfd = {
            .fd = master,
            .events = POLLIN,
            .revents = 0,
        };
        int ready;
        do {
            ready = poll(&pfd, 1, 200);
        } while (ready < 0 && errno == EINTR);
        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            /* The one master pump: bytes land in the indexed log; 0 means the
             * adopted side is gone (EOF, or EIO after the last slave closed). */
            char buf[4096];
            ssize_t n = hold_term_pump_master(master, fd, idxfd, buf, sizeof(buf));
            if (n == 0 || (n < 0 && errno != EINTR)) {
                break;
            }
        }
        if (adopted_pgid > 1 && adopted_sid > 0 &&
            hold_group_session_liveness(adopted_pgid, adopted_sid) != GROUP_LIVE) {
            break;
        }
    }
    kill(shell_pid, SIGHUP);
    if (idxfd >= 0) close(idxfd);
    close(fd);
    close(master);
    _exit(0);
}

static bool shell_id_available(const struct hold_store *store,
                               const struct hold_store *avoid_public_store,
                               const char *id) {
    char path[HOLD_PATH_MAX];
    if (!hold_valid_id(id)) return false;
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) != 0 ||
        hold_path_exists(path)) {
        return false;
    }
    if (hold_checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) != 0 ||
        hold_path_exists(path)) {
        return false;
    }
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) != 0 ||
        hold_path_exists(path)) {
        return false;
    }
    if (store->public_dir[0] &&
        (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) != 0 ||
         hold_path_exists(path))) {
        return false;
    }
    if (avoid_public_store && avoid_public_store->public_dir[0] &&
        (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", avoid_public_store->public_dir, id) != 0 ||
         hold_path_exists(path))) {
        return false;
    }
    return true;
}

static void shell_hash_field(struct sha256_ctx *ctx, const char *key, const char *value) {
    hold_sha256_update_nul_field(ctx, key);
    hold_sha256_update_nul_field(ctx, value ? value : "-");
}

static int shell_hashed_adopt_id(const struct hold_store *store,
                                 const struct hold_store *avoid_public_store,
                                 const char *exe_path,
                                 char **argv,
                                 int argc,
                                 const char *cwd_path,
                                 pid_t pid,
                                 pid_t pgid,
                                 int64_t start_unix_ns,
                                 char out[ID_STR_LEN]) {
    if (!store || !out) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned counter = 0; counter < 1024; counter++) {
        struct sha256_ctx ctx;
        unsigned char digest[32];
        char tmp[64];
        hold_sha256_init(&ctx);
        shell_hash_field(&ctx, "scope", "hold-run-adopt-v1");
        shell_hash_field(&ctx, "exe", exe_path);
        shell_hash_field(&ctx, "cwd", cwd_path && *cwd_path ? cwd_path : "-");
        snprintf(tmp, sizeof(tmp), "%lld", (long long)start_unix_ns);
        shell_hash_field(&ctx, "created_ns", tmp);
        snprintf(tmp, sizeof(tmp), "%ld", (long)pid);
        shell_hash_field(&ctx, "pid", tmp);
        snprintf(tmp, sizeof(tmp), "%ld", (long)pgid);
        shell_hash_field(&ctx, "pgid", tmp);
        snprintf(tmp, sizeof(tmp), "%d", argc);
        shell_hash_field(&ctx, "argc", tmp);
        for (int i = 0; i < argc; i++) {
            snprintf(tmp, sizeof(tmp), "argv[%d]", i);
            shell_hash_field(&ctx, tmp, argv ? argv[i] : NULL);
        }
        if (counter > 0) {
            snprintf(tmp, sizeof(tmp), "%u", counter);
            shell_hash_field(&ctx, "collision", tmp);
        }
        hold_sha256_final(&ctx, digest);
        hold_hex_encode(digest, sizeof(digest), out, ID_STR_LEN);
        if (shell_id_available(store, avoid_public_store, out)) {
            return 0;
        }
    }
    errno = EEXIST;
    return -1;
}
#endif

static int adopt_foreground_group(const struct hold_invocation *inv,
                                  const struct hold_store *store,
                                  int master,
                                  pid_t shell_pid,
                                  pid_t fg_pgid) {
#if !defined(__linux__)
    (void)inv;
    (void)store;
    (void)master;
    (void)shell_pid;
    (void)fg_pgid;
    fprintf(stderr, "hold: shell foreground adoption is not implemented on this platform yet\n");
    return 5;
#else
    if (fg_pgid <= 1 || fg_pgid == shell_pid) {
        kill(shell_pid, SIGHUP);
        return 0;
    }
    pid_t adopted_pid = 0, adopted_sid = 0;
    if (find_process_in_pgid(fg_pgid, &adopted_pid, &adopted_sid) != 0) {
        fprintf(stderr, "hold: failed to identify foreground process group %ld\n", (long)fg_pgid);
        return 5;
    }
    char **observed_argv = NULL;
    int argc = 0;
    if (read_proc_cmdline(adopted_pid, &observed_argv, &argc) != 0) {
        fprintf(stderr, "hold: failed to read foreground process arguments\n");
        return 5;
    }
    char **normalized_argv = NULL;
    if (hold_copy_argv(&normalized_argv, argc, observed_argv) != 0) {
        hold_free_argv_alloc(observed_argv, argc);
        return 3;
    }
    char exe_path[HOLD_PATH_MAX] = {0};
    if (read_proc_exe_path(adopted_pid, exe_path, sizeof(exe_path)) == 0 && exe_path[0]) {
        free(normalized_argv[0]);
        normalized_argv[0] = strdup(exe_path);
        if (!normalized_argv[0]) {
            hold_free_argv_alloc(normalized_argv, argc);
            hold_free_argv_alloc(observed_argv, argc);
            return 3;
        }
    }
    char cwd_path[HOLD_PATH_MAX] = {0};
    if (read_proc_cwd_path(adopted_pid, cwd_path, sizeof(cwd_path)) != 0) {
        cwd_path[0] = '\0';
    }
    if (hold_normalize_existing_argv_paths_from_cwd(normalized_argv, argc, 1, cwd_path[0] ? cwd_path : NULL) != 0) {
        hold_free_argv_alloc(normalized_argv, argc);
        hold_free_argv_alloc(observed_argv, argc);
        return 3;
    }

    struct hold_store system_hint;
    const struct hold_store *avoid_public_store = NULL;
    if (store->kind == STORE_USER_LOCAL && hold_init_system_store(&system_hint) == 0) {
        avoid_public_store = &system_hint;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t start_unix_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    char id[ID_STR_LEN], log_path[HOLD_PATH_MAX];
    if (shell_hashed_adopt_id(store,
                              avoid_public_store,
                              exe_path[0] ? exe_path : (observed_argv && argc > 0 ? observed_argv[0] : NULL),
                              normalized_argv,
                              argc,
                              cwd_path,
                              adopted_pid,
                              fg_pgid,
                              start_unix_ns,
                              id) != 0 ||
        hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
        hold_free_argv_alloc(normalized_argv, argc);
        hold_free_argv_alloc(observed_argv, argc);
        return 3;
    }
    /* Two-phase creation, like launched calls: hold the reserve across the
     * log/socket-before-JSON window so a purge sweep cannot eat them. */
    char adopt_reserve[HOLD_PATH_MAX];
    adopt_reserve[0] = '\0';
    if (hold_checked_snprintf(adopt_reserve, sizeof(adopt_reserve), "%s/.%s.reserve", store->record_dir, id) == 0) {
        int rfd = open(adopt_reserve, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (rfd >= 0) close(rfd);
    }

    /* Prefer serving the adopted PTY through a console broker so the run stays
     * reattachable; every fd is opened here so the child has no failure path.
     * If broker setup fails, fall back to the log-only capture. */
    char console_sock[HOLD_PATH_MAX];
    console_sock[0] = '\0';
    int console_listener = -1, console_logfd = -1, console_logidxfd = -1;
    if (hold_format_console_sock_path(store, id, console_sock, sizeof(console_sock)) == 0) {
        console_listener = hold_make_console_listener(console_sock);
        if (console_listener < 0) {
            console_sock[0] = '\0';
        } else {
            console_logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
            console_logidxfd = console_logfd >= 0 ? hold_open_log_index_fd(log_path, console_logfd) : -1;
            if (console_logfd < 0) {
                close(console_listener);
                console_listener = -1;
                unlink(console_sock);
                console_sock[0] = '\0';
            }
        }
    } else {
        console_sock[0] = '\0';
    }

    pid_t logger = fork();
    if (logger < 0) {
        if (console_listener >= 0) {
            close(console_listener);
            if (console_logidxfd >= 0) close(console_logidxfd);
            if (console_logfd >= 0) close(console_logfd);
            unlink(console_sock);
        }
        if (adopt_reserve[0]) unlink(adopt_reserve);
        hold_free_argv_alloc(normalized_argv, argc);
        hold_free_argv_alloc(observed_argv, argc);
        return 3;
    }
    if (logger == 0) {
        if (console_listener >= 0) {
            signal(SIGHUP, SIG_IGN);
            shell_close_stdio_to_devnull();
            hold_run_console_broker_adopted(store, id, console_sock,
                                            console_listener, master,
                                            console_logfd, console_logidxfd,
                                            geteuid(), fg_pgid, adopted_sid, shell_pid);
            _exit(0);
        }
        shell_background_logger(master, log_path, shell_pid, fg_pgid, adopted_sid);
    }
    if (console_listener >= 0) {
        close(console_listener);
        if (console_logidxfd >= 0) close(console_logidxfd);
        if (console_logfd >= 0) close(console_logfd);
    }

    struct hold_run_record r;
    memset(&r, 0, sizeof(r));
    r.version = 1;
    snprintf(r.id, sizeof(r.id), "%s", id);
    snprintf(r.run_id, sizeof(r.run_id), "%s", id);
    /* Adopted runs are addressable like launched ones: they get a generated name. */
    if (hold_generate_run_name_for_id(store, id, NULL, r.name) == 0 && r.name[0]) {
        r.has_name = true;
    }
    r.pid = adopted_pid;
    r.pgid = fg_pgid;
    r.sid = adopted_sid;
    r.start_unix_ns = start_unix_ns;
    r.created_unix_ns = start_unix_ns;
    hold_format_rfc3339_utc_from_ns(r.start_unix_ns, r.started_at, sizeof(r.started_at));
    r.has_started_at = true;
    hold_format_rfc3339_utc_from_ns(r.created_unix_ns, r.created_at, sizeof(r.created_at));
    r.has_created_at = true;
    snprintf(r.state, sizeof(r.state), "running");
    r.has_state = true;
    r.uid = geteuid();
    r.gid = getegid();
    r.has_log = true;
    snprintf(r.log_path, sizeof(r.log_path), "%s", log_path);
    if (console_sock[0]) {
        r.has_console = true;
        snprintf(r.console_sock, sizeof(r.console_sock), "%s", console_sock);
    }
    r.has_boot = hold_current_boot_id(r.boot_id, sizeof(r.boot_id));
    hold_read_proc_stat_tokens(adopted_pid, NULL, &r.proc_starttime_ticks);
    hold_read_proc_exe(adopted_pid, &r.exe_dev, &r.exe_ino);
    if (hold_format_argv_human(r.cmdline, sizeof(r.cmdline), argc, normalized_argv) != 0) {
        snprintf(r.cmdline, sizeof(r.cmdline), "?");
    }
    r.has_observed = true;
    snprintf(r.observed_exe, sizeof(r.observed_exe), "%s", exe_path[0] ? exe_path : observed_argv[0]);
    snprintf(r.observed_cwd, sizeof(r.observed_cwd), "%s", cwd_path);
    r.observed_argc = argc;
    r.observed_argv = observed_argv;
    char record_path[HOLD_PATH_MAX];
    if (hold_write_record_atomic(store->record_dir, &r, argc, normalized_argv, record_path, sizeof(record_path)) != 0) {
        int saved = errno;
        kill(logger, SIGTERM);
        if (adopt_reserve[0]) unlink(adopt_reserve);
        hold_free_argv_alloc(normalized_argv, argc);
        hold_free_argv_alloc(observed_argv, argc);
        errno = saved;
        hold_die_errno("hold: failed to write adopted run record");
    }
    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r.id, display_id);
    printf("%s\n", display_id);
    hold_sig_note(inv,
                  "hold  adopted  %s   %s\n"
                  "         log      %s\n"
                  "         tail     hold tail %s\n"
                  "         stop     hold stop %s\n",
                  display_id,
                  r.cmdline[0] ? r.cmdline : "?",
                  r.log_path,
                  display_id,
                  display_id);
    fflush(stdout);
    hold_free_argv_alloc(normalized_argv, argc);
    hold_free_argv_alloc(observed_argv, argc);
    return 0;
#endif
}

static int relay_shell_pty(int master, pid_t child, bool *detached) {
    bool stdin_open = true;
    bool pending_ctrl_p = false;
    *detached = false;
    while (1) {
        if (g_hold_on_off_requested) {
            return 0;
        }
        int status = 0;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 0;
        }
        if (w < 0 && errno != EINTR) return 1;

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int master_idx = (int)nfds;
        pfds[nfds].fd = master;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
        int stdin_idx = -1;
        if (stdin_open) {
            stdin_idx = (int)nfds;
            pfds[nfds].fd = STDIN_FILENO;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int ready;
        do {
            ready = poll(pfds, nfds, pending_ctrl_p ? 500 : 100);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) return 1;
        if (ready == 0) {
            if (pending_ctrl_p) {
                unsigned char c = 0x10;
                if (hold_write_all(master, &c, 1) != 0) return 1;
                pending_ctrl_p = false;
            }
            continue;
        }
        if (pfds[master_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) return 1;
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EIO)) {
                return 0;
            }
        }
        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                return 1;
            }
            if (n == 0) {
                stdin_open = false;
                continue;
            }
            for (ssize_t i = 0; i < n; i++) {
                if (pending_ctrl_p) {
                    if (buf[i] == 0x11) {
                        *detached = true;
                        return 0;
                    }
                    unsigned char c = 0x10;
                    if (hold_write_all(master, &c, 1) != 0) return 1;
                    pending_ctrl_p = false;
                }
                if (buf[i] == 0x10) {
                    pending_ctrl_p = true;
                    continue;
                }
                if (hold_write_all(master, (const char *)&buf[i], 1) != 0) return 1;
            }
        }
    }
}

int hold_cmd_shell_action(const struct hold_invocation *inv, const struct hold_store *store) {
    const char *shell = resolve_user_shell();

    /* Size the session PTY from the invoking terminal; when there is none the
     * spawn engine presets 80x24 rather than the kernel's 0x0. */
    unsigned short rows = 0, cols = 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }

    /* The shell (and anything it launches) inherits HOLD_ON_PID so a `hold off`
     * run from inside the session can find and signal this proxy. */
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%ld", (long)getpid());
    setenv("HOLD_ON_PID", pidbuf, 1);

    g_hold_on_off_requested = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = hold_on_off_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr,
            "Hold is now active. Ctrl-P Ctrl-Q puts the foreground program on hold; "
            "'hold off' or exit ends the session.\n");

    int master = -1;
    pid_t child = -1;
    if (spawn_shell_session(shell, rows, cols, &master, &child) != 0) {
        hold_die_errno("hold: failed to start shell");
    }

    struct shell_raw_terminal raw;
    if (enter_raw_if_tty(&raw) != 0) {
        int saved = errno;
        kill(child, SIGHUP);
        close(master);
        errno = saved;
        hold_die_errno("hold: failed to prepare terminal");
    }
    bool detached = false;
    int rc = relay_shell_pty(master, child, &detached);
    leave_raw(&raw);
    if (g_hold_on_off_requested && !detached) {
        /* `hold off`: end the wrapper shell cleanly and return success. */
        kill(child, SIGHUP);
        waitpid(child, NULL, 0);
        close(master);
        return 0;
    }
    if (detached) {
        pid_t fg_pgid = 0;
        if (ioctl(master, TIOCGPGRP, &fg_pgid) != 0) {
            int saved = errno;
            kill(child, SIGHUP);
            close(master);
            errno = saved;
            hold_die_errno("hold: failed to query shell foreground process group");
        }
        rc = adopt_foreground_group(inv, store, master, child, fg_pgid);
        if (rc == 0) {
            close(master);
            return 0;
        }
        close(master);
        return rc;
    }
    close(master);
    return rc;
}

int hold_cmd_off_action(void) {
    const char *pidstr = getenv("HOLD_ON_PID");
    if (!pidstr || !*pidstr) {
        fprintf(stderr, "hold: not inside a hold on session\n");
        return 1;
    }
    char *end = NULL;
    errno = 0;
    long pid = strtol(pidstr, &end, 10);
    if (end == pidstr || *end != '\0' || errno != 0 || pid <= 1) {
        fprintf(stderr, "hold: not inside a hold on session\n");
        return 1;
    }
#if defined(__linux__)
    /* Confirm the target is a live Hold process before signaling it: match its
     * /proc/<pid>/comm against our own so we never TERM an unrelated pid. */
    char self_comm[64] = {0}, target_comm[64] = {0};
    if (read_proc_comm(getpid(), self_comm, sizeof(self_comm)) != 0 ||
        read_proc_comm((pid_t)pid, target_comm, sizeof(target_comm)) != 0 ||
        strcmp(self_comm, target_comm) != 0) {
        fprintf(stderr, "hold: not inside a hold on session\n");
        return 1;
    }
#else
    if (kill((pid_t)pid, 0) != 0) {
        fprintf(stderr, "hold: not inside a hold on session\n");
        return 1;
    }
#endif
    if (kill((pid_t)pid, SIGTERM) != 0) {
        fprintf(stderr, "hold: not inside a hold on session\n");
        return 1;
    }
    return 0;
}
