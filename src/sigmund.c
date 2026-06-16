#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pwd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ID_HEX_LEN 6
#define STOP_TIMEOUT_MS 5000
#define POLL_SLEEP_MS 25
#ifndef SIGMUND_VERSION
#define SIGMUND_VERSION "dev"
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define SIGMUND_PATH_MAX PATH_MAX
#define MAX_RECORD_BYTES (1024 * 1024)
#ifndef SIGMUND_BOOT_ID_PATH
#define SIGMUND_BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define JSON_MAX_DEPTH 64
#ifndef SIGMUND_SYSTEM_STATE_DIR
#if defined(__APPLE__)
#define SIGMUND_SYSTEM_STATE_DIR "/var/db/sigmund"
#else
#define SIGMUND_SYSTEM_STATE_DIR "/var/lib/sigmund"
#endif
#endif

struct record {
    int version;
    char id[16];
    char run_id[16];
    pid_t pid;
    pid_t pgid;
    pid_t sid;
    int64_t start_unix_ns;
    uid_t uid;
    gid_t gid;
    char log_path[SIGMUND_PATH_MAX];
    char boot_id[128];
    uint64_t proc_starttime_ticks;
    uint64_t exe_dev;
    uint64_t exe_ino;
    char cmdline[SIGMUND_PATH_MAX];
    char started_at[64];
    char ended_at[64];
    char state[16];
    int exit_code;
    int term_signal;
    char launch_error[64];
    uid_t invoked_by_uid;
    gid_t invoked_by_gid;
    char invoked_by_user[128];
    bool invoked_via_sudo;
    bool has_log;
    bool has_boot;
    bool has_started_at;
    bool has_ended_at;
    bool has_state;
    bool has_exit_code;
    bool has_term_signal;
    bool has_launch_error;
    bool has_invocation;
};

enum run_state { STATE_RUNNING, STATE_EXITED, STATE_STALE, STATE_FAILED, STATE_UNKNOWN };

enum store_kind { STORE_USER_LOCAL, STORE_SYSTEM_MANAGED };

struct store_paths {
    enum store_kind kind;
    char base[SIGMUND_PATH_MAX];
    char record_dir[SIGMUND_PATH_MAX];
    char log_dir[SIGMUND_PATH_MAX];
    char public_dir[SIGMUND_PATH_MAX];
};

struct invocation {
    bool euid_root;
    bool requested_system;
    bool elevated;
    bool have_sudo_user;
    uid_t invoking_uid;
    gid_t invoking_gid;
    char invoking_user[128];
    char invoking_home[SIGMUND_PATH_MAX];
};

enum resolve_scope { RESOLVE_USER_LOCAL, RESOLVE_SYSTEM_MANAGED, RESOLVE_NOT_FOUND, RESOLVE_ERROR };

struct resolved_target {
    enum resolve_scope scope;
    char id[16];
    struct store_paths store;
    bool needs_elevation;
};

struct public_index {
    char id[16];
    bool root_managed;
    bool requires_elevation;
    char state_hint[16];
    char started_at[64];
};

static volatile sig_atomic_t g_tail_interrupted = 0;
static int write_all(int fd, const void *buf, size_t n);
static void usage(void);
static void format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n);

static void handle_tail_sigint(int signo) {
    (void)signo;
    g_tail_interrupted = 1;
}

static void die_errno(const char *msg) {
    int e = errno;
    fprintf(stderr, "%s: %s\n", msg, strerror(e));
    exit(1);
}

static int checked_snprintf(char *dst, size_t n, const char *fmt, ...) {
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

static bool has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), sufl = strlen(suffix);
    return sl >= sufl && strcmp(s + (sl - sufl), suffix) == 0;
}

static bool valid_id(const char *id) {
    size_t len = strlen(id);
    if (len < 6 || len > 10) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool valid_id_prefix(const char *id) {
    size_t len = strlen(id);
    if (len < 1 || len > 10) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool valid_record(const struct record *r) {
    return r->pid > 0 && r->pgid > 1 && r->id[0] != '\0';
}

static int mkdir_p0700(const char *dir) {
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s", dir) != 0) {
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
            if (stat(path, &st) != 0) {
                if (mkdir(path, 0700) != 0 && errno != EEXIST) {
                    return -1;
                }
                created = true;
            } else if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
            if (created && chmod(path, 0700) != 0) {
                return -1;
            }
        }
        path[i] = saved;
    }
    return 0;
}

static int read_file_trim(const char *path, char *buf, size_t n) {
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

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int get_boot_id(char *buf, size_t n) {
    const char *path = getenv("SIGMUND_BOOT_ID_PATH");
    if (path && *path) {
        return read_file_trim(path, buf, n);
    }
    if (read_file_trim(SIGMUND_BOOT_ID_PATH, buf, n) == 0) {
        return 0;
    }
#if defined(__APPLE__)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0 && len == sizeof(boottime)) {
        snprintf(buf, n, "macos-%lld.%06d", (long long)boottime.tv_sec, boottime.tv_usec);
        return 0;
    }
#endif
    return -1;
}

static bool current_boot_id(char *buf, size_t n) {
    if (n == 0) {
        return false;
    }
    buf[0] = '\0';
    return get_boot_id(buf, n) == 0 && buf[0] != '\0';
}

static int rand_bytes(uint8_t *buf, size_t n) {
    size_t off = 0;
#if defined(__linux__)
    bool fallback = false;
    while (off < n && !fallback) {
        ssize_t r = getrandom(buf + off, n - off, 0);
        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 && (errno == ENOSYS || errno == EINVAL)) {
            fallback = true;
            break;
        }
        return -1;
    }
    if (!fallback) {
        return 0;
    }
#endif

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    while (off < n) {
        ssize_t x = read(fd, buf + off, n - off);
        if (x > 0) {
            off += (size_t)x;
            continue;
        }
        if (x < 0 && errno == EINTR) {
            continue;
        }
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int mkdir_p_mode(const char *dir, mode_t mode) {
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s", dir) != 0) {
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
            if (stat(path, &st) != 0) {
                if (mkdir(path, mode) != 0 && errno != EEXIST) {
                    return -1;
                }
                if (chmod(path, mode) != 0) {
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return -1;
            }
        }
        path[i] = saved;
    }
    return 0;
}

static int chown_root_if_root(const char *path) {
    if (geteuid() != 0) {
        return 0;
    }
    if (chown(path, 0, 0) != 0) {
        return -1;
    }
    return 0;
}

static int parse_uid_env(const char *s, uid_t *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (uid_t)v;
    return 0;
}

static int parse_gid_env(const char *s, gid_t *out) {
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (gid_t)v;
    return 0;
}

static int init_user_store_from_home(const char *home, struct store_paths *store) {
    if (!home || !*home) {
        errno = EINVAL;
        return -1;
    }
    memset(store, 0, sizeof(*store));
    store->kind = STORE_USER_LOCAL;
    if (checked_snprintf(store->base, sizeof(store->base), "%s/.local/state/sigmund", home) != 0 ||
        checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s", store->base) != 0 ||
        checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s", store->base) != 0) {
        return -1;
    }
    return 0;
}

static int init_user_store_for_current_user(struct store_paths *store) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "sigmund: error: HOME is not set\n");
        errno = EINVAL;
        return -1;
    }
    return init_user_store_from_home(home, store);
}

static int ensure_user_store_for_current_user(struct store_paths *store) {
    if (init_user_store_for_current_user(store) != 0) {
        return -1;
    }
    if (mkdir_p0700(store->base) != 0) {
        return -1;
    }
    if (chmod(store->base, 0700) != 0) {
        return -1;
    }
    return 0;
}

static int init_system_store(struct store_paths *store) {
    const char *base = SIGMUND_SYSTEM_STATE_DIR;
#ifdef SIGMUND_TESTING
    const char *override = getenv("SIGMUND_TEST_SYSTEM_STATE_DIR");
    if (override && *override) {
        base = override;
    }
#endif
    memset(store, 0, sizeof(*store));
    store->kind = STORE_SYSTEM_MANAGED;
    if (checked_snprintf(store->base, sizeof(store->base), "%s", base) != 0 ||
        checked_snprintf(store->record_dir, sizeof(store->record_dir), "%s/runs", base) != 0 ||
        checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s/logs", base) != 0 ||
        checked_snprintf(store->public_dir, sizeof(store->public_dir), "%s/public", base) != 0) {
        return -1;
    }
    return 0;
}

static int ensure_system_store(struct store_paths *store) {
    if (init_system_store(store) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->base, 0755) != 0 ||
        chmod(store->base, 0755) != 0 ||
        chown_root_if_root(store->base) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->record_dir, 0700) != 0 ||
        chmod(store->record_dir, 0700) != 0 ||
        chown_root_if_root(store->record_dir) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->log_dir, 0700) != 0 ||
        chmod(store->log_dir, 0700) != 0 ||
        chown_root_if_root(store->log_dir) != 0) {
        return -1;
    }
    if (mkdir_p_mode(store->public_dir, 0755) != 0 ||
        chmod(store->public_dir, 0755) != 0 ||
        chown_root_if_root(store->public_dir) != 0) {
        return -1;
    }
    return 0;
}

static int detect_invocation(struct invocation *inv, bool requested_system, bool elevated) {
    memset(inv, 0, sizeof(*inv));
    inv->euid_root = (geteuid() == 0);
    inv->requested_system = requested_system;
    inv->elevated = elevated;
    inv->invoking_uid = getuid();
    inv->invoking_gid = getgid();
    snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", "");
    snprintf(inv->invoking_home, sizeof(inv->invoking_home), "%s", "");

    if (!inv->euid_root) {
        return 0;
    }

    const char *su = getenv("SUDO_UID");
    const char *sg = getenv("SUDO_GID");
    const char *sn = getenv("SUDO_USER");
    uid_t uid = 0;
    gid_t gid = 0;
    if (parse_uid_env(su, &uid) == 0 && parse_gid_env(sg, &gid) == 0 && sn && *sn) {
        struct passwd *pw = getpwuid(uid);
        if (pw && pw->pw_dir && *pw->pw_dir) {
            const char *home = pw->pw_dir;
#ifdef SIGMUND_TESTING
            const char *home_override = getenv("SIGMUND_TEST_INVOKING_HOME");
            if (home_override && *home_override) {
                home = home_override;
            }
#endif
            inv->have_sudo_user = true;
            inv->invoking_uid = uid;
            inv->invoking_gid = gid;
            if (checked_snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", sn) != 0 ||
                checked_snprintf(inv->invoking_home, sizeof(inv->invoking_home), "%s", home) != 0) {
                return -1;
            }
            return 0;
        }
    }

    inv->have_sudo_user = false;
    inv->invoking_uid = 0;
    inv->invoking_gid = 0;
    if (checked_snprintf(inv->invoking_user, sizeof(inv->invoking_user), "%s", "root") != 0) {
        return -1;
    }
    return 0;
}

