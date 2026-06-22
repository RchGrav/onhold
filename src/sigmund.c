#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/store.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

static volatile sig_atomic_t g_tail_interrupted = 0;
static volatile sig_atomic_t g_console_resized = 0;
static void usage(void);
static int show_help(const char *topic);

static void handle_tail_sigint(int signo) {
    (void)signo;
    g_tail_interrupted = 1;
}

static void handle_console_sigwinch(int signo) {
    (void)signo;
    g_console_resized = 1;
}

static int detect_invocation(struct invocation *inv, bool requested_system, bool elevated) {
    memset(inv, 0, sizeof(*inv));
    inv->euid_root = (geteuid() == 0);
    inv->requested_system = requested_system;
    inv->elevated = elevated;
    inv->invoking_uid = getuid();
    inv->invoking_gid = getgid();
    snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", "");
    snprintf(inv->invoking_home, sizeof(inv->invoking_home), "%s", "");

    if (!inv->euid_root) {
        return 0;
    }

    const char *su = getenv("SUDO_UID");
    const char *sg = getenv("SUDO_GID");
    const char *sn = getenv("SUDO_USER");
    uid_t uid = 0;
    gid_t gid = 0;
    if (parse_uid_env(su, &uid) == 0 && parse_gid_env(sg, &gid) == 0 && sn && *sn) {
        struct passwd *pw = getpwuid(uid);
        if (pw && pw->pw_dir && *pw->pw_dir) {
            const char *home = pw->pw_dir;
#ifdef SIGMUND_TESTING
            const char *home_override = getenv("SIGMUND_TEST_INVOKING_HOME");
            if (home_override && *home_override) {
                home = home_override;
            }
#endif
            inv->have_sudo_user = true;
            inv->invoking_uid = uid;
            inv->invoking_gid = gid;
            if (checked_snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", sn) != 0 ||
                checked_snprintf(inv->invoking_home, sizeof(inv->invoking_home), "%s", home) != 0) {
                return -1;
            }
            return 0;
        }
    }

    inv->have_sudo_user = false;
    inv->invoking_uid = 0;
    inv->invoking_gid = 0;
    if (checked_snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", "root") != 0) {
        return -1;
    }
    return 0;
}

static void sig_note(const struct invocation *inv, const char *fmt, ...) {
    if (inv && inv->quiet) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define CONSOLE_ATTACH_MAGIC "SIGMUND1"
#define CONSOLE_ATTACH_MAGIC_LEN 8
#define CONSOLE_FRAME_DATA 'D'
#define CONSOLE_FRAME_RESIZE 'W'
#define CONSOLE_FRAME_DETACH 'X'
#define CONSOLE_FRAME_HEADER_LEN 3
#define CONSOLE_ATTACH_DETACH 0x1d
#define CONSOLE_REPLAY_LIMIT (64 * 1024)

struct console_client_state {
    bool framed;
    bool decided;
    unsigned char pending[16384];
    size_t pending_len;
};

struct console_replay_buffer {
    unsigned char *data;
    size_t cap;
    size_t len;
    size_t start;
};

static void console_replay_init(struct console_replay_buffer *replay) {
    memset(replay, 0, sizeof(*replay));
    replay->data = malloc(CONSOLE_REPLAY_LIMIT);
    if (replay->data) {
        replay->cap = CONSOLE_REPLAY_LIMIT;
    }
}

static void console_replay_free(struct console_replay_buffer *replay) {
    free(replay->data);
    memset(replay, 0, sizeof(*replay));
}

static void console_replay_append(struct console_replay_buffer *replay,
                                  const void *buf,
                                  size_t n) {
    if (!replay->data || replay->cap == 0 || n == 0) {
        return;
    }
    const unsigned char *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (replay->len < replay->cap) {
            replay->data[(replay->start + replay->len) % replay->cap] = p[i];
            replay->len++;
        } else {
            replay->data[replay->start] = p[i];
            replay->start = (replay->start + 1) % replay->cap;
        }
    }
}

static int console_replay_write(const struct console_replay_buffer *replay, int fd) {
    if (!replay->data || replay->len == 0) {
        return 0;
    }
    size_t first = replay->cap - replay->start;
    if (first > replay->len) {
        first = replay->len;
    }
    if (write_all(fd, replay->data + replay->start, first) != 0) {
        return -1;
    }
    if (replay->len > first &&
        write_all(fd, replay->data, replay->len - first) != 0) {
        return -1;
    }
    return 0;
}

static uint16_t load_be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void store_be16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

static int write_console_frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char header[CONSOLE_FRAME_HEADER_LEN];
    header[0] = type;
    store_be16(header + 1, len);
    if (write_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (len > 0 && write_all(fd, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int send_console_resize(int fd, const struct winsize *ws) {
    if (!ws || ws->ws_row == 0 || ws->ws_col == 0) {
        return 0;
    }
    unsigned char payload[4];
    store_be16(payload, ws->ws_row);
    store_be16(payload + 2, ws->ws_col);
    return write_console_frame(fd, CONSOLE_FRAME_RESIZE, payload, sizeof(payload));
}

static int maybe_get_terminal_size(struct winsize *ws) {
    memset(ws, 0, sizeof(*ws));
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    return -1;
}

static int apply_pty_size(int master, const unsigned char *payload, size_t len) {
    if (len != 4) {
        errno = EPROTO;
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = load_be16(payload);
    ws.ws_col = load_be16(payload + 2);
    if (ws.ws_row == 0 || ws.ws_col == 0) {
        return 0;
    }
    return ioctl(master, TIOCSWINSZ, &ws);
}

static int broker_process_framed_client(struct console_client_state *state, int master) {
    while (state->pending_len >= CONSOLE_FRAME_HEADER_LEN) {
        unsigned char type = state->pending[0];
        uint16_t len = load_be16(state->pending + 1);
        size_t frame_len = CONSOLE_FRAME_HEADER_LEN + (size_t)len;
        if (state->pending_len < frame_len) {
            return 0;
        }

        const unsigned char *payload = state->pending + CONSOLE_FRAME_HEADER_LEN;
        if (type == CONSOLE_FRAME_DATA) {
            if (len > 0 && write_all(master, payload, len) != 0) {
                return -1;
            }
        } else if (type == CONSOLE_FRAME_RESIZE) {
            (void)apply_pty_size(master, payload, len);
        } else if (type == CONSOLE_FRAME_DETACH) {
            return 1;
        }

        memmove(state->pending, state->pending + frame_len, state->pending_len - frame_len);
        state->pending_len -= frame_len;
    }
    return 0;
}

static int broker_process_client_input(struct console_client_state *state,
                                       int master,
                                       const unsigned char *buf,
                                       size_t n) {
    if (n == 0) {
        return 0;
    }
    if (!state->decided && state->pending_len + n > sizeof(state->pending)) {
        state->decided = true;
        state->framed = false;
        if (state->pending_len > 0 && write_all(master, state->pending, state->pending_len) != 0) {
            return -1;
        }
        state->pending_len = 0;
    }
    if (state->decided && !state->framed) {
        return write_all(master, buf, n);
    }

    if (state->pending_len + n > sizeof(state->pending)) {
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(state->pending + state->pending_len, buf, n);
    state->pending_len += n;

    if (!state->decided) {
        size_t cmp_len = state->pending_len < CONSOLE_ATTACH_MAGIC_LEN ? state->pending_len : CONSOLE_ATTACH_MAGIC_LEN;
        if (memcmp(state->pending, CONSOLE_ATTACH_MAGIC, cmp_len) != 0) {
            state->decided = true;
            state->framed = false;
            if (write_all(master, state->pending, state->pending_len) != 0) {
                return -1;
            }
            state->pending_len = 0;
            return 0;
        }
        if (state->pending_len < CONSOLE_ATTACH_MAGIC_LEN) {
            return 0;
        }
        state->decided = true;
        state->framed = true;
        memmove(state->pending,
                state->pending + CONSOLE_ATTACH_MAGIC_LEN,
                state->pending_len - CONSOLE_ATTACH_MAGIC_LEN);
        state->pending_len -= CONSOLE_ATTACH_MAGIC_LEN;
    }

    if (state->framed) {
        return broker_process_framed_client(state, master);
    }
    return 0;
}

static int format_console_sock_path(const struct store_paths *store,
                                    const char *id,
                                    char *out,
                                    size_t n) {
    /* The console socket always lives inside the store's 0700-owned console
     * directory; access is gated by that directory's permissions. It is
     * bound/connected via a short name relative to that directory (see
     * console_addr_relative), so the directory's absolute length does not
     * count against the AF_UNIX sun_path limit. There is therefore no need to
     * fall back to a world-writable location such as /tmp. */
    return checked_snprintf(out, n, "%s/%s.sock", store->console_dir, id);
}

/* AF_UNIX paths are limited to sun_path bytes (104 on macOS, 108 on Linux),
 * far shorter than a normal filesystem path. To keep the console socket inside
 * its store-owned directory regardless of that directory's length, callers
 * chdir into the directory and bind/connect using a short relative name. This
 * helper splits an absolute socket path into its directory and fills an
 * addr whose sun_path holds just the trailing "<id>.sock" name. Only
 * bind()/connect() are subject to the limit; unlink/stat/record keep using the
 * absolute path. */
static int console_addr_relative(const char *sock_path,
                                 struct sockaddr_un *addr,
                                 char *dir,
                                 size_t dirn) {
    const char *slash = strrchr(sock_path, '/');
    if (!slash || slash == sock_path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    const char *name = slash + 1;
    size_t namelen = strlen(name);
    if (namelen >= sizeof(addr->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t dlen = (size_t)(slash - sock_path);
    if (dlen >= dirn) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, sock_path, dlen);
    dir[dlen] = '\0';
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, name, namelen + 1);
    return 0;
}

static int make_console_listener(const char *sock_path) {
    struct sockaddr_un addr;
    char dir[SIGMUND_PATH_MAX];
    if (console_addr_relative(sock_path, &addr, dir, sizeof(dir)) != 0) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
#ifdef SIGMUND_NEED_SOCKET_CLOEXEC
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
#endif

    int cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (chdir(dir) != 0) {
        int saved = errno;
        close(cwd_fd);
        close(fd);
        errno = saved;
        return -1;
    }

    int err = 0;
    unlink(addr.sun_path);
    mode_t old_umask = umask(077);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        err = errno;
    }
    umask(old_umask);
    if (!err && chmod(addr.sun_path, 0600) != 0) {
        err = errno;
    }
    if (!err && listen(fd, 1) != 0) {
        err = errno;
    }
    if (err) {
        unlink(addr.sun_path);
    }

    /* Restore the working directory before returning. The console broker forks
     * and execs the target process after this call; that process must run in
     * the caller's original cwd, not the console directory. fchdir failure is
     * treated as fatal so we never launch the target in the wrong directory. */
    if (fchdir(cwd_fd) != 0 && err == 0) {
        err = errno;
        unlink(addr.sun_path);
    }
    close(cwd_fd);

    if (err) {
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

static int open_console_pty(int *master_out, int *slave_out) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    char *slave_name = ptsname(master);
    if (!slave_name) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
#ifdef TIOCSCTTY
    (void)ioctl(slave, TIOCSCTTY, 0);
#endif
    (void)tcsetpgrp(slave, getpgrp());
    *master_out = master;
    *slave_out = slave;
    return 0;
}

static void broker_cleanup_and_exit(int parent_pipe,
                                    const char *sock_path,
                                    int listener,
                                    int master,
                                    int slave,
                                    int logfd,
                                    pid_t target,
                                    int exit_code) {
    if (target > 0) {
        kill(target, SIGKILL);
        int st = 0;
        while (waitpid(target, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
    if (parent_pipe >= 0) close(parent_pipe);
    if (listener >= 0) close(listener);
    if (master >= 0) close(master);
    if (slave >= 0) close(slave);
    if (logfd >= 0) close(logfd);
    if (sock_path && *sock_path) unlink(sock_path);
    _exit(exit_code);
}

static void broker_fail_errno(int parent_pipe,
                              const char *sock_path,
                              int listener,
                              int master,
                              int slave,
                              int logfd,
                              pid_t target,
                              int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)write_all(parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, slave, logfd, target, 127);
}

static void run_console_broker(int parent_pipe,
                               const char *log_path,
                               const char *sock_path,
                               int argc,
                               char **argv,
                               const char *exec_path) {
    int listener = -1;
    int master = -1;
    int slave = -1;
    int logfd = -1;
    pid_t target = -1;

    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, EINVAL);
    }

    listener = make_console_listener(sock_path);
    if (listener < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
    }
    logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logfd < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
    }
    if (open_console_pty(&master, &slave) != 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
    }

    int exec_pipe[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(exec_pipe, O_CLOEXEC) != 0)
#endif
    {
        if (pipe(exec_pipe) != 0) {
            broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
        }
        if (fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(exec_pipe[0]);
            close(exec_pipe[1]);
            broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, saved);
        }
    }

    target = fork();
    if (target < 0) {
        int saved = errno;
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, saved);
    }
    if (target == 0) {
        close(exec_pipe[0]);
        close(listener);
        close(master);
        close(logfd);
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            int e = errno;
            (void)write_all(exec_pipe[1], &e, sizeof(e));
            _exit(127);
        }
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        if (exec_path && *exec_path) {
            execv(exec_path, argv);
        } else {
            execvp(argv[0], argv);
        }
        int e = errno;
        (void)write_all(exec_pipe[1], &e, sizeof(e));
        _exit(127);
    }

    close(exec_pipe[1]);
    int child_errno = 0;
    int handshake = read_exec_handshake(exec_pipe[0], &child_errno);
    int handshake_errno = errno;
    close(exec_pipe[0]);
    close(slave);
    slave = -1;
    if (handshake < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, handshake_errno);
    }
    if (handshake > 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, child_errno);
    }
    close(parent_pipe);
    parent_pipe = -1;

    struct sigaction pipe_ign, old_pipe;
    bool have_old_pipe = false;
    memset(&pipe_ign, 0, sizeof(pipe_ign));
    pipe_ign.sa_handler = SIG_IGN;
    sigemptyset(&pipe_ign.sa_mask);
    if (sigaction(SIGPIPE, &pipe_ign, &old_pipe) == 0) {
        have_old_pipe = true;
    }

    int client = -1;
    bool client_input_closed = false;
    struct console_client_state client_state;
    memset(&client_state, 0, sizeof(client_state));
    struct console_replay_buffer replay;
    console_replay_init(&replay);
    bool target_done = false;
    while (1) {
        if (!target_done) {
            int st = 0;
            pid_t got = waitpid(target, &st, WNOHANG);
            if (got == target) {
                target_done = true;
                target = -1;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master, &rfds);
        FD_SET(listener, &rfds);
        int maxfd = master > listener ? master : listener;
        if (client >= 0 && !client_input_closed) {
            FD_SET(client, &rfds);
            if (client > maxfd) {
                maxfd = client;
            }
        }
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int sr = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (sr == 0) {
            if (target_done) {
                break;
            }
            continue;
        }
        if (FD_ISSET(listener, &rfds)) {
            int next = accept(listener, NULL, NULL);
            if (next >= 0) {
                if (client >= 0) {
                    close(next);
                } else if (console_replay_write(&replay, next) != 0) {
                    close(next);
                } else {
                    client = next;
                    client_input_closed = false;
                    memset(&client_state, 0, sizeof(client_state));
                }
            }
        }
        if (client >= 0 && !client_input_closed && FD_ISSET(client, &rfds)) {
            unsigned char buf[4096];
            ssize_t n = read(client, buf, sizeof(buf));
            if (n > 0) {
                int input_rc = broker_process_client_input(&client_state, master, buf, (size_t)n);
                if (input_rc != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                    memset(&client_state, 0, sizeof(client_state));
                }
            } else if (n == 0) {
                if (!client_state.decided && client_state.pending_len > 0) {
                    (void)write_all(master, client_state.pending, client_state.pending_len);
                    client_state.pending_len = 0;
                }
                client_input_closed = true;
            } else {
                close(client);
                client = -1;
                client_input_closed = false;
                memset(&client_state, 0, sizeof(client_state));
            }
        }
        if (FD_ISSET(master, &rfds)) {
            char buf[4096];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                (void)write_all(logfd, buf, (size_t)n);
                console_replay_append(&replay, buf, (size_t)n);
                if (client >= 0 && write_all(client, buf, (size_t)n) != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                }
            } else if (n == 0 || errno == EIO) {
                break;
            }
        }
    }

    if (client >= 0) close(client);
    console_replay_free(&replay);
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, slave, logfd, target, 0);
}

