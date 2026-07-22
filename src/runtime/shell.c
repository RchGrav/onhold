#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/console.h"
#include "hold/term.h"

/* hold on/off: the guarded session shell. The PTY ride is term/spawn, the
 * Ctrl-P Ctrl-Q chord is term's detach FSM, the adopted group's identity
 * comes from platform's process snapshot, the adopted id/reserve/record ride
 * the store builders, and the broker-or-logger server is console's — this
 * file is session wiring. */

/* Set when a `hold off` running inside this session SIGTERMs the proxy so the
 * relay loop can unwind cleanly (restore the terminal, hang up the shell). */
static volatile sig_atomic_t g_hold_on_off_requested = 0;
static void hold_on_off_signal_handler(int sig) {
    (void)sig;
    g_hold_on_off_requested = 1;
}

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

/* The session shell rides the one spawn engine (term/spawn); if the user's
 * shell cannot be spawned the session falls back to /bin/sh, preserving the
 * old in-child exec ladder's behavior. */
static int spawn_shell_session(const char *shell,
                               unsigned short rows,
                               unsigned short cols,
                               int *master_out,
                               pid_t *child_out) {
    const char *exec_path[2] = {shell, "/bin/sh"};
    const char *argv0[2] = {shell, "sh"};
    for (int i = 0; i < 2; i++) {
        char *argv[2] = {(char *)argv0[i], NULL};
        struct hold_term_spawn spec = {
            .argv = argv,
            .exec_path = exec_path[i],
            .rows = rows,
            .cols = cols,
        };
        if (hold_term_pty_spawn(&spec, master_out, child_out) == 0) return 0;
    }
    return -1;
}

#if defined(__linux__)
static int adopt_fail(char **normalized_argv, char **observed_argv, int argc, int rc) {
    hold_free_argv_alloc(normalized_argv, argc);
    hold_free_argv_alloc(observed_argv, argc);
    return rc;
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
    if (hold_find_process_in_pgid(fg_pgid, &adopted_pid, &adopted_sid) != 0) {
        fprintf(stderr, "hold: failed to identify foreground process group %ld\n", (long)fg_pgid);
        return 5;
    }
    char **observed_argv = NULL;
    int argc = 0;
    if (hold_proc_read_cmdline(adopted_pid, &observed_argv, &argc) != 0) {
        fprintf(stderr, "hold: failed to read foreground process arguments\n");
        return 5;
    }
    char **normalized_argv = NULL;
    if (hold_copy_argv(&normalized_argv, argc, observed_argv) != 0) {
        return adopt_fail(NULL, observed_argv, argc, 3);
    }
    char exe_path[HOLD_PATH_MAX] = {0};
    if (hold_proc_entry_readlink(adopted_pid, "exe", exe_path, sizeof(exe_path)) == 0 && exe_path[0]) {
        free(normalized_argv[0]);
        normalized_argv[0] = strdup(exe_path);
        if (!normalized_argv[0]) {
            return adopt_fail(normalized_argv, observed_argv, argc, 3);
        }
    }
    char cwd_path[HOLD_PATH_MAX] = {0};
    if (hold_proc_entry_readlink(adopted_pid, "cwd", cwd_path, sizeof(cwd_path)) != 0) {
        cwd_path[0] = '\0';
    }
    if (hold_normalize_existing_argv_paths_from_cwd(normalized_argv, argc, 1, cwd_path[0] ? cwd_path : NULL) != 0) {
        return adopt_fail(normalized_argv, observed_argv, argc, 3);
    }

    /* An adopted id in a user store must not shadow a system projection. */
    struct hold_store system_hint;
    const struct hold_store *avoid_public_store = NULL;
    if (store->kind == STORE_USER_LOCAL && hold_init_system_store(&system_hint) == 0) {
        avoid_public_store = &system_hint;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t start_unix_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    /* Two-phase creation, like launched calls: the store reserve holds the id
     * across the log/socket-before-JSON window so a purge cannot eat them. */
    char id[ID_STR_LEN], log_path[HOLD_PATH_MAX];
    if (hold_reserve_adopted_run_id(store, avoid_public_store,
                                    exe_path[0] ? exe_path : (argc > 0 ? observed_argv[0] : NULL),
                                    argc, normalized_argv, cwd_path,
                                    adopted_pid, fg_pgid, start_unix_ns, id) != 0) {
        return adopt_fail(normalized_argv, observed_argv, argc, 3);
    }
    char console_sock[HOLD_PATH_MAX];
    pid_t server = -1;
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0 ||
        (server = hold_spawn_adopted_console_server(store, id, log_path, master,
                                                    fg_pgid, adopted_sid, shell_pid,
                                                    console_sock, sizeof(console_sock))) < 0) {
        hold_abort_run_reservation(store, id);
        return adopt_fail(normalized_argv, observed_argv, argc, 3);
    }

    struct hold_run_record r;
    hold_record_init_running(&r, id, log_path, adopted_pid, fg_pgid, adopted_sid,
                             start_unix_ns, start_unix_ns);
    /* Adopted runs are addressable like launched ones: they get a generated name. */
    if (hold_generate_run_name_for_id(store, id, NULL, r.name) == 0 && r.name[0]) {
        r.has_name = true;
    }
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
        kill(server, SIGTERM);
        hold_abort_run_reservation(store, id);
        adopt_fail(normalized_argv, observed_argv, argc, 3);
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
    return adopt_fail(normalized_argv, observed_argv, argc, 0);
#endif
}