static bool id_collides_in_store(const struct store_paths *store, const char *id, bool include_public) {
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (checked_snprintf(path, sizeof(path), "%s/.%s.reserve", store->record_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (include_public && store->public_dir[0]) {
        if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && path_exists(path)) {
            return true;
        }
    }
    return false;
}

static bool id_material_collides_in_store(const struct store_paths *store, const char *id, bool include_public) {
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->record_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (checked_snprintf(path, sizeof(path), "%s/%s.log", store->log_dir, id) == 0 && path_exists(path)) {
        return true;
    }
    if (include_public && store->public_dir[0]) {
        if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0 && path_exists(path)) {
            return true;
        }
    }
    return false;
}

static int gen_id_for_store(const struct store_paths *primary,
                            const struct store_paths *avoid_public_store,
                            const struct store_paths *avoid_user_store,
                            char *out,
                            size_t out_n) {
    if (out_n < ID_HEX_LEN + 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    uint8_t b[ID_HEX_LEN / 2];
    char reserve[SIGMUND_PATH_MAX];
    for (int tries = 0; tries < 100; tries++) {
        if (rand_bytes(b, sizeof(b)) != 0) {
            return -1;
        }
        for (size_t i = 0; i < sizeof(b); i++) {
            snprintf(out + i * 2, out_n - i * 2, "%02x", b[i]);
        }
        out[ID_HEX_LEN] = '\0';
        if (id_collides_in_store(primary, out, primary->kind == STORE_SYSTEM_MANAGED)) {
            continue;
        }
        if (avoid_public_store && avoid_public_store->public_dir[0]) {
            char p[SIGMUND_PATH_MAX];
            if (checked_snprintf(p, sizeof(p), "%s/%s.json", avoid_public_store->public_dir, out) == 0 && path_exists(p)) {
                continue;
            }
        }
        if (avoid_user_store && id_collides_in_store(avoid_user_store, out, false)) {
            continue;
        }
        if (checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", primary->record_dir, out) != 0) {
            return -1;
        }
        int fd = open(reserve, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            bool avoid_public_race = false;
            if (avoid_public_store && avoid_public_store->public_dir[0]) {
                char p[SIGMUND_PATH_MAX];
                avoid_public_race = (checked_snprintf(p, sizeof(p), "%s/%s.json", avoid_public_store->public_dir, out) == 0 && path_exists(p));
            }
            if (id_material_collides_in_store(primary, out, primary->kind == STORE_SYSTEM_MANAGED) ||
                avoid_public_race ||
                (avoid_user_store && id_material_collides_in_store(avoid_user_store, out, false))) {
                unlink(reserve);
                continue;
            }
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}

static int write_all(int fd, const void *buf, size_t n) {
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

static void json_escape(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            fprintf(f, "\\%c", *s);
        } else if (*s == '\n') {
            fputs("\\n", f);
        } else if (*s == '\r') {
            fputs("\\r", f);
        } else if (*s == '\t') {
            fputs("\\t", f);
        } else if (*s == '\b') {
            fputs("\\b", f);
        } else if (*s == '\f') {
            fputs("\\f", f);
        } else if ((unsigned char)*s < 32) {
            fprintf(f, "\\u%04x", (unsigned char)*s);
        } else {
            fputc(*s, f);
        }
    }
}

static int write_json_argv(FILE *f, int argc, char **argv) {
    fputs("[", f);
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            fputs(", ", f);
        }
        fputc('"', f);
        json_escape(f, argv[i]);
        fputc('"', f);
    }
    fputs("]", f);
    return 0;
}

static int write_record_atomic(const char *dir, const struct record *r, int argc, char **argv, char *out_json_path, size_t out_n) {
    char tmp[SIGMUND_PATH_MAX], fin[SIGMUND_PATH_MAX], reserve[SIGMUND_PATH_MAX];
    int rc = -1;
    int fd = -1;
    FILE *f = NULL;
    if (checked_snprintf(fin, sizeof(fin), "%s/%s.json", dir, r->id) != 0) {
        return -1;
    }
    if (checked_snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, r->id) != 0) {
        return -1;
    }
    if (checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", dir, r->id) != 0) {
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        fd = -1;
        goto out;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": %d,\n", r->version);
    fprintf(f, "  \"id\": \"");
    json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"run_id\": \"");
    json_escape(f, r->run_id[0] ? r->run_id : r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"pid\": %ld,\n", (long)r->pid);
    fprintf(f, "  \"pgid\": %ld,\n", (long)r->pgid);
    fprintf(f, "  \"sid\": %ld,\n", (long)r->sid);
    fprintf(f, "  \"start_unix_ns\": %" PRId64 ",\n", r->start_unix_ns);
    fprintf(f, "  \"argv\": ");
    write_json_argv(f, argc, argv);
    fprintf(f, ",\n");
    fprintf(f, "  \"cmdline_display\": \"");
    json_escape(f, r->cmdline);
    fprintf(f, "\",\n");
    if (r->has_started_at) {
        fprintf(f, "  \"started_at\": \"");
        json_escape(f, r->started_at);
        fprintf(f, "\",\n");
    }
    if (r->has_ended_at) {
        fprintf(f, "  \"ended_at\": \"");
        json_escape(f, r->ended_at);
        fprintf(f, "\",\n");
    }
    if (r->has_state) {
        fprintf(f, "  \"state\": \"");
        json_escape(f, r->state);
        fprintf(f, "\",\n");
    }
    if (r->has_exit_code) {
        fprintf(f, "  \"exit_code\": %d,\n", r->exit_code);
    }
    if (r->has_term_signal) {
        fprintf(f, "  \"term_signal\": %d,\n", r->term_signal);
    }
    if (r->has_launch_error) {
        fprintf(f, "  \"launch_error\": \"");
        json_escape(f, r->launch_error);
        fprintf(f, "\",\n");
    }
    fprintf(f, "  \"uid\": %u,\n", r->uid);
    fprintf(f, "  \"gid\": %u,\n", r->gid);
    if (r->has_invocation) {
        fprintf(f, "  \"invoked_by_uid\": %u,\n", r->invoked_by_uid);
        fprintf(f, "  \"invoked_by_gid\": %u,\n", r->invoked_by_gid);
        fprintf(f, "  \"invoked_by_user\": \"");
        json_escape(f, r->invoked_by_user);
        fprintf(f, "\",\n");
        fprintf(f, "  \"invoked_via_sudo\": %s,\n", r->invoked_via_sudo ? "true" : "false");
    }
    if (r->has_log) {
        fprintf(f, "  \"log_path\": \"");
        json_escape(f, r->log_path);
        fprintf(f, "\",\n");
    }
    if (r->has_boot) {
        fprintf(f, "  \"boot_id\": \"");
        json_escape(f, r->boot_id);
        fprintf(f, "\",\n");
    }
    fprintf(f, "  \"proc_starttime_ticks\": %" PRIu64 ",\n", r->proc_starttime_ticks);
    fprintf(f, "  \"exe_dev\": %" PRIu64 ",\n", r->exe_dev);
    fprintf(f, "  \"exe_ino\": %" PRIu64 "\n", r->exe_ino);
    fprintf(f, "}\n");

    if (ferror(f) || fflush(f) != 0) {
        goto out;
    }
    if (fsync(fd) != 0) {
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        goto out;
    }
    f = NULL;
    fd = -1;
    if (rename(tmp, fin) != 0) {
        goto out;
    }
    unlink(reserve);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        if (fsync(dfd) != 0) {
            fprintf(stderr, "sigmund: warning: failed to fsync storage dir: %s\n", strerror(errno));
        }
        close(dfd);
    }
    if (out_json_path && checked_snprintf(out_json_path, out_n, "%s", fin) != 0) {
        goto out;
    }
    rc = 0;

out:
    if (f) {
        fclose(f);
    } else if (fd >= 0) {
        close(fd);
    }
    if (rc != 0) {
        unlink(tmp);
    }
    return rc;
}

static int write_public_index_atomic(const struct store_paths *store, const struct record *r) {
    char tmp[SIGMUND_PATH_MAX], fin[SIGMUND_PATH_MAX];
    int rc = -1;
    int fd = -1;
    FILE *f = NULL;
    if (checked_snprintf(fin, sizeof(fin), "%s/%s.json", store->public_dir, r->id) != 0 ||
        checked_snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", store->public_dir, r->id) != 0) {
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0644) != 0) {
        goto out;
    }
    if (geteuid() == 0 && fchown(fd, 0, 0) != 0) {
        goto out;
    }
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        fd = -1;
        goto out;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"id\": \"");
    json_escape(f, r->id);
    fprintf(f, "\",\n");
    fprintf(f, "  \"root_managed\": true,\n");
    fprintf(f, "  \"requires_elevation\": true,\n");
    fprintf(f, "  \"state_hint\": \"unknown\",\n");
    fprintf(f, "  \"started_at\": \"");
    json_escape(f, r->has_started_at && r->started_at[0] ? r->started_at : "-");
    fprintf(f, "\"\n");
    fprintf(f, "}\n");

    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        goto out;
    }
    if (fclose(f) != 0) {
        f = NULL;
        goto out;
    }
    f = NULL;
    fd = -1;
    if (rename(tmp, fin) != 0) {
        goto out;
    }
    if (chmod(fin, 0644) != 0 || chown_root_if_root(fin) != 0) {
        goto out;
    }
    int dfd = open(store->public_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        if (fsync(dfd) != 0) {
            fprintf(stderr, "sigmund: warning: failed to fsync public index dir: %s\n", strerror(errno));
        }
        close(dfd);
    }
    rc = 0;

out:
    if (f) {
        fclose(f);
    } else if (fd >= 0) {
        close(fd);
    }
    if (rc != 0) {
        unlink(tmp);
    }
    return rc;
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

#if defined(__APPLE__)
static int mac_kinfo_pid(pid_t pid, struct kinfo_proc *kp) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t len = sizeof(*kp);
    memset(kp, 0, sizeof(*kp));
    if (sysctl(mib, 4, kp, &len, NULL, 0) != 0 || len == 0) {
        return -1;
    }
    return 0;
}
#endif