static void report_session_escapees(const struct record *r) {
    int escaped = count_session_escapees(r->sid, r->pgid);
    if (escaped > 0) {
        fprintf(stderr,
                "sigmund: warning: %d process(es) escaped process-group %ld but remain in session %ld\n",
                escaped, (long)r->pgid, (long)r->sid);
    }
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

static bool start_target_is_within_invoking_home(const struct invocation *inv,
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

    char resolved[SIGMUND_PATH_MAX];
    if (resolve_binary_path(target, resolved, sizeof(resolved)) != 0) {
        return false;
    }
    return path_is_within_dir(resolved, home);
}

static enum run_state eval_state(const struct record *r, const char *current_boot) {
    if ((r->has_state && strcmp(r->state, "failed") == 0) || (r->has_launch_error && r->launch_error[0] != '\0')) {
        return STATE_FAILED;
    }
    if (r->pgid <= 1 || r->sid <= 0) {
        return STATE_UNKNOWN;
    }
    if (r->has_boot && current_boot && strcmp(r->boot_id, current_boot) != 0) {
        return STATE_STALE;
    }

    char state = 0;
    uint64_t now_starttime = 0;
    bool has_stat = read_proc_stat_tokens(r->pid, &state, &now_starttime) == 0;
    bool present = has_stat || leader_present(r->pid);
    enum group_liveness gl = group_session_liveness(r->pgid, r->sid);

    if (has_stat && state == 'Z') {
        if (gl == GROUP_LIVE) {
            return STATE_RUNNING;
        }
        if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
            return STATE_EXITED;
        }
        return STATE_UNKNOWN;
    }

    if (present) {
        if (r->proc_starttime_ticks && has_stat) {
            if (now_starttime != r->proc_starttime_ticks) {
                return STATE_STALE;
            }
        } else if (r->exe_dev && r->exe_ino) {
            uint64_t d, i;
            if (read_proc_exe(r->pid, &d, &i) == 0 && (d != r->exe_dev || i != r->exe_ino)) {
                return STATE_STALE;
            }
        }
        return STATE_RUNNING;
    }

    if (gl == GROUP_LIVE) {
        return STATE_RUNNING;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return STATE_EXITED;
    }

    int g = group_exists(r->pgid);
    if (g == 0) {
        return STATE_EXITED;
    }
    return STATE_UNKNOWN;
}

static int tail_log_until_exit(const struct record *r, bool from_end, bool follow_until_exit) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        die_errno("sigmund: failed to open log for tail");
    }

    char boot[128] = {0};
    bool have_boot = r->has_boot && current_boot_id(boot, sizeof(boot));
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
            if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
                close(fd);
                sigaction(SIGINT, &old_sa, NULL);
                die_errno("sigmund: failed writing tailed output");
            }
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            sigaction(SIGINT, &old_sa, NULL);
            die_errno("sigmund: failed while tailing log");
        }
        if (!follow_until_exit) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
        sleep_polls++;
        if (sleep_polls % 10 == 0) {
            enum run_state st = eval_state(r, have_boot ? boot : NULL);
            if (st != STATE_RUNNING) {
                break;
            }
        }
    }

    close(fd);
    sigaction(SIGINT, &old_sa, NULL);
    return 0;
}

static void rollback_spawned_group(pid_t pid, pid_t pgid) {
    if (pgid > 1) {
        kill(-pgid, SIGKILL);
    }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
}

static int perform_start(const struct invocation *inv,
                         const struct store_paths *store,
                         bool tail,
                         bool console_mode,
                         int argc,
                         char **argv,
                         const char *exec_path,
                         const char *run_alias) {
    if (argc <= 0 || !argv || !argv[0]) {
        usage();
        return 5;
    }

    char resolved_exec_path[SIGMUND_PATH_MAX];
    const char *path_to_resolve = (exec_path && *exec_path) ? exec_path : argv[0];
    if (resolve_binary_path(path_to_resolve, resolved_exec_path, sizeof(resolved_exec_path)) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "sigmund: cannot start '%s': command not found\n", argv[0]);
        } else {
            fprintf(stderr, "sigmund: cannot start '%s': %s\n", argv[0], strerror(errno));
        }
        return 1;
    }

    char **launch_argv = NULL;
    if (copy_argv(&launch_argv, argc, argv) != 0) {
        die_errno("sigmund: failed to prepare argv");
    }
    free(launch_argv[0]);
    launch_argv[0] = strdup(resolved_exec_path);
    if (!launch_argv[0]) {
        die_errno("sigmund: failed to prepare argv");
    }

    char id[16], log_path[SIGMUND_PATH_MAX], reserve_path[SIGMUND_PATH_MAX], console_sock[SIGMUND_PATH_MAX], boot_id[128] = {0};
    console_sock[0] = '\0';
    bool has_boot = current_boot_id(boot_id, sizeof(boot_id));
    struct store_paths system_hint;
    struct store_paths invoking_user_store;
    const struct store_paths *avoid_public_store = NULL;
    const struct store_paths *avoid_user_store = NULL;

    if (store->kind == STORE_USER_LOCAL) {
        if (init_system_store(&system_hint) == 0) {
            avoid_public_store = &system_hint;
        }
    } else if (inv && inv->have_sudo_user && inv->invoking_home[0]) {
        if (init_user_store_from_home(inv->invoking_home, &invoking_user_store) == 0) {
            avoid_user_store = &invoking_user_store;
        }
    }

    if (gen_id_for_store(store, avoid_public_store, avoid_user_store, id, sizeof(id)) != 0) {
        free_argv_alloc(launch_argv, argc);
        die_errno("sigmund: failed to generate id");
    }
    if (checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
        free_argv_alloc(launch_argv, argc);
        die_errno("sigmund: log path too long");
    }
    if (checked_snprintf(reserve_path, sizeof(reserve_path), "%s/.%s.reserve", store->record_dir, id) != 0) {
        free_argv_alloc(launch_argv, argc);
        die_errno("sigmund: reserve path too long");
    }
    if (console_mode && format_console_sock_path(store, id, console_sock, sizeof(console_sock)) != 0) {
        free_argv_alloc(launch_argv, argc);
        die_errno("sigmund: console socket path too long");
    }

    int pipefd[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(pipefd, O_CLOEXEC) != 0)
#endif
    {
        if (pipe(pipefd) != 0) {
            int saved = errno;
            unlink(reserve_path);
            free_argv_alloc(launch_argv, argc);
            errno = saved;
            die_errno("sigmund: pipe failed");
        }
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(pipefd[0]);
            close(pipefd[1]);
            unlink(reserve_path);
            free_argv_alloc(launch_argv, argc);
            errno = saved;
            die_errno("sigmund: pipe setup failed");
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(reserve_path);
        free_argv_alloc(launch_argv, argc);
        errno = saved;
        die_errno("sigmund: fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (setsid() < 0) {
            int e = errno;
            write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (console_mode) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd < 0 ||
                dup2(nullfd, STDIN_FILENO) < 0 ||
                dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(nullfd, STDERR_FILENO) < 0) {
                int e = errno;
                write_all(pipefd[1], &e, sizeof(e));
                _exit(127);
            }
            if (nullfd > STDERR_FILENO) {
                close(nullfd);
            }
            run_console_broker(pipefd[1], log_path, console_sock, argc, launch_argv, resolved_exec_path);
            _exit(127);
        }
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) {
            int e = errno;
            write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (nullfd > 2) {
            close(nullfd);
        }

        int lfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (lfd < 0 || dup2(lfd, STDOUT_FILENO) < 0 || dup2(lfd, STDERR_FILENO) < 0) {
            int e = errno;
            write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (lfd > 2) {
            close(lfd);
        }
        execv(resolved_exec_path, launch_argv);
        /* No envp variant here: children intentionally inherit Sigmund's environment. */
        int e = errno;
        write_all(pipefd[1], &e, sizeof(e));
        _exit(127);
    }

    close(pipefd[1]);
    int child_errno = 0;
    int handshake = read_exec_handshake(pipefd[0], &child_errno);
    int handshake_errno = errno;
    close(pipefd[0]);
    if (handshake < 0) {
        rollback_spawned_group(pid, pid);
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        free_argv_alloc(launch_argv, argc);
        errno = handshake_errno;
        die_errno("sigmund: exec handshake failed");
    }
    if (handshake > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
        if (child_errno == ENOENT) {
            fprintf(stderr, "sigmund: cannot start '%s': command not found\n", launch_argv[0]);
        } else {
            fprintf(stderr, "sigmund: cannot start '%s': %s\n", launch_argv[0], strerror(child_errno));
        }
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        free_argv_alloc(launch_argv, argc);
        return 1;
    }

    struct record r = {0};
    r.version = 1;
    if (checked_snprintf(r.id, sizeof(r.id), "%s", id) != 0) {
        die_errno("sigmund: id too long");
    }
    if (checked_snprintf(r.run_id, sizeof(r.run_id), "%s", id) != 0) {
        die_errno("sigmund: id too long");
    }
    r.pid = pid;
    r.pgid = pid;
    r.sid = pid;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    r.start_unix_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    format_rfc3339_utc_from_ns(r.start_unix_ns, r.started_at, sizeof(r.started_at));
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
            if (checked_snprintf(r.invoked_by_user, sizeof(r.invoked_by_user), "%s", inv->invoking_user) != 0) {
                die_errno("sigmund: invoking user too long");
            }
            r.invoked_via_sudo = true;
        } else {
            r.invoked_by_uid = 0;
            r.invoked_by_gid = 0;
            if (checked_snprintf(r.invoked_by_user, sizeof(r.invoked_by_user), "%s", "root") != 0) {
                die_errno("sigmund: invoking user too long");
            }
            r.invoked_via_sudo = false;
        }
    }
    if (run_alias && valid_alias(run_alias)) {
        r.has_alias = true;
        if (checked_snprintf(r.alias, sizeof(r.alias), "%s", run_alias) != 0) {
            die_errno("sigmund: alias too long");
        }
    }
    if (console_sock[0]) {
        r.has_console = true;
        if (checked_snprintf(r.console_sock, sizeof(r.console_sock), "%s", console_sock) != 0) {
            die_errno("sigmund: console socket path too long");
        }
    }
    r.has_log = true;
    if (checked_snprintf(r.log_path, sizeof(r.log_path), "%s", log_path) != 0) {
        die_errno("sigmund: log path too long");
    }
    r.has_boot = has_boot;
    if (r.has_boot) {
        snprintf(r.boot_id, sizeof(r.boot_id), "%s", boot_id);
    }
    read_proc_stat_tokens(pid, NULL, &r.proc_starttime_ticks);
    read_proc_exe(pid, &r.exe_dev, &r.exe_ino);
    if (format_argv_human(r.cmdline, sizeof(r.cmdline), argc, launch_argv) != 0) {
        snprintf(r.cmdline, sizeof(r.cmdline), "?");
    }

    char record_path[SIGMUND_PATH_MAX] = {0};
    bool chown_user_local_artifacts = store->kind == STORE_USER_LOCAL && inv && inv->euid_root && inv->have_sudo_user;
    if (getenv("SIGMUND_TEST_FAIL_RECORD_WRITE")) {
        errno = EIO;
    } else if (write_record_atomic(store->record_dir, &r, argc, launch_argv, record_path, sizeof(record_path)) == 0) {
        if (chown_user_local_artifacts) {
            int chown_rc = 0;
            if (record_path[0] &&
                chown(record_path, inv->invoking_uid, inv->invoking_gid) != 0) {
                chown_rc = -1;
            }
            if (chown(log_path, inv->invoking_uid, inv->invoking_gid) != 0) {
                chown_rc = -1;
            }
            if (console_sock[0] && path_exists(console_sock) &&
                chown(console_sock, inv->invoking_uid, inv->invoking_gid) != 0) {
                chown_rc = -1;
            }
            if (chown_rc != 0) {
                int saved = errno ? errno : EIO;
                rollback_spawned_group(pid, pid);
                if (record_path[0]) {
                    unlink(record_path);
                }
                unlink(log_path);
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink(reserve_path);
                free_argv_alloc(launch_argv, argc);
                errno = saved;
                die_errno("sigmund: failed to set user-local ownership");
            }
        }
        if (store->kind == STORE_SYSTEM_MANAGED) {
            int public_rc = 0;
            if (getenv("SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE")) {
                errno = EIO;
                public_rc = -1;
            } else if (write_public_index_atomic(store, &r) != 0) {
                public_rc = -1;
            }
            if (public_rc != 0) {
                int saved = errno;
                if (saved == 0) {
                    saved = EIO;
                }
                rollback_spawned_group(pid, pid);
                if (record_path[0]) {
                    unlink(record_path);
                }
                unlink(log_path);
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink(reserve_path);
                free_argv_alloc(launch_argv, argc);
                char public_path[SIGMUND_PATH_MAX];
                if (checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                    unlink(public_path);
                }
                errno = saved;
                die_errno("sigmund: failed to write public index");
            }
        }
        printf("%s\n", r.id);
        sig_note(inv,
                 "sigmund  started  %s   %s\n"
                 "         log      %s\n"
                 "         tail     sigmund tail %s\n"
                 "%s%s%s"
                 "         stop     sigmund stop %s\n",
                 r.id,
                 r.cmdline[0] ? r.cmdline : "?",
                 r.log_path,
                 r.id,
                 r.has_console ? "         console  sigmund console " : "",
                 r.has_console ? r.id : "",
                 r.has_console ? "\n" : "",
                 r.id);
        fflush(stdout);

        if (tail) {
            free_argv_alloc(launch_argv, argc);
            return tail_log_until_exit(&r, false, true);
        }
        free_argv_alloc(launch_argv, argc);
        return 0;
    }
    {
        int saved = errno;
        rollback_spawned_group(pid, pid);
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
        free_argv_alloc(launch_argv, argc);
        errno = saved;
        die_errno("sigmund: failed to write record");
    }
    return 1;
}

