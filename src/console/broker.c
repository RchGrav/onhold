#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/console_internal.h"

#include <poll.h>

/* Set by the SIGWINCH handler below; read by run_native_console in attach.c. */
volatile sig_atomic_t g_console_resized = 0;

static void broker_cleanup_and_exit(int parent_pipe,
                                    const char *sock_path,
                                    int listener,
                                    int master,
                                    int slave,
                                    int logfd,
                                    int logidxfd,
                                    pid_t target,
                                    int exit_code);
static void broker_fail_errno(int parent_pipe,
                              const char *sock_path,
                              int listener,
                              int master,
                              int slave,
                              int logfd,
                              int logidxfd,
                              pid_t target,
                              int err);
static bool console_peer_uid_allowed(uid_t peer_uid,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid);
static bool authorize_console_client(int client,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid);
static int set_fd_nonblocking(int fd);
static void broker_serve(const struct hold_store *store,
                         const char *run_id,
                         const char *sock_path,
                         int listener,
                         int master,
                         int logfd,
                         int logidxfd,
                         uid_t owner_uid,
                         bool have_allowed_peer_uid,
                         uid_t allowed_peer_uid,
                         pid_t child_target,
                         pid_t adopted_pgid,
                         pid_t adopted_sid,
                         pid_t hup_pid);

void hold_handle_console_sigwinch(int signo) {
    (void)signo;
    g_console_resized = 1;
}

static void broker_cleanup_and_exit(int parent_pipe,
                                    const char *sock_path,
                                    int listener,
                                    int master,
                                    int slave,
                                    int logfd,
                                    int logidxfd,
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
    if (logidxfd >= 0) close(logidxfd);
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
                              int logidxfd,
                              pid_t target,
                              int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)hold_write_all(parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, 127);
}

static int set_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

static bool console_peer_uid_allowed(uid_t peer_uid,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid) {
    return peer_uid == 0 || peer_uid == owner_uid || (have_allowed_peer_uid && peer_uid == allowed_peer_uid);
}

static bool authorize_console_client(int client,
                                     uid_t owner_uid,
                                     bool have_allowed_peer_uid,
                                     uid_t allowed_peer_uid) {
    uid_t peer_uid = (uid_t)-1;
    if (hold_console_peer_uid(client, &peer_uid) != 0 ||
        !console_peer_uid_allowed(peer_uid, owner_uid, have_allowed_peer_uid, allowed_peer_uid)) {
        static const char msg[] = "hold: console attach denied\n";
        (void)hold_write_all(client, msg, sizeof(msg) - 1);
        return false;
    }
    return true;
}

void hold_run_console_broker(int parent_pipe,
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
    int listener = -1;
    int master = -1;
    int slave = -1;
    int logfd = -1;
    int logidxfd = -1;
    pid_t target = -1;

    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, EINVAL);
    }

    listener = hold_make_console_listener(sock_path);
    if (listener < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, errno);
    }
    logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    logidxfd = logfd >= 0 ? hold_open_log_index_fd(log_path, logfd) : -1;
    if (logfd < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, errno);
    }
    if (hold_open_console_pty(&master, &slave, init_rows, init_cols) != 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, errno);
    }

    int exec_pipe[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(exec_pipe, O_CLOEXEC) != 0)
#endif
    {
        /* Non-Linux builds set FD_CLOEXEC after pipe(); unlike pipe2(O_CLOEXEC),
         * that is not atomic with respect to concurrent fork/exec in a
         * multi-threaded process. The broker reaches this path before it starts
         * any broker-owned threads, so this fallback is acceptable here. */
        if (pipe(exec_pipe) != 0) {
            broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, errno);
        }
        if (fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(exec_pipe[0]);
            close(exec_pipe[1]);
            broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, saved);
        }
    }

    target = fork();
    if (target < 0) {
        int saved = errno;
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, saved);
    }
    if (target == 0) {
        close(exec_pipe[0]);
        close(listener);
        close(master);
        close(logfd);
        /* Become a session leader and claim the PTY slave as our controlling
         * terminal. This scopes terminal-generated signals and the foreground
         * process group to the target (so Ctrl-C interrupts the child instead of
         * killing the broker) and lets interactive shells run job control. */
        if (setsid() < 0) {
            int e = errno;
            (void)hold_write_all(exec_pipe[1], &e, sizeof(e));
            _exit(127);
        }
#ifdef TIOCSCTTY
        if (ioctl(slave, TIOCSCTTY, 0) != 0) {
            int e = errno;
            (void)hold_write_all(exec_pipe[1], &e, sizeof(e));
            _exit(127);
        }
#endif
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            int e = errno;
            (void)hold_write_all(exec_pipe[1], &e, sizeof(e));
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
        (void)hold_write_all(exec_pipe[1], &e, sizeof(e));
        _exit(127);
    }

    close(exec_pipe[1]);
    int child_errno = 0;
    int handshake = hold_read_exec_handshake(exec_pipe[0], &child_errno);
    int handshake_errno = errno;
    close(exec_pipe[0]);
    close(slave);
    slave = -1;
    if (handshake < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, handshake_errno);
    }
    if (handshake > 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, child_errno);
    }
    close(parent_pipe);
    parent_pipe = -1;

    broker_serve(store, run_id, sock_path, listener, master, logfd, logidxfd,
                 owner_uid, have_allowed_peer_uid, allowed_peer_uid,
                 target, 0, 0, -1);
}