enum group_liveness { GROUP_SCAN_ERROR = -1, GROUP_EMPTY = 0, GROUP_ZOMBIE_ONLY = 1, GROUP_LIVE = 2 };

static int parse_pid_token(const char *tok, pid_t *out) {
    char *end = NULL;
    errno = 0;
    long x = strtol(tok, &end, 10);
    if (end == tok || *end != '\0' || errno != 0 || x <= 0) {
        return -1;
    }
    *out = (pid_t)x;
    return 0;
}

static int read_process_ids_state(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) != 0) {
        return -1;
    }
    if (pgid_out) {
        *pgid_out = kp.kp_eproc.e_pgid;
    }
    if (sid_out) {
        pid_t sid = getsid(pid);
        if (sid <= 0) {
            return -1;
        }
        *sid_out = sid;
    }
    if (state_out) {
        *state_out = kp.kp_proc.p_stat == SZOMB ? 'Z' : '?';
    }
    return 0;
#else
    char path[128], buf[4096];
    if (checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t nr;
    do {
        nr = read(fd, buf, sizeof(buf) - 1);
    } while (nr < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (nr <= 0) {
        if (nr < 0) {
            errno = saved;
        } else {
            errno = EIO;
        }
        return -1;
    }
    buf[nr] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp) {
        errno = EINVAL;
        return -1;
    }
    char *fields = rp + 2;
    char *save = NULL;
    bool got_state = false, got_pgid = false, got_sid = false;
    pid_t pgid = 0, sid = 0;
    char state = 0;
    int idx = 0;
    for (char *tok = strtok_r(fields, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
        if (idx == 0) {
            state = tok[0];
            got_state = true;
        } else if (idx == 2) {
            if (parse_pid_token(tok, &pgid) != 0) {
                return -1;
            }
            got_pgid = true;
        } else if (idx == 3) {
            if (parse_pid_token(tok, &sid) != 0) {
                return -1;
            }
            got_sid = true;
            break;
        }
    }
    if ((state_out && !got_state) || (pgid_out && !got_pgid) || (sid_out && !got_sid)) {
        errno = EINVAL;
        return -1;
    }
    if (state_out) {
        *state_out = state;
    }
    if (pgid_out) {
        *pgid_out = pgid;
    }
    if (sid_out) {
        *sid_out = sid;
    }
    return 0;
#endif
}

static enum group_liveness group_session_liveness(pid_t pgid, pid_t sid) {
    if (pgid <= 1 || sid <= 0) {
        return GROUP_SCAN_ERROR;
    }
#if defined(__APPLE__)
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
        return GROUP_SCAN_ERROR;
    }
    struct kinfo_proc *procs = malloc(len ? len : sizeof(*procs));
    if (!procs) {
        return GROUP_SCAN_ERROR;
    }
    if (sysctl(mib, 3, procs, &len, NULL, 0) != 0) {
        free(procs);
        return GROUP_SCAN_ERROR;
    }
    bool any = false;
    bool live = false;
    size_t nprocs = len / sizeof(procs[0]);
    for (size_t i = 0; i < nprocs; i++) {
        pid_t pid = procs[i].kp_proc.p_pid;
        if (pid <= 0 || procs[i].kp_eproc.e_pgid != pgid) {
            continue;
        }
        pid_t proc_sid = getsid(pid);
        if (proc_sid != sid) {
            continue;
        }
        any = true;
        if (procs[i].kp_proc.p_stat != SZOMB) {
            live = true;
            break;
        }
    }
    free(procs);
    if (live) {
        return GROUP_LIVE;
    }
    return any ? GROUP_ZOMBIE_ONLY : GROUP_EMPTY;
#else
    DIR *d = opendir("/proc");
    if (!d) {
        return GROUP_SCAN_ERROR;
    }
    bool any = false;
    bool live = false;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) {
            continue;
        }
        char *pid_end = NULL;
        errno = 0;
        long pid_long = strtol(e->d_name, &pid_end, 10);
        if (pid_end == e->d_name || *pid_end != '\0' || errno != 0 || pid_long <= 0) {
            continue;
        }
        pid_t proc_pgid = 0, proc_sid = 0;
        char state = 0;
        if (read_process_ids_state((pid_t)pid_long, &proc_pgid, &proc_sid, &state) != 0) {
            continue;
        }
        if (proc_pgid != pgid || proc_sid != sid) {
            continue;
        }
        any = true;
        if (state != 'Z') {
            live = true;
            break;
        }
    }
    closedir(d);
    if (live) {
        return GROUP_LIVE;
    }
    return any ? GROUP_ZOMBIE_ONLY : GROUP_EMPTY;
#endif
}

static int count_session_escapees(pid_t sid, pid_t expected_pgid) {
#if defined(__APPLE__)
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) {
        return -1;
    }
    struct kinfo_proc *procs = malloc(len ? len : sizeof(*procs));
    if (!procs) {
        return -1;
    }
    if (sysctl(mib, 3, procs, &len, NULL, 0) != 0) {
        free(procs);
        return -1;
    }
    int count = 0;
    size_t nprocs = len / sizeof(procs[0]);
    for (size_t i = 0; i < nprocs; i++) {
        pid_t pid = procs[i].kp_proc.p_pid;
        if (pid <= 0) {
            continue;
        }
        pid_t proc_sid = getsid(pid);
        if (proc_sid == sid && procs[i].kp_eproc.e_pgid != expected_pgid) {
            count++;
        }
    }
    free(procs);
    return count;
#else
    DIR *d = opendir("/proc");
    if (!d) {
        return -1;
    }
    int count = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) {
            continue;
        }
        char *pid_end = NULL;
        errno = 0;
        long pid_long = strtol(e->d_name, &pid_end, 10);
        if (pid_end == e->d_name || *pid_end != '\0' || errno != 0 || pid_long <= 0) {
            continue;
        }
        pid_t proc_pgid = 0, proc_sid = 0;
        if (read_process_ids_state((pid_t)pid_long, &proc_pgid, &proc_sid, NULL) != 0) {
            continue;
        }
        if (proc_sid == sid && proc_pgid != expected_pgid) {
            count++;
        }
    }
    closedir(d);
    return count;
#endif
}

static void report_session_escapees(const struct record *r) {
    int escaped = count_session_escapees(r->sid, r->pgid);
    if (escaped > 0) {
        fprintf(stderr,
                "sigmund: warning: %d process(es) escaped process-group %ld but remain in session %ld\n",
                escaped, (long)r->pgid, (long)r->sid);
    }
}

static int read_proc_stat_tokens(pid_t pid, char *state_out, uint64_t *starttime_out) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) != 0) {
        return -1;
    }
    if (state_out) {
        *state_out = kp.kp_proc.p_stat == SZOMB ? 'Z' : '?';
    }
    if (starttime_out) {
        struct timeval tv = kp.kp_proc.p_starttime;
        if (tv.tv_sec == 0 && tv.tv_usec == 0) {
            return -1;
        }
        *starttime_out = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
    }
    return 0;
#else
    char path[128], buf[4096];
    if (checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n <= 0) {
        if (n < 0) {
            errno = saved;
        } else {
            errno = EIO;
        }
        return -1;
    }
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp) {
        return -1;
    }
    char *p = rp + 2;
    int idx = 0;
    char *save = NULL;
    bool got_state = false;
    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
        if (idx == 0 && state_out) {
            *state_out = tok[0];
            got_state = true;
        }
        /* /proc/<pid>/stat starttime is field 22 (1-indexed overall),
         * which is index 19 after the trailing ')' where idx 0 starts at state. */
        if (idx == 19 && starttime_out) {
            char *end = NULL;
            errno = 0;
            unsigned long long parsed = strtoull(tok, &end, 10);
            if (end == tok || errno != 0) {
                return -1;
            }
            *starttime_out = parsed;
            return 0;
        }
    }
    return (state_out && got_state && !starttime_out) ? 0 : -1;
#endif
}

static int read_proc_exe(pid_t pid, uint64_t *dev, uint64_t *ino) {
    struct stat st;
#if defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return -1;
    }
#else
    char path[128];
    if (checked_snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid) != 0) {
        return -1;
    }
#endif
    if (stat(path, &st) != 0) {
        return -1;
    }
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return 0;
}

static bool leader_present(pid_t pid) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) == 0) {
        return kp.kp_proc.p_stat != SZOMB;
    }
#else
    char path[128];
    struct stat st;
    if (checked_snprintf(path, sizeof(path), "/proc/%ld", (long)pid) != 0) {
        return false;
    }
    if (stat(path, &st) == 0) {
        char stc = 0;
        if (read_proc_stat_tokens(pid, &stc, NULL) == 0 && stc == 'Z') {
            return false;
        }
        return true;
    }
#endif
    if (kill(pid, 0) == 0 || errno == EPERM) {
        return true;
    }
    return false;
}

static int group_exists(pid_t pgid) {
    if (kill(-pgid, 0) == 0 || errno == EPERM) {
        return 1;
    }
    if (errno == ESRCH) {
        return 0;
    }
    return -1;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int skip_json_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return -1;
    p++;
    while (*p) {
        if (*p == '"') {
            *pp = p + 1;
            return 0;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return -1;
            if (*p == 'u') {
                for (int i = 0; i < 4; i++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                }
            }
        }
        p++;
    }
    return -1;
}

/* BMP-only; surrogate pairs are rejected. */
static int parse_json_string(const char *p, char *out, size_t n, const char **endp) {
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p) {
        if (*p == '"') {
            if (i >= n) return -1;
            out[i] = '\0';
            if (endp) *endp = p + 1;
            return 0;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return -1;
            char c = *p;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\\': case '"': case '/': break;
            case 'u': {
                unsigned v = 0;
                for (int j = 0; j < 4; j++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                    v = (v << 4) + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                }
                if (v == 0) return -1;
                if (v >= 0xD800 && v <= 0xDFFF) return -1;
                if (v <= 0x7F) {
                    c = (char)v;
                    if (i + 1 >= n) return -1;
                    out[i++] = c;
                } else if (v <= 0x7FF) {
                    if (i + 2 >= n) return -1;
                    out[i++] = (char)(0xC0 | (v >> 6));
                    out[i++] = (char)(0x80 | (v & 0x3F));
                } else {
                    if (i + 3 >= n) return -1;
                    out[i++] = (char)(0xE0 | (v >> 12));
                    out[i++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    out[i++] = (char)(0x80 | (v & 0x3F));
                }
                p++;
                continue;
            }
            default: return -1;
            }
            if (i + 1 >= n) return -1;
            out[i++] = c;
            p++;
            continue;
        }
        if (i + 1 >= n) return -1;
        out[i++] = *p++;
    }
    return -1;
}