static bool target_group_gone(const struct record *r) {
    enum group_liveness gl = group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return true;
    }
    if (gl == GROUP_LIVE) {
        return false;
    }
    return group_exists(r->pgid) == 0;
}

static bool wait_target_group_gone(const struct record *r, int timeout_ms) {
    int waited = 0;
    while (waited <= timeout_ms) {
        if (target_group_gone(r)) {
            return true;
        }
        if (waited == timeout_ms) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_MS * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
        waited += POLL_SLEEP_MS;
    }
    return false;
}

static int do_signal_action(const struct store_paths *store, const char *id, int sig, bool graceful, bool *already_done) {
    struct record r;
    char path[SIGMUND_PATH_MAX], boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    if (already_done) {
        *already_done = false;
    }
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
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

    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
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
        if (wait_target_group_gone(&r, STOP_TIMEOUT_MS)) {
            report_session_escapees(&r);
            return 0;
        }
        if (kill(-r.pgid, SIGKILL) != 0 && errno != ESRCH) {
            if (errno == EPERM) {
                return 3;
            }
            return 4;
        }
        if (wait_target_group_gone(&r, 1000)) {
            report_session_escapees(&r);
            return 0;
        }
        return 4;
    }
    report_session_escapees(&r);
    return 0;
}

static const char *state_str(enum run_state s) {
    switch (s) {
    case STATE_RUNNING:
        return "running";
    case STATE_EXITED:
        return "exited";
    case STATE_STALE:
        return "stale";
    case STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void format_result(const struct record *r, enum run_state st, char *out, size_t n) {
    if (st == STATE_RUNNING) {
        snprintf(out, n, "%s", r->has_console ? "console" : "-");
        return;
    }
    if (r->has_launch_error && r->launch_error[0]) {
        snprintf(out, n, "launch=%.48s", r->launch_error);
        return;
    }
    if (r->has_term_signal) {
        snprintf(out, n, "signal=%d", r->term_signal);
        return;
    }
    if (r->has_exit_code) {
        snprintf(out, n, "exit=%d", r->exit_code);
        return;
    }
    if (st == STATE_FAILED) {
        snprintf(out, n, "launch=unknown");
    } else {
        snprintf(out, n, "-");
    }
}

struct list_row {
    char id[16];
    char state[16];
    char started[64];
    char result[64];
    char cmd[SIGMUND_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
};

struct list_rows {
    struct list_row *items;
    size_t count;
};

static void free_list_rows(struct list_rows *rows) {
    free(rows->items);
    rows->items = NULL;
    rows->count = 0;
}

static int append_list_row(struct list_rows *rows, const struct list_row *row) {
    struct list_row *next = realloc(rows->items, (rows->count + 1) * sizeof(*rows->items));
    if (!next) {
        return -1;
    }
    rows->items = next;
    rows->items[rows->count++] = *row;
    return 0;
}

static int compare_list_rows(const void *a, const void *b) {
    const struct list_row *ra = (const struct list_row *)a;
    const struct list_row *rb = (const struct list_row *)b;
    if (ra->running != rb->running) {
        return ra->running ? -1 : 1;
    }
    if (ra->start_unix_ns > rb->start_unix_ns) {
        return -1;
    }
    if (ra->start_unix_ns < rb->start_unix_ns) {
        return 1;
    }
    return strcmp(ra->id, rb->id);
}

static void print_list_header(bool iso) {
    printf("%-10s %-8s %-*s %-10s %s\n", "RUNID", "STATE", iso ? 24 : 8, iso ? "STARTED_AT" : "STARTED", "RESULT", "CMD");
}

static void print_list_row(const struct list_row *row, bool iso) {
    char cmd[80];
    const char *src = row->cmd[0] ? row->cmd : "?";
    snprintf(cmd, sizeof(cmd), "%.72s%s", src, strlen(src) > 72 ? "..." : "");
    printf("%-10s %-8s %-*s %-10s %s\n", row->id, row->state, iso ? 24 : 8, row->started, row->result, cmd);
}

static int collect_list_private(const struct store_paths *store,
                                const char *alias_filter,
                                bool iso,
                                struct list_rows *rows) {
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct record r;
        if (load_record(path, &r) != 0) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (!valid_record(&r) || strcmp(r.id, file_id) != 0) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (alias_filter && (!r.has_alias || strcmp(r.alias, alias_filter) != 0)) {
            continue;
        }
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        struct list_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.id, sizeof(row.id), "%s", r.id);
        snprintf(row.state, sizeof(row.state), "%s", state_str(st));
        row.start_unix_ns = r.start_unix_ns;
        row.running = st == STATE_RUNNING;
        if (iso) {
            if (r.has_started_at && r.started_at[0]) {
                snprintf(row.started, sizeof(row.started), "%s", r.started_at);
            } else {
                format_rfc3339_utc_from_ns(r.start_unix_ns, row.started, sizeof(row.started));
            }
        } else {
            format_relative_age(r.start_unix_ns, row.started, sizeof(row.started));
        }
        format_result(&r, st, row.result, sizeof(row.result));
        snprintf(row.cmd, sizeof(row.cmd), "%s", r.cmdline[0] ? r.cmdline : "?");
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int collect_list_public(const struct store_paths *store,
                               const char *alias_filter,
                               bool iso,
                               struct list_rows *rows) {
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char file_id[16];
        memcpy(file_id, e->d_name, len - 5);
        file_id[len - 5] = '\0';
        if (!valid_id(file_id)) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct public_index pi;
        if (load_public_index(path, &pi) != 0 || strcmp(pi.id, file_id) != 0) {
            continue;
        }
        if (alias_filter && (!pi.has_alias || strcmp(pi.alias, alias_filter) != 0)) {
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.id, sizeof(row.id), "%s", pi.id);
        snprintf(row.state, sizeof(row.state), "%s", "unknown");
        row.running = false;
        row.start_unix_ns = 0;
        if (iso) {
            snprintf(row.started, sizeof(row.started), "%s", pi.started_at[0] ? pi.started_at : "-");
        } else {
            snprintf(row.started, sizeof(row.started), "%s", "-");
        }
        snprintf(row.result, sizeof(row.result), "%s", "-");
        snprintf(row.cmd, sizeof(row.cmd), "%s", "<root-managed>");
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int print_collected_list(struct list_rows *rows, bool iso) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }
    print_list_header(iso);
    for (size_t i = 0; i < rows->count; i++) {
        print_list_row(&rows->items[i], iso);
    }
    return 0;
}

static int cmd_list_normal(const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, alias_filter, iso, &rows) != 0 ||
        collect_list_public(system_store, alias_filter, iso, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

static int cmd_list_system(const struct store_paths *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, alias_filter, iso, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (valid_id(input)) {
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s.json", dir, input) == 0 && access(path, F_OK) == 0) {
            return checked_snprintf(resolved, n, "%s", input);
        }
    }
    if (!valid_id_prefix(input)) {
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char id[ID_HEX_LEN + 1];
        if (!record_json_filename_id(e->d_name, id, sizeof(id))) {
            continue;
        }
        if (strncmp(id, input, strlen(input)) == 0) {
            matches++;
            if (checked_snprintf(resolved, n, "%s", id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return (matches == 1) ? 0 : -1;
}

static void unlink_public_index(const struct store_paths *store, const char *id) {
    if (store->kind != STORE_SYSTEM_MANAGED || !id || !*id) {
        return;
    }
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0) {
        unlink(path);
    }
}

static int prune_one_run(const struct store_paths *store, const char *id, const char *boot, bool allow_stale, bool *removed) {
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum run_state st = eval_state(&r, boot ? boot : NULL);
    bool prunable = (st == STATE_EXITED || st == STATE_FAILED || (allow_stale && st == STATE_STALE));
    if (!prunable) {
        fprintf(stderr, "sigmund: error: run %s is %s and cannot be pruned\n", id, state_str(st));
        return 2;
    }
    unlink(path);
    if (r.has_log) {
        unlink(r.log_path);
    }
    if (r.has_console) {
        unlink(r.console_sock);
    }
    unlink_public_index(store, id);
    if (removed) {
        *removed = true;
    }
    return 0;
}

static int cmd_prune_store_all(const struct store_paths *store, bool include_stale, int *removed_count) {
    if (removed_count) {
        *removed_count = 0;
    }
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));

    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct record r;
        if (load_record(path, &r) != 0) {
            unlink(path);
            continue;
        }
        if (!valid_record(&r) || strcmp(r.id, file_id) != 0) {
            unlink(path);
            continue;
        }
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        if (st == STATE_EXITED || st == STATE_FAILED || (include_stale && st == STATE_STALE)) {
            unlink(path);
            if (r.has_log) {
                unlink(r.log_path);
            }
            if (r.has_console) {
                unlink(r.console_sock);
            }
            unlink_public_index(store, r.id);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);

    d = opendir(store->log_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".log")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 4) {
            continue;
        }
        char id[32];
        size_t id_len = len - 4;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        char json_path[SIGMUND_PATH_MAX], log_path[SIGMUND_PATH_MAX];
        if (checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0) {
            continue;
        }
        if (checked_snprintf(log_path, sizeof(log_path), "%s/%s", store->log_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(log_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    d = opendir(store->console_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".sock")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[32];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        char json_path[SIGMUND_PATH_MAX], sock_path[SIGMUND_PATH_MAX];
        if (checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            checked_snprintf(sock_path, sizeof(sock_path), "%s/%s", store->console_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(sock_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    return 0;
}

static enum id_token_scope parse_id_token(const char *token, const char **id_out) {
    if (!token || !*token) {
        return ID_TOKEN_INVALID;
    }
    if (strncmp(token, "user:", 5) == 0) {
        *id_out = token + 5;
        return **id_out ? ID_TOKEN_USER : ID_TOKEN_INVALID;
    }
    if (strncmp(token, "system:", 7) == 0) {
        *id_out = token + 7;
        return **id_out ? ID_TOKEN_SYSTEM : ID_TOKEN_INVALID;
    }
    *id_out = token;
    return ID_TOKEN_PLAIN;
}

static int ensure_run_recorded_under_alias(const struct store_paths *store, const char *id, const char *alias) {
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return -1;
    }
    if (!r.has_alias || strcmp(r.alias, alias) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int init_invoking_user_store(const struct invocation *inv, struct store_paths *store) {
    if (!inv || !inv->have_sudo_user || !inv->invoking_home[0]) {
        errno = EINVAL;
        return -1;
    }
    return init_user_store_from_home(inv->invoking_home, store);
}

static int resolve_user_store_id(const struct store_paths *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_private_id(const struct store_paths *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_public_id(const struct store_paths *store, const char *id, char *resolved, size_t n) {
    if (resolve_run_id(store->public_dir, id, resolved, n) != 0) {
        return -1;
    }
    struct public_index pi;
    if (load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

static int resolve_public_profile_token(const struct store_paths *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]) {
    if (valid_profile_hash(token)) {
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (valid_alias(token) && alias_lookup_hash(store, token, hash) == 0) {
        return 1;
    }
    return 0;
}

static void fill_target(struct resolved_target *out,
                        enum resolve_scope scope,
                        const struct store_paths *store,
                        const char *id,
                        bool needs_elevation) {
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->needs_elevation = needs_elevation;
    checked_snprintf(out->id, sizeof(out->id), "%s", id);
}

static int set_target_capability(struct resolved_target *target, const char *alias, const char *hash) {
    if (!target || !valid_alias(alias) || !valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    if (checked_snprintf(target->cap_alias, sizeof(target->cap_alias), "%s", alias) != 0 ||
        checked_snprintf(target->cap_hash, sizeof(target->cap_hash), "%s", hash) != 0) {
        return -1;
    }
    target->has_capability = true;
    return 0;
}

struct alias_match {
    char id[16];
    enum run_state state;
    char started_at[64];
};

struct alias_match_list {
    struct alias_match *items;
    size_t count;
    bool alias_known;
};

static void free_alias_match_list(struct alias_match_list *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool command_all_allowed(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "prune"));
}

static bool record_matches_alias_intent(const char *command, const struct record *r, enum run_state st) {
    if (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
        !strcmp(command, "tail")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "console")) {
        return st == STATE_RUNNING && r->has_console;
    }
    if (!strcmp(command, "dump")) {
        return r->has_log;
    }
    if (!strcmp(command, "prune")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
    }
    return false;
}

static int append_alias_match(struct alias_match_list *list,
                              const struct record *r,
                              enum run_state st,
                              const char *started_at) {
    struct alias_match *next = realloc(list->items, (list->count + 1) * sizeof(*list->items));
    if (!next) {
        return -1;
    }
    list->items = next;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));
    if (checked_snprintf(list->items[list->count].id, sizeof(list->items[list->count].id), "%s", r->id) != 0 ||
        checked_snprintf(list->items[list->count].started_at,
                         sizeof(list->items[list->count].started_at),
                         "%s",
                         started_at && *started_at ? started_at : "-") != 0) {
        return -1;
    }
    list->items[list->count].state = st;
    list->count++;
    return 0;
}

static int collect_private_alias_matches(const struct store_paths *store,
                                         const char *alias,
                                         const char *command,
                                         struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = alias_exists_in_store(store, alias);
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            closedir(d);
            return -1;
        }
        struct record r;
        if (load_record(path, &r) != 0 || !valid_record(&r) ||
            strcmp(r.id, file_id) != 0 || !r.has_alias || strcmp(r.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        if (!record_matches_alias_intent(command, &r, st)) {
            continue;
        }
        char started_at[64];
        if (r.has_started_at && r.started_at[0]) {
            snprintf(started_at, sizeof(started_at), "%s", r.started_at);
        } else {
            format_rfc3339_utc_from_ns(r.start_unix_ns, started_at, sizeof(started_at));
        }
        if (append_alias_match(list, &r, st, started_at) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static bool public_alias_visible(const struct store_paths *store, const char *alias) {
    if (alias_exists_in_store(store, alias)) {
        return true;
    }
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return false;
    }
    bool found = false;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char id[16];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct public_index pi;
        if (load_public_index(path, &pi) == 0 && pi.has_alias && strcmp(pi.alias, alias) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void report_alias_ambiguity(const char *command, const char *alias, const struct alias_match_list *list) {
    fprintf(stderr,
            "sigmund: error: alias '%s' matches more than one %s candidate\n",
            alias,
            command ? command : "target");
    fprintf(stderr, "sigmund: candidates:\n");
    for (size_t i = 0; i < list->count; i++) {
        fprintf(stderr,
                "  %s %-8s %s\n",
                list->items[i].id,
                state_str(list->items[i].state),
                list->items[i].started_at);
    }
    if (command_all_allowed(command)) {
        fprintf(stderr, "sigmund: use --all to apply %s to every listed run\n", command);
    }
}

static int append_resolved_target(struct resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct store_paths *store,
                                  const char *id,
                                  bool needs_elevation) {
    struct resolved_target *next = realloc(*targets, (size_t)(*count + 1) * sizeof(**targets));
    if (!next) {
        return -1;
    }
    *targets = next;
    fill_target(&(*targets)[*count], scope, store, id, needs_elevation);
    (*count)++;
    return 0;
}

static int append_capability_target(struct resolved_target **targets,
                                    int *count,
                                    const struct store_paths *store,
                                    const char *runid_sel,
                                    const char *alias,
                                    const char *hash) {
    if (append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, store, runid_sel, true) != 0) {
        return -1;
    }
    if (set_target_capability(&(*targets)[*count - 1], alias, hash) != 0) {
        return -1;
    }
    return 0;
}

static int append_private_alias_targets(struct resolved_target **targets,
                                        int *count,
                                        enum resolve_scope scope,
                                        const struct store_paths *store,
                                        const char *alias,
                                        const char *command,
                                        bool all) {
    struct alias_match_list matches;
    if (collect_private_alias_matches(store, alias, command, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count > 1 && (!all || !command_all_allowed(command))) {
        report_alias_ambiguity(command, alias, &matches);
        free_alias_match_list(&matches);
        return -2;
    }
    for (size_t i = 0; i < matches.count; i++) {
        if (append_resolved_target(targets, count, scope, store, matches.items[i].id, false) != 0) {
            free_alias_match_list(&matches);
            return -1;
        }
    }
    free_alias_match_list(&matches);
    return 1;
}

static int collect_public_alias_matches(const struct store_paths *store,
                                        const char *alias,
                                        struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = alias_exists_in_store(store, alias);
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char id[16];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        struct public_index pi;
        if (load_public_index_by_id(store, id, &pi) != 0 ||
            !pi.has_alias || strcmp(pi.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        struct record pseudo;
        memset(&pseudo, 0, sizeof(pseudo));
        snprintf(pseudo.id, sizeof(pseudo.id), "%s", id);
        if (append_alias_match(list, &pseudo, STATE_UNKNOWN, pi.started_at[0] ? pi.started_at : "-") != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int append_public_alias_elevation_target(struct resolved_target **targets,
                                                int *count,
                                                const struct store_paths *system_store,
                                                const char *alias,
                                                const char *command,
                                                bool all) {
    char hash[PROFILE_HASH_STR_LEN];
    if (alias_lookup_hash(system_store, alias, hash) != 0) {
        if (!public_alias_visible(system_store, alias)) {
            return 0;
        }
        return 1;
    }
    struct alias_match_list matches;
    if (collect_public_alias_matches(system_store, alias, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count == 0) {
        free_alias_match_list(&matches);
        return 1;
    }
    if (matches.count > 1) {
        if (!all || !command_all_allowed(command)) {
            report_alias_ambiguity(command, alias, &matches);
            free_alias_match_list(&matches);
            return -2;
        }
        int rc = append_capability_target(targets, count, system_store, "ffffffff", alias, hash) == 0 ? 1 : -1;
        free_alias_match_list(&matches);
        return rc;
    }
    int rc = append_capability_target(targets, count, system_store, matches.items[0].id, alias, hash) == 0 ? 1 : -1;
    free_alias_match_list(&matches);
    return rc;
}

static int resolve_target(const struct invocation *inv,
                          const struct store_paths *current_user_store,
                          const struct store_paths *system_store,
                          const char *token,
                          struct resolved_target *out) {
    memset(out, 0, sizeof(*out));
    out->scope = RESOLVE_NOT_FOUND;

    const char *id = NULL;
    enum id_token_scope token_scope = parse_id_token(token, &id);
    if (token_scope == ID_TOKEN_INVALID || !valid_target_atom(id)) {
        fprintf(stderr, "sigmund: error: invalid target '%s'\n", token ? token : "");
        out->scope = RESOLVE_ERROR;
        return -1;
    }

    if (inv->euid_root) {
        if (token_scope == ID_TOKEN_USER) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", id);
                out->scope = RESOLVE_ERROR;
                return -1;
            }
            char resolved[16];
            if (resolve_user_store_id(&user_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_USER_LOCAL, &user_store, resolved, false);
                return 0;
            }
            return 0;
        }
        if (token_scope == ID_TOKEN_SYSTEM) {
            char resolved[16];
            if (resolve_system_private_id(system_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false);
                return 0;
            }
            return 0;
        }

        char root_resolved[16] = {0};
        char user_resolved[16] = {0};
        bool root_match = resolve_system_private_id(system_store, id, root_resolved, sizeof(root_resolved)) == 0;
        bool user_match = false;
        struct store_paths user_store;
        if (inv->have_sudo_user && init_invoking_user_store(inv, &user_store) == 0) {
            user_match = resolve_user_store_id(&user_store, id, user_resolved, sizeof(user_resolved)) == 0;
        }
        if (root_match) {
            (void)user_match;
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, root_resolved, false);
            return 0;
        }
        if (user_match) {
            fill_target(out, RESOLVE_USER_LOCAL, &user_store, user_resolved, false);
            return 0;
        }
        return 0;
    }

    if (token_scope == ID_TOKEN_USER) {
        char resolved[16];
        if (resolve_user_store_id(current_user_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_USER_LOCAL, current_user_store, resolved, false);
            return 0;
        }
        return 0;
    }
    if (token_scope == ID_TOKEN_SYSTEM) {
        char resolved[16];
        if (resolve_system_public_id(system_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true);
            return 0;
        }
        return 0;
    }

    char user_resolved[16];
    if (resolve_user_store_id(current_user_store, id, user_resolved, sizeof(user_resolved)) == 0) {
        fill_target(out, RESOLVE_USER_LOCAL, current_user_store, user_resolved, false);
        return 0;
    }
    char system_resolved[16];
    if (resolve_system_public_id(system_store, id, system_resolved, sizeof(system_resolved)) == 0) {
        fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, system_resolved, true);
        return 0;
    }
    return 0;
}

static int report_not_found(const char *token) {
    fprintf(stderr, "sigmund: error: no run matches '%s'\n", token ? token : "");
    return 5;
}

static int resolve_action_token(const struct invocation *inv,
                                const struct store_paths *current_user_store,
                                const struct store_paths *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct resolved_target **targets_out,
                                int *count_out) {
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = parse_id_token(token, &atom);
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    bool cap_token = false;
    if (scope == ID_TOKEN_SYSTEM && parse_alias_cap_atom(atom, cap_alias, cap_hash) == 0) {
        if (!inv->euid_root || verify_system_alias_cap(system_store, cap_alias, cap_hash) != 0) {
            return report_not_found(token);
        }
        atom = cap_alias;
        cap_token = true;
    }
    if (scope == ID_TOKEN_INVALID || (!valid_id_prefix(atom) && !valid_alias(atom))) {
        fprintf(stderr, "sigmund: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    if (!cap_token && valid_id_prefix(atom)) {
        char resolved[16];
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct store_paths user_store;
                if (init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                if (resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                }
            } else if (scope == ID_TOKEN_SYSTEM) {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
            } else {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
                if (inv->have_sudo_user) {
                    struct store_paths user_store;
                    if (init_invoking_user_store(inv, &user_store) == 0 &&
                        resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                        return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                if (resolve_user_store_id(current_user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, resolved, false) == 0 ? 0 : 3;
                }
                if (scope == ID_TOKEN_USER) {
                    return report_not_found(token);
                }
            }
            if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
                if (resolve_system_public_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true) == 0 ? 0 : 3;
                }
            }
        }
    }

    if (!valid_alias(atom)) {
        return report_not_found(token);
    }

    int rc = 0;
    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                return 5;
            }
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : report_not_found(token);
        }
        if (scope == ID_TOKEN_SYSTEM) {
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : report_not_found(token);
        }

        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (inv->have_sudo_user) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) == 0) {
                rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
                if (rc == 1) return 0;
                if (rc == -2) return 6;
                if (rc < 0) return 3;
            }
        }
        return report_not_found(token);
    }

    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (scope == ID_TOKEN_USER) {
            return report_not_found(token);
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        rc = append_public_alias_elevation_target(targets_out, count_out, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
    }
    return report_not_found(token);
}

static int child_status_to_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 3;
}

static int elevate_with_sudo_canonical(const char *program, int canonical_argc, char **canonical_argv) {
    char abs_sigmund[SIGMUND_PATH_MAX];
    if (resolve_self_executable_path(program, abs_sigmund, sizeof(abs_sigmund)) != 0) {
        fprintf(stderr, "sigmund: cannot determine executable path for sudo self-elevation\n");
        return 3;
    }

    int argc = 5 + canonical_argc;
    char **sudo_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!sudo_argv) {
        return 3;
    }
    sudo_argv[0] = "sudo";
    sudo_argv[1] = "--";
    sudo_argv[2] = abs_sigmund;
    sudo_argv[3] = "--system";
    sudo_argv[4] = "--elevated";
    for (int i = 0; i < canonical_argc; i++) {
        sudo_argv[5 + i] = canonical_argv[i];
    }
    sudo_argv[argc] = NULL;

    const char *sudo_prog = "sudo";
#ifdef SIGMUND_TESTING
    const char *test_sudo_prog = getenv("SIGMUND_TEST_SUDO_PROG");
    if (test_sudo_prog && *test_sudo_prog) {
        sudo_prog = test_sudo_prog;
    }
#endif

    fflush(NULL);

    struct sigaction ign;
    struct sigaction old_int;
    struct sigaction old_quit;
    bool have_old_int = false;
    bool have_old_quit = false;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &ign, &old_int) == 0) {
        have_old_int = true;
    }
    if (sigaction(SIGQUIT, &ign, &old_quit) == 0) {
        have_old_quit = true;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        free(sudo_argv);
        errno = saved;
        fprintf(stderr, "sigmund: failed to fork sudo: %s\n", strerror(errno));
        return 3;
    }
    if (pid == 0) {
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        execvp(sudo_prog, sudo_argv);
        int saved = errno;
        fprintf(stderr, "sigmund: failed to exec sudo: %s\n", strerror(saved));
        _exit(127);
    }

    free(sudo_argv);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        int saved = errno;
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        errno = saved;
        fprintf(stderr, "sigmund: failed to wait for sudo: %s\n", strerror(errno));
        return 3;
    }

    if (have_old_int) sigaction(SIGINT, &old_int, NULL);
    if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
    return child_status_to_exit_code(status);
}

static int elevate_with_sudo_parsed(const char *program,
                                    bool owned,
                                    const char *command,
                                    bool tail,
                                    bool console_mode,
                                    bool all,
                                    bool print_cmd,
                                    bool multi,
                                    int multi_count,
                                    bool force_raw,
                                    int argc,
                                    char **argv) {
    int extra = argc;
    if (owned) {
        extra += 1;
        if (!strcmp(command, "start") && tail) {
            extra += 1;
        }
        if (!strcmp(command, "start") && console_mode) {
            extra += 1;
        }
        if (all) {
            extra += 1;
        }
        if (print_cmd) {
            extra += 1;
        }
        if (!strcmp(command, "start") && multi) {
            extra += multi_count == 1 ? 1 : 2;
        }
    } else {
        if (tail) {
            extra += 1;
        }
        if (console_mode) {
            extra += 1;
        }
        if (force_raw) {
            extra += 1;
        }
    }

    char **canon = calloc((size_t)extra, sizeof(char *));
    if (!canon) {
        return 3;
    }
    char count_buf[32];
    int n = 0;
    if (owned) {
        canon[n++] = (char *)command;
        if (!strcmp(command, "start") && tail) {
            canon[n++] = "--tail";
        }
        if (!strcmp(command, "start") && console_mode) {
            canon[n++] = "--console";
        }
        if (all) {
            canon[n++] = "--all";
        }
        if (print_cmd) {
            canon[n++] = "--print";
        }
        if (!strcmp(command, "start") && multi) {
            canon[n++] = "--multi";
            if (multi_count != 1) {
                snprintf(count_buf, sizeof(count_buf), "%d", multi_count);
                canon[n++] = count_buf;
            }
        }
    } else {
        if (tail) {
            canon[n++] = "--tail";
        }
        if (console_mode) {
            canon[n++] = "--console";
        }
        if (force_raw) {
            canon[n++] = "--";
        }
    }
    for (int i = 0; i < argc; i++) {
        canon[n++] = argv[i];
    }
    int rc = elevate_with_sudo_canonical(program, n, canon);
    free(canon);
    return rc;
}

static int elevate_with_sudo_targets(const char *program,
                               const char *command,
                               char **original_tokens,
                               const struct resolved_target *targets,
                               int ntargets,
                               bool all,
                               bool print_cmd) {
    int target_argc = 0;
    bool has_capability = false;
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].has_capability) {
            target_argc += 3;
            has_capability = true;
        } else {
            target_argc += 1;
        }
    }
    int canonical_argc = 1 + ((!has_capability && all) ? 1 : 0) + (print_cmd ? 1 : 0) + target_argc;
    char **canon = calloc((size_t)canonical_argc, sizeof(char *));
    char **tokens = calloc((size_t)ntargets, sizeof(char *));
    if (!canon || !tokens) {
        free(canon);
        free(tokens);
        return 3;
    }

    int n = 0;
    canon[n++] = (char *)command;
    if (!has_capability && all) {
        canon[n++] = "--all";
    }
    if (print_cmd) {
        canon[n++] = "--print";
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].has_capability) {
            canon[n++] = (char *)targets[i].id;
            canon[n++] = (char *)targets[i].cap_alias;
            canon[n++] = (char *)targets[i].cap_hash;
            continue;
        }
        const char *orig_id = NULL;
        enum id_token_scope orig_scope = parse_id_token(original_tokens ? original_tokens[i] : NULL, &orig_id);
        const char *prefix = "";
        if (targets[i].scope == RESOLVE_USER_LOCAL) {
            prefix = "user:";
        } else if (targets[i].scope == RESOLVE_SYSTEM_MANAGED &&
                   (orig_scope == ID_TOKEN_SYSTEM || targets[i].needs_elevation)) {
            prefix = "system:";
        }
        size_t need = strlen(prefix) + strlen(targets[i].id) + 1;
        tokens[i] = malloc(need);
        if (!tokens[i]) {
            for (int j = 0; j < i; j++) free(tokens[j]);
            free(tokens);
            free(canon);
            return 3;
        }
        snprintf(tokens[i], need, "%s%s", prefix, targets[i].id);
        canon[n++] = tokens[i];
    }

    int rc = elevate_with_sudo_canonical(program, canonical_argc, canon);
    for (int i = 0; i < ntargets; i++) free(tokens[i]);
    free(tokens);
    free(canon);
    return rc;
}

