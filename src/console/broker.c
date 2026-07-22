#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/term.h"
#include "hold/console_internal.h"

#define CONSOLE_REPLAY_LIMIT (64 * 1024)

/* The child-mode broker's forked target group, set right after the pid is
 * reported and cleared at every reap site. The SIGTERM forwarder reads it so a
 * TERM aimed at the broker (e.g. a raw kill of the broker pid) reaches the held
 * group instead of orphaning it. Never set by the adopted entry, which must
 * never signal its adopted group. */
static volatile pid_t g_broker_forward_target = 0;

static void broker_forward_term(int signo) {
    (void)signo;
    pid_t t = g_broker_forward_target;
    if (t > 1) {
        kill(-t, SIGTERM);
    }
}

/* ---- broker context: every fd/path the broker owns, one cleanup path ---- */

struct broker {
    const struct hold_store *store;
    const char *run_id;
    const char *sock_path; /* NULL/"" = nothing to unlink */
    int parent_pipe;
    int listener; /* -1 in the adopted log-only fallback: no clients */
    int master;
    int logfd;
    int logidxfd;
    pid_t target; /* >0: our held child (waited, SIGKILLed on cleanup) */
    uid_t owner_uid;
    bool have_allowed_peer_uid;
    uid_t allowed_peer_uid;
    pid_t adopted_pgid; /* adopted mode: group/session liveness probe */
    pid_t adopted_sid;
    pid_t hup_pid; /* adopted mode: HUP the wrapper shell on exit */
};