static int skip_json_value(const char **pp);
static int skip_json_value_impl(const char **pp, int depth);

static int match_json_string(const char *p, const char *lit, const char **endp, bool *matched) {
    if (*p != '"') return -1;
    p++;
    size_t li = 0;
    bool ok = true;
    while (*p) {
        if (*p == '"') {
            if (lit[li] != '\0') {
                ok = false;
            }
            if (endp) *endp = p + 1;
            if (matched) *matched = ok;
            return 0;
        }
        unsigned cp = 0;
        if (*p == '\\') {
            p++;
            if (!*p) return -1;
            switch (*p) {
            case 'n': cp = '\n'; p++; break;
            case 't': cp = '\t'; p++; break;
            case 'r': cp = '\r'; p++; break;
            case 'b': cp = '\b'; p++; break;
            case 'f': cp = '\f'; p++; break;
            case '\\': cp = '\\'; p++; break;
            case '"': cp = '"'; p++; break;
            case '/': cp = '/'; p++; break;
            case 'u': {
                unsigned v = 0;
                for (int j = 0; j < 4; j++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                    v = (v << 4) + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                }
                if (v == 0 || (v >= 0xD800 && v <= 0xDFFF)) return -1;
                cp = v;
                p++;
                break;
            }
            default:
                return -1;
            }
        } else {
            cp = (unsigned char)*p;
            p++;
        }

        if (cp <= 0x7F) {
            if (lit[li] == '\0' || (unsigned char)lit[li] != cp) {
                ok = false;
            }
            if (lit[li] != '\0') {
                li++;
            }
        } else {
            ok = false;
        }
    }
    return -1;
}

static int skip_json_value_impl(const char **pp, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        errno = EINVAL;
        return -1;
    }

    const char *p = skip_ws(*pp);
    if (*p == '"') {
        if (skip_json_string(&p) != 0) return -1;
        *pp = p;
        return 0;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        p++;
        while (*p) {
            p = skip_ws(p);
            if (*p == close) { *pp = p + 1; return 0; }
            if (open == '{') {
                if (skip_json_string(&p) != 0) return -1;
                p = skip_ws(p);
                if (*p != ':') return -1;
                p++;
            }
            if (skip_json_value_impl(&p, depth + 1) != 0) return -1;
            p = skip_ws(p);
            if (*p == ',') p++;
        }
        return -1;
    }
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '}' && *p != ']') p++;
    *pp = p;
    return 0;
}

static int skip_json_value(const char **pp) {
    return skip_json_value_impl(pp, 0);
}

static int json_find_key(const char *j, const char *k, const char **v) {
    const char *p = skip_ws(j);
    if (*p != '{') return -1;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return -1;
        bool key_match = false;
        if (match_json_string(p, k, &p, &key_match) != 0) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p = skip_ws(p + 1);
        if (key_match) {
            *v = p;
            return 0;
        }
        if (skip_json_value(&p) != 0) return -1;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    return -1;
}

static int json_get_i64(const char *j, const char *k, int64_t *out) {
    const char *v;
    if (json_find_key(j, k, &v) != 0) {
        return -1;
    }
    if (*v == '+') return -1;
    char *end = NULL;
    errno = 0;
    long long x = strtoll(v, &end, 10);
    if (end == v || errno != 0) return -1;
    end = (char *)skip_ws(end);
    if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
    *out = x;
    return 0;
}

static int json_get_bool(const char *j, const char *k, bool *out) {
    const char *v;
    if (json_find_key(j, k, &v) != 0) {
        return -1;
    }
    v = skip_ws(v);
    if (strncmp(v, "true", 4) == 0) {
        const char *end = skip_ws(v + 4);
        if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
        *out = true;
        return 0;
    }
    if (strncmp(v, "false", 5) == 0) {
        const char *end = skip_ws(v + 5);
        if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
        *out = false;
        return 0;
    }
    return -1;
}