static int do_print_signal_command(const struct store_paths *store, const char *id, int sig) {
    struct record r;
    char path[SIGMUND_PATH_MAX], boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    if (r.pgid <= 1) {
        fprintf(stderr, "sigmund: error: invalid pgid %ld in record file\n", (long)r.pgid);
        return 5;
    }
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
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

static int cmd_signal_action(const struct invocation *inv,
                             const struct store_paths *user_store,
                             const struct store_paths *system_store,
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
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    for (int i = 0; i < argc; i++) {
        struct resolved_target *one = NULL;
        int none = 0;
        int rc = resolve_action_token(inv, user_store, system_store, command, argv[i], all, &one, &none);
        if (rc != 0) {
            free(one);
            free(targets);
            return rc;
        }
        if (none > 0) {
            struct resolved_target *next = realloc(targets, (size_t)(ntargets + none) * sizeof(*targets));
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
        sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }
    bool need_elevation = false;
    for (int i = 0; i < ntargets; i++) {
        need_elevation = need_elevation || targets[i].needs_elevation;
    }
    if (need_elevation) {
        int rc = elevate_with_sudo_targets(program, command, NULL, targets, ntargets, all, print_cmd);
        free(targets);
        return rc;
    }
    int worst = 0;
    for (int i = 0; i < ntargets; i++) {
        bool already_done = false;
        int rc = print_cmd ? do_print_signal_command(&targets[i].store, targets[i].id, sig)
                           : do_signal_action(&targets[i].store, targets[i].id, sig, graceful, &already_done);
        if (!print_cmd && rc == 0) {
            if (already_done) {
                sig_note(inv, "sigmund: %s already exited\n", targets[i].id);
            } else {
                sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", targets[i].id);
            }
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    free(targets);
    return worst;
}

static int cmd_tail_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token) {
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "tail", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to tail\n");
        return 0;
    }
    struct resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = elevate_with_sudo_targets(program, "tail", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    rc = tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
    free(targets);
    return rc;
}

static int cmd_dump_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token) {
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "dump", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to dump\n");
        return 0;
    }
    struct resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = elevate_with_sudo_targets(program, "dump", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
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
        die_errno("sigmund: failed to open log for dump");
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
            die_errno("sigmund: failed while dumping log");
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            close(fd);
            free(targets);
            die_errno("sigmund: failed writing dumped output");
        }
    }
    close(fd);
    free(targets);
    return 0;
}

