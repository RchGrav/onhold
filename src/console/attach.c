#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/console_internal.h"

#define HOLD_CONSOLE_MAX_DETACH_KEYS 8

static unsigned char g_detach_keys[HOLD_CONSOLE_MAX_DETACH_KEYS] = {
    CONSOLE_ATTACH_CTRL_P,
    CONSOLE_ATTACH_CTRL_Q,
};
static size_t g_detach_key_count = 2;

static int64_t hold_console_now_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}

int hold_console_set_detach_keys(const unsigned char *keys, size_t len) {
    if (!keys || len == 0 || len > HOLD_CONSOLE_MAX_DETACH_KEYS) {
        errno = EINVAL;
        return -1;
    }
    memcpy(g_detach_keys, keys, len);
    g_detach_key_count = len;
    return 0;
}

static bool detach_prefix_matches(const unsigned char *pending, size_t pending_len) {
    return pending_len <= g_detach_key_count &&
           memcmp(pending, g_detach_keys, pending_len) == 0;
}

static bool detach_sequence_matches(const unsigned char *pending, size_t pending_len) {
    return pending_len == g_detach_key_count &&
           memcmp(pending, g_detach_keys, pending_len) == 0;
}

static int hold_console_forward_pending(int sock, unsigned char *pending, size_t *pending_len) {
    if (*pending_len == 0) return 0;
    if (hold_write_console_frame(sock, CONSOLE_FRAME_DATA, pending, (uint16_t)*pending_len) != 0) return -1;
    *pending_len = 0;
    return 0;
}

static int hold_console_process_interactive_byte(int sock,
                                                 unsigned char c,
                                                 unsigned char *pending,
                                                 size_t *pending_len,
                                                 int64_t *pending_deadline,
                                                 bool *detached) {
    *detached = false;
retry_current:
    if (*pending_len > 0) {
        if (*pending_len >= HOLD_CONSOLE_MAX_DETACH_KEYS) {
            if (hold_console_forward_pending(sock, pending, pending_len) != 0) return -1;
            goto retry_current;
        }
        pending[(*pending_len)++] = c;
        if (detach_sequence_matches(pending, *pending_len)) {
            if (hold_write_console_frame(sock, CONSOLE_FRAME_DETACH, NULL, 0) != 0) return -1;
            *pending_len = 0;
            *detached = true;
            return 0;
        }
        if (detach_prefix_matches(pending, *pending_len)) {
            *pending_deadline = hold_console_now_usec() + CONSOLE_ATTACH_DETACH_TIMEOUT_USEC;
            return 0;
        }
        unsigned char current = pending[*pending_len - 1];
        (*pending_len)--;
        if (hold_console_forward_pending(sock, pending, pending_len) != 0) return -1;
        c = current;
        goto retry_current;
    }

    if (g_detach_key_count > 0 && c == g_detach_keys[0]) {
        pending[0] = c;
        *pending_len = 1;
        if (detach_sequence_matches(pending, *pending_len)) {
            if (hold_write_console_frame(sock, CONSOLE_FRAME_DETACH, NULL, 0) != 0) return -1;
            *pending_len = 0;
            *detached = true;
            return 0;
        }
        *pending_deadline = hold_console_now_usec() + CONSOLE_ATTACH_DETACH_TIMEOUT_USEC;
        return 0;
    }
    return hold_write_console_frame(sock, CONSOLE_FRAME_DATA, &c, 1);
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
    unsigned char pending_detach[HOLD_CONSOLE_MAX_DETACH_KEYS];
    size_t pending_detach_len = 0;
    int64_t pending_detach_deadline = 0;
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
        if (pending_detach_len > 0) {
            int64_t now = hold_console_now_usec();
            int64_t remaining = pending_detach_deadline > now ? pending_detach_deadline - now : 0;
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
        if (pending_detach_len > 0 && (sr == 0 || hold_console_now_usec() >= pending_detach_deadline)) {
            if (hold_console_forward_pending(sock, pending_detach, &pending_detach_len) != 0) {
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
                        bool detached = false;
                        if (hold_console_process_interactive_byte(sock,
                                                                  buf[i],
                                                                  pending_detach,
                                                                  &pending_detach_len,
                                                                  &pending_detach_deadline,
                                                                  &detached) != 0) {
                            rc = 3;
                            break;
                        }
                        if (detached) goto out;
                    }
                    if (rc != 0) {
                        break;
                    }
                } else if (hold_write_console_frame(sock, CONSOLE_FRAME_DATA, buf, (uint16_t)n) != 0) {
                    rc = 3;
                    break;
                }
            } else if (n == 0) {
                if (hold_console_forward_pending(sock, pending_detach, &pending_detach_len) != 0) {
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
    if (terminal_saved) {
        /* A program run inside the console may have left terminal-global modes
         * enabled -- mouse reporting, bracketed paste, application keypad/cursor
         * keys, a hidden cursor. These survive the PTY teardown and would leak
         * back to the user's shell (mouse selection emitting control bytes, paste
         * corruption, an invisible cursor -- i.e. an "unstable" terminal). Reset
         * the common offenders before handing the terminal back. */
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