static int json_get_u64(const char *j, const char *k, uint64_t *out) {
    const char *v;
    if (json_find_key(j, k, &v) != 0) {
        return -1;
    }
    if (*v == '+' || *v == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    if (end == v || errno != 0) return -1;
    end = (char *)skip_ws(end);
    if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
    *out = x;
    return 0;
}

static int json_get_str(const char *j, const char *k, char *out, size_t n) {
    const char *v;
    if (json_find_key(j, k, &v) != 0) return -1;
    return parse_json_string(skip_ws(v), out, n, NULL);
}

static int json_get_argv_display(const char *j, char *out, size_t n) {
    const char *v;
    if (json_find_key(j, "argv", &v) != 0 || *v != '[') {
        return -1;
    }
    v = skip_ws(v + 1);
    size_t off = 0;
    bool first = true;
    while (*v && *v != ']') {
        char arg[SIGMUND_PATH_MAX];
        if (parse_json_string(v, arg, sizeof(arg), &v) != 0) {
            return -1;
        }
        if (!first) {
            if (off + 1 >= n) return -1;
            out[off++] = ' ';
            out[off] = '\0';
        }
        if (append_cmd_escaped(out, n, &off, arg) != 0) {
            return -1;
        }
        first = false;
        v = skip_ws(v);
        if (*v == ',') {
            v = skip_ws(v + 1);
        } else if (*v != ']') {
            return -1;
        }
    }
    if (*v != ']') return -1;
    return 0;
}

static int read_owned_file_no_symlink(const char *path, char **out) {
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

static int load_record(const char *path, struct record *r) {
    memset(r, 0, sizeof(*r));
    char *j = NULL;
    if (read_owned_file_no_symlink(path, &j) != 0) {
        return -1;
    }

    int64_t tmp = 0;
    if (json_get_i64(j, "version", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->version = (int)tmp;
    if (json_get_str(j, "id", r->id, sizeof(r->id)) != 0) {
        free(j);
        return -1;
    }
    if (json_get_str(j, "run_id", r->run_id, sizeof(r->run_id)) != 0) {
        snprintf(r->run_id, sizeof(r->run_id), "%s", r->id);
    }
    if (json_get_i64(j, "pid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->pid = (pid_t)tmp;
    if (json_get_i64(j, "pgid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->pgid = (pid_t)tmp;
    if (json_get_i64(j, "sid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->sid = (pid_t)tmp;
    if (json_get_i64(j, "start_unix_ns", &r->start_unix_ns) != 0) {
        free(j);
        return -1;
    }
    if (json_get_i64(j, "uid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->uid = (uid_t)tmp;
    if (json_get_i64(j, "gid", &tmp) != 0) {
        free(j);
        return -1;
    }
    r->gid = (gid_t)tmp;
    if (json_get_i64(j, "invoked_by_uid", &tmp) == 0) {
        r->has_invocation = true;
        r->invoked_by_uid = (uid_t)tmp;
    }
    if (json_get_i64(j, "invoked_by_gid", &tmp) == 0) {
        r->has_invocation = true;
        r->invoked_by_gid = (gid_t)tmp;
    }
    if (json_get_str(j, "invoked_by_user", r->invoked_by_user, sizeof(r->invoked_by_user)) == 0) {
        r->has_invocation = true;
    }
    if (json_get_bool(j, "invoked_via_sudo", &r->invoked_via_sudo) == 0) {
        r->has_invocation = true;
    }
    if (json_get_str(j, "log_path", r->log_path, sizeof(r->log_path)) == 0) {
        r->has_log = true;
    }
    if (json_get_str(j, "boot_id", r->boot_id, sizeof(r->boot_id)) == 0) {
        r->has_boot = true;
    }
    if (json_get_str(j, "started_at", r->started_at, sizeof(r->started_at)) == 0) {
        r->has_started_at = true;
    }
    if (json_get_str(j, "ended_at", r->ended_at, sizeof(r->ended_at)) == 0) {
        r->has_ended_at = true;
    }
    if (json_get_str(j, "state", r->state, sizeof(r->state)) == 0) {
        r->has_state = true;
    }
    if (json_get_i64(j, "exit_code", &tmp) == 0) {
        r->has_exit_code = true;
        r->exit_code = (int)tmp;
    }
    if (json_get_i64(j, "term_signal", &tmp) == 0) {
        r->has_term_signal = true;
        r->term_signal = (int)tmp;
    }
    if (json_get_str(j, "launch_error", r->launch_error, sizeof(r->launch_error)) == 0) {
        r->has_launch_error = true;
    }
    if (json_get_u64(j, "proc_starttime_ticks", &r->proc_starttime_ticks) != 0 ||
        json_get_u64(j, "exe_dev", &r->exe_dev) != 0 ||
        json_get_u64(j, "exe_ino", &r->exe_ino) != 0) {
        free(j);
        return -1;
    }
    if (json_get_str(j, "cmdline_display", r->cmdline, sizeof(r->cmdline)) != 0) {
        if (json_get_argv_display(j, r->cmdline, sizeof(r->cmdline)) != 0) {
            snprintf(r->cmdline, sizeof(r->cmdline), "?");
        }
    }
    free(j);
    return 0;
}

static int read_small_file(const char *path, char **out) {
    return read_owned_file_no_symlink(path, out);
}

static int load_public_index(const char *path, struct public_index *pi) {
    memset(pi, 0, sizeof(*pi));
    char *j = NULL;
    if (read_small_file(path, &j) != 0) {
        return -1;
    }
    if (json_get_str(j, "id", pi->id, sizeof(pi->id)) != 0 || !valid_id(pi->id)) {
        free(j);
        return -1;
    }
    if (json_get_bool(j, "root_managed", &pi->root_managed) != 0) {
        pi->root_managed = true;
    }
    if (json_get_bool(j, "requires_elevation", &pi->requires_elevation) != 0) {
        pi->requires_elevation = true;
    }
    if (json_get_str(j, "state_hint", pi->state_hint, sizeof(pi->state_hint)) != 0) {
        snprintf(pi->state_hint, sizeof(pi->state_hint), "%s", "unknown");
    }
    if (json_get_str(j, "started_at", pi->started_at, sizeof(pi->started_at)) != 0) {
        snprintf(pi->started_at, sizeof(pi->started_at), "%s", "-");
    }
    free(j);
    return 0;
}

static int load_public_index_by_id(const struct store_paths *store, const char *id, struct public_index *pi) {
    if (!valid_id(id)) {
        return -1;
    }
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) != 0) {
        return -1;
    }
    if (load_public_index(path, pi) != 0 || strcmp(pi->id, id) != 0) {
        return -1;
    }
    return 0;
}

static enum run_state eval_state(const struct record *r, const char *current_boot) {
    if ((r->has_state && strcmp(r->state, "failed") == 0) || (r->has_launch_error && r->launch_error[0] != '\0')) {
        return STATE_FAILED;
    }
    if (r->pgid <= 1 || r->sid <= 0) {
        return STATE_UNKNOWN;
    }
    if (r->has_boot && current_boot && strcmp(r->boot_id, current_boot) != 0) {
        return STATE_STALE;
    }

    char state = 0;
    uint64_t now_starttime = 0;
    bool has_stat = read_proc_stat_tokens(r->pid, &state, &now_starttime) == 0;
    bool present = has_stat || leader_present(r->pid);
    enum group_liveness gl = group_session_liveness(r->pgid, r->sid);

    if (has_stat && state == 'Z') {
        if (gl == GROUP_LIVE) {
            return STATE_RUNNING;
        }
        if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
            return STATE_EXITED;
        }
        return STATE_UNKNOWN;
    }

    if (present) {
        if (r->proc_starttime_ticks && has_stat) {
            if (now_starttime != r->proc_starttime_ticks) {
                return STATE_STALE;
            }
        } else if (r->exe_dev && r->exe_ino) {
            uint64_t d, i;
            if (read_proc_exe(r->pid, &d, &i) == 0 && (d != r->exe_dev || i != r->exe_ino)) {
                return STATE_STALE;
            }
        }
        return STATE_RUNNING;
    }

    if (gl == GROUP_LIVE) {
        return STATE_RUNNING;
    }
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return STATE_EXITED;
    }

    int g = group_exists(r->pgid);
    if (g == 0) {
        return STATE_EXITED;
    }
    return STATE_UNKNOWN;
}

static int tail_log_until_exit(const struct record *r, bool from_end, bool follow_until_exit) {
    int fd = open(r->log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        die_errno("sigmund: failed to open log for tail");
    }

    char boot[128] = {0};
    bool have_boot = r->has_boot && current_boot_id(boot, sizeof(boot));
    if (from_end) {
        lseek(fd, 0, SEEK_END);
    }

    struct sigaction sa = {0}, old_sa = {0};
    sa.sa_handler = handle_tail_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);
    g_tail_interrupted = 0;

    char buf[4096];
    int sleep_polls = 0;
    while (!g_tail_interrupted) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
                close(fd);
                sigaction(SIGINT, &old_sa, NULL);
                die_errno("sigmund: failed writing tailed output");
            }
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            sigaction(SIGINT, &old_sa, NULL);
            die_errno("sigmund: failed while tailing log");
        }
        if (!follow_until_exit) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = 100 * 1000000L};
        nanosleep(&sl, NULL);
        sleep_polls++;
        if (sleep_polls % 10 == 0) {
            enum run_state st = eval_state(r, have_boot ? boot : NULL);
            if (st != STATE_RUNNING) {
                break;
            }
        }
    }

    close(fd);
    sigaction(SIGINT, &old_sa, NULL);
    return 0;
}

static void rollback_spawned_group(pid_t pid, pid_t pgid) {
    if (pgid > 1) {
        kill(-pgid, SIGKILL);
    }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
}

static int read_exec_handshake(int fd, int *child_errno) {
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

static int perform_start(const struct invocation *inv, const struct store_paths *store, bool tail, int argc, char **argv) {
    if (argc <= 0 || !argv || !argv[0]) {
        usage();
        return 5;
    }

    char id[16], log_path[SIGMUND_PATH_MAX], reserve_path[SIGMUND_PATH_MAX], boot_id[128] = {0};
    bool has_boot = current_boot_id(boot_id, sizeof(boot_id));
    struct store_paths system_hint;
    struct store_paths invoking_user_store;
    const struct store_paths *avoid_public_store = NULL;
    const struct store_paths *avoid_user_store = NULL;

    if (store->kind == STORE_USER_LOCAL) {
        if (init_system_store(&system_hint) == 0) {
            avoid_public_store = &system_hint;
        }
    } else if (inv && inv->have_sudo_user && inv->invoking_home[0]) {
        if (init_user_store_from_home(inv->invoking_home, &invoking_user_store) == 0) {
            avoid_user_store = &invoking_user_store;
        }
    }

    if (gen_id_for_store(store, avoid_public_store, avoid_user_store, id, sizeof(id)) != 0) {
        die_errno("sigmund: failed to generate id");
    }
    if (checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, id) != 0) {
        die_errno("sigmund: log path too long");
    }
    if (checked_snprintf(reserve_path, sizeof(reserve_path), "%s/.%s.reserve", store->record_dir, id) != 0) {
        die_errno("sigmund: reserve path too long");
    }

    int pipefd[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(pipefd, O_CLOEXEC) != 0)
#endif
    {
        if (pipe(pipefd) != 0) {
            int saved = errno;
            unlink(reserve_path);
            errno = saved;
            die_errno("sigmund: pipe failed");
        }
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(pipefd[0]);
            close(pipefd[1]);
            unlink(reserve_path);
            errno = saved;
            die_errno("sigmund: pipe setup failed");
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(reserve_path);
        errno = saved;
        die_errno("sigmund: fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (setsid() < 0) {
            int e = errno;
            write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) {
            int e = errno;
            write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (nullfd > 2) {
            close(nullfd);
        }

        int lfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (lfd < 0 || dup2(lfd, STDOUT_FILENO) < 0 || dup2(lfd, STDERR_FILENO) < 0) {
            int e = errno;
            write_all(pipefd[1], &e, sizeof(e));
            _exit(127);
        }
        if (lfd > 2) {
            close(lfd);
        }
        execvp(argv[0], argv);
        int e = errno;
        write_all(pipefd[1], &e, sizeof(e));
        _exit(127);
    }

    close(pipefd[1]);
    int child_errno = 0;
    int handshake = read_exec_handshake(pipefd[0], &child_errno);
    int handshake_errno = errno;
    close(pipefd[0]);
    if (handshake < 0) {
        rollback_spawned_group(pid, pid);
        unlink(reserve_path);
        unlink(log_path);
        errno = handshake_errno;
        die_errno("sigmund: exec handshake failed");
    }
    if (handshake > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
        fprintf(stderr, "sigmund: exec failed: %s\n", strerror(child_errno));
        unlink(reserve_path);
        unlink(log_path);
        return 1;
    }

    struct record r = {0};
    r.version = 1;
    if (checked_snprintf(r.id, sizeof(r.id), "%s", id) != 0) {
        die_errno("sigmund: id too long");
    }
    if (checked_snprintf(r.run_id, sizeof(r.run_id), "%s", id) != 0) {
        die_errno("sigmund: id too long");
    }
    r.pid = pid;
    r.pgid = pid;
    r.sid = pid;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    r.start_unix_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    format_rfc3339_utc_from_ns(r.start_unix_ns, r.started_at, sizeof(r.started_at));
    r.has_started_at = true;
    snprintf(r.state, sizeof(r.state), "running");
    r.has_state = true;
    r.uid = geteuid();
    r.gid = getegid();
    if (store->kind == STORE_SYSTEM_MANAGED) {
        r.has_invocation = true;
        if (inv && inv->have_sudo_user) {
            r.invoked_by_uid = inv->invoking_uid;
            r.invoked_by_gid = inv->invoking_gid;
            if (checked_snprintf(r.invoked_by_user, sizeof(r.invoked_by_user), "%s", inv->invoking_user) != 0) {
                die_errno("sigmund: invoking user too long");
            }
            r.invoked_via_sudo = true;
        } else {
            r.invoked_by_uid = 0;
            r.invoked_by_gid = 0;
            if (checked_snprintf(r.invoked_by_user, sizeof(r.invoked_by_user), "%s", "root") != 0) {
                die_errno("sigmund: invoking user too long");
            }
            r.invoked_via_sudo = false;
        }
    }
    r.has_log = true;
    if (checked_snprintf(r.log_path, sizeof(r.log_path), "%s", log_path) != 0) {
        die_errno("sigmund: log path too long");
    }
    r.has_boot = has_boot;
    if (r.has_boot) {
        snprintf(r.boot_id, sizeof(r.boot_id), "%s", boot_id);
    }
    read_proc_stat_tokens(pid, NULL, &r.proc_starttime_ticks);
    read_proc_exe(pid, &r.exe_dev, &r.exe_ino);
    size_t off = 0;
    r.cmdline[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            if (off + 1 >= sizeof(r.cmdline)) {
                break;
            }
            r.cmdline[off++] = ' ';
            r.cmdline[off] = '\0';
        }
        if (append_cmd_escaped(r.cmdline, sizeof(r.cmdline), &off, argv[i]) != 0) {
            break;
        }
    }

    char record_path[SIGMUND_PATH_MAX] = {0};
    if (getenv("SIGMUND_TEST_FAIL_RECORD_WRITE")) {
        errno = EIO;
    } else if (write_record_atomic(store->record_dir, &r, argc, argv, record_path, sizeof(record_path)) == 0) {
        if (store->kind == STORE_SYSTEM_MANAGED) {
            int public_rc = 0;
            if (getenv("SIGMUND_TEST_FAIL_PUBLIC_INDEX_WRITE")) {
                errno = EIO;
                public_rc = -1;
            } else if (write_public_index_atomic(store, &r) != 0) {
                public_rc = -1;
            }
            if (public_rc != 0) {
                int saved = errno;
                if (saved == 0) {
                    saved = EIO;
                }
                rollback_spawned_group(pid, pid);
                if (record_path[0]) {
                    unlink(record_path);
                }
                unlink(log_path);
                unlink(reserve_path);
                char public_path[SIGMUND_PATH_MAX];
                if (checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                    unlink(public_path);
                }
                errno = saved;
                die_errno("sigmund: failed to write public index");
            }
        }
        printf("sigmund: id=%s pid=%ld pgid=%ld sid=%ld\n", r.id, (long)r.pid, (long)r.pgid, (long)r.sid);
        printf("sigmund: log: %s\n", r.log_path);
        printf("sigmund: stop: sigmund stop %s\n", r.id);
        fflush(stdout);

        if (tail) {
            return tail_log_until_exit(&r, false, true);
        }
        return 0;
    }
    {
        int saved = errno;
        rollback_spawned_group(pid, pid);
        unlink(reserve_path);
        unlink(log_path);
        errno = saved;
        die_errno("sigmund: failed to write record");
    }
    return 1;
}

static int load_record_by_id(const char *dir, const char *id, struct record *r, char *path, size_t n) {
    if (!valid_id(id)) {
        return -1;
    }
    if (checked_snprintf(path, n, "%s/%s.json", dir, id) != 0) {
        return -1;
    }
    if (load_record(path, r) != 0 || !valid_record(r) || strcmp(r->id, id) != 0) {
        return -1;
    }
    return 0;
}

static bool target_group_gone(const struct record *r) {
    enum group_liveness gl = group_session_liveness(r->pgid, r->sid);
    if (gl == GROUP_EMPTY || gl == GROUP_ZOMBIE_ONLY) {
        return true;
    }
    if (gl == GROUP_LIVE) {
        return false;
    }
    return group_exists(r->pgid) == 0;
}

static bool wait_target_group_gone(const struct record *r, int timeout_ms) {
    int waited = 0;
    while (waited <= timeout_ms) {
        if (target_group_gone(r)) {
            return true;
        }
        if (waited == timeout_ms) {
            break;
        }
        struct timespec sl = {.tv_sec = 0, .tv_nsec = POLL_SLEEP_MS * 1000000L};
        while (nanosleep(&sl, &sl) != 0 && errno == EINTR) {
            continue;
        }
        waited += POLL_SLEEP_MS;
    }
    return false;
}

static int do_signal_action(const struct store_paths *store, const char *id, int sig, bool graceful) {
    struct record r;
    char path[SIGMUND_PATH_MAX], boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    if (r.pgid <= 1) {
        fprintf(stderr, "sigmund: error: invalid pgid %ld in record file\n", (long)r.pgid);
        return 5;
    }
    if (r.has_boot && have_boot && strcmp(r.boot_id, boot) != 0) {
        fprintf(stderr, "sigmund: error: run %s is stale (record boot_id differs from current boot)\n", id);
        return 2;
    }

    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    if (st == STATE_STALE) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }
    if (st == STATE_EXITED || st == STATE_FAILED) {
        return 0;
    }
    if (st == STATE_UNKNOWN) {
        fprintf(stderr, "sigmund: error: run %s could not be validated and cannot be signaled\n", id);
        return 2;
    }

    if (kill(-r.pgid, sig) != 0) {
        if (errno == EPERM) {
            return 3;
        }
        if (errno == ESRCH) {
            return 0;
        }
        return 4;
    }

    if (graceful) {
        if (wait_target_group_gone(&r, STOP_TIMEOUT_MS)) {
            report_session_escapees(&r);
            return 0;
        }
        if (kill(-r.pgid, SIGKILL) != 0 && errno != ESRCH) {
            if (errno == EPERM) {
                return 3;
            }
            return 4;
        }
        if (wait_target_group_gone(&r, 1000)) {
            report_session_escapees(&r);
            return 0;
        }
        return 4;
    }
    report_session_escapees(&r);
    return 0;
}

static const char *state_str(enum run_state s) {
    switch (s) {
    case STATE_RUNNING:
        return "running";
    case STATE_EXITED:
        return "exited";
    case STATE_STALE:
        return "stale";
    case STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n) {
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

static void format_result(const struct record *r, enum run_state st, char *out, size_t n) {
    if (st == STATE_RUNNING) {
        snprintf(out, n, "-");
        return;
    }
    if (r->has_launch_error && r->launch_error[0]) {
        snprintf(out, n, "launch=%.48s", r->launch_error);
        return;
    }
    if (r->has_term_signal) {
        snprintf(out, n, "signal=%d", r->term_signal);
        return;
    }
    if (r->has_exit_code) {
        snprintf(out, n, "exit=%d", r->exit_code);
        return;
    }
    if (st == STATE_FAILED) {
        snprintf(out, n, "launch=unknown");
    } else {
        snprintf(out, n, "-");
    }
}

static void print_list_header(void) {
    printf("%-10s %-8s %-24s %-14s %s\n", "RUNID", "STATE", "STARTED_AT", "RESULT", "CMD");
}

static int cmd_list_private(const struct store_paths *store, bool print_header) {
    DIR *d = opendir(store->record_dir);
    if (print_header) {
        print_list_header();
    }
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct record r;
        if (load_record(path, &r) != 0) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (!valid_record(&r)) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        char started_at[64];
        if (r.has_started_at && r.started_at[0]) {
            snprintf(started_at, sizeof(started_at), "%s", r.started_at);
        } else {
            format_rfc3339_utc_from_ns(r.start_unix_ns, started_at, sizeof(started_at));
        }
        char result[64];
        format_result(&r, st, result, sizeof(result));
        char cmd[64];
        const char *cmd_src = r.cmdline[0] ? r.cmdline : "?";
        snprintf(cmd, sizeof(cmd), "%.48s%s", cmd_src, strlen(cmd_src) > 48 ? "..." : "");
        printf("%-10s %-8s %-24s %-14s %s\n", r.id, state_str(st), started_at, result, cmd);
    }
    closedir(d);
    return 0;
}

static int cmd_list_public(const struct store_paths *store, bool print_header) {
    DIR *d = opendir(store->public_dir);
    if (print_header) {
        print_list_header();
    }
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char file_id[16];
        memcpy(file_id, e->d_name, len - 5);
        file_id[len - 5] = '\0';
        if (!valid_id(file_id)) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct public_index pi;
        if (load_public_index(path, &pi) != 0 || strcmp(pi.id, file_id) != 0) {
            continue;
        }
        char state[16];
        snprintf(state, sizeof(state), "%s", "unknown");
        char started[64];
        if (checked_snprintf(started, sizeof(started), "%s", pi.started_at[0] ? pi.started_at : "-") != 0) {
            snprintf(started, sizeof(started), "%s", "-");
        }
        printf("%-10s %-8s %-24s %-14s %s\n", pi.id, state, started, "-", "<root-managed>");
    }
    closedir(d);
    return 0;
}

static int cmd_list_normal(const struct store_paths *user_store, const struct store_paths *system_store) {
    print_list_header();
    cmd_list_private(user_store, false);
    cmd_list_public(system_store, false);
    return 0;
}

static int cmd_list_system(const struct store_paths *system_store) {
    return cmd_list_private(system_store, true);
}

static int resolve_run_id(const char *dir, const char *input, char *resolved, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (valid_id(input)) {
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s.json", dir, input) == 0 && access(path, F_OK) == 0) {
            return checked_snprintf(resolved, n, "%s", input);
        }
    }
    if (!valid_id_prefix(input)) {
        return -1;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    int matches = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[32];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (strncmp(id, input, strlen(input)) == 0) {
            matches++;
            if (checked_snprintf(resolved, n, "%s", id) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return (matches == 1) ? 0 : -1;
}

static void unlink_public_index(const struct store_paths *store, const char *id) {
    if (store->kind != STORE_SYSTEM_MANAGED || !id || !*id) {
        return;
    }
    char path[SIGMUND_PATH_MAX];
    if (checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0) {
        unlink(path);
    }
}

static int prune_one_run(const struct store_paths *store, const char *id, const char *boot, bool allow_stale, bool *removed) {
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum run_state st = eval_state(&r, boot ? boot : NULL);
    bool prunable = (st == STATE_EXITED || st == STATE_FAILED || (allow_stale && st == STATE_STALE));
    if (!prunable) {
        fprintf(stderr, "sigmund: error: run %s is %s and cannot be pruned\n", id, state_str(st));
        return 2;
    }
    unlink(path);
    if (r.has_log) {
        unlink(r.log_path);
    }
    unlink_public_index(store, id);
    if (removed) {
        *removed = true;
    }
    return 0;
}

static int cmd_prune_store_all(const struct store_paths *store, bool include_stale) {
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));

    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct record r;
        if (load_record(path, &r) != 0) {
            unlink(path);
            continue;
        }
        if (!valid_record(&r)) {
            unlink(path);
            continue;
        }
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        if (st == STATE_EXITED || st == STATE_FAILED || (include_stale && st == STATE_STALE)) {
            unlink(path);
            if (r.has_log) {
                unlink(r.log_path);
            }
            unlink_public_index(store, r.id);
        }
    }
    closedir(d);

    d = opendir(store->log_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".log")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 4) {
            continue;
        }
        char id[32];
        size_t id_len = len - 4;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        char json_path[SIGMUND_PATH_MAX], log_path[SIGMUND_PATH_MAX];
        if (checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0) {
            continue;
        }
        if (checked_snprintf(log_path, sizeof(log_path), "%s/%s", store->log_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(log_path);
        }
    }
    closedir(d);
    return 0;
}