static int connect_console_socket(const char *sock_path) {
    struct stat st;
    if (stat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "sigmund: console socket is not available\n");
        errno = ENOTSOCK;
        return -1;
    }

    struct sockaddr_un addr;
    char dir[SIGMUND_PATH_MAX];
    if (console_addr_relative(sock_path, &addr, dir, sizeof(dir)) != 0) {
        if (errno == ENAMETOOLONG) {
            fprintf(stderr, "sigmund: console socket path is too long\n");
        }
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
#ifdef SIGMUND_NEED_SOCKET_CLOEXEC
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
#endif

    /* Connect via the short relative name from inside the socket's directory
     * (see console_addr_relative), restoring cwd afterward. */
    int cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (chdir(dir) != 0) {
        int saved = errno;
        close(cwd_fd);
        close(fd);
        errno = saved;
        return -1;
    }

    int err = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        err = errno;
    }
    if (fchdir(cwd_fd) != 0 && err == 0) {
        err = errno;
    }
    close(cwd_fd);

    if (err) {
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

static void make_raw_termios(const struct termios *in, struct termios *out) {
    *out = *in;
    out->c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    out->c_oflag &= (tcflag_t)~OPOST;
    out->c_cflag |= CS8;
    out->c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    out->c_cc[VMIN] = 1;
    out->c_cc[VTIME] = 0;
}

static int run_native_console(const char *sock_path) {
    int sock = connect_console_socket(sock_path);
    if (sock < 0) {
        return errno == ENOTSOCK || errno == ENAMETOOLONG ? 5 : 3;
    }

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool terminal_saved = false;
    bool alt_screen = false;
    struct termios old_termios;
    if (interactive && tcgetattr(STDIN_FILENO, &old_termios) == 0) {
        struct termios raw;
        make_raw_termios(&old_termios, &raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            terminal_saved = true;
            if (write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
                alt_screen = true;
            }
        }
    }

    struct sigaction sa, old_winch;
    bool have_old_winch = false;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_console_sigwinch;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGWINCH, &sa, &old_winch) == 0) {
        have_old_winch = true;
    }
    struct sigaction pipe_ign, old_pipe;
    bool have_old_pipe = false;
    memset(&pipe_ign, 0, sizeof(pipe_ign));
    pipe_ign.sa_handler = SIG_IGN;
    sigemptyset(&pipe_ign.sa_mask);
    if (sigaction(SIGPIPE, &pipe_ign, &old_pipe) == 0) {
        have_old_pipe = true;
    }
    g_console_resized = 1;

    int rc = 0;
    bool stdin_open = true;
    if (write_all(sock, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        rc = 3;
        goto out;
    }

    while (1) {
        if (g_console_resized) {
            struct winsize ws;
            g_console_resized = 0;
            if (maybe_get_terminal_size(&ws) == 0 && send_console_resize(sock, &ws) != 0) {
                rc = 3;
                break;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        int maxfd = sock;
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd) {
                maxfd = STDIN_FILENO;
            }
        }

        int sr = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 3;
            break;
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (interactive) {
                    size_t write_start = 0;
                    for (ssize_t i = 0; i < n; i++) {
                        if (buf[i] != CONSOLE_ATTACH_DETACH) {
                            continue;
                        }
                        if ((size_t)i > write_start &&
                            write_console_frame(sock, CONSOLE_FRAME_DATA, buf + write_start, (uint16_t)((size_t)i - write_start)) != 0) {
                            rc = 3;
                        }
                        if (rc == 0 && write_console_frame(sock, CONSOLE_FRAME_DETACH, NULL, 0) != 0) {
                            rc = 3;
                        }
                        goto out;
                    }
                    if (rc != 0) {
                        break;
                    }
                    if (write_console_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                        rc = 3;
                        break;
                    }
                } else if (write_console_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                stdin_open = false;
                shutdown(sock, SHUT_WR);
            } else if (errno != EINTR) {
                rc = 3;
                break;
            }
        }

        if (FD_ISSET(sock, &rfds)) {
            char buf[4096];
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                rc = 3;
                break;
            }
        }
    }

out:
    if (have_old_winch) {
        sigaction(SIGWINCH, &old_winch, NULL);
    }
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    if (alt_screen) {
        (void)write_all(STDOUT_FILENO, "\033[?1049l", 8);
    }
    if (terminal_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    }
    close(sock);
    return rc;
}

static int attach_console_record(const struct invocation *inv,
                                 const struct record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        sig_note(inv, "sigmund: %s has exited - see 'sigmund dump %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        sig_note(inv, "sigmund: %s has no console (start with --console)\n", r->id);
        return 0;
    }
    return run_native_console(r->console_sock);
}

static int cmd_console_action(const struct invocation *inv,
                              const struct store_paths *user_store,
                              const struct store_paths *system_store,
                              const char *program,
                              const char *id_token) {
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to console\n");
        return 0;
    }
    struct resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = elevate_with_sudo_targets(program, "console", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    rc = attach_console_record(inv, &r, st);
    free(targets);
    return rc;
}