static void broker_cleanup_and_exit(struct broker *b, int exit_code) {
    if (b->target > 0) {
        int st = 0;
        kill(b->target, SIGKILL);
        while (waitpid(b->target, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
    if (b->parent_pipe >= 0) close(b->parent_pipe);
    if (b->listener >= 0) close(b->listener);
    if (b->master >= 0) close(b->master);
    if (b->logidxfd >= 0) close(b->logidxfd);
    if (b->logfd >= 0) close(b->logfd);
    if (b->sock_path && *b->sock_path) unlink(b->sock_path);
    _exit(exit_code);
}

static void broker_fail_errno(struct broker *b, int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)hold_write_all(b->parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(b, 127);
}

static int set_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

static bool authorize_client(const struct broker *b, int client) {
    uid_t peer_uid = (uid_t)-1;
    if (hold_console_peer_uid(client, &peer_uid) == 0 &&
        (peer_uid == 0 || peer_uid == b->owner_uid ||
         (b->have_allowed_peer_uid && peer_uid == b->allowed_peer_uid))) {
        return true;
    }
    static const char msg[] = "hold: console attach denied\n";
    (void)hold_write_all(client, msg, sizeof(msg) - 1);
    return false;
}

/* ---- 64 KiB replay ring: recent PTY output, replayed to a new client ---- */

struct replay_ring {
    unsigned char *data;
    size_t cap;
    size_t len;
    size_t start;
};

static void replay_init(struct replay_ring *r) {
    memset(r, 0, sizeof(*r));
    r->data = malloc(CONSOLE_REPLAY_LIMIT);
    if (r->data) {
        r->cap = CONSOLE_REPLAY_LIMIT;
    }
}

static void replay_append(struct replay_ring *r, const void *buf, size_t n) {
    const unsigned char *p = buf;
    if (!r->data) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (r->len < r->cap) {
            r->data[(r->start + r->len) % r->cap] = p[i];
            r->len++;
        } else {
            r->data[r->start] = p[i];
            r->start = (r->start + 1) % r->cap;
        }
    }
}

static int replay_write(const struct replay_ring *r, int fd) {
    if (!r->data || r->len == 0) {
        return 0;
    }
    size_t first = r->cap - r->start;
    if (first > r->len) {
        first = r->len;
    }
    if (hold_write_all(fd, r->data + r->start, first) != 0) {
        return -1;
    }
    if (r->len > first && hold_write_all(fd, r->data, r->len - first) != 0) {
        return -1;
    }
    return 0;
}

/* ---- client input: magic sniff, then 3-byte frames or raw passthrough ---- */

struct client {
    int fd;            /* -1 = no client attached */
    bool input_closed; /* client half-closed its write side */
    bool decided;
    bool framed;
    unsigned char pending[16384];
    size_t pending_len;
};

static void drop_client(struct client *c) {
    if (c->fd >= 0) {
        close(c->fd);
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static int apply_pty_size(int master, const unsigned char *payload, size_t len) {
    if (len != 4) {
        errno = EPROTO;
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = console_load_be16(payload);
    ws.ws_col = console_load_be16(payload + 2);
    if (ws.ws_row == 0 || ws.ws_col == 0) {
        return 0;
    }
    return ioctl(master, TIOCSWINSZ, &ws);
}

static int process_framed(struct client *c, int master) {
    while (c->pending_len >= CONSOLE_FRAME_HEADER_LEN) {
        unsigned char type = c->pending[0];
        uint16_t len = console_load_be16(c->pending + 1);
        size_t frame_len = CONSOLE_FRAME_HEADER_LEN + (size_t)len;
        if (c->pending_len < frame_len) {
            return 0;
        }
        const unsigned char *payload = c->pending + CONSOLE_FRAME_HEADER_LEN;
        if (type == CONSOLE_FRAME_DATA) {
            if (len > 0 && hold_write_all(master, payload, len) != 0) {
                return -1;
            }
        } else if (type == CONSOLE_FRAME_RESIZE) {
            (void)apply_pty_size(master, payload, len);
        } else if (type == CONSOLE_FRAME_DETACH) {
            return 1;
        }
        memmove(c->pending, c->pending + frame_len, c->pending_len - frame_len);
        c->pending_len -= frame_len;
    }
    return 0;
}

/* Until CONSOLE_ATTACH_MAGIC_LEN bytes decide the mode, input is buffered; a
 * magic prefix selects framed mode, anything else replays as raw passthrough.
 * Returns nonzero to drop the client (1 = clean detach frame). */
static int process_client_input(struct client *c, int master, const unsigned char *buf, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (c->decided && !c->framed) {
        return hold_write_all(master, buf, n);
    }
    if (c->pending_len + n > sizeof(c->pending)) {
        if (c->decided) {
            errno = EOVERFLOW;
            return -1;
        }
        /* Overflow before a full magic: the client is raw. */
        c->decided = true;
        if (c->pending_len > 0 && hold_write_all(master, c->pending, c->pending_len) != 0) {
            return -1;
        }
        c->pending_len = 0;
        return hold_write_all(master, buf, n);
    }
    memcpy(c->pending + c->pending_len, buf, n);
    c->pending_len += n;
    if (!c->decided) {
        size_t cmp = c->pending_len < CONSOLE_ATTACH_MAGIC_LEN ? c->pending_len : CONSOLE_ATTACH_MAGIC_LEN;
        if (memcmp(c->pending, CONSOLE_ATTACH_MAGIC, cmp) != 0) {
            c->decided = true;
            if (hold_write_all(master, c->pending, c->pending_len) != 0) {
                return -1;
            }
            c->pending_len = 0;
            return 0;
        }
        if (c->pending_len < CONSOLE_ATTACH_MAGIC_LEN) {
            return 0;
        }
        c->decided = true;
        c->framed = true;
        c->pending_len -= CONSOLE_ATTACH_MAGIC_LEN;
        memmove(c->pending, c->pending + CONSOLE_ATTACH_MAGIC_LEN, c->pending_len);
    }
    return process_framed(c, master);
}

/* ---- the one serve loop ------------------------------------------------- */

/* b->target > 0 means the target is our child (waitpid lifecycle, killed on
 * cleanup); otherwise the target is an adopted foreground group we must never
 * kill, whose exit shows as PTY EOF or dead group/session. b->listener < 0 is
 * the adopted log-only fallback: same loop, no clients. Never returns. */
static void broker_serve(struct broker *b) {
    bool adopted = b->target <= 0;
    /* The serve loop always ends in _exit, so no handler save/restore. */
    signal(SIGPIPE, SIG_IGN);

    struct client cl;
    memset(&cl, 0, sizeof(cl));
    cl.fd = -1;
    struct replay_ring replay;
    replay_init(&replay);
    bool target_done = false;
    bool target_marked = false;
    int target_status = 0;
    while (1) {
        if (!adopted && !target_done) {
            int st = 0;
            if (waitpid(b->target, &st, WNOHANG) == b->target) {
                target_done = true;
                target_status = st;
                if (b->store && b->run_id && *b->run_id) {
                    /* Best-effort while serving: an instantly-exiting target
                     * can beat the parent's record write, so a failure here
                     * is retried patiently after the serve loop ends. */
                    target_marked = hold_mark_run_finished(b->store, b->run_id, target_status) == 0;
                }
                b->target = -1;
                g_broker_forward_target = 0;
            }
        }
        if (adopted && !target_done && b->adopted_pgid > 1 && b->adopted_sid > 0 &&
            hold_group_session_liveness(b->adopted_pgid, b->adopted_sid) != GROUP_LIVE) {
            target_done = true;
        }

        struct pollfd pfds[3];
        nfds_t nfds = 0;
        pfds[nfds++] = (struct pollfd){.fd = b->master, .events = POLLIN};
        nfds_t listener_idx = 0;
        if (b->listener >= 0) {
            listener_idx = nfds;
            pfds[nfds++] = (struct pollfd){.fd = b->listener, .events = POLLIN};
        }
        nfds_t client_idx = 0;
        bool poll_client = cl.fd >= 0 && !cl.input_closed;
        if (poll_client) {
            client_idx = nfds;
            pfds[nfds++] = (struct pollfd){.fd = cl.fd, .events = POLLIN};
        }

        int pr = poll(pfds, nfds, adopted ? 200 : 1000);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            if (target_done) {
                break;
            }
            continue;
        }

        short master_events = pfds[0].revents;
        short client_events = poll_client ? pfds[client_idx].revents : 0;

        if (b->listener >= 0 && (pfds[listener_idx].revents & POLLIN)) {
            int next = accept(b->listener, NULL, NULL);
            if (next >= 0) {
                if (set_fd_nonblocking(next) != 0 || !authorize_client(b, next)) {
                    close(next);
                } else if (cl.fd >= 0) {
                    static const char msg[] = "hold: console already attached\n";
                    (void)hold_write_all(next, msg, sizeof(msg) - 1);
                    close(next);
                } else if (replay_write(&replay, next) != 0) {
                    close(next);
                } else {
                    memset(&cl, 0, sizeof(cl));
                    cl.fd = next;
                }
            }
        }
        if (cl.fd >= 0 && !cl.input_closed &&
            (client_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(cl.fd, buf, sizeof(buf));
            if (n > 0) {
                if (process_client_input(&cl, b->master, buf, (size_t)n) != 0) {
                    drop_client(&cl);
                }
            } else if (n == 0) {
                if (!cl.decided && cl.pending_len > 0) {
                    (void)hold_write_all(b->master, cl.pending, cl.pending_len);
                    cl.pending_len = 0;
                }
                cl.input_closed = true;
            } else {
                drop_client(&cl);
            }
        }
        if (master_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            char buf[4096];
            ssize_t n = hold_term_pump_master(b->master, b->logfd, b->logidxfd, buf, sizeof(buf));
            if (n > 0) {
                replay_append(&replay, buf, (size_t)n);
                if (cl.fd >= 0 && hold_write_all(cl.fd, buf, (size_t)n) != 0) {
                    drop_client(&cl);
                }
            } else if (n == 0) {
                break;
            }
        }
    }

    if (!adopted && !target_done && b->target > 0) {
        int st = 0;
        pid_t got;
        do {
            got = waitpid(b->target, &st, 0);
        } while (got < 0 && errno == EINTR);
        if (got == b->target) {
            target_done = true;
            target_status = st;
        }
        if (got == b->target || (got < 0 && errno == ECHILD)) {
            b->target = -1;
            g_broker_forward_target = 0;
        }
    }
    if (!adopted && target_done && !target_marked && b->store && b->run_id && *b->run_id) {
        /* The exit stamp must not be lost to the launch race: retry until the
         * parent's record exists. rc 1 means the record was purged while the
         * call was exiting — that removal is final, do not resurrect it. */
        for (int i = 0; i < 50; i++) {
            int mark_rc = hold_mark_run_finished(b->store, b->run_id, target_status);
            if (mark_rc == 0 || mark_rc == 1) break;
            struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
            while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
                continue;
            }
        }
    }
    if (adopted && b->hup_pid > 0) {
        kill(b->hup_pid, SIGHUP);
    }
    if (cl.fd >= 0) close(cl.fd);
    broker_cleanup_and_exit(b, 0);
}

/* ---- entry points ------------------------------------------------------- */

void hold_run_console_broker(int parent_pipe,
                               int target_pid_fd,
                               const struct hold_store *store,
                               const char *run_id,
                               const char *log_path,
                               const char *sock_path,
                               uid_t owner_uid,
                               bool have_allowed_peer_uid,
                               uid_t allowed_peer_uid,
                               int argc,
                               char **argv,
                               const char *exec_path,
                               unsigned short init_rows,
                               unsigned short init_cols) {
    struct broker b = {
        .store = store,
        .run_id = run_id,
        .sock_path = sock_path,
        .parent_pipe = parent_pipe,
        .listener = -1,
        .master = -1,
        .logfd = -1,
        .logidxfd = -1,
        .target = -1,
        .owner_uid = owner_uid,
        .have_allowed_peer_uid = have_allowed_peer_uid,
        .allowed_peer_uid = allowed_peer_uid,
        .hup_pid = -1,
    };
    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(&b, EINVAL);
    }
    b.listener = hold_make_console_listener(sock_path);
    if (b.listener < 0) {
        broker_fail_errno(&b, errno);
    }
    b.logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    b.logidxfd = b.logfd >= 0 ? hold_open_log_index_fd(log_path, b.logfd) : -1;
    if (b.logfd < 0) {
        broker_fail_errno(&b, errno);
    }

    /* The one spawn engine puts the target on a fresh PTY (setsid + TIOCSCTTY
     * + dup2 x3 + exec behind the errno handshake). On failure nothing is
     * left open or unreaped, and the errno rides our own handshake pipe up to
     * the launching parent. The broker's listener/log fds are all CLOEXEC, so
     * the target never inherits them past exec. */
    struct hold_term_spawn spawn = {
        .argv = argv,
        .exec_path = exec_path,
        .rows = init_rows,
        .cols = init_cols,
    };
    if (hold_term_pty_spawn(&spawn, &b.master, &b.target) != 0) {
        broker_fail_errno(&b, errno);
    }
    /* Report the real held group to the parent before releasing the handshake:
     * a failed write must still ride the handshake pipe as an errno, and the
     * write-before-close ordering keeps the parent's pid read deadlock-free. */
    if (target_pid_fd >= 0) {
        if (hold_write_all(target_pid_fd, &b.target, sizeof(b.target)) != 0) {
            broker_fail_errno(&b, errno);
        }
        close(target_pid_fd);
    }
    close(parent_pipe);
    b.parent_pipe = -1;

    /* Forward a TERM aimed at the broker to the held group (no SA_RESTART so
     * the serve loop's poll/waitpid see EINTR). The handler is async-signal-
     * safe and also unblocks a TERM arriving during the final blocking waitpid. */
    g_broker_forward_target = b.target;
    struct sigaction term_sa;
    memset(&term_sa, 0, sizeof(term_sa));
    term_sa.sa_handler = broker_forward_term;
    sigemptyset(&term_sa.sa_mask);
    (void)sigaction(SIGTERM, &term_sa, NULL);

    broker_serve(&b);
}

pid_t hold_spawn_adopted_console_server(const struct hold_store *store,
                                          const char *run_id,
                                          const char *log_path,
                                          int master,
                                          pid_t adopted_pgid,
                                          pid_t adopted_sid,
                                          pid_t hup_pid,
                                          char *console_sock_out,
                                          size_t console_sock_n) {
    /* Prefer serving the adopted PTY through a broker so the run stays
     * reattachable; every fd is opened here so the child has no failure path.
     * If listener setup fails the same serve loop runs as a log-only capture
     * (listener -1), and a failed log open forks a child that just exits. */
    char console_sock[HOLD_PATH_MAX];
    console_sock[0] = '\0';
    struct broker b = {
        .store = store,
        .run_id = run_id,
        .sock_path = console_sock,
        .parent_pipe = -1,
        .listener = -1,
        .master = master,
        .logfd = -1,
        .logidxfd = -1,
        .target = -1,
        .owner_uid = geteuid(),
        .adopted_pgid = adopted_pgid,
        .adopted_sid = adopted_sid,
        .hup_pid = hup_pid,
    };
    b.logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    b.logidxfd = b.logfd >= 0 ? hold_open_log_index_fd(log_path, b.logfd) : -1;
    if (b.logfd >= 0 &&
        hold_format_console_sock_path(store, run_id, console_sock, sizeof(console_sock)) == 0) {
        b.listener = hold_make_console_listener(console_sock);
    }
    if (b.listener < 0) {
        console_sock[0] = '\0';
    }

    pid_t server = fork();
    if (server < 0) {
        int saved = errno;
        if (b.listener >= 0) close(b.listener);
        if (b.logidxfd >= 0) close(b.logidxfd);
        if (b.logfd >= 0) close(b.logfd);
        if (console_sock[0]) unlink(console_sock);
        errno = saved;
        return -1;
    }
    if (server == 0) {
        signal(SIGHUP, SIG_IGN);
        hold_close_stdio_to_devnull();
        if (b.logfd < 0) _exit(1);
        broker_serve(&b); /* never returns */
    }
    if (b.listener >= 0) close(b.listener);
    if (b.logidxfd >= 0) close(b.logidxfd);
    if (b.logfd >= 0) close(b.logfd);
    if (hold_checked_snprintf(console_sock_out, console_sock_n, "%s", console_sock) != 0) {
        console_sock_out[0] = '\0';
    }
    return server;
}