enum id_token_scope { ID_TOKEN_PLAIN, ID_TOKEN_USER, ID_TOKEN_SYSTEM, ID_TOKEN_INVALID };

static enum id_token_scope parse_id_token(const char *token, const char **id_out) {
    if (!token || !*token) {
        return ID_TOKEN_INVALID;
    }
    if (strncmp(token, "user:", 5) == 0) {
        *id_out = token + 5;
        return **id_out ? ID_TOKEN_USER : ID_TOKEN_INVALID;
    }
    if (strncmp(token, "system:", 7) == 0) {
        *id_out = token + 7;
        return **id_out ? ID_TOKEN_SYSTEM : ID_TOKEN_INVALID;
    }
    *id_out = token;
    return ID_TOKEN_PLAIN;
}

static int init_invoking_user_store(const struct invocation *inv, struct store_paths *store) {
    if (!inv || !inv->have_sudo_user || !inv->invoking_home[0]) {
        errno = EINVAL;
        return -1;
    }
    return init_user_store_from_home(inv->invoking_home, store);
}

static int resolve_user_store_id(const struct store_paths *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_private_id(const struct store_paths *store, const char *id, char *resolved, size_t n) {
    return resolve_run_id(store->record_dir, id, resolved, n);
}

static int resolve_system_public_id(const struct store_paths *store, const char *id, char *resolved, size_t n) {
    if (resolve_run_id(store->public_dir, id, resolved, n) != 0) {
        return -1;
    }
    struct public_index pi;
    if (load_public_index_by_id(store, resolved, &pi) != 0 || !pi.root_managed) {
        return -1;
    }
    return 0;
}

static int resolve_target(const struct invocation *inv,
                          const struct store_paths *current_user_store,
                          const struct store_paths *system_store,
                          const char *token,
                          struct resolved_target *out) {
    memset(out, 0, sizeof(*out));
    out->scope = RESOLVE_NOT_FOUND;

    const char *id = NULL;
    enum id_token_scope token_scope = parse_id_token(token, &id);
    if (token_scope == ID_TOKEN_INVALID || !valid_id_prefix(id)) {
        fprintf(stderr, "sigmund: error: invalid run id '%s'\n", token ? token : "");
        out->scope = RESOLVE_ERROR;
        return -1;
    }

    if (inv->euid_root) {
        if (token_scope == ID_TOKEN_USER) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", id);
                out->scope = RESOLVE_ERROR;
                return -1;
            }
            char resolved[16];
            if (resolve_user_store_id(&user_store, id, resolved, sizeof(resolved)) == 0) {
                out->scope = RESOLVE_USER_LOCAL;
                out->store = user_store;
                checked_snprintf(out->id, sizeof(out->id), "%s", resolved);
                return 0;
            }
            return 0;
        }
        if (token_scope == ID_TOKEN_SYSTEM) {
            char resolved[16];
            if (resolve_system_private_id(system_store, id, resolved, sizeof(resolved)) == 0) {
                out->scope = RESOLVE_SYSTEM_MANAGED;
                out->store = *system_store;
                checked_snprintf(out->id, sizeof(out->id), "%s", resolved);
                return 0;
            }
            return 0;
        }

        char root_resolved[16] = {0};
        char user_resolved[16] = {0};
        bool root_match = resolve_system_private_id(system_store, id, root_resolved, sizeof(root_resolved)) == 0;
        bool user_match = false;
        struct store_paths user_store;
        if (inv->have_sudo_user && init_invoking_user_store(inv, &user_store) == 0) {
            user_match = resolve_user_store_id(&user_store, id, user_resolved, sizeof(user_resolved)) == 0;
        }
        if (root_match) {
            (void)user_match;
            out->scope = RESOLVE_SYSTEM_MANAGED;
            out->store = *system_store;
            checked_snprintf(out->id, sizeof(out->id), "%s", root_resolved);
            return 0;
        }
        if (user_match) {
            out->scope = RESOLVE_USER_LOCAL;
            out->store = user_store;
            checked_snprintf(out->id, sizeof(out->id), "%s", user_resolved);
            return 0;
        }
        return 0;
    }

    if (token_scope == ID_TOKEN_USER) {
        char resolved[16];
        if (resolve_user_store_id(current_user_store, id, resolved, sizeof(resolved)) == 0) {
            out->scope = RESOLVE_USER_LOCAL;
            out->store = *current_user_store;
            checked_snprintf(out->id, sizeof(out->id), "%s", resolved);
            return 0;
        }
        return 0;
    }
    if (token_scope == ID_TOKEN_SYSTEM) {
        char resolved[16];
        if (resolve_system_public_id(system_store, id, resolved, sizeof(resolved)) == 0) {
            out->scope = RESOLVE_SYSTEM_MANAGED;
            out->store = *system_store;
            out->needs_elevation = true;
            checked_snprintf(out->id, sizeof(out->id), "%s", resolved);
            return 0;
        }
        return 0;
    }

    char user_resolved[16];
    if (resolve_user_store_id(current_user_store, id, user_resolved, sizeof(user_resolved)) == 0) {
        out->scope = RESOLVE_USER_LOCAL;
        out->store = *current_user_store;
        checked_snprintf(out->id, sizeof(out->id), "%s", user_resolved);
        return 0;
    }
    char system_resolved[16];
    if (resolve_system_public_id(system_store, id, system_resolved, sizeof(system_resolved)) == 0) {
        out->scope = RESOLVE_SYSTEM_MANAGED;
        out->store = *system_store;
        out->needs_elevation = true;
        checked_snprintf(out->id, sizeof(out->id), "%s", system_resolved);
        return 0;
    }
    return 0;
}