static int cmd_prune_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const char *target_token,
                            bool all) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        const struct store_paths *store = inv->euid_root ? system_store : user_store;
        int removed = 0;
        int rc = cmd_prune_store_all(store, target_token && strcmp(target_token, "all") == 0, &removed);
        if (rc == 0) {
            if (removed > 0) {
                sig_note(inv, "sigmund: pruned %d past run%s\n", removed, removed == 1 ? "" : "s");
            } else {
                sig_note(inv, "sigmund: nothing to prune\n");
            }
        }
        return rc;
    }
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "prune", target_token, all, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to prune\n");
        return 0;
    }
    bool need_elevation = false;
    for (int i = 0; i < ntargets; i++) {
        need_elevation = need_elevation || targets[i].needs_elevation;
    }
    if (need_elevation) {
        rc = elevate_with_sudo_targets(program, "prune", NULL, targets, ntargets, all, false);
        free(targets);
        return rc;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    int worst = 0;
    int removed_count = 0;
    for (int i = 0; i < ntargets; i++) {
        bool removed = false;
        rc = prune_one_run(&targets[i].store, targets[i].id, have_boot ? boot : NULL, true, &removed);
        if (removed) {
            removed_count++;
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    if (worst == 0) {
        if (removed_count > 0) {
            const char *atom = NULL;
            enum id_token_scope token_scope = parse_id_token(target_token, &atom);
            bool target_looks_like_alias = (token_scope != ID_TOKEN_INVALID && atom &&
                                            valid_alias(atom) && !valid_id_prefix(atom));
            if (target_looks_like_alias) {
                sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n",
                         removed_count, removed_count == 1 ? "" : "s", atom);
            } else {
                sig_note(inv, "sigmund: pruned %d past run%s\n", removed_count, removed_count == 1 ? "" : "s");
            }
        } else {
            sig_note(inv, "sigmund: nothing to prune\n");
        }
    }
    free(targets);
    return worst;
}

static int private_start_hash_for_token(const struct store_paths *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN],
                                        bool *matched) {
    *matched = false;
    if (valid_profile_hash(token)) {
        *matched = true;
        if (profile_exists_in_store(store, token) != 0) {
            return -1;
        }
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (valid_alias(token)) {
        if (alias_lookup_hash(store, token, hash) != 0) {
            return 0;
        }
        *matched = true;
        if (profile_exists_in_store(store, hash) != 0) {
            return -1;
        }
        return 1;
    }
    return 0;
}

static int private_start_recipe_for_token(const struct store_paths *store,
                                          const char *token,
                                          struct profile *recipe,
                                          bool *matched) {
    *matched = false;
    memset(recipe, 0, sizeof(*recipe));
    if (!valid_alias(token)) {
        return 0;
    }
    if (alias_lookup_recipe(store, token, recipe) != 0) {
        return 0;
    }
    *matched = true;
    return 1;
}

struct start_profile_target {
    struct store_paths store;
    char hash[PROFILE_HASH_STR_LEN];
    char alias[ALIAS_MAX_LEN + 1];
    struct profile recipe;
    bool has_hash;
    bool has_alias;
    bool has_recipe;
    bool needs_elevation;
};

static void free_start_profile_target(struct start_profile_target *target) {
    if (target && target->has_recipe) {
        free_profile(&target->recipe);
        target->has_recipe = false;
    }
}

static void start_target_set_alias(struct start_profile_target *target, const char *alias) {
    if (valid_alias(alias)) {
        if (checked_snprintf(target->alias, sizeof(target->alias), "%s", alias) == 0) {
            target->has_alias = true;
        }
    }
}

static int start_target_set_recipe(struct start_profile_target *target,
                                   const struct store_paths *store,
                                   const char *alias) {
    target->store = *store;
    target->has_recipe = true;
    start_target_set_alias(target, alias);
    return 1;
}

static int start_target_set_hash(struct start_profile_target *target,
                                 const struct store_paths *store,
                                 const char *hash,
                                 const char *alias,
                                 bool needs_elevation) {
    if (!valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    target->store = *store;
    if (checked_snprintf(target->hash, sizeof(target->hash), "%s", hash) != 0) {
        return -1;
    }
    target->has_hash = true;
    target->needs_elevation = needs_elevation;
    start_target_set_alias(target, alias);
    return 1;
}

static int count_running_alias(const struct store_paths *store, const char *alias, size_t *count_out) {
    struct alias_match_list matches;
    if (collect_private_alias_matches(store, alias, "start", &matches) != 0) {
        return -1;
    }
    *count_out = matches.count;
    free_alias_match_list(&matches);
    return 0;
}

static int resolve_start_profile_target(const struct invocation *inv,
                                        const struct store_paths *current_user_store,
                                        const struct store_paths *system_store,
                                        const char *token,
                                        struct start_profile_target *out) {
    memset(out, 0, sizeof(*out));
    const char *atom = NULL;
    enum id_token_scope scope = parse_id_token(token, &atom);
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    bool cap_token = false;
    if (scope == ID_TOKEN_SYSTEM && parse_alias_cap_atom(atom, cap_alias, cap_hash) == 0) {
        if (!inv->euid_root || verify_system_alias_cap(system_store, cap_alias, cap_hash) != 0) {
            fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        atom = cap_alias;
        cap_token = true;
    }
    if (scope == ID_TOKEN_INVALID) {
        return 0;
    }
    if (!valid_profile_hash(atom) && !valid_alias(atom)) {
        return 0;
    }

    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                return -1;
            }
            bool matched = false;
            int rc = private_start_recipe_for_token(&user_store, atom, &out->recipe, &matched);
            if (rc == 1) {
                return start_target_set_recipe(out, &user_store, atom);
            }
            rc = private_start_hash_for_token(&user_store, atom, out->hash, &matched);
            if (rc == 1) {
                return start_target_set_hash(out, &user_store, out->hash, valid_alias(atom) ? atom : NULL, false);
            }
            if (rc < 0 && matched) {
                fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
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
                fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            bool matched = false;
            int rc = private_start_hash_for_token(system_store, atom, out->hash, &matched);
            if (rc == 1) {
                return start_target_set_hash(out, system_store, out->hash, valid_alias(atom) ? atom : NULL, false);
            }
            if (rc < 0 && matched) {
                fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            return 0;
        }

        bool matched = false;
        int rc = private_start_hash_for_token(system_store, atom, out->hash, &matched);
        if (rc == 1) {
            return start_target_set_hash(out, system_store, out->hash, valid_alias(atom) ? atom : NULL, false);
        }
        if (rc < 0 && matched) {
            fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        if (inv->have_sudo_user) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) == 0) {
                matched = false;
                rc = private_start_recipe_for_token(&user_store, atom, &out->recipe, &matched);
                if (rc == 1) {
                    return start_target_set_recipe(out, &user_store, atom);
                }
                rc = private_start_hash_for_token(&user_store, atom, out->hash, &matched);
                if (rc == 1) {
                    return start_target_set_hash(out, &user_store, out->hash, valid_alias(atom) ? atom : NULL, false);
                }
                if (rc < 0 && matched) {
                    fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
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
            return start_target_set_hash(out, current_user_store, out->hash, valid_alias(atom) ? atom : NULL, false);
        }
        if (rc < 0 && matched) {
            fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        if (scope == ID_TOKEN_USER) {
            return 0;
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        int rc = resolve_public_profile_token(system_store, atom, out->hash);
        if (rc == 1) {
            return start_target_set_hash(out, system_store, out->hash, valid_alias(atom) ? atom : NULL, true);
        }
    }
    return 0;
}

static int elevate_start_token(const char *program,
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
    if (hash && valid_alias(token_atom) && valid_profile_hash(hash)) {
        canon[n++] = "00000000";
        canon[n++] = (char *)token_atom;
        canon[n++] = (char *)hash;
        return elevate_with_sudo_canonical(program, n, canon);
    } else if (checked_snprintf(token, sizeof(token), "system:%s", token_atom) != 0) {
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
    return elevate_with_sudo_canonical(program, n, canon);
}

static int perform_profile_start(const struct invocation *inv,
                                 const struct store_paths *store,
                                 bool tail,
                                 bool console_mode,
                                 const char *hash,
                                 const char *alias) {
    struct profile p;
    if (load_profile_by_hash(store, hash, &p) != 0) {
        fprintf(stderr, "sigmund: error: profile %s is unavailable\n", hash);
        return 5;
    }
    int rc = perform_start(inv, store, tail, console_mode, p.argc, p.argv, p.binary_path, alias);
    free_profile(&p);
    return rc;
}

static int perform_explicit_start(const struct invocation *inv,
                                  const struct store_paths *store,
                                  bool tail,
                                  bool console_mode,
                                  int argc,
                                  char **argv);

static int cmd_start_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const struct store_paths *fallback_store,
                            bool tail,
                            bool console_mode,
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
                fprintf(stderr, "sigmund: error: --tail cannot follow multiple starts\n");
                free_start_profile_target(&target);
                return 5;
            }
            if (target.needs_elevation) {
                start_rc = 0;
                for (int i = 0; i < starts; i++) {
                    start_rc = elevate_start_token(program,
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
                                "sigmund: error: alias '%s' already has a running process; use --multi to start another\n",
                                target.alias);
                        free_start_profile_target(&target);
                        return 6;
                    }
                }
                start_rc = 0;
                for (int i = 0; i < starts; i++) {
                    if (target.has_recipe) {
                        start_rc = perform_start(inv,
                                                 &target.store,
                                                 tail,
                                                 console_mode,
                                                 target.recipe.argc,
                                                 target.recipe.argv,
                                                 target.recipe.binary_path,
                                                 target.has_alias ? target.alias : NULL);
                    } else {
                        start_rc = perform_profile_start(inv,
                                                         &target.store,
                                                         tail,
                                                         console_mode,
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
        fprintf(stderr, "sigmund: error: --multi applies only to alias starts\n");
        return 5;
    }
    return perform_explicit_start(inv, fallback_store, tail, console_mode, argc, argv);
}

static int ensure_start_store_for_command(const struct invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct store_paths *store) {
    bool wants_system_store = (inv && inv->euid_root) || requested_system;

    if (wants_system_store &&
        start_target_is_within_invoking_home(inv, owned, command, argc, argv)) {
        if (inv && inv->euid_root && inv->have_sudo_user) {
            return ensure_invoking_user_store(inv, store);
        }
        return ensure_user_store_for_current_user(store);
    }

    if (wants_system_store) {
        return ensure_system_store(store);
    }
    return ensure_user_store_for_current_user(store);
}

static bool command_accepts_target_tokens(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "tail") || !strcmp(command, "dump") ||
                       !strcmp(command, "prune") || !strcmp(command, "console"));
}

static int maybe_elevate_requested_system_targets(const char *program,
                                                  const char *command,
                                                  int argc,
                                                  char **argv,
                                                  bool all,
                                                  int *rc_out) {
    if (!command_accepts_target_tokens(command) || argc <= 0) {
        return 0;
    }
    struct store_paths system_store;
    if (init_system_store(&system_store) != 0) {
        return 0;
    }
    char **canon = calloc((size_t)argc * 3 + 3, sizeof(char *));
    char **owned_tokens = calloc((size_t)argc * 3, sizeof(char *));
    if (!canon || !owned_tokens) {
        free(canon);
        free(owned_tokens);
        *rc_out = 3;
        return 1;
    }
    bool changed = false;
    int n = 0;
    canon[n++] = (char *)command;
    for (int i = 0; i < argc; i++) {
        const char *token = argv[i];
        const char *atom = NULL;
        enum id_token_scope scope = parse_id_token(token, &atom);
        if (!strcmp(command, "prune") && strcmp(token, "all") == 0) {
            canon[n++] = argv[i];
            continue;
        }
        if ((scope == ID_TOKEN_PLAIN || scope == ID_TOKEN_SYSTEM) && atom && valid_alias(atom)) {
            char hash[PROFILE_HASH_STR_LEN];
            if (alias_lookup_hash(&system_store, atom, hash) == 0) {
                struct alias_match_list matches;
                if (collect_public_alias_matches(&system_store, atom, &matches) != 0) {
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    *rc_out = 3;
                    return 1;
                }
                const char *selector = NULL;
                char selector_buf[16];
                if (matches.count == 0) {
                    free_alias_match_list(&matches);
                    *rc_out = 0;
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    return 1;
                }
                if (matches.count > 1) {
                    if (!all || !command_all_allowed(command)) {
                        report_alias_ambiguity(command, atom, &matches);
                        free_alias_match_list(&matches);
                        *rc_out = 6;
                        for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                        free(owned_tokens);
                        free(canon);
                        return 1;
                    }
                    selector = "ffffffff";
                } else {
                    snprintf(selector_buf, sizeof(selector_buf), "%s", matches.items[0].id);
                    selector = selector_buf;
                }
                size_t slot = (size_t)i * 3;
                owned_tokens[slot] = strdup(selector);
                owned_tokens[slot + 1] = strdup(atom);
                owned_tokens[slot + 2] = strdup(hash);
                free_alias_match_list(&matches);
                if (!owned_tokens[slot] || !owned_tokens[slot + 1] || !owned_tokens[slot + 2]) {
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    *rc_out = 3;
                    return 1;
                }
                canon[n++] = owned_tokens[slot];
                canon[n++] = owned_tokens[slot + 1];
                canon[n++] = owned_tokens[slot + 2];
                changed = true;
                continue;
            }
        }
        canon[n++] = argv[i];
    }
    if (!changed) {
        free(owned_tokens);
        free(canon);
        return 0;
    }
    *rc_out = elevate_with_sudo_canonical(program, n, canon);
    for (int i = 0; i < argc * 3; i++) {
        free(owned_tokens[i]);
    }
    free(owned_tokens);
    free(canon);
    return 1;
}

static int cmd_alias_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            int argc,
                            char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: sigmund alias <id> <name> [-v]\n");
        return 5;
    }
    const char *target_token = argv[0];
    const char *name = argv[1];
    bool verbose = false;
    if (argc == 3) {
        if (strcmp(argv[2], "-v") != 0 && strcmp(argv[2], "--verbose") != 0) {
            fprintf(stderr, "usage: sigmund alias <id> <name> [-v]\n");
            return 5;
        }
        verbose = true;
    }
    if (!valid_alias(name)) {
        fprintf(stderr, "sigmund: error: invalid alias '%s'\n", name);
        return 5;
    }

    struct resolved_target target;
    if (resolve_target(inv, user_store, system_store, target_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return report_not_found(target_token);
    }
    if (target.needs_elevation) {
        char scoped[8 + PROFILE_HASH_STR_LEN];
        if (checked_snprintf(scoped, sizeof(scoped), "system:%s", target.id) != 0) {
            return 3;
        }
        char *canon[3] = {"alias", scoped, (char *)name};
        return elevate_with_sudo_canonical(program, 3, canon);
    }
    if (target.store.kind == STORE_USER_LOCAL && inv->euid_root) {
        fprintf(stderr, "sigmund: error: create user-local aliases as that user\n");
        return 5;
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        if (ensure_system_store(&target.store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
    } else if (ensure_user_store_for_current_user(&target.store) != 0) {
        die_errno("sigmund: failed to init user storage");
    }

    struct record r;
    char record_path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    char *j = NULL;
    if (read_owned_file_no_symlink(record_path, &j) != 0) {
        return 5;
    }
    char **profile_argv = NULL;
    int profile_argc = 0;
    char binary_path[SIGMUND_PATH_MAX];
    char hash[PROFILE_HASH_STR_LEN];
    int rc = 0;
    if (json_get_argv_alloc(j, &profile_argv, &profile_argc) != 0 ||
        resolve_binary_path(profile_argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "sigmund: error: failed to derive profile from run %s\n", target.id);
        rc = 5;
        goto out;
    }
    char command[256];
    if (format_argv_human(command, sizeof(command), profile_argc, profile_argv) != 0) {
        snprintf(command, sizeof(command), "%s", "?");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        profile_hash_for_argv(binary_path, profile_argc, profile_argv, hash);
        if (write_profile_atomic(&target.store, hash, binary_path, profile_argc, profile_argv) != 0) {
            die_errno("sigmund: failed to write profile");
        }
        if (alias_upsert_hash(&target.store, name, hash) != 0) {
            die_errno("sigmund: failed to write alias");
        }
        if (verbose) {
            sig_note(inv, "sigmund: pinned '%s' -> %s (hash %s)\n", name, command, hash);
        } else {
            sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
        }
    } else {
        if (alias_upsert_recipe(&target.store, name, binary_path, profile_argc, profile_argv) != 0) {
            die_errno("sigmund: failed to write alias");
        }
        sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
    }

out:
    free_argv_alloc(profile_argv, profile_argc);
    free(j);
    return rc;
}

static int print_aliases_for_store(const char *scope, const struct store_paths *store, bool verbose) {
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (load_aliases(store, &entries, &count) != 0) {
        fprintf(stderr, "sigmund: warning: failed to read %s aliases\n", scope);
        return 5;
    }
    for (size_t i = 0; i < count; i++) {
        char command[96];
        char hash_display[PROFILE_HASH_STR_LEN];
        if (entries[i].has_recipe) {
            if (format_argv_human(command, sizeof(command), entries[i].argc, entries[i].argv) != 0) {
                snprintf(command, sizeof(command), "%s", "?");
            }
        } else {
            snprintf(command, sizeof(command), "%s", "<root-managed>");
        }
        if (entries[i].has_hash) {
            if (verbose) {
                snprintf(hash_display, sizeof(hash_display), "%s", entries[i].hash);
            } else {
                snprintf(hash_display, sizeof(hash_display), "%.12s...", entries[i].hash);
            }
        } else {
            snprintf(hash_display, sizeof(hash_display), "%s", "-");
        }
        printf("%-12s %-6s %-40.40s %s\n", entries[i].name, scope, command, hash_display);
    }
    free_aliases(entries, count);
    return 0;
}

static int cmd_aliases_action(const struct invocation *inv,
                              const struct store_paths *user_store,
                              const struct store_paths *system_store,
                              bool verbose) {
    printf("%-12s %-6s %-40s %s\n", "NAME", "SCOPE", "COMMAND", "HASH");
    int rc = 0;
    if (inv->euid_root) {
        if (print_aliases_for_store("system", system_store, verbose) != 0) {
            rc = 5;
        }
        if (!inv->requested_system && inv->have_sudo_user) {
            struct store_paths sudo_user_store;
            if (init_invoking_user_store(inv, &sudo_user_store) == 0 &&
                print_aliases_for_store("user", &sudo_user_store, verbose) != 0) {
                rc = 5;
            }
        }
        return rc;
    }
    if (print_aliases_for_store("user", user_store, verbose) != 0) {
        rc = 5;
    }
    if (print_aliases_for_store("system", system_store, verbose) != 0) {
        rc = 5;
    }
    return rc;
}

static const char *const grant_action_names[] = {"start", "stop", "kill", "tail", "dump", "prune", "console"};
#define GRANT_ACTION_COUNT ((int)(sizeof(grant_action_names) / sizeof(grant_action_names[0])))

static int parse_grant_subject(const char *input, char *out, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (strcmp(input, "all") == 0 || strcmp(input, "ALL") == 0) {
        return checked_snprintf(out, n, "%s", "ALL");
    }
    const char *name = input;
    bool group = false;
    if (*name == '%') {
        group = true;
        name++;
        if (!*name) {
            return -1;
        }
    }
    size_t len = strlen(name);
    if (len == 0 || len > ALIAS_MAX_LEN) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return -1;
        }
    }
    return checked_snprintf(out, n, "%s%s", group ? "%" : "", name);
}

static int grant_action_index(const char *name) {
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (strcmp(name, grant_action_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int parse_grant_actions(const char *input, bool selected[GRANT_ACTION_COUNT], bool *all_scope) {
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        selected[i] = false;
    }
    *all_scope = false;
    if (!input || !*input) {
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            selected[i] = true;
        }
        *all_scope = true;
        return 0;
    }
    if (strlen(input) > 128) {
        return -1;
    }
    char buf[129];
    snprintf(buf, sizeof(buf), "%s", input);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (!*tok) {
            return -1;
        }
        int idx = grant_action_index(tok);
        if (idx < 0) {
            return -1;
        }
        selected[idx] = true;
    }
    return 0;
}

static int validate_sigmund_self_for_sudoers(const char *program, char *abs_sigmund, size_t n) {
    if (resolve_self_executable_path(program, abs_sigmund, n) != 0) {
        fprintf(stderr, "sigmund: error: cannot determine executable path for sudoers grant\n");
        return -1;
    }
    for (const char *p = abs_sigmund; *p; p++) {
        if (isspace((unsigned char)*p)) {
            fprintf(stderr, "sigmund: error: executable path contains whitespace and cannot be safely managed in sudoers\n");
            return -1;
        }
    }
    struct stat st;
    if (stat(abs_sigmund, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "sigmund: error: executable path is not a regular file: %s\n", abs_sigmund);
        return -1;
    }
    if (st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        fprintf(stderr,
                "sigmund: error: refusing sudoers grant because %s is not root-owned with group/world writes disabled\n",
                abs_sigmund);
        return -1;
    }
    return 0;
}

static const char *sudoers_dir_path(void) {
#ifdef SIGMUND_TESTING
    const char *override = getenv("SIGMUND_TEST_SUDOERS_DIR");
    if (override && *override) {
        return override;
    }
#endif
    return "/etc/sudoers.d";
}

static int resolve_system_alias_hash_for_grant(const struct store_paths *system_store,
                                               const char *alias,
                                               char hash[PROFILE_HASH_STR_LEN]) {
    if (!valid_alias(alias)) {
        return -1;
    }
    if (alias_lookup_hash(system_store, alias, hash) != 0) {
        return -1;
    }
    return profile_exists_in_store(system_store, hash);
}

static int build_action_alternation(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n) {
    size_t off = 0;
    out[0] = '\0';
    bool first = true;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (!selected[i]) {
            continue;
        }
        const char *name = grant_action_names[i];
        size_t need = strlen(name) + (first ? 0 : 1);
        if (off + need + 3 >= n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (!first) {
            out[off++] = '|';
        }
        memcpy(out + off, name, strlen(name));
        off += strlen(name);
        out[off] = '\0';
        first = false;
    }
    if (first) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int build_actions_csv(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n) {
    size_t off = 0;
    out[0] = '\0';
    bool first = true;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (!selected[i]) {
            continue;
        }
        const char *name = grant_action_names[i];
        size_t need = strlen(name) + (first ? 0 : 1);
        if (off + need + 1 >= n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (!first) {
            out[off++] = ',';
        }
        memcpy(out + off, name, strlen(name));
        off += strlen(name);
        out[off] = '\0';
        first = false;
    }
    return first ? -1 : 0;
}

static int build_sudoers_line(char *out,
                              size_t n,
                              const char *subject,
                              const char *abs_sigmund,
                              const bool selected[GRANT_ACTION_COUNT],
                              const char *alias,
                              const char *hash) {
    char verb_alt[128];
    if (build_action_alternation(selected, verb_alt, sizeof(verb_alt)) != 0) {
        return -1;
    }
    return checked_snprintf(out, n,
                            "%s ALL=(root) NOPASSWD: %s ^--system --elevated (%s) [0-9a-f]{8} %s %s$",
                            subject, abs_sigmund, verb_alt, alias, hash);
}

static bool any_grant_action_selected(const bool selected[GRANT_ACTION_COUNT]) {
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (selected[i]) {
            return true;
        }
    }
    return false;
}

static int find_visudo(char *out, size_t n) {
#ifdef SIGMUND_TESTING
    const char *override = getenv("SIGMUND_TEST_VISUDO_PROG");
    if (override && *override) {
        return checked_snprintf(out, n, "%s", override);
    }
#endif
    return resolve_binary_path("visudo", out, n);
}

static int validate_sudoers_candidate(const char *path) {
    char visudo[SIGMUND_PATH_MAX];
    if (find_visudo(visudo, sizeof(visudo)) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl(visudo, visudo, "-cf", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int subject_file_label_for_grant(const char *subject, char *out, size_t n) {
    if (strcmp(subject, "ALL") == 0) {
        return checked_snprintf(out, n, "%s", "all");
    }
    if (subject[0] == '%') {
        return checked_snprintf(out, n, "group_%s", subject + 1);
    }
    return checked_snprintf(out, n, "%s", subject);
}

static void actions_from_existing_sudoers(const char *existing,
                                          const char *subject,
                                          const char *abs_sigmund,
                                          const char *alias,
                                          const char *hash,
                                          bool selected[GRANT_ACTION_COUNT]) {
    (void)subject;
    (void)abs_sigmund;
    (void)alias;
    (void)hash;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        selected[i] = false;
    }
    if (!existing) {
        return;
    }
    const char *p = strstr(existing, "# actions-list:");
    if (!p) {
        return;
    }
    p += strlen("# actions-list:");
    p = skip_ws(p);
    char buf[256];
    size_t len = 0;
    while (p[len] && p[len] != '\n' && len + 1 < sizeof(buf)) {
        buf[len] = p[len];
        len++;
    }
    buf[len] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = (char *)skip_ws(tok);
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        int idx = grant_action_index(tok);
        if (idx >= 0) {
            selected[idx] = true;
        }
    }
}

static int write_sudoers_template_file(const char *sudoers_path,
                                       const char *target_label,
                                       const char *subject,
                                       const char *abs_sigmund,
                                       const char *hash,
                                       const bool selected[GRANT_ACTION_COUNT],
                                       bool all_scope) {
    char dir[SIGMUND_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sudoers_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    char tmp[SIGMUND_PATH_MAX];
    if (checked_snprintf(tmp, sizeof(tmp), "%s.tmp", sudoers_path) != 0) {
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0440);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0440) != 0 || (geteuid() == 0 && fchown(fd, 0, 0) != 0)) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fputs("# sigmund managed sudoers; use sigmund grant/revoke\n", f);
    fprintf(f, "# target: ");
    json_escape(f, target_label);
    fputc('\n', f);
    fprintf(f, "# hash: ");
    json_escape(f, hash);
    fputc('\n', f);
    fprintf(f, "# actions: %s\n", all_scope ? "ALL" : "explicit");
    char actions_csv[256];
    if (build_actions_csv(selected, actions_csv, sizeof(actions_csv)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "# actions-list: %s\n", actions_csv);
    char line[SIGMUND_PATH_MAX + 256];
    if (build_sudoers_line(line, sizeof(line), subject, abs_sigmund, selected, target_label, hash) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fputs(line, f);
    fputc('\n', f);
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (validate_sudoers_candidate(tmp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, sudoers_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)fsync_dir_path(dir);
    return 0;
}

static int unlink_sudoers_template_file(const char *sudoers_path) {
    if (unlink(sudoers_path) != 0 && errno != ENOENT) {
        return -1;
    }
    char dir[SIGMUND_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sudoers_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)fsync_dir_path(dir);
    }
    return 0;
}

static int cmd_grant_revoke_action(const struct invocation *inv,
                                   const struct store_paths *system_store,
                                   const char *program,
                                   bool grant,
                                   int argc,
                                   char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n",
                grant ? "grant" : "revoke");
        return 5;
    }
    if (!inv->euid_root) {
        fprintf(stderr, "sigmund: error: %s requires root authority\n", grant ? "grant" : "revoke");
        return 5;
    }
    char abs_sigmund[SIGMUND_PATH_MAX];
    if (validate_sigmund_self_for_sudoers(program, abs_sigmund, sizeof(abs_sigmund)) != 0) {
        return 5;
    }
    char subject[128];
    if (parse_grant_subject(argv[1], subject, sizeof(subject)) != 0) {
        fprintf(stderr, "sigmund: error: invalid sudoers subject '%s'\n", argv[1]);
        return 5;
    }
    bool selected[GRANT_ACTION_COUNT];
    bool all_scope = false;
    if (parse_grant_actions(argc == 3 ? argv[2] : NULL, selected, &all_scope) != 0) {
        fprintf(stderr, "sigmund: error: invalid action list '%s'\n", argc == 3 ? argv[2] : "");
        return 5;
    }
    char hash[PROFILE_HASH_STR_LEN];
    if (resolve_system_alias_hash_for_grant(system_store, argv[0], hash) != 0) {
        fprintf(stderr, "sigmund: error: grant target must be an existing system alias\n");
        return 5;
    }

    const char *target_label = argv[0];
    char subject_label[128];
    if (subject_file_label_for_grant(subject, subject_label, sizeof(subject_label)) != 0) {
        return 3;
    }
    const char *dir = sudoers_dir_path();
    char sudoers_path[SIGMUND_PATH_MAX];
    if (checked_snprintf(sudoers_path, sizeof(sudoers_path), "%s/sigmund_%s_%s", dir, target_label, subject_label) != 0) {
        return 3;
    }

    if (!grant) {
        if (all_scope) {
            if (unlink_sudoers_template_file(sudoers_path) != 0) {
                die_errno("sigmund: failed to remove managed sudoers file");
            }
            sig_note(inv, "sigmund: revoked sudoers entries for %s %s\n", subject, hash);
            return 0;
        }
        char *existing = NULL;
        bool remaining[GRANT_ACTION_COUNT];
        if (read_owned_file_no_symlink(sudoers_path, &existing) != 0) {
            if (errno == ENOENT) {
                sig_note(inv, "sigmund: revoked sudoers entries for %s %s\n", subject, hash);
                return 0;
            }
            die_errno("sigmund: failed to read managed sudoers file");
        }
        actions_from_existing_sudoers(existing, subject, abs_sigmund, target_label, hash, remaining);
        free(existing);
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            if (selected[i]) {
                remaining[i] = false;
            }
        }
        if (!any_grant_action_selected(remaining)) {
            if (unlink_sudoers_template_file(sudoers_path) != 0) {
                die_errno("sigmund: failed to remove managed sudoers file");
            }
        } else if (write_sudoers_template_file(sudoers_path, target_label, subject, abs_sigmund, hash, remaining, false) != 0) {
            die_errno("sigmund: failed to update managed sudoers file");
        }
        sig_note(inv, "sigmund: revoked sudoers entries for %s %s\n", subject, hash);
        return 0;
    }

    if (!all_scope) {
        char *existing = NULL;
        bool merged[GRANT_ACTION_COUNT];
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            merged[i] = selected[i];
        }
        if (read_owned_file_no_symlink(sudoers_path, &existing) == 0) {
            bool existing_actions[GRANT_ACTION_COUNT];
            actions_from_existing_sudoers(existing, subject, abs_sigmund, target_label, hash, existing_actions);
            for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
                merged[i] = merged[i] || existing_actions[i];
            }
            free(existing);
        } else if (errno != ENOENT) {
            die_errno("sigmund: failed to read managed sudoers file");
        }
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            selected[i] = merged[i];
        }
    }

    if (write_sudoers_template_file(sudoers_path, target_label, subject, abs_sigmund, hash, selected, all_scope) != 0) {
        die_errno("sigmund: failed to update managed sudoers file");
    }
    sig_note(inv, "sigmund: granted sudoers entries for %s %s\n", subject, hash);
    return 0;
}

