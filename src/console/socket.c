#include "hold/config.h"
#include "hold/types.h"
#include "hold/console.h"
#include "hold/core.h"
#include "hold/console_internal.h"

#if !defined(__linux__) && !defined(__APPLE__) && defined(__has_include)
#if __has_include(<sys/ucred.h>)
#include <sys/ucred.h>
#endif
#endif

static int console_addr_relative(const char *sock_path,
                                 struct sockaddr_un *addr,
                                 char *dir,
                                 size_t dirn);

int hold_format_console_sock_path(const struct hold_store *store,
                                    const char *id,
                                    char *out,
                                    size_t n) {
    /* The console socket always lives inside the store's 0700-owned console
     * directory; access is gated by that directory's permissions. It is
     * bound/connected via a short name relative to that directory (see
     * console_addr_relative), so the directory's absolute length does not
     * count against the AF_UNIX sun_path limit. There is therefore no need to
     * fall back to a world-writable location such as /tmp. */
    return hold_checked_snprintf(out, n, "%s/%s.sock", store->console_dir, id);
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

int hold_make_console_listener(const char *sock_path) {
    struct sockaddr_un addr;
    char dir[HOLD_PATH_MAX];
    if (console_addr_relative(sock_path, &addr, dir, sizeof(dir)) != 0) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
#ifdef HOLD_NEED_SOCKET_CLOEXEC
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

int hold_console_peer_uid(int fd, uid_t *uid_out) {
    if (!uid_out) {
        errno = EINVAL;
        return -1;
    }
#if defined(__linux__)
    struct ucred cred;
    socklen_t len = (socklen_t)sizeof(cred);
    memset(&cred, 0, sizeof(cred));
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || len < sizeof(cred)) {
        return -1;
    }
    *uid_out = cred.uid;
    return 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0) {
        return -1;
    }
    (void)gid;
    *uid_out = uid;
    return 0;
#elif defined(LOCAL_PEERCRED)
    struct xucred cred;
    socklen_t len = (socklen_t)sizeof(cred);
    memset(&cred, 0, sizeof(cred));
    if (getsockopt(fd, 0, LOCAL_PEERCRED, &cred, &len) != 0) {
        return -1;
    }
#if defined(XUCRED_VERSION)
    if (cred.cr_version != XUCRED_VERSION) {
        errno = EINVAL;
        return -1;
    }
#endif
    *uid_out = cred.cr_uid;
    return 0;
#else
    errno = ENOTSUP;
    return -1;
#endif
}

int hold_open_console_pty(int *master_out, int *slave_out,
                          unsigned short init_rows, unsigned short init_cols) {
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
    /* Open the slave with O_NOCTTY so the broker (a session leader without a
     * controlling terminal) does not accidentally adopt this PTY as its own
     * controlling tty. The target child claims it explicitly via setsid() +
     * TIOCSCTTY so that terminal-generated signals (Ctrl-C, etc.) and the
     * foreground process group are scoped to the child, never the broker. */
    int slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
    /* Give the PTY a real window size before the child execs. Without this the
     * kernel defaults to 0x0, which makes shells/readline and TUIs misrender and
     * appear to drop keystrokes. Fall back to a sane 80x24 when no terminal size
     * was supplied (e.g. a detached `-d -t` start with no client attached). */
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = init_rows ? init_rows : 24;
    ws.ws_col = init_cols ? init_cols : 80;
    (void)ioctl(master, TIOCSWINSZ, &ws);
    *master_out = master;
    *slave_out = slave;
    return 0;
}

int hold_connect_console_socket(const char *sock_path) {
    struct stat st;
    if (stat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "hold: console socket is not available\n");
        errno = ENOTSOCK;
        return -1;
    }

    struct sockaddr_un addr;
    char dir[HOLD_PATH_MAX];
    if (console_addr_relative(sock_path, &addr, dir, sizeof(dir)) != 0) {
        if (errno == ENAMETOOLONG) {
            fprintf(stderr, "hold: console socket path is too long\n");
        }
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
#ifdef HOLD_NEED_SOCKET_CLOEXEC
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