static int report_not_found(const char *token) {
    fprintf(stderr, "sigmund: error: no run matches '%s'\n", token ? token : "");
    return 5;
}

static int resolve_self_executable_path(const char *argv0, char *out, size_t n) {
    if (!out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    out[0] = '\0';
#if defined(__linux__)
    char proc_path[SIGMUND_PATH_MAX];
    ssize_t got = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
    if (got > 0) {
        proc_path[got] = '\0';
        return checked_snprintf(out, n, "%s", proc_path);
    }
#endif
#if defined(__APPLE__)
    char mac_path[SIGMUND_PATH_MAX];
    uint32_t mac_size = (uint32_t)sizeof(mac_path);
    if (_NSGetExecutablePath(mac_path, &mac_size) == 0) {
        char resolved[SIGMUND_PATH_MAX];
        if (realpath(mac_path, resolved)) {
            return checked_snprintf(out, n, "%s", resolved);
        }
    }
#endif
    if (argv0 && strchr(argv0, '/')) {
        char resolved[SIGMUND_PATH_MAX];
        if (realpath(argv0, resolved)) {
            return checked_snprintf(out, n, "%s", resolved);
        }
    }
    errno = ENOENT;
    return -1;
}

static int child_status_to_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 3;
}

static int elevate_with_sudo_canonical(const char *program, int canonical_argc, char **canonical_argv) {
    char abs_sigmund[SIGMUND_PATH_MAX];
    if (resolve_self_executable_path(program, abs_sigmund, sizeof(abs_sigmund)) != 0) {
        fprintf(stderr, "sigmund: cannot determine executable path for sudo self-elevation\n");
        return 3;
    }

    int argc = 5 + canonical_argc;
    char **sudo_argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!sudo_argv) {
        return 3;
    }
    sudo_argv[0] = "sudo";
    sudo_argv[1] = "--";
    sudo_argv[2] = abs_sigmund;
    sudo_argv[3] = "--system";
    sudo_argv[4] = "--elevated";
    for (int i = 0; i < canonical_argc; i++) {
        sudo_argv[5 + i] = canonical_argv[i];
    }
    sudo_argv[argc] = NULL;

    const char *sudo_prog = "sudo";
#ifdef SIGMUND_TESTING
    const char *test_sudo_prog = getenv("SIGMUND_TEST_SUDO_PROG");
    if (test_sudo_prog && *test_sudo_prog) {
        sudo_prog = test_sudo_prog;
    }
#endif

    fflush(NULL);

    struct sigaction ign;
    struct sigaction old_int;
    struct sigaction old_quit;
    bool have_old_int = false;
    bool have_old_quit = false;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGINT, &ign, &old_int) == 0) {
        have_old_int = true;
    }
    if (sigaction(SIGQUIT, &ign, &old_quit) == 0) {
        have_old_quit = true;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        free(sudo_argv);
        errno = saved;
        fprintf(stderr, "sigmund: failed to fork sudo: %s\n", strerror(errno));
        return 3;
    }
    if (pid == 0) {
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        execvp(sudo_prog, sudo_argv);
        int saved = errno;
        fprintf(stderr, "sigmund: failed to exec sudo: %s\n", strerror(saved));
        _exit(127);
    }

    free(sudo_argv);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        int saved = errno;
        if (have_old_int) sigaction(SIGINT, &old_int, NULL);
        if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
        errno = saved;
        fprintf(stderr, "sigmund: failed to wait for sudo: %s\n", strerror(errno));
        return 3;
    }

    if (have_old_int) sigaction(SIGINT, &old_int, NULL);
    if (have_old_quit) sigaction(SIGQUIT, &old_quit, NULL);
    return child_status_to_exit_code(status);
}

static int elevate_with_sudo_parsed(const char *program, bool owned, const char *command, bool tail, bool force_raw, int argc, char **argv) {
    int extra = argc;
    if (owned) {
        extra += 1;
        if (!strcmp(command, "start") && tail) {
            extra += 1;
        }
    } else {
        if (tail) {
            extra += 1;
        }
        if (force_raw) {
            extra += 1;
        }
    }

    char **canon = calloc((size_t)extra, sizeof(char *));
    if (!canon) {
        return 3;
    }
    int n = 0;
    if (owned) {
        canon[n++] = (char *)command;
        if (!strcmp(command, "start") && tail) {
            canon[n++] = "--tail";
        }
    } else {
        if (tail) {
            canon[n++] = "--tail";
        }
        if (force_raw) {
            canon[n++] = "--";
        }
    }
    for (int i = 0; i < argc; i++) {
        canon[n++] = argv[i];
    }
    int rc = elevate_with_sudo_canonical(program, n, canon);
    free(canon);
    return rc;
}

static int elevate_with_sudo_targets(const char *program,
                               const char *command,
                               char **original_tokens,
                               const struct resolved_target *targets,
                               int ntargets) {
    int canonical_argc = 1 + ntargets;
    char **canon = calloc((size_t)canonical_argc, sizeof(char *));
    char **tokens = calloc((size_t)ntargets, sizeof(char *));
    if (!canon || !tokens) {
        free(canon);
        free(tokens);
        return 3;
    }

    canon[0] = (char *)command;
    for (int i = 0; i < ntargets; i++) {
        const char *orig_id = NULL;
        enum id_token_scope orig_scope = parse_id_token(original_tokens ? original_tokens[i] : NULL, &orig_id);
        const char *prefix = "";
        if (targets[i].scope == RESOLVE_USER_LOCAL) {
            prefix = "user:";
        } else if (orig_scope == ID_TOKEN_SYSTEM) {
            prefix = "system:";
        }
        size_t need = strlen(prefix) + strlen(targets[i].id) + 1;
        tokens[i] = malloc(need);
        if (!tokens[i]) {
            for (int j = 0; j < i; j++) free(tokens[j]);
            free(tokens);
            free(canon);
            return 3;
        }
        snprintf(tokens[i], need, "%s%s", prefix, targets[i].id);
        canon[1 + i] = tokens[i];
    }

    int rc = elevate_with_sudo_canonical(program, canonical_argc, canon);
    for (int i = 0; i < ntargets; i++) free(tokens[i]);
    free(tokens);
    free(canon);
    return rc;
}

