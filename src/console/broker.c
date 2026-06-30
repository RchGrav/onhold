#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/console_internal.h"

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
    logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
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
    int target_status = 0;
    while (1) {
        if (!target_done) {
            int st = 0;
            pid_t got = waitpid(target, &st, WNOHANG);
            if (got == target) {
                target_done = true;
                target_status = st;
                if (store && run_id && *run_id) {
                    (void)hold_mark_run_finished(store, run_id, target_status);
                }
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
                if (!authorize_console_client(next, owner_uid, have_allowed_peer_uid, allowed_peer_uid)) {
                    close(next);
                } else if (client >= 0) {
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
        if (client >= 0 && !client_input_closed && FD_ISSET(client, &rfds)) {
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
        if (FD_ISSET(master, &rfds)) {
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

    if (!target_done && target > 0) {
        int st = 0;
        pid_t got;
        do {
            got = waitpid(target, &st, 0);
        } while (got < 0 && errno == EINTR);
        if (got == target) {
            target_done = true;
            target_status = st;
            if (store && run_id && *run_id) {
                (void)hold_mark_run_finished(store, run_id, target_status);
            }
            target = -1;
        } else if (got < 0 && errno == ECHILD) {
            target = -1;
        }
    }

    if (client >= 0) close(client);
    hold_console_replay_free(&replay);
    if (have_old_pipe) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, slave, logfd, logidxfd, target, 0);
}