static void usage(void) {
    printf("sigmund %s - more than nohup, less than systemd\n\n"
           "Run a command that outlives your shell, then find it, watch it, and stop it\n"
           "safely later. No daemon, no config.\n\n"
           "USAGE\n"
           "  sigmund <command> [args...]      start a command in the background\n"
           "  sigmund <action>  [target...]    act on a tracked command\n\n"
           "START\n"
           "  sigmund <command>...             start it; prints a short run id\n"
           "  sigmund -f <command>...          start it and stream output\n"
           "  sigmund --console <command>...   start it with an attachable console\n"
           "  sigmund start <alias>            start a pinned alias\n\n"
           "MANAGE\n"
           "  sigmund list   [alias]          show tracked runs (optionally one alias)\n"
           "  sigmund tail   <target>         follow a run's live output\n"
           "  sigmund console <target>        attach to a run's console\n"
           "  sigmund dump   <target>         print a run's log and exit\n"
           "  sigmund stop   <target>         graceful stop (TERM, then KILL)\n"
           "  sigmund kill   <target>         force kill now (KILL)\n"
           "  sigmund prune  [target|all]     clear past run data\n\n"
           "  target = run id, id prefix, or alias name\n\n"
           "MORE\n"
           "  sigmund help profiles           pin a command as a reusable alias\n"
           "  sigmund help access             give another user scoped access\n"
           "  sigmund help targets            id, alias, and scope resolution\n"
           "  sigmund help system             root-managed runs and elevation\n"
           "  sigmund help scripting          exit codes, --print, --quiet, stdout\n"
           "  sigmund help console            attachable PTY consoles\n"
           "  sigmund <action> -h             help for one action\n\n"
           "  sigmund --version\n",
           SIGMUND_VERSION);
}

static int help_profiles(void) {
    printf("sigmund help profiles\n\n"
           "Pin a run's exact command (resolved binary path + argv) under a reusable\n"
           "name.\n\n"
           "  sigmund alias <id> <name>       pin the command behind <id> as <name>\n"
           "  sigmund aliases [-v]            list visible aliases\n"
           "  sigmund start <name>            start a fresh run under that name\n\n"
           "The name is also recorded on runs started as <name>, so later\n"
           "list, tail, console, dump, stop, kill, and prune commands can use <name>. If the command behind\n"
           "<name> is updated later, future starts use the updated command; prior runs\n"
           "remain under the same recorded alias label.\n");
    return 0;
}

static int help_targets(void) {
    printf("sigmund help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = run id, leading id prefix, or alias name\n\n"
           "A run id addresses one run directly, always. An alias resolves among runs\n"
           "recorded under that name, narrowed by the verb: stop/kill/tail look at\n"
           "running runs, console looks at running console-enabled runs, dump looks at\n"
           "logged runs, and prune looks at removable past\n"
           "run data. One match acts. Several matches exit 6 and print candidates;\n"
           "--all resolves that ambiguity for stop, kill, and prune. A known alias with\n"
           "nothing to do exits 0.\n");
    return 0;
}

static int help_access(void) {
    printf("sigmund help access\n\n"
           "Grant another user permission to act on a specific root-managed alias as\n"
           "root, without a password, scoped to one immutable protected profile.\n\n"
           "  sigmund grant  <alias> <user> [actions]\n"
           "  sigmund revoke <alias> <user> [actions]\n\n"
           "actions = any of: start,stop,kill,tail,dump,prune,console   (default: all)\n\n"
           "The <user> field may be a username, %%group, or all. Sigmund stores one\n"
           "managed sudoers file per alias/user pair. The file contains the current\n"
           "protected profile hash for that alias, an anchored action alternation, and\n"
           "an 8-hex run selector slot. If root updates the alias profile and the hash\n"
           "changes, grant rewrites the same managed file via temp file, visudo check,\n"
           "and atomic rename.\n");
    return 0;
}

static int help_system(void) {
    printf("sigmund help system\n\n"
           "Root, sudo, and --system runs use the root-managed store:\n\n"
           "  Linux: /var/lib/sigmund\n"
           "  macOS: /var/db/sigmund\n\n"
           "Private root records, logs, and profiles stay root-only. Normal users see\n"
           "only the redacted public index and public alias dictionary. A normal action\n"
           "on a root-only public target self-elevates through sudo; user-local targets\n"
           "win over root-public collisions.\n\n"
           "  sigmund --system <cmd...>       start in root-managed state\n"
           "  sigmund --system list           list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("sigmund help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(sigmund <cmd...>)          capture the bare 8-hex run id\n"
           "  sigmund stop --print <id>       print kill -TERM -- -<pgid>\n"
           "  sigmund kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known alias with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_console(void) {
    printf("sigmund help console\n\n"
           "Start a run with an attachable PTY console, then reconnect to it later.\n"
           "Console output is still tee'd to the normal log, so tail and dump continue\n"
           "to work.\n\n"
           "  sigmund --console <cmd...>      start with an attachable console\n"
           "  sigmund start <alias> --console start an alias with a console\n"
           "  sigmund console <target>        attach to that console\n\n"
           "Console attach is native: Sigmund saves your terminal, enters an alternate\n"
           "screen for interactive attaches, forwards terminal size changes to the PTY,\n"
           "and restores your original screen on exit. Ctrl-] detaches without asking\n"
           "Sigmund to stop the run.\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: sigmund list [alias] [--iso|-l]\n\nShow all visible runs, optionally filtered by recorded alias label.\n");
    } else if (!strcmp(action, "start")) {
        printf("usage: sigmund start <alias> [--multi [N]] [--console]\n       sigmund start <cmd> [args...]\n\nStart an alias recipe, or use explicit start form for a raw command.\n");
    } else if (!strcmp(action, "stop")) {
        printf("usage: sigmund stop [--print] [--all] <target>...\n\nGracefully stop matching runs with TERM, then KILL if needed.\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: sigmund kill [--print] [--all] <target>...\n\nForce matching runs down with KILL.\n");
    } else if (!strcmp(action, "tail")) {
        printf("usage: sigmund tail <target>\n\nFollow live output for an alias match, or follow an id's log directly.\n");
    } else if (!strcmp(action, "console")) {
        printf("usage: sigmund console <target>\n\nAttach to a running console-enabled run.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: sigmund dump <target>\n\nPrint a run log and exit.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: sigmund prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "alias")) {
        printf("usage: sigmund alias <id> <name> [-v]\n\nPin the command behind a run id as a reusable alias.\n");
    } else if (!strcmp(action, "aliases")) {
        printf("usage: sigmund aliases [-v]\n\nList visible aliases. User aliases show commands; system commands are redacted.\n");
    } else if (!strcmp(action, "grant") || !strcmp(action, "revoke")) {
        printf("usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n\nManage Sigmund-owned sudoers access for a root-managed alias.\n", action);
    } else {
        return -1;
    }
    return 0;
}

static int show_help(const char *topic) {
    if (!topic || !*topic) {
        usage();
        return 0;
    }
    if (!strcmp(topic, "profiles")) return help_profiles();
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "access")) return help_access();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "console")) return help_console();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "sigmund: unknown help topic '%s'\n", topic);
    return 5;
}