static int cmd_signal_action(const struct invocation *inv,
                             const struct store_paths *user_store,
                             const struct store_paths *system_store,
                             const char *program,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful) {
    if (argc <= 0) {
        fprintf(stderr, "usage: sigmund %s <id>...\n", command);
        return 5;
    }
    struct resolved_target *targets = calloc((size_t)argc, sizeof(*targets));
    if (!targets) {
        return 3;
    }
    bool need_elevation = false;
    for (int i = 0; i < argc; i++) {
        if (resolve_target(inv, user_store, system_store, argv[i], &targets[i]) != 0) {
            free(targets);
            return 5;
        }
        if (targets[i].scope == RESOLVE_NOT_FOUND) {
            int rc = report_not_found(argv[i]);
            free(targets);
            return rc;
        }
        if (targets[i].needs_elevation) {
            need_elevation = true;
        }
    }
    if (need_elevation) {
        int rc = elevate_with_sudo_targets(program, command, argv, targets, argc);
        free(targets);
        return rc;
    }
    int worst = 0;
    for (int i = 0; i < argc; i++) {
        int rc = do_signal_action(&targets[i].store, targets[i].id, sig, graceful);
        if (rc > worst) {
            worst = rc;
        }
    }
    free(targets);
    return worst;
}

static int cmd_tail_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token) {
    struct resolved_target target;
    if (resolve_target(inv, user_store, system_store, id_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return report_not_found(id_token);
    }
    if (target.needs_elevation) {
        char *origv[1] = {(char *)id_token};
        return elevate_with_sudo_targets(program, "tail", origv, &target, 1);
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    return tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
}

static int cmd_dump_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token) {
    struct resolved_target target;
    if (resolve_target(inv, user_store, system_store, id_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return report_not_found(id_token);
    }
    if (target.needs_elevation) {
        char *origv[1] = {(char *)id_token};
        return elevate_with_sudo_targets(program, "dump", origv, &target, 1);
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        return 5;
    }
    int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        die_errno("sigmund: failed to open log for dump");
    }
    char buf[4096];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            die_errno("sigmund: failed while dumping log");
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            close(fd);
            die_errno("sigmund: failed writing dumped output");
        }
    }
    close(fd);
    return 0;
}

static int cmd_prune_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const char *target_token) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        const struct store_paths *store = inv->euid_root ? system_store : user_store;
        return cmd_prune_store_all(store, target_token && strcmp(target_token, "all") == 0);
    }
    struct resolved_target target;
    if (resolve_target(inv, user_store, system_store, target_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return report_not_found(target_token);
    }
    if (target.needs_elevation) {
        char *origv[1] = {(char *)target_token};
        return elevate_with_sudo_targets(program, "prune", origv, &target, 1);
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    bool removed = false;
    return prune_one_run(&target.store, target.id, have_boot ? boot : NULL, true, &removed);
}

static void usage(void) {
    printf("sigmund %s — More than nohup, less than systemd.\n\n"
           "start forms:\n"
           "  sigmund <cmd...>                  launch command in user-local state\n"
           "  sudo sigmund <cmd...>             launch command in root-managed state\n"
           "  sigmund --system <cmd...>         launch command in root-managed state via sudo\n"
           "  sigmund --tail <cmd...>           launch command and stream log output\n"
           "  sigmund start \"<cmd>\" --system  explicit system/root-managed start\n"
           "\n"
           "commands:\n"
           "  sigmund list                      list user-local and public root-managed runs\n"
           "  sigmund tail <id>                 follow existing log output\n"
           "  sigmund dump <id>                 print saved log output and exit\n"
           "  sigmund stop <id>...              graceful stop (SIGTERM → SIGKILL)\n"
           "  sigmund kill <id>...              immediate kill (SIGKILL)\n"
           "  sigmund killcmd <id>...           print kill command for scripting\n"
           "  sigmund prune                     remove exited/failed records and orphan logs\n"
           "  sigmund prune <id>                remove one prunable run and its log\n"
           "  sigmund prune all                 remove all prunable runs (stale+exited+failed)\n"
           "\n"
           "target forms:\n"
           "  <id>                              normal invocation-context resolution\n"
           "  user:<id>                         force user-local lookup\n"
           "  system:<id>                       force root-managed lookup\n"
           "\n"
           "switches:\n"
           "  --system                          run this invocation with root-managed authority\n"
           "  --tail                            start-mode switch (use with <cmd...>)\n"
           "\n"
           "note:\n"
           "  In raw start form, switches after the child command belong to the child.\n"
           "  Use 'sigmund -- <cmd...>' to run a command whose name overlaps\n"
           "  with a sigmund command (for example: sigmund -- list).\n",
           SIGMUND_VERSION);
}

static bool is_sigmund_owned_command(const char *s) {
    return s && (!strcmp(s, "list") || !strcmp(s, "stop") || !strcmp(s, "kill") ||
                 !strcmp(s, "tail") || !strcmp(s, "dump") || !strcmp(s, "prune") ||
                 !strcmp(s, "start") || !strcmp(s, "killcmd"));
}

static int perform_explicit_start(const struct invocation *inv,
                                  const struct store_paths *store,
                                  bool tail,
                                  int argc,
                                  char **argv) {
    if (argc <= 0) {
        fprintf(stderr, "usage: sigmund start <cmd> [args...]\n");
        return 5;
    }
    if (argc == 1) {
        char *shell_argv[4];
        shell_argv[0] = "sh";
        shell_argv[1] = "-c";
        shell_argv[2] = argv[0];
        shell_argv[3] = NULL;
        return perform_start(inv, store, tail, 3, shell_argv);
    }
    return perform_start(inv, store, tail, argc, argv);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    int argi = 1;
    bool requested_system = false;
    bool elevated = false;
    bool tail = false;
    bool force_raw = false;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--system")) {
            requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--elevated")) {
            elevated = true;
            requested_system = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--tail")) {
            tail = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--")) {
            force_raw = true;
            argi++;
            break;
        }
        break;
    }

    if (argi >= argc) {
        usage();
        return 5;
    }

    bool owned = !force_raw && !tail && is_sigmund_owned_command(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 3;
        }
        for (int i = argi; i < argc; i++) {
            if (!strcmp(argv[i], "--system")) {
                requested_system = true;
                continue;
            }
            if (!strcmp(argv[i], "--elevated")) {
                elevated = true;
                requested_system = true;
                continue;
            }
            if (!strcmp(command, "start") && !strcmp(argv[i], "--tail")) {
                tail = true;
                continue;
            }
            cmd_argv[cmd_argc++] = argv[i];
        }
    } else {
        command = NULL;
        cmd_argc = argc - argi;
        cmd_argv = argv + argi;
    }

    if (owned && !strcmp(command, "--version")) {
        puts(SIGMUND_VERSION);
        free(cmd_argv);
        return 0;
    }

    if (!owned && !force_raw && !tail && !strcmp(argv[argi], "--version")) {
        puts(SIGMUND_VERSION);
        return 0;
    }
    if (!owned && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        usage();
        return 0;
    }

    struct invocation inv;
    if (detect_invocation(&inv, requested_system, elevated) != 0) {
        die_errno("sigmund: failed to resolve invocation context");
    }
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "sigmund: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && !is_list) {
        int rc = elevate_with_sudo_parsed(argv[0], owned, command, tail, force_raw, cmd_argc, cmd_argv);
        if (owned) {
            free(cmd_argv);
        }
        return rc;
    }

    struct store_paths user_store;
    struct store_paths system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (init_system_store(&system_store) != 0) {
        die_errno("sigmund: failed to resolve system storage");
    }

    if (!inv.euid_root || is_list || (owned && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                                               !strcmp(command, "tail") || !strcmp(command, "dump") ||
                                               !strcmp(command, "prune") || !strcmp(command, "killcmd")))) {
        if (!inv.euid_root) {
            if (ensure_user_store_for_current_user(&user_store) != 0) {
                die_errno("sigmund: failed to init user storage");
            }
        }
    }

    if (!owned) {
        struct store_paths start_store;
        if (inv.euid_root || requested_system) {
            if (ensure_system_store(&start_store) != 0) {
                die_errno("sigmund: failed to init system storage");
            }
        } else {
            if (ensure_user_store_for_current_user(&start_store) != 0) {
                die_errno("sigmund: failed to init user storage");
            }
        }
        return perform_start(&inv, &start_store, tail, cmd_argc, cmd_argv);
    }

    if (!strcmp(command, "start")) {
        struct store_paths start_store;
        if (inv.euid_root || requested_system) {
            if (ensure_system_store(&start_store) != 0) {
                die_errno("sigmund: failed to init system storage");
            }
        } else {
            if (ensure_user_store_for_current_user(&start_store) != 0) {
                die_errno("sigmund: failed to init user storage");
            }
        }
        int rc = perform_explicit_start(&inv, &start_store, tail, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (inv.euid_root && requested_system) {
            rc = cmd_list_system(&system_store);
        } else if (inv.euid_root && !requested_system) {
            rc = cmd_list_system(&system_store);
        } else {
            rc = cmd_list_normal(&user_store, &system_store);
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "tail")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund tail <id>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_tail_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund dump <id>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_dump_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        int rc = cmd_prune_action(&inv, &user_store, &system_store, argv[0], target);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", cmd_argc, cmd_argv, SIGTERM, true);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "kill", cmd_argc, cmd_argv, SIGKILL, false);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "killcmd")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund killcmd <id>...\n");
            free(cmd_argv);
            return 5;
        }
        int worst = 0;
        const struct store_paths *store = inv.euid_root ? &system_store : &user_store;
        for (int i = 0; i < cmd_argc; i++) {
            struct record r;
            char path[SIGMUND_PATH_MAX];
            int rc = 0;
            char resolved[16];
            if (resolve_run_id(store->record_dir, cmd_argv[i], resolved, sizeof(resolved)) != 0 ||
                load_record_by_id(store->record_dir, resolved, &r, path, sizeof(path)) != 0) {
                rc = 5;
            } else if (r.pgid <= 1) {
                fprintf(stderr, "sigmund: error: invalid pgid %ld in record file\n", (long)r.pgid);
                rc = 5;
            } else {
                char boot[128] = {0};
                bool have_boot = current_boot_id(boot, sizeof(boot));
                enum run_state st = eval_state(&r, have_boot ? boot : NULL);
                if (r.has_boot && have_boot && strcmp(r.boot_id, boot) != 0) {
                    fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", cmd_argv[i]);
                    rc = 2;
                } else if (st == STATE_STALE) {
                    fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", cmd_argv[i]);
                    rc = 2;
                } else if (st == STATE_UNKNOWN) {
                    fprintf(stderr, "sigmund: error: run %s could not be validated and cannot be signaled\n", cmd_argv[i]);
                    rc = 2;
                } else {
                    printf("kill -TERM -- -%ld\n", (long)r.pgid);
                }
            }
            if (rc > worst) {
                worst = rc;
            }
        }
        free(cmd_argv);
        return worst;
    }

    free(cmd_argv);
    usage();
    return 1;
}
