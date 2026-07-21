#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

int hold_chmod_dir_no_symlink(const char *dir, mode_t mode) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return -1;
    }
    if (fchmod(fd, mode) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}

int hold_chown_dir_no_symlink_if_root(const char *dir, uid_t uid, gid_t gid) {
    if (geteuid() != 0) {
        return 0;
    }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ENOTDIR;
        return -1;
    }
    if (fchown(fd, uid, gid) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}

int hold_mkdir_p0700(const char *dir) {
    return hold_mkdir_p_mode(dir, 0700);
}

int hold_read_file_trim(const char *path, char *buf, size_t n) {
    if (n == 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t r;
    do {
        r = read(fd, buf, n - 1);
    } while (r < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (r < 0) {
        errno = saved;
        return -1;
    }
    buf[r] = '\0';
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r' || isspace((unsigned char)buf[r - 1]))) {
        buf[r - 1] = '\0';
        r--;
    }
    return 0;
}

bool hold_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int hold_mkdir_p_mode(const char *dir, mode_t mode) {
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s", dir) != 0) {
        return -1;
    }

    size_t len = strlen(path);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }
        char saved = path[i];
        path[i] = '\0';
        if (path[0] != '\0') {
            struct stat st;
            bool created = false;
            if (lstat(path, &st) != 0) {
                if (mkdir(path, mode) != 0 && errno != EEXIST) {
                    return -1;
                }
                if (lstat(path, &st) != 0) {
                    return -1;
                }
                created = true;
            } else if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
            if (created && hold_chmod_dir_no_symlink(path, mode) != 0) {
                return -1;
            }
        }
        path[i] = saved;
    }
    return 0;
}

int hold_read_owned_file_no_symlink(const char *path, char **out) {
    *out = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (geteuid() != 0 && st.st_uid != 0 && st.st_uid != geteuid()) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > MAX_RECORD_BYTES) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t sz = (size_t)st.st_size;
    char *j = malloc(sz + 1);
    if (!j) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    size_t off = 0;
    while (off < sz) {
        ssize_t nr = read(fd, j + off, sz - off);
        if (nr > 0) {
            off += (size_t)nr;
            continue;
        }
        if (nr == 0) {
            free(j);
            close(fd);
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        int saved = errno;
        free(j);
        close(fd);
        errno = saved;
        return -1;
    }
    j[sz] = '\0';

    if (close(fd) != 0) {
        int saved = errno;
        free(j);
        errno = saved;
        return -1;
    }
    *out = j;
    return 0;
}

int hold_read_small_file(const char *path, char **out) {
    return hold_read_owned_file_no_symlink(path, out);
}
