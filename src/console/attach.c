#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/console_internal.h"


static int64_t hold_console_now_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}

static int hold_console_forward_pending_ctrl_p(int sock, bool *pending_ctrl_p) {
    if (!*pending_ctrl_p) return 0;
    unsigned char c = CONSOLE_ATTACH_CTRL_P;
    if (hold_write_console_frame(sock, CONSOLE_FRAME_DATA, &c, 1) != 0) return -1;
    *pending_ctrl_p = false;
    return 0;
}

int hold_run_native_console(const char *sock_path) {
    int sock = hold_connect_console_socket(sock_path);
    if (sock < 0) {
        return errno == ENOTSOCK || errno == ENAMETOOLONG ? 5 : 3;
    }

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool terminal_saved = false;
    bool alt_screen = false;
    struct termios old_termios;
    if (interactive && tcgetattr(STDIN_FILENO, &old_termios) == 0) {
        struct termios raw;
        hold_make_raw_termios(&old_termios, &raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            terminal_saved = true;
            if (hold_write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
                alt_screen = true;
            }
        }
    }

    struct sigaction sa, old_winch;
    bool have_old_winch = false;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = hold_handle_console_sigwinch;
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
    bool pending_ctrl_p = false;
    int64_t pending_ctrl_p_deadline = 0;
    if (hold_write_all(sock, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        rc = 3;
        goto out;
    }

    while (1) {
        if (g_console_resized) {
            struct winsize ws;
            g_console_resized = 0;
            if (hold_maybe_get_terminal_size(&ws) == 0 && hold_send_console_resize(sock, &ws) != 0) {
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

        struct timeval tv;
        struct timeval *tvp = NULL;
        if (pending_ctrl_p) {
            int64_t now = hold_console_now_usec();
            int64_t remaining = pending_ctrl_p_deadline > now ? pending_ctrl_p_deadline - now : 0;
            tv.tv_sec = (time_t)(remaining / 1000000);
            tv.tv_usec = (suseconds_t)(remaining % 1000000);
            tvp = &tv;
        }
        int sr = select(maxfd + 1, &rfds, NULL, NULL, tvp);
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 3;
            break;
        }
        if (pending_ctrl_p && (sr == 0 || hold_console_now_usec() >= pending_ctrl_p_deadline)) {
            if (hold_console_forward_pending_ctrl_p(sock, &pending_ctrl_p) != 0) {
                rc = 3;
                break;
            }
            if (sr == 0) continue;
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (interactive) {
                    for (ssize_t i = 0; i < n; i++) {
                        if (pending_ctrl_p) {
                            if (buf[i] == CONSOLE_ATTACH_CTRL_Q) {
                                if (hold_write_console_frame(sock, CONSOLE_FRAME_DETACH, NULL, 0) != 0) {
                                    rc = 3;
                                }
                                goto out;
                            }
                            if (hold_console_forward_pending_ctrl_p(sock, &pending_ctrl_p) != 0) {
                                rc = 3;
                                break;
                            }
                        }
                        if (buf[i] == CONSOLE_ATTACH_CTRL_P) {
                            pending_ctrl_p = true;
                            pending_ctrl_p_deadline = hold_console_now_usec() + CONSOLE_ATTACH_DETACH_TIMEOUT_USEC;
                            continue;
                        }
                        if (hold_write_console_frame(sock, CONSOLE_FRAME_DATA, buf + i, 1) != 0) {
                            rc = 3;
                            break;
                        }
                    }
                    if (rc != 0) {
                        break;
                    }
                } else if (hold_write_console_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                if (hold_console_forward_pending_ctrl_p(sock, &pending_ctrl_p) != 0) {
                    rc = 3;
                    break;
                }
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
                if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
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
        (void)hold_write_all(STDOUT_FILENO, "\033[?1049l", 8);
    }
    if (terminal_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    }
    close(sock);
    return rc;
}
