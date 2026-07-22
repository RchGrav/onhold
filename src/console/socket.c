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

int hold_format_console_sock_path(const struct hold_store *store,
                                    const char *id,
                                    char *out,
                                    size_t n) {
    /* The console socket always lives inside the store's 0700-owned console
     * directory; access is gated by that directory's permissions, and the
     * relative bind/connect below keeps the directory's absolute length out of
     * the sun_path limit, so no world-writable fallback location is needed. */
    return hold_checked_snprintf(out, n, "%s/%s.sock", store->console_dir, id);
}

/* AF_UNIX paths are limited to sun_path bytes (104 on macOS, 108 on Linux),
 * far shorter than a normal filesystem path. To keep the console socket inside
 * its store-owned directory regardless of that directory's length, bind and
 * connect chdir into the directory and use the short trailing "<id>.sock"
 * name; only bind()/connect() are subject to the limit, so unlink/stat/record
 * keep using the absolute path. The working directory is restored before
 * returning — the broker forks and execs the target after this call, and that
 * process must run in the caller's original cwd, so fchdir failure is fatal
 * (never launch the target in the wrong directory). */
static int console_socket_in_dir(const char *sock_path, bool bind_listener) {
    struct sockaddr_un addr;
    char dir[HOLD_PATH_MAX];
    const char *slash = strrchr(sock_path, '/');
    if (!slash || slash == sock_path || !slash[1]) {
        errno = EINVAL;
        return -1;
    }
    size_t namelen = strlen(slash + 1);
    size_t dlen = (size_t)(slash - sock_path);
    if (namelen >= sizeof(addr.sun_path) || dlen >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dir, sock_path, dlen);
    dir[dlen] = '\0';
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, slash + 1, namelen + 1);

    int err = 0;
    int cwd_fd = -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
#ifdef HOLD_NEED_SOCKET_CLOEXEC
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        err = errno;
        goto out;
    }
#endif
    cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0) {
        err = errno;
        goto out;
    }
    if (chdir(dir) != 0) {
        err = errno;
        goto out;
    }

    if (bind_listener) {
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
    } else if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        err = errno;
    }
    if (fchdir(cwd_fd) != 0 && err == 0) {
        err = errno;
        if (bind_listener) {
            unlink(addr.sun_path);
        }
    }
out:
    if (cwd_fd >= 0) close(cwd_fd);
    if (err) {
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

int hold_make_console_listener(const char *sock_path) {
    return console_socket_in_dir(sock_path, true);
}

int hold_connect_console_socket(const char *sock_path) {
    struct stat st;
    if (stat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "hold: console socket is not available\n");
        errno = ENOTSOCK;
        return -1;
    }
    int fd = console_socket_in_dir(sock_path, false);
    if (fd < 0) {
        if (errno == ENAMETOOLONG) {
            fprintf(stderr, "hold: console socket path is too long\n");
        }
        return -1;
    }
    /* Mutual auth: the client verifies the listener too (root or the socket
     * owner), so a swapped-in listener cannot impersonate the broker. */
    uid_t peer_uid = (uid_t)-1;
    if (hold_console_peer_uid(fd, &peer_uid) != 0 || (peer_uid != 0 && peer_uid != st.st_uid)) {
        close(fd);
        errno = EPERM;
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
