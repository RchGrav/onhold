#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/core.h"

static int append_cmd_escaped(char *dst, size_t n, size_t *off, const char *arg);
static bool cmd_arg_needs_quotes(const char *arg);

void sig_note(const struct invocation *inv, const char *fmt, ...) {
    if (inv && inv->quiet) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void die_errno(const char *msg) {
    int e = errno;
    fprintf(stderr, "%s: %s\n", msg, strerror(e));
    exit(1);
}

int checked_snprintf(char *dst, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

bool has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), sufl = strlen(suffix);
    return sl >= sufl && strcmp(s + (sl - sufl), suffix) == 0;
}

int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            errno = EIO;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int append_cmd_escaped(char *dst, size_t n, size_t *off, const char *arg) {
    const char *sq = "'\\''";
    if (*off + 1 >= n) {
        return -1;
    }
    dst[(*off)++] = '\'';
    for (; *arg; arg++) {
        if (*arg == '\'') {
            for (size_t j = 0; sq[j]; j++) {
                if (*off + 1 >= n) {
                    return -1;
                }
                dst[(*off)++] = sq[j];
            }
        } else {
            if (*off + 1 >= n) {
                return -1;
            }
            dst[(*off)++] = *arg;
        }
    }
    if (*off + 1 >= n) {
        return -1;
    }
    dst[(*off)++] = '\'';
    dst[*off] = '\0';
    return 0;
}

static bool cmd_arg_needs_quotes(const char *arg) {
    if (!arg || !*arg) {
        return true;
    }
    for (const unsigned char *p = (const unsigned char *)arg; *p; p++) {
        if (isspace(*p) || strchr("'\"\\$`!*?[]{}()<>|&;#", (int)*p)) {
            return true;
        }
    }
    return false;
}

int append_cmd_human(char *dst, size_t n, size_t *off, const char *arg) {
    if (!arg) {
        arg = "";
    }
    if (!cmd_arg_needs_quotes(arg)) {
        for (const char *p = arg; *p; p++) {
            if (*off + 1 >= n) {
                return -1;
            }
            dst[(*off)++] = *p;
        }
        dst[*off] = '\0';
        return 0;
    }
    return append_cmd_escaped(dst, n, off, arg);
}

int format_argv_human(char *dst, size_t n, int argc, char **argv) {
    if (!dst || n == 0 || argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    size_t off = 0;
    dst[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            if (off + 1 >= n) {
                return -1;
            }
            dst[off++] = ' ';
            dst[off] = '\0';
        }
        if (append_cmd_human(dst, n, &off, argv[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int read_exec_handshake(int fd, int *child_errno) {
    unsigned char *p = (unsigned char *)child_errno;
    size_t got = 0;
    *child_errno = 0;
    while (got < sizeof(*child_errno)) {
        ssize_t n = read(fd, p + got, sizeof(*child_errno) - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (got == 0) {
                return 0;
            }
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 1;
}

void format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n) {
    time_t sec = (time_t)(unix_ns / 1000000000LL);
    struct tm tm_utc;
    if (!gmtime_r(&sec, &tm_utc)) {
        snprintf(out, n, "-");
        return;
    }
    if (strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        snprintf(out, n, "-");
    }
}

void format_relative_age(int64_t start_unix_ns, char *out, size_t n) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || start_unix_ns <= 0) {
        snprintf(out, n, "-");
        return;
    }
    int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    int64_t age_s = (now_ns > start_unix_ns) ? (now_ns - start_unix_ns) / 1000000000LL : 0;
    if (age_s < 60) {
        snprintf(out, n, "%" PRId64 "s", age_s);
    } else if (age_s < 3600) {
        snprintf(out, n, "%" PRId64 "m", age_s / 60);
    } else if (age_s < 86400) {
        snprintf(out, n, "%" PRId64 "h", age_s / 3600);
    } else {
        snprintf(out, n, "%" PRId64 "d", age_s / 86400);
    }
}