/* Bytes the detach FSM releases go straight to the shell's PTY master. */
static int master_sink(void *ctx, const unsigned char *bytes, size_t n) {
    return hold_write_all(*(const int *)ctx, bytes, n);
}

static int relay_shell_pty(int master, pid_t child, bool *detached) {
    static const unsigned char detach_keys[2] = {0x10, 0x11}; /* Ctrl-P Ctrl-Q */
    struct hold_term_detach fsm;
    hold_term_detach_init(&fsm, detach_keys, sizeof(detach_keys));
    bool stdin_open = true;
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
        /* Cap the wait at 100 ms so the child-exit poll above stays live. */
        int timeout_ms = hold_term_detach_timeout_ms(&fsm);
        if (timeout_ms < 0 || timeout_ms > 100) timeout_ms = 100;
        int ready;
        do {
            ready = poll(pfds, nfds, timeout_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) return 1;
        if (fsm.pending_len > 0 && (ready == 0 || hold_term_detach_timeout_ms(&fsm) == 0) &&
            hold_term_detach_flush(&fsm, master_sink, &master) != 0) {
            return 1;
        }
        if (ready == 0) continue;
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
                int chord = 0;
                if (hold_term_detach_feed(&fsm, buf[i], master_sink, &master, &chord) != 0) return 1;
                if (chord) {
                    *detached = true;
                    return 0;
                }
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

    struct termios orig_termios;
    bool raw_active = false;
    if (isatty(STDIN_FILENO)) {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
            t = orig_termios;
            cfmakeraw(&t);
            raw_active = tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0;
        }
        if (!raw_active) {
            int saved = errno;
            kill(child, SIGHUP);
            close(master);
            errno = saved;
            hold_die_errno("hold: failed to prepare terminal");
        }
    }
    bool detached = false;
    int rc = relay_shell_pty(master, child, &detached);
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
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
    }
    close(master);
    return rc;
}

static int not_inside_session(void) {
    fprintf(stderr, "hold: not inside a hold on session\n");
    return 1;
}

int hold_cmd_off_action(void) {
    const char *pidstr = getenv("HOLD_ON_PID");
    if (!pidstr || !*pidstr) return not_inside_session();
    char *end = NULL;
    errno = 0;
    long pid = strtol(pidstr, &end, 10);
    if (end == pidstr || *end != '\0' || errno != 0 || pid <= 1) return not_inside_session();
#if defined(__linux__)
    /* Confirm the target is a live Hold process before signaling it: match its
     * /proc/<pid>/comm against our own so we never TERM an unrelated pid. */
    char self_comm[64] = {0}, target_comm[64] = {0};
    if (hold_proc_read_comm(getpid(), self_comm, sizeof(self_comm)) != 0 ||
        hold_proc_read_comm((pid_t)pid, target_comm, sizeof(target_comm)) != 0 ||
        strcmp(self_comm, target_comm) != 0) {
        return not_inside_session();
    }
#else
    if (kill((pid_t)pid, 0) != 0) return not_inside_session();
#endif
    if (kill((pid_t)pid, SIGTERM) != 0) return not_inside_session();
    return 0;
}
