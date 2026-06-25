#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/core.h"

struct shell_raw_terminal {
    struct termios original;
    bool active;
};

static const char *resolve_user_shell(void) {
    const char *shell = getenv("SHELL");
    if (shell && *shell) return shell;
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_shell && *pw->pw_shell) return pw->pw_shell;
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

static int open_pty_master(char *slave_path, size_t slave_path_n) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    char *name = ptsname(master);
    if (!name || hold_checked_snprintf(slave_path, slave_path_n, "%s", name) != 0) {
        int saved = errno ? errno : ENAMETOOLONG;
        close(master);
        errno = saved;
        return -1;
    }
    return master;
}

static void apply_window_size(int master) {
    if (!isatty(STDOUT_FILENO)) return;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        ioctl(master, TIOCSWINSZ, &ws);
    }
}

static int spawn_shell_child(int master, const char *slave_path, const char *shell) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        close(master);
        if (setsid() < 0) _exit(127);
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) _exit(127);
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (slave > STDERR_FILENO) close(slave);
        execl(shell, shell, (char *)NULL);
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    return child;
}

static int relay_shell_pty(int master, pid_t child, bool *detached) {
    bool stdin_open = true;
    bool pending_ctrl_p = false;
    *detached = false;
    while (1) {
        int status = 0;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 0;
        }
        if (w < 0 && errno != EINTR) return 1;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master, &rfds);
        int maxfd = master;
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = pending_ctrl_p ? 500000 : 100000;
        int ready;
        do {
            ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
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
        if (FD_ISSET(master, &rfds)) {
            char buf[4096];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                if (hold_write_all(STDOUT_FILENO, buf, (size_t)n) != 0) return 1;
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EIO)) {
                return 0;
            }
        }
        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
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
    (void)inv;
    (void)store;
    char slave_path[HOLD_PATH_MAX];
    const char *shell = resolve_user_shell();
    int master = open_pty_master(slave_path, sizeof(slave_path));
    if (master < 0) {
        hold_die_errno("hold: failed to allocate shell PTY");
    }
    apply_window_size(master);
    pid_t child = spawn_shell_child(master, slave_path, shell);
    if (child < 0) {
        int saved = errno;
        close(master);
        errno = saved;
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
    if (detached) {
        fprintf(stderr, "hold: shell detach capture is not complete yet; leaving shell process group is unsafe in this build\n");
        kill(child, SIGHUP);
        close(master);
        return 5;
    }
    close(master);
    return rc;
}