void hold_run_console_broker_adopted(const struct hold_store *store,
                                       const char *run_id,
                                       const char *sock_path,
                                       int listener,
                                       int master,
                                       int logfd,
                                       int logidxfd,
                                       uid_t owner_uid,
                                       pid_t adopted_pgid,
                                       pid_t adopted_sid,
                                       pid_t hup_pid) {
    broker_serve(store, run_id, sock_path, listener, master, logfd, logidxfd,
                 owner_uid, false, 0, -1, adopted_pgid, adopted_sid, hup_pid);
}

/* Shared serve loop. child_target > 0 means the target is our child (waitpid
 * lifecycle, killed on cleanup); otherwise the target is an adopted foreground
 * group we must never kill, whose exit shows as PTY EOF or dead group/session. */
static void broker_serve(const struct hold_store *store,
                         const char *run_id,
                         const char *sock_path,
                         int listener,
                         int master,
                         int logfd,
                         int logidxfd,
                         uid_t owner_uid,
                         bool have_allowed_peer_uid,
                         uid_t allowed_peer_uid,
                         pid_t child_target,
                         pid_t adopted_pgid,
                         pid_t adopted_sid,
                         pid_t hup_pid) {
    pid_t target = child_target;
    bool adopted = child_target <= 0;

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
    hold_console_replay_init(&replay);
    bool target_done = false;
    bool target_marked = false;
    int target_status = 0;
    while (1) {
        if (!adopted && !target_done) {
            int st = 0;
            pid_t got = waitpid(target, &st, WNOHANG);
            if (got == target) {
                target_done = true;
                target_status = st;
                if (store && run_id && *run_id) {
                    /* Best-effort while serving: an instantly-exiting target
                     * can beat the parent's record write, so a failure here
                     * is retried patiently after the serve loop ends. */
                    target_marked = hold_mark_run_finished(store, run_id, target_status) == 0;
                }
                target = -1;
            }
        }
        if (adopted && !target_done && adopted_pgid > 1 && adopted_sid > 0 &&
            hold_group_session_liveness(adopted_pgid, adopted_sid) != GROUP_LIVE) {
            target_done = true;
        }

        struct pollfd pfds[3];
        nfds_t nfds = 0;
        nfds_t master_idx = nfds;
        pfds[nfds++] = (struct pollfd){.fd = master, .events = POLLIN};
        nfds_t listener_idx = nfds;
        pfds[nfds++] = (struct pollfd){.fd = listener, .events = POLLIN};
        nfds_t client_idx = 0;
        bool poll_client = client >= 0 && !client_input_closed;
        if (poll_client) {
            client_idx = nfds;
            pfds[nfds++] = (struct pollfd){.fd = client, .events = POLLIN};
        }

        int pr = poll(pfds, nfds, 1000);
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

        short listener_events = pfds[listener_idx].revents;
        short client_events = poll_client ? pfds[client_idx].revents : 0;
        short master_events = pfds[master_idx].revents;

        if (listener_events & POLLIN) {
            int next = accept(listener, NULL, NULL);
            if (next >= 0) {
                if (set_fd_nonblocking(next) != 0) {
                    close(next);
                } else if (!authorize_console_client(next, owner_uid, have_allowed_peer_uid, allowed_peer_uid)) {
                    close(next);
                } else if (client >= 0) {
                    static const char msg[] = "hold: console already attached\n";
                    (void)hold_write_all(next, msg, sizeof(msg) - 1);
                    close(next);
                } else if (hold_console_replay_write(&replay, next) != 0) {
                    close(next);
                } else {
                    client = next;
                    client_input_closed = false;
                    memset(&client_state, 0, sizeof(client_state));
                }
            }
        }
        if (client >= 0 && !client_input_closed &&
            (client_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            unsigned char buf[4096];
            ssize_t n = read(client, buf, sizeof(buf));
            if (n > 0) {
                int input_rc = hold_broker_process_client_input(&client_state, master, buf, (size_t)n);
                if (input_rc != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                    memset(&client_state, 0, sizeof(client_state));
                }
            } else if (n == 0) {
                if (!client_state.decided && client_state.pending_len > 0) {
                    (void)hold_write_all(master, client_state.pending, client_state.pending_len);
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
        if (master_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            char buf[4096];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                (void)hold_write_indexed_log_bytes_fd(logfd, logidxfd, "stdout", buf, (size_t)n);
                hold_console_replay_append(&replay, buf, (size_t)n);
                if (client >= 0 && hold_write_all(client, buf, (size_t)n) != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                }
            } else if (n == 0 || errno == EIO) {
                break;
            }
        }
    }

    if (!adopted && !target_done && target > 0) {
        int st = 0;
        pid_t got;
        do {
            got = waitpid(target, &st, 0);
        } while (got < 0 && errno == EINTR);
        if (got == target) {
            target_done = true;
            target_status = st;
            target = -1;
        } else if (got < 0 && errno == ECHILD) {
            target = -1;
        }
    }
    if (!adopted && target_done && !target_marked && store && run_id && *run_id) {
        /* The exit stamp must not be lost to the launch race: retry until the
         * parent's record exists. rc 1 means the record was purged while the
         * call was exiting — that removal is final, do not resurrect it. */
        for (int i = 0; i < 50; i++) {
            int mark_rc = hold_mark_run_finished(store, run_id, target_status);
            if (mark_rc == 0 || mark_rc == 1) break;
            struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
            while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
                continue;
            }
        }
    }
    if (adopted && hup_pid > 0) {
        kill(hup_pid, SIGHUP);
    }

    if (client >= 0) close(client);
    hold_console_replay_free(&replay);
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    broker_cleanup_and_exit(-1, sock_path, listener, master, -1, logfd, logidxfd, adopted ? -1 : target, 0);
}
