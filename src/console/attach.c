#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/term.h"
#include "hold/console_internal.h"

#define CONSOLE_ATTACH_CTRL_P 0x10
#define CONSOLE_ATTACH_CTRL_Q 0x11

static unsigned char g_detach_keys[HOLD_TERM_DETACH_MAX_KEYS] = {
    CONSOLE_ATTACH_CTRL_P,
    CONSOLE_ATTACH_CTRL_Q,
};
static size_t g_detach_key_count = 2;

static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int signo) {
    (void)signo;
    g_resized = 1;
}

int hold_console_set_detach_keys(const unsigned char *keys, size_t len) {
    if (!keys || len == 0 || len > HOLD_TERM_DETACH_MAX_KEYS) {
        errno = EINVAL;
        return -1;
    }
    memcpy(g_detach_keys, keys, len);
    g_detach_key_count = len;
    return 0;
}

/* Client-side wire: 3-byte type + be16-length frames after the attach magic. */
static int write_frame(int fd, unsigned char type, const void *payload, uint16_t len) {
    unsigned char header[CONSOLE_FRAME_HEADER_LEN];
    header[0] = type;
    console_store_be16(header + 1, len);
    if (hold_write_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (len > 0 && hold_write_all(fd, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int send_resize(int fd, const struct winsize *ws) {
    if (ws->ws_row == 0 || ws->ws_col == 0) {
        return 0;
    }
    unsigned char payload[4];
    console_store_be16(payload, ws->ws_row);
    console_store_be16(payload + 2, ws->ws_col);
    return write_frame(fd, CONSOLE_FRAME_RESIZE, payload, sizeof(payload));
}

static int terminal_size(struct winsize *ws) {
    memset(ws, 0, sizeof(*ws));
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, ws) == 0 && ws->ws_row > 0 && ws->ws_col > 0) {
        return 0;
    }
    return -1;
}

static bool install_handler(int sig, void (*fn)(int), struct sigaction *old) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, old) == 0;
}

/* Bytes the detach FSM releases go to the broker as DATA frames. */
static int attach_data_sink(void *ctx, const unsigned char *bytes, size_t n) {
    return write_frame(*(const int *)ctx, CONSOLE_FRAME_DATA, bytes, (uint16_t)n);
}

int hold_run_native_console(const char *sock_path) {
    int sock = hold_connect_console_socket(sock_path);
    if (sock < 0) {
        int e = errno;
        fprintf(stderr, "hold: cannot attach: %s\n", strerror(e));
        return e == ENOTSOCK || e == ENAMETOOLONG ? 5 : 3;
    }

    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool terminal_saved = false;
    bool alt_screen = false;
    struct termios old_termios;
    if (interactive && tcgetattr(STDIN_FILENO, &old_termios) == 0) {
        struct termios raw = old_termios;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
            terminal_saved = true;
            if (hold_write_all(STDOUT_FILENO, "\033[?1049h\033[H\033[2J", 15) == 0) {
                alt_screen = true;
            }
        }
    }

    struct sigaction old_winch, old_pipe;
    bool have_old_winch = install_handler(SIGWINCH, handle_sigwinch, &old_winch);
    bool have_old_pipe = install_handler(SIGPIPE, SIG_IGN, &old_pipe);
    g_resized = 1;

    int rc = 0;
    bool stdin_open = true;
    struct hold_term_detach detach;
    hold_term_detach_init(&detach, g_detach_keys, g_detach_key_count);
    if (hold_write_all(sock, CONSOLE_ATTACH_MAGIC, CONSOLE_ATTACH_MAGIC_LEN) != 0) {
        rc = 3;
        goto out;
    }

    while (1) {
        if (g_resized) {
            struct winsize ws;
            g_resized = 0;
            if (terminal_size(&ws) == 0 && send_resize(sock, &ws) != 0) {
                rc = 3;
                break;
            }
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        pfds[nfds++] = (struct pollfd){.fd = sock, .events = POLLIN};
        int stdin_idx = -1;
        if (stdin_open) {
            stdin_idx = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        }

        int sr = poll(pfds, nfds, hold_term_detach_timeout_ms(&detach));
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 3;
            break;
        }
        if (detach.pending_len > 0 && (sr == 0 || hold_term_detach_timeout_ms(&detach) == 0)) {
            if (hold_term_detach_flush(&detach, attach_data_sink, &sock) != 0) {
                rc = 3;
                break;
            }
            if (sr == 0) continue;
        }

        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (interactive) {
                    for (ssize_t i = 0; i < n; i++) {
                        bool detached = false;
                        if (hold_term_detach_feed(&detach, buf[i], attach_data_sink, &sock, &detached) != 0) {
                            rc = 3;
                            break;
                        }
                        if (detached) {
                            if (write_frame(sock, CONSOLE_FRAME_DETACH, NULL, 0) != 0) {
                                rc = 3;
                                break;
                            }
                            goto out;
                        }
                    }
                    if (rc != 0) {
                        break;
                    }
                } else if (write_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                if (hold_term_detach_flush(&detach, attach_data_sink, &sock) != 0) {
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

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
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
    if (terminal_saved) {
        /* A program run inside the console may have left terminal-global modes
         * enabled — mouse reporting, bracketed paste, application keypad/cursor
         * keys, a hidden cursor. These survive the PTY teardown and would leak
         * back to the user's shell, so reset the common offenders before
         * handing the terminal back. */
        static const char reset_modes[] =
            "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l"
            "\033[?2004l\033[?1l\033>\033[?25h";
        (void)hold_write_all(STDOUT_FILENO, reset_modes, sizeof(reset_modes) - 1);
    }
    if (alt_screen) {
        (void)hold_write_all(STDOUT_FILENO, "\033[?1049l", 8);
    }
    if (terminal_saved) {
        (void)hold_write_all(STDOUT_FILENO, "\033[?25h", 6);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    }
    close(sock);
    return rc;
}