static bool is_sigmund_owned_command(const char *s) {
    return s && (!strcmp(s, "list") || !strcmp(s, "stop") || !strcmp(s, "kill") ||
                 !strcmp(s, "tail") || !strcmp(s, "dump") || !strcmp(s, "prune") ||
                 !strcmp(s, "console") ||
                 !strcmp(s, "start") ||
                 !strcmp(s, "alias") || !strcmp(s, "aliases") ||
                 !strcmp(s, "grant") || !strcmp(s, "revoke") ||
                 !strcmp(s, "help"));
}

static bool parse_positive_count(const char *s, int *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < 1 || v > 1000) {
        return false;
    }
    *out = (int)v;
    return true;
}

static int perform_explicit_start(const struct invocation *inv,
                                  const struct store_paths *store,
                                  bool tail,
                                  bool console_mode,
                                  int argc,
                                  char **argv) {
    if (argc <= 0) {
        fprintf(stderr, "usage: sigmund start <cmd> [args...]\n");
        return 5;
    }
    if (argc == 1) {
        char *shell_argv[4];
        shell_argv[0] = "sh";
        shell_argv[1] = "-c";
        shell_argv[2] = argv[0];
        shell_argv[3] = NULL;
        return perform_start(inv, store, tail, console_mode, 3, shell_argv, NULL, NULL);
    }
    return perform_start(inv, store, tail, console_mode, argc, argv, NULL, NULL);
}

static int cmd_elevated_capability_action(const struct invocation *inv,
                                          const struct store_paths *system_store,
                                          const char *command,
                                          bool tail,
                                          bool console_mode,
                                          int sig,
                                          bool graceful,
                                          int argc,
                                          char **argv) {
    if (!inv->euid_root || argc != 3) {
        return -1;
    }
    const char *runid_sel = argv[0];
    const char *alias = argv[1];
    const char *hash = argv[2];
    if (!valid_runid_selector(runid_sel) || !valid_alias(alias) || !valid_profile_hash(hash)) {
        return -1;
    }
    if (verify_system_alias_cap(system_store, alias, hash) != 0) {
        fprintf(stderr, "sigmund: error: capability for '%s' is no longer valid\n", alias);
        return 3;
    }

    if (!strcmp(command, "start")) {
        if (strcmp(runid_sel, "00000000") != 0) {
            fprintf(stderr, "sigmund: error: start capability requires selector 00000000\n");
            return 5;
        }
        return perform_profile_start(inv, system_store, tail, console_mode, hash, alias);
    }

    if (strcmp(runid_sel, "00000000") == 0) {
        fprintf(stderr, "sigmund: error: selector 00000000 is only valid for start\n");
        return 5;
    }

    if (strcmp(runid_sel, "ffffffff") == 0) {
        if (!command_all_allowed(command)) {
            fprintf(stderr, "sigmund: error: selector ffffffff is not valid for %s\n", command);
            return 5;
        }
        struct alias_match_list matches;
        if (collect_private_alias_matches(system_store, alias, command, &matches) != 0) {
            return 3;
        }
        int worst = 0;
        int acted = 0;
        char boot[128] = {0};
        bool have_boot = current_boot_id(boot, sizeof(boot));
        for (size_t i = 0; i < matches.count; i++) {
            int rc = 0;
            if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
                bool already_done = false;
                rc = do_signal_action(system_store,
                                      matches.items[i].id,
                                      sig,
                                      graceful,
                                      &already_done);
                if (rc == 0) {
                    sig_note(inv,
                             "sigmund: %s %s\n",
                             already_done ? matches.items[i].id : (!strcmp(command, "kill") ? "killed" : "stopped"),
                             already_done ? "already exited" : matches.items[i].id);
                }
            } else if (!strcmp(command, "prune")) {
                bool removed = false;
                rc = prune_one_run(system_store, matches.items[i].id, have_boot ? boot : NULL, true, &removed);
                if (removed) {
                    acted++;
                }
            }
            if (rc == 0 && strcmp(command, "prune") != 0) {
                acted++;
            }
            if (rc > worst) {
                worst = rc;
            }
        }
        if (!strcmp(command, "prune") && worst == 0) {
            if (acted > 0) {
                sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n", acted, acted == 1 ? "" : "s", alias);
            } else {
                sig_note(inv, "sigmund: nothing to prune\n");
            }
        } else if (acted == 0 && worst == 0) {
            sig_note(inv, "sigmund: nothing to %s\n", command);
        }
        free_alias_match_list(&matches);
        return worst;
    }

    if (ensure_run_recorded_under_alias(system_store, runid_sel, alias) != 0) {
        fprintf(stderr, "sigmund: error: run %s is not recorded under alias '%s'\n", runid_sel, alias);
        return 3;
    }

    struct record selected_record;
    char selected_path[SIGMUND_PATH_MAX];
    if (load_record_by_id(system_store->record_dir, runid_sel, &selected_record, selected_path, sizeof(selected_path)) != 0) {
        return 5;
    }
    char selected_boot[128] = {0};
    bool have_selected_boot = current_boot_id(selected_boot, sizeof(selected_boot));
    enum run_state selected_state = eval_state(&selected_record, have_selected_boot ? selected_boot : NULL);
    if (!strcmp(command, "console")) {
        return attach_console_record(inv, &selected_record, selected_state);
    }
    if (!record_matches_alias_intent(command, &selected_record, selected_state)) {
        sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }

    if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
        bool already_done = false;
        int rc = do_signal_action(system_store, runid_sel, sig, graceful, &already_done);
        if (rc == 0) {
            if (already_done) {
                sig_note(inv, "sigmund: %s already exited\n", runid_sel);
            } else {
                sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", runid_sel);
            }
        }
        return rc;
    }
    if (!strcmp(command, "prune")) {
        char boot[128] = {0};
        bool have_boot = current_boot_id(boot, sizeof(boot));
        bool removed = false;
        int rc = prune_one_run(system_store, runid_sel, have_boot ? boot : NULL, true, &removed);
        if (rc == 0) {
            sig_note(inv, removed ? "sigmund: pruned 1 past run for '%s'\n" : "sigmund: nothing to prune\n", alias);
        }
        return rc;
    }
    if (!strcmp(command, "tail") || !strcmp(command, "dump")) {
        struct record r;
        char path[SIGMUND_PATH_MAX];
        if (load_record_by_id(system_store->record_dir, runid_sel, &r, path, sizeof(path)) != 0 || !r.has_log) {
            return 5;
        }
        if (!strcmp(command, "tail")) {
            char boot[128] = {0};
            bool have_boot = current_boot_id(boot, sizeof(boot));
            enum run_state st = eval_state(&r, have_boot ? boot : NULL);
            return tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
        }
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            die_errno("sigmund: failed to open log for dump");
        }
        char buf[4096];
        while (1) {
            ssize_t nr = read(fd, buf, sizeof(buf));
            if (nr == 0) {
                break;
            }
            if (nr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                die_errno("sigmund: failed while dumping log");
            }
            if (write_all(STDOUT_FILENO, buf, (size_t)nr) != 0) {
                close(fd);
                die_errno("sigmund: failed writing dumped output");
            }
        }
        close(fd);
        return 0;
    }

    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    int argi = 1;
    bool requested_system = false;
    bool elevated = false;
    bool tail = false;
    bool console_mode = false;
    bool force_raw = false;
    bool all = false;
    bool multi = false;
    bool quiet = false;
    bool print_cmd = false;
    bool list_iso = false;
    int multi_count = 1;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--system")) {
            requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--elevated")) {
            elevated = true;
            requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--tail") || !strcmp(argv[argi], "-f")) {
            tail = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--console")) {
            console_mode = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--quiet")) {
            quiet = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--")) {
            force_raw = true;
            argi++;
            break;
        }
        break;
    }

    if (argi >= argc) {
        usage();
        return 5;
    }

    bool owned = !force_raw && !tail && is_sigmund_owned_command(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 3;
        }
        bool literal_owned_arg = false;
        for (int i = argi; i < argc; i++) {
            if (!literal_owned_arg && !strcmp(argv[i], "--")) {
                literal_owned_arg = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--system")) {
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--elevated")) {
                elevated = true;
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--quiet")) {
                quiet = true;
                continue;
            }
            if (!literal_owned_arg && command_accepts_target_tokens(command) && !strcmp(argv[i], "--all")) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "stop") || !strcmp(command, "kill")) &&
                !strcmp(argv[i], "--print")) {
                print_cmd = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") &&
                (!strcmp(argv[i], "--tail") || !strcmp(argv[i], "-f"))) {
                tail = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--console")) {
                console_mode = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "list") &&
                (!strcmp(argv[i], "--iso") || !strcmp(argv[i], "-l"))) {
                list_iso = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--multi")) {
                multi = true;
                multi_count = 1;
                if (i + 1 < argc) {
                    int parsed = 0;
                    if (parse_positive_count(argv[i + 1], &parsed)) {
                        multi_count = parsed;
                        i++;
                    }
                }
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && strncmp(argv[i], "--multi=", 8) == 0) {
                multi = true;
                if (!parse_positive_count(argv[i] + 8, &multi_count)) {
                    fprintf(stderr, "sigmund: error: invalid --multi count '%s'\n", argv[i] + 8);
                    free(cmd_argv);
                    return 5;
                }
                continue;
            }
            cmd_argv[cmd_argc++] = argv[i];
        }
    } else {
        command = NULL;
        cmd_argc = argc - argi;
        cmd_argv = argv + argi;
    }

    if (!owned && !force_raw && !tail && !strcmp(argv[argi], "--version")) {
        puts(SIGMUND_VERSION);
        return 0;
    }
    if (!owned && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund help [topic]\n");
            rc = 5;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = show_help(NULL);
        } else {
            rc = show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (console_mode && owned && strcmp(command, "start") != 0) {
        fprintf(stderr, "sigmund: error: --console applies only to starts\n");
        free(cmd_argv);
        return 5;
    }

    struct invocation inv;
    if (detect_invocation(&inv, requested_system, elevated) != 0) {
        die_errno("sigmund: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "sigmund: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && owned && !strcmp(command, "start") && cmd_argc == 1) {
        struct store_paths pre_system_store;
        if (init_system_store(&pre_system_store) == 0) {
            const char *atom = NULL;
            enum id_token_scope start_scope = parse_id_token(cmd_argv[0], &atom);
            if ((start_scope == ID_TOKEN_PLAIN || start_scope == ID_TOKEN_SYSTEM) && atom &&
                (valid_profile_hash(atom) || valid_alias(atom))) {
                char hash[PROFILE_HASH_STR_LEN];
                if (resolve_public_profile_token(&pre_system_store, atom, hash) == 1) {
                    int rc = 0;
                    int starts = multi ? multi_count : 1;
                    for (int i = 0; i < starts; i++) {
                        rc = elevate_start_token(argv[0],
                                                 tail,
                                                 console_mode,
                                                 valid_alias(atom) ? atom : hash,
                                                 valid_alias(atom) ? hash : NULL,
                                                 false,
                                                 1);
                        if (rc != 0) {
                            break;
                        }
                    }
                    free(cmd_argv);
                    return rc;
                }
            }
        }
    }
    if (requested_system && !inv.euid_root && !is_list) {
        int canonical_rc = 0;
        if (owned && maybe_elevate_requested_system_targets(argv[0], command, cmd_argc, cmd_argv, all, &canonical_rc)) {
            free(cmd_argv);
            return canonical_rc;
        }
        int rc = elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
        if (owned) {
            free(cmd_argv);
        }
        return rc;
    }

    struct store_paths user_store;
    struct store_paths system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (init_system_store(&system_store) != 0) {
        die_errno("sigmund: failed to resolve system storage");
    }

    if (!inv.euid_root || is_list || (owned && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                                               !strcmp(command, "tail") || !strcmp(command, "dump") ||
                                               !strcmp(command, "prune") || !strcmp(command, "console")))) {
        if (!inv.euid_root) {
            if (ensure_user_store_for_current_user(&user_store) != 0) {
                die_errno("sigmund: failed to init user storage");
            }
        }
    }

    if (inv.elevated && inv.euid_root && owned && cmd_argc == 3 &&
        (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
         !strcmp(command, "tail") || !strcmp(command, "dump") || !strcmp(command, "prune") ||
         !strcmp(command, "console"))) {
        int sig = !strcmp(command, "kill") ? SIGKILL : SIGTERM;
        bool graceful = !strcmp(command, "stop");
        int rc = cmd_elevated_capability_action(&inv, &system_store, command, tail, console_mode, sig, graceful, cmd_argc, cmd_argv);
        if (rc >= 0) {
            free(cmd_argv);
            return rc;
        }
    }

    if (!owned) {
        struct store_paths start_store;
        if (ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                die_errno("sigmund: failed to init invoking-user storage");
            }
            die_errno("sigmund: failed to init start storage");
        }
        return perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
    }

    if (!strcmp(command, "start")) {
        struct store_paths start_store;
        if (ensure_start_store_for_command(&inv, requested_system, true, command, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                start_target_is_within_invoking_home(&inv, true, command, cmd_argc, cmd_argv)) {
                die_errno("sigmund: failed to init invoking-user storage");
            }
            die_errno("sigmund: failed to init start storage");
        }
        int rc = cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, multi, multi_count, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund list [alias]\n");
            free(cmd_argv);
            return 5;
        }
        const char *alias_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (alias_filter && !valid_alias(alias_filter)) {
            fprintf(stderr, "sigmund: error: invalid alias '%s'\n", alias_filter);
            free(cmd_argv);
            return 5;
        }
        if (inv.euid_root) {
            rc = cmd_list_system(&system_store, alias_filter, list_iso);
        } else {
            rc = cmd_list_normal(&user_store, &system_store, alias_filter, list_iso);
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "tail")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund tail <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_tail_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund dump <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_dump_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "console")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund console <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_console_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        int rc = cmd_prune_action(&inv, &user_store, &system_store, argv[0], target, all);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "alias")) {
        int rc = cmd_alias_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "aliases")) {
        bool aliases_verbose = false;
        if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "-v") || !strcmp(cmd_argv[0], "--verbose"))) {
            aliases_verbose = true;
        } else if (cmd_argc != 0) {
            fprintf(stderr, "usage: sigmund aliases [-v]\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_aliases_action(&inv, &user_store, &system_store, aliases_verbose);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "grant")) {
        if (ensure_system_store(&system_store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
        int rc = cmd_grant_revoke_action(&inv, &system_store, argv[0], true, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "revoke")) {
        if (ensure_system_store(&system_store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
        int rc = cmd_grant_revoke_action(&inv, &system_store, argv[0], false, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", cmd_argc, cmd_argv, SIGTERM, true, all, print_cmd);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "kill", cmd_argc, cmd_argv, SIGKILL, false, all, print_cmd);
        free(cmd_argv);
        return rc;
    }

    free(cmd_argv);
    usage();
    return 1;
}
