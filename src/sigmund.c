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
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
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

#define ID_HEX_LEN 8
#define PROFILE_HASH_HEX_LEN 64
#define PROFILE_HASH_STR_LEN (PROFILE_HASH_HEX_LEN + 1)
#define ALIAS_MAX_LEN 64
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
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#define SIGMUND_NEED_SOCKET_CLOEXEC 1
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
    char alias[ALIAS_MAX_LEN + 1];
    char console_sock[SIGMUND_PATH_MAX];
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
    bool has_alias;
    bool has_console;
};

enum run_state { STATE_RUNNING, STATE_EXITED, STATE_STALE, STATE_FAILED, STATE_UNKNOWN };

enum store_kind { STORE_USER_LOCAL, STORE_SYSTEM_MANAGED };

struct store_paths {
    enum store_kind kind;
    char base[SIGMUND_PATH_MAX];
    char record_dir[SIGMUND_PATH_MAX];
    char log_dir[SIGMUND_PATH_MAX];
    char public_dir[SIGMUND_PATH_MAX];
    char console_dir[SIGMUND_PATH_MAX];
    char profile_path[SIGMUND_PATH_MAX];
    char alias_path[SIGMUND_PATH_MAX];
};

struct invocation {
    bool euid_root;
    bool requested_system;
    bool elevated;
    bool quiet;
    bool have_sudo_user;
    uid_t invoking_uid;
    gid_t invoking_gid;
    char invoking_user[128];
    char invoking_home[SIGMUND_PATH_MAX];
};

enum resolve_scope { RESOLVE_USER_LOCAL, RESOLVE_SYSTEM_MANAGED, RESOLVE_NOT_FOUND, RESOLVE_ERROR };

struct resolved_target {
    enum resolve_scope scope;
    char id[ALIAS_MAX_LEN + 1 + PROFILE_HASH_STR_LEN];
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    struct store_paths store;
    bool needs_elevation;
    bool has_capability;
};

struct public_index {
    char id[16];
    char alias[ALIAS_MAX_LEN + 1];
    bool root_managed;
    bool requires_elevation;
    bool has_alias;
    char state_hint[16];
    char started_at[64];
};

static volatile sig_atomic_t g_tail_interrupted = 0;
static int write_all(int fd, const void *buf, size_t n);
static void usage(void);
static int show_help(const char *topic);
static void format_rfc3339_utc_from_ns(int64_t unix_ns, char *out, size_t n);
static int read_exec_handshake(int fd, int *child_errno);

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
    if (len != ID_HEX_LEN) {
        return false;
    }
    if (strcmp(id, "00000000") == 0 || strcmp(id, "ffffffff") == 0) {
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
    if (len < 1 || len > ID_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool valid_profile_hash(const char *hash) {
    if (!hash || strlen(hash) != PROFILE_HASH_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < PROFILE_HASH_HEX_LEN; i++) {
        if (!isdigit((unsigned char)hash[i]) && !(hash[i] >= 'a' && hash[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool valid_alias(const char *alias) {
    if (!alias) {
        return false;
    }
    size_t len = strlen(alias);
    if (len == 0 || len > ALIAS_MAX_LEN || valid_profile_hash(alias)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)alias[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
        if (c == '/' || c == '.') {
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

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t off;
};

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256_ctx *c, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7], cc = c->h[2];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c) {
    c->h[0] = 0x6a09e667U; c->h[1] = 0xbb67ae85U; c->h[2] = 0x3c6ef372U; c->h[3] = 0xa54ff53aU;
    c->h[4] = 0x510e527fU; c->h[5] = 0x9b05688cU; c->h[6] = 0x1f83d9abU; c->h[7] = 0x5be0cd19U;
    c->len = 0;
    c->off = 0;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += (uint64_t)n * 8U;
    while (n > 0) {
        size_t take = 64 - c->off;
        if (take > n) take = n;
        memcpy(c->buf + c->off, p, take);
        c->off += take;
        p += take;
        n -= take;
        if (c->off == 64) {
            sha256_block(c, c->buf);
            c->off = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *c, unsigned char out[32]) {
    c->buf[c->off++] = 0x80;
    if (c->off > 56) {
        while (c->off < 64) c->buf[c->off++] = 0;
        sha256_block(c, c->buf);
        c->off = 0;
    }
    while (c->off < 56) c->buf[c->off++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->off++] = (unsigned char)(c->len >> (i * 8));
    sha256_block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

static void hex_encode(const unsigned char *bytes, size_t n, char *out, size_t out_n) {
    static const char hex[] = "0123456789abcdef";
    if (out_n < n * 2 + 1) {
        if (out_n > 0) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
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
        checked_snprintf(store->log_dir, sizeof(store->log_dir), "%s", store->base) != 0 ||
        checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", store->base) != 0 ||
        checked_snprintf(store->profile_path, sizeof(store->profile_path), "%s/profiles.json", store->base) != 0 ||
        checked_snprintf(store->alias_path, sizeof(store->alias_path), "%s/aliases.json", store->base) != 0) {
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
    if (mkdir_p0700(store->console_dir) != 0 ||
        chmod(store->console_dir, 0700) != 0) {
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
        checked_snprintf(store->public_dir, sizeof(store->public_dir), "%s/public", base) != 0 ||
        checked_snprintf(store->console_dir, sizeof(store->console_dir), "%s/console", base) != 0 ||
        checked_snprintf(store->profile_path, sizeof(store->profile_path), "%s/profiles.json", base) != 0 ||
        checked_snprintf(store->alias_path, sizeof(store->alias_path), "%s/public/aliases.json", base) != 0) {
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
    if (mkdir_p_mode(store->console_dir, 0700) != 0 ||
        chmod(store->console_dir, 0700) != 0 ||
        chown_root_if_root(store->console_dir) != 0) {
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
    if (store->console_dir[0] &&
        checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && path_exists(path)) {
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
    if (store->console_dir[0] &&
        checked_snprintf(path, sizeof(path), "%s/%s.sock", store->console_dir, id) == 0 && path_exists(path)) {
        return true;
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
        if (!valid_id(out)) {
            continue;
        }
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

static void sig_note(const struct invocation *inv, const char *fmt, ...) {
    if (inv && inv->quiet) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool executable_available(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/usr/bin:/bin";
    }
    char *copy = strdup(path);
    if (!copy) {
        return false;
    }
    bool found = false;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (!*dir) {
            dir = ".";
        }
        char candidate[SIGMUND_PATH_MAX];
        if (checked_snprintf(candidate, sizeof(candidate), "%s/%s", dir, name) == 0 &&
            access(candidate, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

static int make_console_listener(const char *sock_path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(sock_path);
    if (len == 0 || len >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, sock_path, len + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
#ifdef SIGMUND_NEED_SOCKET_CLOEXEC
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
#endif
    unlink(sock_path);
    mode_t old_umask = umask(077);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (rc != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (chmod(sock_path, 0600) != 0 || listen(fd, 1) != 0) {
        int saved = errno;
        close(fd);
        unlink(sock_path);
        errno = saved;
        return -1;
    }
    return fd;
}

static int open_console_pty(int *master_out, int *slave_out) {
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
    int slave = open(slave_name, O_RDWR | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
#ifdef TIOCSCTTY
    (void)ioctl(slave, TIOCSCTTY, 0);
#endif
    (void)tcsetpgrp(slave, getpgrp());
    *master_out = master;
    *slave_out = slave;
    return 0;
}

static void broker_cleanup_and_exit(int parent_pipe,
                                    const char *sock_path,
                                    int listener,
                                    int master,
                                    int slave,
                                    int logfd,
                                    pid_t target,
                                    int exit_code) {
    if (target > 0) {
        kill(target, SIGKILL);
        int st = 0;
        while (waitpid(target, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
    }
    if (parent_pipe >= 0) close(parent_pipe);
    if (listener >= 0) close(listener);
    if (master >= 0) close(master);
    if (slave >= 0) close(slave);
    if (logfd >= 0) close(logfd);
    if (sock_path && *sock_path) unlink(sock_path);
    _exit(exit_code);
}

static void broker_fail_errno(int parent_pipe,
                              const char *sock_path,
                              int listener,
                              int master,
                              int slave,
                              int logfd,
                              pid_t target,
                              int err) {
    if (err == 0) {
        err = EIO;
    }
    (void)write_all(parent_pipe, &err, sizeof(err));
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, slave, logfd, target, 127);
}

static void run_console_broker(int parent_pipe,
                               const char *log_path,
                               const char *sock_path,
                               int argc,
                               char **argv,
                               const char *exec_path) {
    int listener = -1;
    int master = -1;
    int slave = -1;
    int logfd = -1;
    pid_t target = -1;

    if (argc <= 0 || !argv || !argv[0]) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, EINVAL);
    }

    listener = make_console_listener(sock_path);
    if (listener < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
    }
    logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logfd < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
    }
    if (open_console_pty(&master, &slave) != 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
    }

    int exec_pipe[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(exec_pipe, O_CLOEXEC) != 0)
#endif
    {
        if (pipe(exec_pipe) != 0) {
            broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, errno);
        }
        if (fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
            int saved = errno;
            close(exec_pipe[0]);
            close(exec_pipe[1]);
            broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, saved);
        }
    }

    target = fork();
    if (target < 0) {
        int saved = errno;
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, saved);
    }
    if (target == 0) {
        close(exec_pipe[0]);
        close(listener);
        close(master);
        close(logfd);
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            int e = errno;
            (void)write_all(exec_pipe[1], &e, sizeof(e));
            _exit(127);
        }
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        if (exec_path && *exec_path) {
            execv(exec_path, argv);
        } else {
            execvp(argv[0], argv);
        }
        int e = errno;
        (void)write_all(exec_pipe[1], &e, sizeof(e));
        _exit(127);
    }

    close(exec_pipe[1]);
    int child_errno = 0;
    int handshake = read_exec_handshake(exec_pipe[0], &child_errno);
    int handshake_errno = errno;
    close(exec_pipe[0]);
    close(slave);
    slave = -1;
    if (handshake < 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, handshake_errno);
    }
    if (handshake > 0) {
        broker_fail_errno(parent_pipe, sock_path, listener, master, slave, logfd, target, child_errno);
    }
    close(parent_pipe);
    parent_pipe = -1;

    int client = -1;
    bool client_input_closed = false;
    bool target_done = false;
    while (1) {
        if (!target_done) {
            int st = 0;
            pid_t got = waitpid(target, &st, WNOHANG);
            if (got == target) {
                target_done = true;
                target = -1;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master, &rfds);
        FD_SET(listener, &rfds);
        int maxfd = master > listener ? master : listener;
        if (client >= 0 && !client_input_closed) {
            FD_SET(client, &rfds);
            if (client > maxfd) {
                maxfd = client;
            }
        }
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int sr = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (sr == 0) {
            if (target_done) {
                char drain[4096];
                ssize_t n = read(master, drain, sizeof(drain));
                if (n > 0) {
                    (void)write_all(logfd, drain, (size_t)n);
                    if (client >= 0 && write_all(client, drain, (size_t)n) != 0) {
                        close(client);
                        client = -1;
                        client_input_closed = false;
                    }
                    continue;
                }
                break;
            }
            continue;
        }
        if (FD_ISSET(listener, &rfds)) {
            int next = accept(listener, NULL, NULL);
            if (next >= 0) {
                if (client >= 0) {
                    close(next);
                } else {
                    client = next;
                    client_input_closed = false;
                }
            }
        }
        if (client >= 0 && !client_input_closed && FD_ISSET(client, &rfds)) {
            char buf[4096];
            ssize_t n = read(client, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(master, buf, (size_t)n) != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                }
            } else if (n == 0) {
                client_input_closed = true;
            } else {
                close(client);
                client = -1;
                client_input_closed = false;
            }
        }
        if (FD_ISSET(master, &rfds)) {
            char buf[4096];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                (void)write_all(logfd, buf, (size_t)n);
                if (client >= 0 && write_all(client, buf, (size_t)n) != 0) {
                    close(client);
                    client = -1;
                    client_input_closed = false;
                }
            } else if (n == 0 || errno == EIO) {
                break;
            }
        }
    }

    if (client >= 0) close(client);
    broker_cleanup_and_exit(parent_pipe, sock_path, listener, master, slave, logfd, target, 0);
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

static void sha256_update_cstr(struct sha256_ctx *ctx, const char *s) {
    sha256_update(ctx, s, strlen(s));
}

static void sha256_update_nul_field(struct sha256_ctx *ctx, const char *s) {
    static const unsigned char nul = 0;
    sha256_update_cstr(ctx, s ? s : "");
    sha256_update(ctx, &nul, 1);
}

/*
 * This digest is a stable capability key. Do not add versions, environment,
 * cwd, uid, timestamps, or other context: existing aliases, profiles, and
 * sudoers grants are keyed to exactly this binary-path + argv framing.
 */
static void profile_hash_for_argv(const char *binary_path, int argc, char **argv, char out[PROFILE_HASH_STR_LEN]) {
    struct sha256_ctx ctx;
    unsigned char digest[32];
    sha256_init(&ctx);
    sha256_update_nul_field(&ctx, "sigmund-profile");
    sha256_update_nul_field(&ctx, binary_path);
    char count[32];
    snprintf(count, sizeof(count), "%d", argc);
    sha256_update_nul_field(&ctx, count);
    for (int i = 0; i < argc; i++) {
        char idx[32];
        snprintf(idx, sizeof(idx), "%d", i);
        sha256_update_nul_field(&ctx, idx);
        sha256_update_nul_field(&ctx, argv[i]);
    }
    sha256_final(&ctx, digest);
    hex_encode(digest, sizeof(digest), out, PROFILE_HASH_STR_LEN);
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
    if (r->has_alias) {
        fprintf(f, "  \"alias\": \"");
        json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
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
    if (r->has_console) {
        fprintf(f, "  \"console_sock\": \"");
        json_escape(f, r->console_sock);
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
    if (r->has_alias) {
        fprintf(f, "  \"alias\": \"");
        json_escape(f, r->alias);
        fprintf(f, "\",\n");
    }
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

static int append_cmd_human(char *dst, size_t n, size_t *off, const char *arg) {
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

static int format_argv_human(char *dst, size_t n, int argc, char **argv) {
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

#if !defined(__APPLE__)
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
}
#endif

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
        if (append_cmd_human(out, n, &off, arg) != 0) {
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

static void free_argv_alloc(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int json_get_string_array_alloc(const char *j, const char *key, char ***argv_out, int *argc_out) {
    *argv_out = NULL;
    *argc_out = 0;
    const char *v;
    if (json_find_key(j, key, &v) != 0 || *v != '[') {
        return -1;
    }
    v = skip_ws(v + 1);
    int cap = 4;
    int argc = 0;
    char **argv = calloc((size_t)cap + 1, sizeof(char *));
    if (!argv) {
        return -1;
    }
    while (*v && *v != ']') {
        char arg[SIGMUND_PATH_MAX];
        if (parse_json_string(v, arg, sizeof(arg), &v) != 0) {
            free_argv_alloc(argv, argc);
            return -1;
        }
        if (argc == cap) {
            cap *= 2;
            char **next = realloc(argv, ((size_t)cap + 1) * sizeof(char *));
            if (!next) {
                free_argv_alloc(argv, argc);
                return -1;
            }
            argv = next;
        }
        argv[argc] = strdup(arg);
        if (!argv[argc]) {
            free_argv_alloc(argv, argc);
            return -1;
        }
        argc++;
        argv[argc] = NULL;
        v = skip_ws(v);
        if (*v == ',') {
            v = skip_ws(v + 1);
        } else if (*v != ']') {
            free_argv_alloc(argv, argc);
            return -1;
        }
    }
    if (*v != ']' || argc == 0) {
        free_argv_alloc(argv, argc);
        return -1;
    }
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static int json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out) {
    return json_get_string_array_alloc(j, "argv", argv_out, argc_out);
}

static int json_get_args_alloc(const char *j, char ***argv_out, int *argc_out) {
    return json_get_string_array_alloc(j, "args", argv_out, argc_out);
}

static int resolve_binary_path(const char *argv0, char *out, size_t n) {
    if (!argv0 || !*argv0) {
        errno = EINVAL;
        return -1;
    }
    if (strchr(argv0, '/')) {
        char resolved[SIGMUND_PATH_MAX];
        if (!realpath(argv0, resolved)) {
            return -1;
        }
        return checked_snprintf(out, n, "%s", resolved);
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char dir[SIGMUND_PATH_MAX];
        if (len == 0) {
            if (checked_snprintf(dir, sizeof(dir), ".") != 0) {
                return -1;
            }
        } else {
            if (len >= sizeof(dir)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(dir, p, len);
            dir[len] = '\0';
        }
        char candidate[SIGMUND_PATH_MAX], resolved[SIGMUND_PATH_MAX];
        if (checked_snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0) == 0 &&
            access(candidate, X_OK) == 0 && realpath(candidate, resolved)) {
            return checked_snprintf(out, n, "%s", resolved);
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
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

struct profile {
    char hash[PROFILE_HASH_STR_LEN];
    char binary_path[SIGMUND_PATH_MAX];
    int argc;
    char **argv;
};

struct alias_entry {
    char name[ALIAS_MAX_LEN + 1];
    char hash[PROFILE_HASH_STR_LEN];
    char binary_path[SIGMUND_PATH_MAX];
    int argc;
    char **argv;
    bool has_hash;
    bool has_recipe;
};

static void free_profile(struct profile *p) {
    if (!p) {
        return;
    }
    free_argv_alloc(p->argv, p->argc);
    memset(p, 0, sizeof(*p));
}

static int fsync_dir_path(const char *dir) {
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return -1;
    }
    int rc = fsync(dfd);
    int saved = errno;
    close(dfd);
    errno = saved;
    return rc;
}

static void free_profiles(struct profile *profiles, size_t count) {
    if (!profiles) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free_profile(&profiles[i]);
    }
    free(profiles);
}

static int copy_argv(char ***out, int argc, char **argv) {
    *out = NULL;
    if (argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    char **copy = calloc((size_t)argc + 1, sizeof(char *));
    if (!copy) {
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        copy[i] = strdup(argv[i]);
        if (!copy[i]) {
            free_argv_alloc(copy, i);
            return -1;
        }
    }
    copy[argc] = NULL;
    *out = copy;
    return 0;
}

static bool profile_equal_argv(const struct profile *p, const char *binary_path, int argc, char **argv) {
    if (strcmp(p->binary_path, binary_path) != 0 || p->argc != argc) {
        return false;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(p->argv[i], argv[i]) != 0) {
            return false;
        }
    }
    return true;
}

static int parse_profile_object(const char *j, const char *hash, struct profile *profile) {
    memset(profile, 0, sizeof(*profile));
    if (!valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    if (json_get_str(j, "bin", profile->binary_path, sizeof(profile->binary_path)) != 0 &&
        json_get_str(j, "binary_path", profile->binary_path, sizeof(profile->binary_path)) != 0) {
        return -1;
    }
    if (profile->binary_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (json_get_args_alloc(j, &profile->argv, &profile->argc) != 0 &&
        json_get_argv_alloc(j, &profile->argv, &profile->argc) != 0) {
        free_profile(profile);
        return -1;
    }
    profile_hash_for_argv(profile->binary_path, profile->argc, profile->argv, profile->hash);
    if (strcmp(profile->hash, hash) != 0) {
        free_profile(profile);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int load_profiles(const struct store_paths *store, struct profile **profiles_out, size_t *count_out) {
    *profiles_out = NULL;
    *count_out = 0;
    char *j = NULL;
    if (read_owned_file_no_symlink(store->profile_path, &j) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    const char *p = skip_ws(j);
    if (*p != '{') {
        free(j);
        errno = EINVAL;
        return -1;
    }
    p++;
    size_t cap = 0, count = 0;
    struct profile *profiles = NULL;
    while (1) {
        p = skip_ws(p);
        if (*p == '}') {
            break;
        }
        char hash[PROFILE_HASH_STR_LEN];
        if (parse_json_string(p, hash, sizeof(hash), &p) != 0 || !valid_profile_hash(hash)) {
            free(j);
            free_profiles(profiles, count);
            errno = EINVAL;
            return -1;
        }
        p = skip_ws(p);
        if (*p != ':') {
            free(j);
            free_profiles(profiles, count);
            errno = EINVAL;
            return -1;
        }
        p = skip_ws(p + 1);
        if (*p != '{') {
            free(j);
            free_profiles(profiles, count);
            errno = EINVAL;
            return -1;
        }
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            struct profile *next = realloc(profiles, next_cap * sizeof(*profiles));
            if (!next) {
                free(j);
                free_profiles(profiles, count);
                return -1;
            }
            profiles = next;
            cap = next_cap;
        }
        if (parse_profile_object(p, hash, &profiles[count]) != 0) {
            free(j);
            free_profiles(profiles, count);
            return -1;
        }
        count++;
        if (skip_json_value(&p) != 0) {
            free(j);
            free_profiles(profiles, count);
            return -1;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        free(j);
        free_profiles(profiles, count);
        errno = EINVAL;
        return -1;
    }
    free(j);
    *profiles_out = profiles;
    *count_out = count;
    return 0;
}

static int write_profiles_atomic(const struct store_paths *store, const struct profile *profiles, size_t count) {
    char dir[SIGMUND_PATH_MAX], tmp[SIGMUND_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", store->profile_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    if (checked_snprintf(tmp, sizeof(tmp), "%s/.profiles.tmp", dir) != 0) {
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0600) != 0 ||
        (store->kind == STORE_SYSTEM_MANAGED && geteuid() == 0 && fchown(fd, 0, 0) != 0)) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "{\n");
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "  \"");
        json_escape(f, profiles[i].hash);
        fprintf(f, "\": {\"bin\": \"");
        json_escape(f, profiles[i].binary_path);
        fprintf(f, "\", \"args\": ");
        write_json_argv(f, profiles[i].argc, profiles[i].argv);
        fprintf(f, "}%s\n", i + 1 == count ? "" : ",");
    }
    fprintf(f, "}\n");
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, store->profile_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)fsync_dir_path(dir);
    return 0;
}

static int write_profile_atomic(const struct store_paths *store,
                                const char *hash,
                                const char *binary_path,
                                int argc,
                                char **argv) {
    if (!valid_profile_hash(hash) || !binary_path || binary_path[0] != '/' || argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    char check_hash[PROFILE_HASH_STR_LEN];
    profile_hash_for_argv(binary_path, argc, argv, check_hash);
    if (strcmp(check_hash, hash) != 0) {
        errno = EINVAL;
        return -1;
    }
    struct profile *profiles = NULL;
    size_t count = 0;
    if (load_profiles(store, &profiles, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(profiles[i].hash, hash) == 0) {
            int rc = 0;
            if (!profile_equal_argv(&profiles[i], binary_path, argc, argv)) {
                errno = EEXIST;
                rc = -1;
            }
            free_profiles(profiles, count);
            return rc;
        }
    }
    struct profile *next = realloc(profiles, (count + 1) * sizeof(*profiles));
    if (!next) {
        free_profiles(profiles, count);
        return -1;
    }
    profiles = next;
    memset(&profiles[count], 0, sizeof(profiles[count]));
    snprintf(profiles[count].hash, sizeof(profiles[count].hash), "%s", hash);
    if (checked_snprintf(profiles[count].binary_path, sizeof(profiles[count].binary_path), "%s", binary_path) != 0 ||
        copy_argv(&profiles[count].argv, argc, argv) != 0) {
        free_profiles(profiles, count + 1);
        return -1;
    }
    profiles[count].argc = argc;
    count++;
    int rc = write_profiles_atomic(store, profiles, count);
    free_profiles(profiles, count);
    return rc;
}

static int load_profile_by_hash(const struct store_paths *store, const char *hash, struct profile *profile) {
    memset(profile, 0, sizeof(*profile));
    if (!valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    char *j = NULL;
    if (read_owned_file_no_symlink(store->profile_path, &j) != 0) {
        return -1;
    }
    const char *v = NULL;
    if (json_find_key(j, hash, &v) != 0 || parse_profile_object(v, hash, profile) != 0) {
        free(j);
        return -1;
    }
    free(j);
    return 0;
}

static void free_aliases(struct alias_entry *entries, size_t count) {
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free_argv_alloc(entries[i].argv, entries[i].argc);
    }
    free(entries);
}

static int parse_alias_recipe_object(const char *j, struct alias_entry *entry) {
    if (json_get_str(j, "bin", entry->binary_path, sizeof(entry->binary_path)) != 0 &&
        json_get_str(j, "binary_path", entry->binary_path, sizeof(entry->binary_path)) != 0) {
        return -1;
    }
    if (entry->binary_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (json_get_args_alloc(j, &entry->argv, &entry->argc) != 0 &&
        json_get_argv_alloc(j, &entry->argv, &entry->argc) != 0) {
        return -1;
    }
    entry->has_recipe = true;
    return 0;
}

static int load_aliases(const struct store_paths *store, struct alias_entry **entries_out, size_t *count_out) {
    *entries_out = NULL;
    *count_out = 0;
    char *j = NULL;
    if (read_owned_file_no_symlink(store->alias_path, &j) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }
    const char *p = skip_ws(j);
    if (*p != '{') {
        free(j);
        errno = EINVAL;
        return -1;
    }
    p++;
    size_t cap = 0, count = 0;
    struct alias_entry *entries = NULL;
    while (1) {
        p = skip_ws(p);
        if (*p == '}') {
            break;
        }
        char name[ALIAS_MAX_LEN + 1], hash[PROFILE_HASH_STR_LEN];
        if (parse_json_string(p, name, sizeof(name), &p) != 0 || !valid_alias(name)) {
            free(j);
            free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = skip_ws(p);
        if (*p != ':') {
            free(j);
            free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = skip_ws(p + 1);
        if (count == cap) {
            size_t next_cap = cap ? cap * 2 : 8;
            struct alias_entry *next = realloc(entries, next_cap * sizeof(*entries));
            if (!next) {
                free(j);
                free_aliases(entries, count);
                return -1;
            }
            entries = next;
            cap = next_cap;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
        const char *value = p;
        if (*value == '"') {
            if (parse_json_string(value, hash, sizeof(hash), NULL) != 0 || !valid_profile_hash(hash)) {
                free(j);
                free_aliases(entries, count);
                errno = EINVAL;
                return -1;
            }
            snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
            entries[count].has_hash = true;
        } else if (*value == '{') {
            if (parse_alias_recipe_object(value, &entries[count]) != 0) {
                free(j);
                free_aliases(entries, count + 1);
                return -1;
            }
        } else {
            free(j);
            free_aliases(entries, count);
            errno = EINVAL;
            return -1;
        }
        p = value;
        if (skip_json_value(&p) != 0) {
            free(j);
            free_aliases(entries, count + 1);
            return -1;
        }
        count++;
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        free(j);
        free_aliases(entries, count);
        errno = EINVAL;
        return -1;
    }
    free(j);
    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int write_aliases_atomic(const struct store_paths *store, const struct alias_entry *entries, size_t count) {
    const char *dir = store->kind == STORE_SYSTEM_MANAGED ? store->public_dir : store->base;
    char tmp[SIGMUND_PATH_MAX];
    mode_t mode = store->kind == STORE_SYSTEM_MANAGED ? 0644 : 0600;
    if (checked_snprintf(tmp, sizeof(tmp), "%s/.aliases.tmp", dir) != 0) {
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (store->kind == STORE_SYSTEM_MANAGED && geteuid() == 0 && fchown(fd, 0, 0) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "{\n");
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "  \"");
        json_escape(f, entries[i].name);
        if (store->kind == STORE_SYSTEM_MANAGED) {
            if (!entries[i].has_hash || !valid_profile_hash(entries[i].hash)) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": \"");
            json_escape(f, entries[i].hash);
            fprintf(f, "\"%s\n", i + 1 == count ? "" : ",");
        } else {
            if (!entries[i].has_recipe || entries[i].binary_path[0] != '/' || entries[i].argc <= 0 || !entries[i].argv) {
                fclose(f);
                unlink(tmp);
                errno = EINVAL;
                return -1;
            }
            fprintf(f, "\": {\"bin\": \"");
            json_escape(f, entries[i].binary_path);
            fprintf(f, "\", \"args\": ");
            write_json_argv(f, entries[i].argc, entries[i].argv);
            fprintf(f, "}%s\n", i + 1 == count ? "" : ",");
        }
    }
    fprintf(f, "}\n");
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, store->alias_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)fsync_dir_path(dir);
    return 0;
}

static int alias_lookup_hash(const struct store_paths *store, const char *alias, char hash[PROFILE_HASH_STR_LEN]) {
    if (!valid_alias(alias)) {
        return -1;
    }
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0 && entries[i].has_hash) {
            snprintf(hash, PROFILE_HASH_STR_LEN, "%s", entries[i].hash);
            rc = 0;
            break;
        }
    }
    free_aliases(entries, count);
    return rc;
}

static int alias_upsert_hash(const struct store_paths *store, const char *alias, const char *hash) {
    if (!valid_alias(alias) || !valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            snprintf(entries[i].hash, sizeof(entries[i].hash), "%s", hash);
            entries[i].has_hash = true;
            int rc = write_aliases_atomic(store, entries, count);
            free_aliases(entries, count);
            return rc;
        }
    }
    struct alias_entry *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    snprintf(entries[count].hash, sizeof(entries[count].hash), "%s", hash);
    entries[count].has_hash = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    free_aliases(entries, count);
    return rc;
}

static int alias_lookup_recipe(const struct store_paths *store, const char *alias, struct profile *recipe) {
    memset(recipe, 0, sizeof(*recipe));
    if (!valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    int rc = -1;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) != 0) {
            continue;
        }
        if (entries[i].has_recipe) {
            if (checked_snprintf(recipe->binary_path, sizeof(recipe->binary_path), "%s", entries[i].binary_path) == 0 &&
                copy_argv(&recipe->argv, entries[i].argc, entries[i].argv) == 0) {
                recipe->argc = entries[i].argc;
                rc = 0;
            }
            break;
        }
        if (entries[i].has_hash && load_profile_by_hash(store, entries[i].hash, recipe) == 0) {
            rc = 0;
            break;
        }
    }
    free_aliases(entries, count);
    return rc;
}

static int alias_upsert_recipe(const struct store_paths *store,
                               const char *alias,
                               const char *binary_path,
                               int argc,
                               char **argv) {
    if (!valid_alias(alias) || !binary_path || binary_path[0] != '/' || argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (load_aliases(store, &entries, &count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            free_argv_alloc(entries[i].argv, entries[i].argc);
            entries[i].argv = NULL;
            entries[i].argc = 0;
            if (checked_snprintf(entries[i].binary_path, sizeof(entries[i].binary_path), "%s", binary_path) != 0 ||
                copy_argv(&entries[i].argv, argc, argv) != 0) {
                free_aliases(entries, count);
                return -1;
            }
            entries[i].argc = argc;
            entries[i].has_recipe = true;
            entries[i].has_hash = false;
            entries[i].hash[0] = '\0';
            int rc = write_aliases_atomic(store, entries, count);
            free_aliases(entries, count);
            return rc;
        }
    }
    struct alias_entry *next = realloc(entries, (count + 1) * sizeof(*entries));
    if (!next) {
        free_aliases(entries, count);
        return -1;
    }
    entries = next;
    memset(&entries[count], 0, sizeof(entries[count]));
    snprintf(entries[count].name, sizeof(entries[count].name), "%s", alias);
    if (checked_snprintf(entries[count].binary_path, sizeof(entries[count].binary_path), "%s", binary_path) != 0 ||
        copy_argv(&entries[count].argv, argc, argv) != 0) {
        free_aliases(entries, count + 1);
        return -1;
    }
    entries[count].argc = argc;
    entries[count].has_recipe = true;
    count++;
    int rc = write_aliases_atomic(store, entries, count);
    free_aliases(entries, count);
    return rc;
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
    if (json_get_str(j, "alias", r->alias, sizeof(r->alias)) == 0 && valid_alias(r->alias)) {
        r->has_alias = true;
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
    if (json_get_str(j, "console_sock", r->console_sock, sizeof(r->console_sock)) == 0 &&
        r->console_sock[0] == '/') {
        r->has_console = true;
    }
    if (json_get_u64(j, "proc_starttime_ticks", &r->proc_starttime_ticks) != 0 ||
        json_get_u64(j, "exe_dev", &r->exe_dev) != 0 ||
        json_get_u64(j, "exe_ino", &r->exe_ino) != 0) {
        free(j);
        return -1;
    }
    if (json_get_argv_display(j, r->cmdline, sizeof(r->cmdline)) != 0 &&
        json_get_str(j, "cmdline_display", r->cmdline, sizeof(r->cmdline)) != 0) {
        snprintf(r->cmdline, sizeof(r->cmdline), "?");
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
    if (json_get_str(j, "alias", pi->alias, sizeof(pi->alias)) == 0 && valid_alias(pi->alias)) {
        pi->has_alias = true;
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

static int perform_start(const struct invocation *inv,
                         const struct store_paths *store,
                         bool tail,
                         bool console_mode,
                         int argc,
                         char **argv,
                         const char *exec_path,
                         const char *run_alias) {
    if (argc <= 0 || !argv || !argv[0]) {
        usage();
        return 5;
    }

    if (console_mode && !executable_available("socat")) {
        fprintf(stderr, "sigmund: --console requires socat (not found in PATH)\n");
        return 1;
    }

    char id[16], log_path[SIGMUND_PATH_MAX], reserve_path[SIGMUND_PATH_MAX], console_sock[SIGMUND_PATH_MAX], boot_id[128] = {0};
    console_sock[0] = '\0';
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
    if (console_mode &&
        checked_snprintf(console_sock, sizeof(console_sock), "%s/%s.sock", store->console_dir, id) != 0) {
        die_errno("sigmund: console socket path too long");
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
        if (console_mode) {
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd < 0 ||
                dup2(nullfd, STDIN_FILENO) < 0 ||
                dup2(nullfd, STDOUT_FILENO) < 0 ||
                dup2(nullfd, STDERR_FILENO) < 0) {
                int e = errno;
                write_all(pipefd[1], &e, sizeof(e));
                _exit(127);
            }
            if (nullfd > STDERR_FILENO) {
                close(nullfd);
            }
            run_console_broker(pipefd[1], log_path, console_sock, argc, argv, exec_path);
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
        if (exec_path && *exec_path) {
            execv(exec_path, argv);
        } else {
            execvp(argv[0], argv);
        }
        /* No envp variant here: children intentionally inherit Sigmund's environment. */
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
        if (console_sock[0]) {
            unlink(console_sock);
        }
        errno = handshake_errno;
        die_errno("sigmund: exec handshake failed");
    }
    if (handshake > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            continue;
        }
        if (child_errno == ENOENT) {
            fprintf(stderr, "sigmund: cannot start '%s': command not found\n", argv[0]);
        } else {
            fprintf(stderr, "sigmund: cannot start '%s': %s\n", argv[0], strerror(child_errno));
        }
        unlink(reserve_path);
        unlink(log_path);
        if (console_sock[0]) {
            unlink(console_sock);
        }
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
    if (run_alias && valid_alias(run_alias)) {
        r.has_alias = true;
        if (checked_snprintf(r.alias, sizeof(r.alias), "%s", run_alias) != 0) {
            die_errno("sigmund: alias too long");
        }
    }
    if (console_sock[0]) {
        r.has_console = true;
        if (checked_snprintf(r.console_sock, sizeof(r.console_sock), "%s", console_sock) != 0) {
            die_errno("sigmund: console socket path too long");
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
    if (format_argv_human(r.cmdline, sizeof(r.cmdline), argc, argv) != 0) {
        snprintf(r.cmdline, sizeof(r.cmdline), "?");
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
                if (console_sock[0]) {
                    unlink(console_sock);
                }
                unlink(reserve_path);
                char public_path[SIGMUND_PATH_MAX];
                if (checked_snprintf(public_path, sizeof(public_path), "%s/%s.json", store->public_dir, r.id) == 0) {
                    unlink(public_path);
                }
                errno = saved;
                die_errno("sigmund: failed to write public index");
            }
        }
        printf("%s\n", r.id);
        sig_note(inv,
                 "sigmund  started  %s   %s\n"
                 "         log      %s\n"
                 "         tail     sigmund tail %s\n"
                 "%s%s%s"
                 "         stop     sigmund stop %s\n",
                 r.id,
                 r.cmdline[0] ? r.cmdline : "?",
                 r.log_path,
                 r.id,
                 r.has_console ? "         console  sigmund console " : "",
                 r.has_console ? r.id : "",
                 r.has_console ? "\n" : "",
                 r.id);
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
        if (console_sock[0]) {
            unlink(console_sock);
        }
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

static int do_signal_action(const struct store_paths *store, const char *id, int sig, bool graceful, bool *already_done) {
    struct record r;
    char path[SIGMUND_PATH_MAX], boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    if (already_done) {
        *already_done = false;
    }
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
        if (already_done) {
            *already_done = true;
        }
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
            if (already_done) {
                *already_done = true;
            }
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
        snprintf(out, n, "%s", r->has_console ? "console" : "-");
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

struct list_row {
    char id[16];
    char state[16];
    char started[64];
    char result[64];
    char cmd[SIGMUND_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
};

struct list_rows {
    struct list_row *items;
    size_t count;
};

static void free_list_rows(struct list_rows *rows) {
    free(rows->items);
    rows->items = NULL;
    rows->count = 0;
}

static int append_list_row(struct list_rows *rows, const struct list_row *row) {
    struct list_row *next = realloc(rows->items, (rows->count + 1) * sizeof(*rows->items));
    if (!next) {
        return -1;
    }
    rows->items = next;
    rows->items[rows->count++] = *row;
    return 0;
}

static void format_relative_age(int64_t start_unix_ns, char *out, size_t n) {
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

static int compare_list_rows(const void *a, const void *b) {
    const struct list_row *ra = (const struct list_row *)a;
    const struct list_row *rb = (const struct list_row *)b;
    if (ra->running != rb->running) {
        return ra->running ? -1 : 1;
    }
    if (ra->start_unix_ns > rb->start_unix_ns) {
        return -1;
    }
    if (ra->start_unix_ns < rb->start_unix_ns) {
        return 1;
    }
    return strcmp(ra->id, rb->id);
}

static void print_list_header(bool iso) {
    printf("%-10s %-8s %-*s %-10s %s\n", "RUNID", "STATE", iso ? 24 : 8, iso ? "STARTED_AT" : "STARTED", "RESULT", "CMD");
}

static void print_list_row(const struct list_row *row, bool iso) {
    char cmd[80];
    const char *src = row->cmd[0] ? row->cmd : "?";
    snprintf(cmd, sizeof(cmd), "%.72s%s", src, strlen(src) > 72 ? "..." : "");
    printf("%-10s %-8s %-*s %-10s %s\n", row->id, row->state, iso ? 24 : 8, row->started, row->result, cmd);
}

static int collect_list_private(const struct store_paths *store,
                                const char *alias_filter,
                                bool iso,
                                struct list_rows *rows) {
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
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (!valid_record(&r)) {
            fprintf(stderr, "sigmund: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (alias_filter && (!r.has_alias || strcmp(r.alias, alias_filter) != 0)) {
            continue;
        }
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        struct list_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.id, sizeof(row.id), "%s", r.id);
        snprintf(row.state, sizeof(row.state), "%s", state_str(st));
        row.start_unix_ns = r.start_unix_ns;
        row.running = st == STATE_RUNNING;
        if (iso) {
            if (r.has_started_at && r.started_at[0]) {
                snprintf(row.started, sizeof(row.started), "%s", r.started_at);
            } else {
                format_rfc3339_utc_from_ns(r.start_unix_ns, row.started, sizeof(row.started));
            }
        } else {
            format_relative_age(r.start_unix_ns, row.started, sizeof(row.started));
        }
        format_result(&r, st, row.result, sizeof(row.result));
        snprintf(row.cmd, sizeof(row.cmd), "%s", r.cmdline[0] ? r.cmdline : "?");
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int collect_list_public(const struct store_paths *store,
                               const char *alias_filter,
                               bool iso,
                               struct list_rows *rows) {
    DIR *d = opendir(store->public_dir);
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
        if (alias_filter && (!pi.has_alias || strcmp(pi.alias, alias_filter) != 0)) {
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.id, sizeof(row.id), "%s", pi.id);
        snprintf(row.state, sizeof(row.state), "%s", "unknown");
        row.running = false;
        row.start_unix_ns = 0;
        if (iso) {
            snprintf(row.started, sizeof(row.started), "%s", pi.started_at[0] ? pi.started_at : "-");
        } else {
            snprintf(row.started, sizeof(row.started), "%s", "-");
        }
        snprintf(row.result, sizeof(row.result), "%s", "-");
        snprintf(row.cmd, sizeof(row.cmd), "%s", "<root-managed>");
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int print_collected_list(struct list_rows *rows, bool iso) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }
    print_list_header(iso);
    for (size_t i = 0; i < rows->count; i++) {
        print_list_row(&rows->items[i], iso);
    }
    return 0;
}

static int cmd_list_normal(const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, alias_filter, iso, &rows) != 0 ||
        collect_list_public(system_store, alias_filter, iso, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

static int cmd_list_system(const struct store_paths *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, alias_filter, iso, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
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
    if (r.has_console) {
        unlink(r.console_sock);
    }
    unlink_public_index(store, id);
    if (removed) {
        *removed = true;
    }
    return 0;
}

static int cmd_prune_store_all(const struct store_paths *store, bool include_stale, int *removed_count) {
    if (removed_count) {
        *removed_count = 0;
    }
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
            if (r.has_console) {
                unlink(r.console_sock);
            }
            unlink_public_index(store, r.id);
            if (removed_count) {
                (*removed_count)++;
            }
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
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    d = opendir(store->console_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".sock")) {
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
        if (!valid_id(id)) {
            continue;
        }
        char json_path[SIGMUND_PATH_MAX], sock_path[SIGMUND_PATH_MAX];
        if (checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            checked_snprintf(sock_path, sizeof(sock_path), "%s/%s", store->console_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(sock_path);
            if (removed_count) {
                (*removed_count)++;
            }
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

static int parse_alias_cap_atom(const char *atom,
                                char alias[ALIAS_MAX_LEN + 1],
                                char hash[PROFILE_HASH_STR_LEN]) {
    const char *sep = atom ? strchr(atom, '@') : NULL;
    if (!sep || sep == atom) {
        return -1;
    }
    size_t alias_len = (size_t)(sep - atom);
    if (alias_len > ALIAS_MAX_LEN) {
        return -1;
    }
    char alias_tmp[ALIAS_MAX_LEN + 1];
    memcpy(alias_tmp, atom, alias_len);
    alias_tmp[alias_len] = '\0';
    if (!valid_alias(alias_tmp) || !valid_profile_hash(sep + 1)) {
        return -1;
    }
    snprintf(alias, ALIAS_MAX_LEN + 1, "%s", alias_tmp);
    snprintf(hash, PROFILE_HASH_STR_LEN, "%s", sep + 1);
    return 0;
}

static int verify_system_alias_cap(const struct store_paths *system_store,
                                   const char *alias,
                                   const char *hash) {
    char current[PROFILE_HASH_STR_LEN];
    struct profile p;
    if (!valid_alias(alias) || !valid_profile_hash(hash) ||
        alias_lookup_hash(system_store, alias, current) != 0 ||
        strcmp(current, hash) != 0 ||
        load_profile_by_hash(system_store, hash, &p) != 0) {
        return -1;
    }
    free_profile(&p);
    return 0;
}

static bool valid_runid_selector(const char *sel) {
    return sel && (valid_id(sel) || strcmp(sel, "00000000") == 0 || strcmp(sel, "ffffffff") == 0);
}

static int ensure_run_recorded_under_alias(const struct store_paths *store, const char *id, const char *alias) {
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return -1;
    }
    if (!r.has_alias || strcmp(r.alias, alias) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
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

static bool valid_target_atom(const char *id) {
    return valid_id_prefix(id) || valid_alias(id);
}

static int resolve_public_profile_token(const struct store_paths *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]) {
    if (valid_profile_hash(token)) {
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (valid_alias(token) && alias_lookup_hash(store, token, hash) == 0) {
        return 1;
    }
    return 0;
}

static void fill_target(struct resolved_target *out,
                        enum resolve_scope scope,
                        const struct store_paths *store,
                        const char *id,
                        bool needs_elevation) {
    memset(out, 0, sizeof(*out));
    out->scope = scope;
    out->store = *store;
    out->needs_elevation = needs_elevation;
    checked_snprintf(out->id, sizeof(out->id), "%s", id);
}

static int set_target_capability(struct resolved_target *target, const char *alias, const char *hash) {
    if (!target || !valid_alias(alias) || !valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    if (checked_snprintf(target->cap_alias, sizeof(target->cap_alias), "%s", alias) != 0 ||
        checked_snprintf(target->cap_hash, sizeof(target->cap_hash), "%s", hash) != 0) {
        return -1;
    }
    target->has_capability = true;
    return 0;
}

struct alias_match {
    char id[16];
    enum run_state state;
    char started_at[64];
};

struct alias_match_list {
    struct alias_match *items;
    size_t count;
    bool alias_known;
};

static void free_alias_match_list(struct alias_match_list *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool alias_exists_in_store(const struct store_paths *store, const char *alias) {
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (!valid_alias(alias) || load_aliases(store, &entries, &count) != 0) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, alias) == 0) {
            found = true;
            break;
        }
    }
    free_aliases(entries, count);
    return found;
}

static bool command_all_allowed(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "prune"));
}

static bool record_matches_alias_intent(const char *command, const struct record *r, enum run_state st) {
    if (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
        !strcmp(command, "tail")) {
        return st == STATE_RUNNING;
    }
    if (!strcmp(command, "console")) {
        return st == STATE_RUNNING && r->has_console;
    }
    if (!strcmp(command, "dump")) {
        return r->has_log;
    }
    if (!strcmp(command, "prune")) {
        return st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE;
    }
    return false;
}

static int append_alias_match(struct alias_match_list *list,
                              const struct record *r,
                              enum run_state st,
                              const char *started_at) {
    struct alias_match *next = realloc(list->items, (list->count + 1) * sizeof(*list->items));
    if (!next) {
        return -1;
    }
    list->items = next;
    memset(&list->items[list->count], 0, sizeof(list->items[list->count]));
    if (checked_snprintf(list->items[list->count].id, sizeof(list->items[list->count].id), "%s", r->id) != 0 ||
        checked_snprintf(list->items[list->count].started_at,
                         sizeof(list->items[list->count].started_at),
                         "%s",
                         started_at && *started_at ? started_at : "-") != 0) {
        return -1;
    }
    list->items[list->count].state = st;
    list->count++;
    return 0;
}

static int collect_private_alias_matches(const struct store_paths *store,
                                         const char *alias,
                                         const char *command,
                                         struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = alias_exists_in_store(store, alias);
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
            closedir(d);
            return -1;
        }
        struct record r;
        if (load_record(path, &r) != 0 || !valid_record(&r) ||
            !r.has_alias || strcmp(r.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        enum run_state st = eval_state(&r, have_boot ? boot : NULL);
        if (!record_matches_alias_intent(command, &r, st)) {
            continue;
        }
        char started_at[64];
        if (r.has_started_at && r.started_at[0]) {
            snprintf(started_at, sizeof(started_at), "%s", r.started_at);
        } else {
            format_rfc3339_utc_from_ns(r.start_unix_ns, started_at, sizeof(started_at));
        }
        if (append_alias_match(list, &r, st, started_at) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static bool public_alias_visible(const struct store_paths *store, const char *alias) {
    if (alias_exists_in_store(store, alias)) {
        return true;
    }
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return false;
    }
    bool found = false;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= 16) {
            continue;
        }
        char id[16];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        char path[SIGMUND_PATH_MAX];
        if (checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct public_index pi;
        if (load_public_index(path, &pi) == 0 && pi.has_alias && strcmp(pi.alias, alias) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void report_alias_ambiguity(const char *command, const char *alias, const struct alias_match_list *list) {
    fprintf(stderr,
            "sigmund: error: alias '%s' matches more than one %s candidate\n",
            alias,
            command ? command : "target");
    fprintf(stderr, "sigmund: candidates:\n");
    for (size_t i = 0; i < list->count; i++) {
        fprintf(stderr,
                "  %s %-8s %s\n",
                list->items[i].id,
                state_str(list->items[i].state),
                list->items[i].started_at);
    }
    if (command_all_allowed(command)) {
        fprintf(stderr, "sigmund: use --all to apply %s to every listed run\n", command);
    }
}

static int append_resolved_target(struct resolved_target **targets,
                                  int *count,
                                  enum resolve_scope scope,
                                  const struct store_paths *store,
                                  const char *id,
                                  bool needs_elevation) {
    struct resolved_target *next = realloc(*targets, (size_t)(*count + 1) * sizeof(**targets));
    if (!next) {
        return -1;
    }
    *targets = next;
    fill_target(&(*targets)[*count], scope, store, id, needs_elevation);
    (*count)++;
    return 0;
}

static int append_capability_target(struct resolved_target **targets,
                                    int *count,
                                    const struct store_paths *store,
                                    const char *runid_sel,
                                    const char *alias,
                                    const char *hash) {
    if (append_resolved_target(targets, count, RESOLVE_SYSTEM_MANAGED, store, runid_sel, true) != 0) {
        return -1;
    }
    if (set_target_capability(&(*targets)[*count - 1], alias, hash) != 0) {
        return -1;
    }
    return 0;
}

static int append_private_alias_targets(struct resolved_target **targets,
                                        int *count,
                                        enum resolve_scope scope,
                                        const struct store_paths *store,
                                        const char *alias,
                                        const char *command,
                                        bool all) {
    struct alias_match_list matches;
    if (collect_private_alias_matches(store, alias, command, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count > 1 && (!all || !command_all_allowed(command))) {
        report_alias_ambiguity(command, alias, &matches);
        free_alias_match_list(&matches);
        return -2;
    }
    for (size_t i = 0; i < matches.count; i++) {
        if (append_resolved_target(targets, count, scope, store, matches.items[i].id, false) != 0) {
            free_alias_match_list(&matches);
            return -1;
        }
    }
    free_alias_match_list(&matches);
    return 1;
}

static int collect_public_alias_matches(const struct store_paths *store,
                                        const char *alias,
                                        struct alias_match_list *list) {
    memset(list, 0, sizeof(*list));
    if (!valid_alias(alias)) {
        errno = EINVAL;
        return -1;
    }
    list->alias_known = alias_exists_in_store(store, alias);
    DIR *d = opendir(store->public_dir);
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
        char id[16];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        if (!valid_id(id)) {
            continue;
        }
        struct public_index pi;
        if (load_public_index_by_id(store, id, &pi) != 0 ||
            !pi.has_alias || strcmp(pi.alias, alias) != 0) {
            continue;
        }
        list->alias_known = true;
        struct record pseudo;
        memset(&pseudo, 0, sizeof(pseudo));
        snprintf(pseudo.id, sizeof(pseudo.id), "%s", id);
        if (append_alias_match(list, &pseudo, STATE_UNKNOWN, pi.started_at[0] ? pi.started_at : "-") != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int append_public_alias_elevation_target(struct resolved_target **targets,
                                                int *count,
                                                const struct store_paths *system_store,
                                                const char *alias,
                                                const char *command,
                                                bool all) {
    char hash[PROFILE_HASH_STR_LEN];
    if (alias_lookup_hash(system_store, alias, hash) != 0) {
        if (!public_alias_visible(system_store, alias)) {
            return 0;
        }
        return 1;
    }
    struct alias_match_list matches;
    if (collect_public_alias_matches(system_store, alias, &matches) != 0) {
        return -1;
    }
    if (!matches.alias_known) {
        free_alias_match_list(&matches);
        return 0;
    }
    if (matches.count == 0) {
        free_alias_match_list(&matches);
        return 1;
    }
    if (matches.count > 1) {
        if (!all || !command_all_allowed(command)) {
            report_alias_ambiguity(command, alias, &matches);
            free_alias_match_list(&matches);
            return -2;
        }
        int rc = append_capability_target(targets, count, system_store, "ffffffff", alias, hash) == 0 ? 1 : -1;
        free_alias_match_list(&matches);
        return rc;
    }
    int rc = append_capability_target(targets, count, system_store, matches.items[0].id, alias, hash) == 0 ? 1 : -1;
    free_alias_match_list(&matches);
    return rc;
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
    if (token_scope == ID_TOKEN_INVALID || !valid_target_atom(id)) {
        fprintf(stderr, "sigmund: error: invalid target '%s'\n", token ? token : "");
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
                fill_target(out, RESOLVE_USER_LOCAL, &user_store, resolved, false);
                return 0;
            }
            return 0;
        }
        if (token_scope == ID_TOKEN_SYSTEM) {
            char resolved[16];
            if (resolve_system_private_id(system_store, id, resolved, sizeof(resolved)) == 0) {
                fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false);
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
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, root_resolved, false);
            return 0;
        }
        if (user_match) {
            fill_target(out, RESOLVE_USER_LOCAL, &user_store, user_resolved, false);
            return 0;
        }
        return 0;
    }

    if (token_scope == ID_TOKEN_USER) {
        char resolved[16];
        if (resolve_user_store_id(current_user_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_USER_LOCAL, current_user_store, resolved, false);
            return 0;
        }
        return 0;
    }
    if (token_scope == ID_TOKEN_SYSTEM) {
        char resolved[16];
        if (resolve_system_public_id(system_store, id, resolved, sizeof(resolved)) == 0) {
            fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true);
            return 0;
        }
        return 0;
    }

    char user_resolved[16];
    if (resolve_user_store_id(current_user_store, id, user_resolved, sizeof(user_resolved)) == 0) {
        fill_target(out, RESOLVE_USER_LOCAL, current_user_store, user_resolved, false);
        return 0;
    }
    char system_resolved[16];
    if (resolve_system_public_id(system_store, id, system_resolved, sizeof(system_resolved)) == 0) {
        fill_target(out, RESOLVE_SYSTEM_MANAGED, system_store, system_resolved, true);
        return 0;
    }
    return 0;
}

static int report_not_found(const char *token) {
    fprintf(stderr, "sigmund: error: no run matches '%s'\n", token ? token : "");
    return 5;
}

static int resolve_action_token(const struct invocation *inv,
                                const struct store_paths *current_user_store,
                                const struct store_paths *system_store,
                                const char *command,
                                const char *token,
                                bool all,
                                struct resolved_target **targets_out,
                                int *count_out) {
    *targets_out = NULL;
    *count_out = 0;

    const char *atom = NULL;
    enum id_token_scope scope = parse_id_token(token, &atom);
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    bool cap_token = false;
    if (scope == ID_TOKEN_SYSTEM && parse_alias_cap_atom(atom, cap_alias, cap_hash) == 0) {
        if (!inv->euid_root || verify_system_alias_cap(system_store, cap_alias, cap_hash) != 0) {
            return report_not_found(token);
        }
        atom = cap_alias;
        cap_token = true;
    }
    if (scope == ID_TOKEN_INVALID || (!valid_id_prefix(atom) && !valid_alias(atom))) {
        fprintf(stderr, "sigmund: error: invalid target '%s'\n", token ? token : "");
        return 5;
    }

    if (!cap_token && valid_id_prefix(atom)) {
        char resolved[16];
        if (inv->euid_root) {
            if (scope == ID_TOKEN_USER) {
                struct store_paths user_store;
                if (init_invoking_user_store(inv, &user_store) != 0) {
                    fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                    return 5;
                }
                if (resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                }
            } else if (scope == ID_TOKEN_SYSTEM) {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
            } else {
                if (resolve_system_private_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, false) == 0 ? 0 : 3;
                }
                if (inv->have_sudo_user) {
                    struct store_paths user_store;
                    if (init_invoking_user_store(inv, &user_store) == 0 &&
                        resolve_user_store_id(&user_store, atom, resolved, sizeof(resolved)) == 0) {
                        return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, resolved, false) == 0 ? 0 : 3;
                    }
                }
            }
        } else {
            if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
                if (resolve_user_store_id(current_user_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, resolved, false) == 0 ? 0 : 3;
                }
                if (scope == ID_TOKEN_USER) {
                    return report_not_found(token);
                }
            }
            if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
                if (resolve_system_public_id(system_store, atom, resolved, sizeof(resolved)) == 0) {
                    return append_resolved_target(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, resolved, true) == 0 ? 0 : 3;
                }
            }
        }
    }

    if (!valid_alias(atom)) {
        return report_not_found(token);
    }

    int rc = 0;
    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                return 5;
            }
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : report_not_found(token);
        }
        if (scope == ID_TOKEN_SYSTEM) {
            rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
            if (rc == 1) return 0;
            if (rc == -2) return 6;
            return rc < 0 ? 3 : report_not_found(token);
        }

        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_SYSTEM_MANAGED, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (inv->have_sudo_user) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) == 0) {
                rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, &user_store, atom, command, all);
                if (rc == 1) return 0;
                if (rc == -2) return 6;
                if (rc < 0) return 3;
            }
        }
        return report_not_found(token);
    }

    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        rc = append_private_alias_targets(targets_out, count_out, RESOLVE_USER_LOCAL, current_user_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
        if (scope == ID_TOKEN_USER) {
            return report_not_found(token);
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        rc = append_public_alias_elevation_target(targets_out, count_out, system_store, atom, command, all);
        if (rc == 1) return 0;
        if (rc == -2) return 6;
        if (rc < 0) return 3;
    }
    return report_not_found(token);
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

static int elevate_with_sudo_parsed(const char *program,
                                    bool owned,
                                    const char *command,
                                    bool tail,
                                    bool console_mode,
                                    bool all,
                                    bool print_cmd,
                                    bool multi,
                                    int multi_count,
                                    bool force_raw,
                                    int argc,
                                    char **argv) {
    int extra = argc;
    if (owned) {
        extra += 1;
        if (!strcmp(command, "start") && tail) {
            extra += 1;
        }
        if (!strcmp(command, "start") && console_mode) {
            extra += 1;
        }
        if (all) {
            extra += 1;
        }
        if (print_cmd) {
            extra += 1;
        }
        if (!strcmp(command, "start") && multi) {
            extra += multi_count == 1 ? 1 : 2;
        }
    } else {
        if (tail) {
            extra += 1;
        }
        if (console_mode) {
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
    char count_buf[32];
    int n = 0;
    if (owned) {
        canon[n++] = (char *)command;
        if (!strcmp(command, "start") && tail) {
            canon[n++] = "--tail";
        }
        if (!strcmp(command, "start") && console_mode) {
            canon[n++] = "--console";
        }
        if (all) {
            canon[n++] = "--all";
        }
        if (print_cmd) {
            canon[n++] = "--print";
        }
        if (!strcmp(command, "start") && multi) {
            canon[n++] = "--multi";
            if (multi_count != 1) {
                snprintf(count_buf, sizeof(count_buf), "%d", multi_count);
                canon[n++] = count_buf;
            }
        }
    } else {
        if (tail) {
            canon[n++] = "--tail";
        }
        if (console_mode) {
            canon[n++] = "--console";
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
                               int ntargets,
                               bool all,
                               bool print_cmd) {
    int target_argc = 0;
    bool has_capability = false;
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].has_capability) {
            target_argc += 3;
            has_capability = true;
        } else {
            target_argc += 1;
        }
    }
    int canonical_argc = 1 + ((!has_capability && all) ? 1 : 0) + (print_cmd ? 1 : 0) + target_argc;
    char **canon = calloc((size_t)canonical_argc, sizeof(char *));
    char **tokens = calloc((size_t)ntargets, sizeof(char *));
    if (!canon || !tokens) {
        free(canon);
        free(tokens);
        return 3;
    }

    int n = 0;
    canon[n++] = (char *)command;
    if (!has_capability && all) {
        canon[n++] = "--all";
    }
    if (print_cmd) {
        canon[n++] = "--print";
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].has_capability) {
            canon[n++] = (char *)targets[i].id;
            canon[n++] = (char *)targets[i].cap_alias;
            canon[n++] = (char *)targets[i].cap_hash;
            continue;
        }
        const char *orig_id = NULL;
        enum id_token_scope orig_scope = parse_id_token(original_tokens ? original_tokens[i] : NULL, &orig_id);
        const char *prefix = "";
        if (targets[i].scope == RESOLVE_USER_LOCAL) {
            prefix = "user:";
        } else if (targets[i].scope == RESOLVE_SYSTEM_MANAGED &&
                   (orig_scope == ID_TOKEN_SYSTEM || targets[i].needs_elevation)) {
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
        canon[n++] = tokens[i];
    }

    int rc = elevate_with_sudo_canonical(program, canonical_argc, canon);
    for (int i = 0; i < ntargets; i++) free(tokens[i]);
    free(tokens);
    free(canon);
    return rc;
}

static int do_print_signal_command(const struct store_paths *store, const char *id, int sig) {
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
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    if (r.has_boot && have_boot && strcmp(r.boot_id, boot) != 0) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }
    if (st == STATE_STALE) {
        fprintf(stderr, "sigmund: error: run %s is stale and cannot be signaled\n", id);
        return 2;
    }
    if (st == STATE_UNKNOWN) {
        fprintf(stderr, "sigmund: error: run %s could not be validated and cannot be signaled\n", id);
        return 2;
    }
    printf("kill -%s -- -%ld\n", sig == SIGKILL ? "KILL" : "TERM", (long)r.pgid);
    return 0;
}

static int cmd_signal_action(const struct invocation *inv,
                             const struct store_paths *user_store,
                             const struct store_paths *system_store,
                             const char *program,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd) {
    if (argc <= 0) {
        fprintf(stderr, "usage: sigmund %s <target>...\n", command);
        return 5;
    }
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    for (int i = 0; i < argc; i++) {
        struct resolved_target *one = NULL;
        int none = 0;
        int rc = resolve_action_token(inv, user_store, system_store, command, argv[i], all, &one, &none);
        if (rc != 0) {
            free(one);
            free(targets);
            return rc;
        }
        if (none > 0) {
            struct resolved_target *next = realloc(targets, (size_t)(ntargets + none) * sizeof(*targets));
            if (!next) {
                free(one);
                free(targets);
                return 3;
            }
            targets = next;
            memcpy(targets + ntargets, one, (size_t)none * sizeof(*targets));
            ntargets += none;
        }
        free(one);
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }
    bool need_elevation = false;
    for (int i = 0; i < ntargets; i++) {
        need_elevation = need_elevation || targets[i].needs_elevation;
    }
    if (need_elevation) {
        int rc = elevate_with_sudo_targets(program, command, NULL, targets, ntargets, all, print_cmd);
        free(targets);
        return rc;
    }
    int worst = 0;
    for (int i = 0; i < ntargets; i++) {
        bool already_done = false;
        int rc = print_cmd ? do_print_signal_command(&targets[i].store, targets[i].id, sig)
                           : do_signal_action(&targets[i].store, targets[i].id, sig, graceful, &already_done);
        if (!print_cmd && rc == 0) {
            if (already_done) {
                sig_note(inv, "sigmund: %s already exited\n", targets[i].id);
            } else {
                sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", targets[i].id);
            }
        }
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
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "tail", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to tail\n");
        return 0;
    }
    struct resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = elevate_with_sudo_targets(program, "tail", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    rc = tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
    free(targets);
    return rc;
}

static int cmd_dump_action(const struct invocation *inv,
                           const struct store_paths *user_store,
                           const struct store_paths *system_store,
                           const char *program,
                           const char *id_token) {
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "dump", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to dump\n");
        return 0;
    }
    struct resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = elevate_with_sudo_targets(program, "dump", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    if (!r.has_log) {
        fprintf(stderr, "sigmund: record has no log path: %s\n", target.id);
        free(targets);
        return 5;
    }
    int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        free(targets);
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
            free(targets);
            die_errno("sigmund: failed while dumping log");
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            close(fd);
            free(targets);
            die_errno("sigmund: failed writing dumped output");
        }
    }
    close(fd);
    free(targets);
    return 0;
}

static int run_socat_console(const char *sock_path) {
    if (!executable_available("socat")) {
        fprintf(stderr, "sigmund: console requires socat (not found in PATH)\n");
        return 1;
    }
    struct stat st;
    if (stat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "sigmund: console socket is not available\n");
        return 5;
    }
    char connect_arg[SIGMUND_PATH_MAX + 16];
    if (checked_snprintf(connect_arg, sizeof(connect_arg), "UNIX-CONNECT:%s", sock_path) != 0) {
        fprintf(stderr, "sigmund: console socket path is too long\n");
        return 5;
    }
    const char *stdio_arg = isatty(STDIN_FILENO) ? "-,raw,echo=0" : "-";
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "sigmund: failed to fork socat: %s\n", strerror(errno));
        return 3;
    }
    if (pid == 0) {
        execlp("socat", "socat", stdio_arg, connect_arg, (char *)NULL);
        fprintf(stderr, "sigmund: failed to exec socat: %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fprintf(stderr, "sigmund: failed to wait for socat: %s\n", strerror(errno));
        return 3;
    }
    return child_status_to_exit_code(status);
}

static int attach_console_record(const struct invocation *inv,
                                 const struct record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        sig_note(inv, "sigmund: %s has exited - see 'sigmund dump %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        sig_note(inv, "sigmund: %s has no console (start with --console)\n", r->id);
        return 0;
    }
    return run_socat_console(r->console_sock);
}

static int cmd_console_action(const struct invocation *inv,
                              const struct store_paths *user_store,
                              const struct store_paths *system_store,
                              const char *program,
                              const char *id_token) {
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to console\n");
        return 0;
    }
    struct resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = elevate_with_sudo_targets(program, "console", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct record r;
    char path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    enum run_state st = eval_state(&r, have_boot ? boot : NULL);
    rc = attach_console_record(inv, &r, st);
    free(targets);
    return rc;
}

static int cmd_prune_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const char *target_token,
                            bool all) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        const struct store_paths *store = inv->euid_root ? system_store : user_store;
        int removed = 0;
        int rc = cmd_prune_store_all(store, target_token && strcmp(target_token, "all") == 0, &removed);
        if (rc == 0) {
            if (removed > 0) {
                sig_note(inv, "sigmund: pruned %d past run%s\n", removed, removed == 1 ? "" : "s");
            } else {
                sig_note(inv, "sigmund: nothing to prune\n");
            }
        }
        return rc;
    }
    struct resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = resolve_action_token(inv, user_store, system_store, "prune", target_token, all, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sig_note(inv, "sigmund: nothing to prune\n");
        return 0;
    }
    bool need_elevation = false;
    for (int i = 0; i < ntargets; i++) {
        need_elevation = need_elevation || targets[i].needs_elevation;
    }
    if (need_elevation) {
        rc = elevate_with_sudo_targets(program, "prune", NULL, targets, ntargets, all, false);
        free(targets);
        return rc;
    }
    char boot[128] = {0};
    bool have_boot = current_boot_id(boot, sizeof(boot));
    int worst = 0;
    int removed_count = 0;
    for (int i = 0; i < ntargets; i++) {
        bool removed = false;
        rc = prune_one_run(&targets[i].store, targets[i].id, have_boot ? boot : NULL, true, &removed);
        if (removed) {
            removed_count++;
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    if (worst == 0) {
        if (removed_count > 0) {
            const char *atom = NULL;
            enum id_token_scope token_scope = parse_id_token(target_token, &atom);
            bool target_looks_like_alias = (token_scope != ID_TOKEN_INVALID && atom &&
                                            valid_alias(atom) && !valid_id_prefix(atom));
            if (target_looks_like_alias) {
                sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n",
                         removed_count, removed_count == 1 ? "" : "s", atom);
            } else {
                sig_note(inv, "sigmund: pruned %d past run%s\n", removed_count, removed_count == 1 ? "" : "s");
            }
        } else {
            sig_note(inv, "sigmund: nothing to prune\n");
        }
    }
    free(targets);
    return worst;
}

static int profile_exists_in_store(const struct store_paths *store, const char *hash) {
    struct profile p;
    if (load_profile_by_hash(store, hash, &p) != 0) {
        return -1;
    }
    free_profile(&p);
    return 0;
}

static int private_start_hash_for_token(const struct store_paths *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN],
                                        bool *matched) {
    *matched = false;
    if (valid_profile_hash(token)) {
        *matched = true;
        if (profile_exists_in_store(store, token) != 0) {
            return -1;
        }
        snprintf(hash, PROFILE_HASH_STR_LEN, "%s", token);
        return 1;
    }
    if (valid_alias(token)) {
        if (alias_lookup_hash(store, token, hash) != 0) {
            return 0;
        }
        *matched = true;
        if (profile_exists_in_store(store, hash) != 0) {
            return -1;
        }
        return 1;
    }
    return 0;
}

static int private_start_recipe_for_token(const struct store_paths *store,
                                          const char *token,
                                          struct profile *recipe,
                                          bool *matched) {
    *matched = false;
    memset(recipe, 0, sizeof(*recipe));
    if (!valid_alias(token)) {
        return 0;
    }
    if (alias_lookup_recipe(store, token, recipe) != 0) {
        return 0;
    }
    *matched = true;
    return 1;
}

struct start_profile_target {
    struct store_paths store;
    char hash[PROFILE_HASH_STR_LEN];
    char alias[ALIAS_MAX_LEN + 1];
    struct profile recipe;
    bool has_hash;
    bool has_alias;
    bool has_recipe;
    bool needs_elevation;
};

static void free_start_profile_target(struct start_profile_target *target) {
    if (target && target->has_recipe) {
        free_profile(&target->recipe);
        target->has_recipe = false;
    }
}

static void start_target_set_alias(struct start_profile_target *target, const char *alias) {
    if (valid_alias(alias)) {
        if (checked_snprintf(target->alias, sizeof(target->alias), "%s", alias) == 0) {
            target->has_alias = true;
        }
    }
}

static int start_target_set_recipe(struct start_profile_target *target,
                                   const struct store_paths *store,
                                   const char *alias) {
    target->store = *store;
    target->has_recipe = true;
    start_target_set_alias(target, alias);
    return 1;
}

static int start_target_set_hash(struct start_profile_target *target,
                                 const struct store_paths *store,
                                 const char *hash,
                                 const char *alias,
                                 bool needs_elevation) {
    if (!valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    target->store = *store;
    if (checked_snprintf(target->hash, sizeof(target->hash), "%s", hash) != 0) {
        return -1;
    }
    target->has_hash = true;
    target->needs_elevation = needs_elevation;
    start_target_set_alias(target, alias);
    return 1;
}

static int count_running_alias(const struct store_paths *store, const char *alias, size_t *count_out) {
    struct alias_match_list matches;
    if (collect_private_alias_matches(store, alias, "start", &matches) != 0) {
        return -1;
    }
    *count_out = matches.count;
    free_alias_match_list(&matches);
    return 0;
}

static int resolve_start_profile_target(const struct invocation *inv,
                                        const struct store_paths *current_user_store,
                                        const struct store_paths *system_store,
                                        const char *token,
                                        struct start_profile_target *out) {
    memset(out, 0, sizeof(*out));
    const char *atom = NULL;
    enum id_token_scope scope = parse_id_token(token, &atom);
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    bool cap_token = false;
    if (scope == ID_TOKEN_SYSTEM && parse_alias_cap_atom(atom, cap_alias, cap_hash) == 0) {
        if (!inv->euid_root || verify_system_alias_cap(system_store, cap_alias, cap_hash) != 0) {
            fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        atom = cap_alias;
        cap_token = true;
    }
    if (scope == ID_TOKEN_INVALID) {
        return 0;
    }
    if (!valid_profile_hash(atom) && !valid_alias(atom)) {
        return 0;
    }

    if (inv->euid_root) {
        if (scope == ID_TOKEN_USER) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) != 0) {
                fprintf(stderr, "sigmund: error: user:%s requires sudo provenance\n", atom);
                return -1;
            }
            bool matched = false;
            int rc = private_start_recipe_for_token(&user_store, atom, &out->recipe, &matched);
            if (rc == 1) {
                return start_target_set_recipe(out, &user_store, atom);
            }
            rc = private_start_hash_for_token(&user_store, atom, out->hash, &matched);
            if (rc == 1) {
                return start_target_set_hash(out, &user_store, out->hash, valid_alias(atom) ? atom : NULL, false);
            }
            if (rc < 0 && matched) {
                fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            return 0;
        }
        if (scope == ID_TOKEN_SYSTEM) {
            if (cap_token) {
                bool matched = false;
                int rc = private_start_hash_for_token(system_store, cap_hash, out->hash, &matched);
                if (rc == 1) {
                    return start_target_set_hash(out, system_store, out->hash, cap_alias, false);
                }
                fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            bool matched = false;
            int rc = private_start_hash_for_token(system_store, atom, out->hash, &matched);
            if (rc == 1) {
                return start_target_set_hash(out, system_store, out->hash, valid_alias(atom) ? atom : NULL, false);
            }
            if (rc < 0 && matched) {
                fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
                return -1;
            }
            return 0;
        }

        bool matched = false;
        int rc = private_start_hash_for_token(system_store, atom, out->hash, &matched);
        if (rc == 1) {
            return start_target_set_hash(out, system_store, out->hash, valid_alias(atom) ? atom : NULL, false);
        }
        if (rc < 0 && matched) {
            fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        if (inv->have_sudo_user) {
            struct store_paths user_store;
            if (init_invoking_user_store(inv, &user_store) == 0) {
                matched = false;
                rc = private_start_recipe_for_token(&user_store, atom, &out->recipe, &matched);
                if (rc == 1) {
                    return start_target_set_recipe(out, &user_store, atom);
                }
                rc = private_start_hash_for_token(&user_store, atom, out->hash, &matched);
                if (rc == 1) {
                    return start_target_set_hash(out, &user_store, out->hash, valid_alias(atom) ? atom : NULL, false);
                }
                if (rc < 0 && matched) {
                    fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
                    return -1;
                }
            }
        }
        return 0;
    }

    if (scope == ID_TOKEN_USER || scope == ID_TOKEN_PLAIN) {
        bool matched = false;
        int rc = private_start_recipe_for_token(current_user_store, atom, &out->recipe, &matched);
        if (rc == 1) {
            return start_target_set_recipe(out, current_user_store, atom);
        }
        rc = private_start_hash_for_token(current_user_store, atom, out->hash, &matched);
        if (rc == 1) {
            return start_target_set_hash(out, current_user_store, out->hash, valid_alias(atom) ? atom : NULL, false);
        }
        if (rc < 0 && matched) {
            fprintf(stderr, "sigmund: error: profile for '%s' is unavailable\n", token);
            return -1;
        }
        if (scope == ID_TOKEN_USER) {
            return 0;
        }
    }

    if (scope == ID_TOKEN_SYSTEM || scope == ID_TOKEN_PLAIN) {
        int rc = resolve_public_profile_token(system_store, atom, out->hash);
        if (rc == 1) {
            return start_target_set_hash(out, system_store, out->hash, valid_alias(atom) ? atom : NULL, true);
        }
    }
    return 0;
}

static int elevate_start_token(const char *program,
                               bool tail,
                               bool console_mode,
                               const char *token_atom,
                               const char *hash,
                               bool multi,
                               int multi_count) {
    char token[8 + ALIAS_MAX_LEN + 1 + PROFILE_HASH_STR_LEN];
    char count_buf[32];
    char *canon[7];
    int n = 0;
    canon[n++] = "start";
    if (tail) {
        canon[n++] = "--tail";
    }
    if (console_mode) {
        canon[n++] = "--console";
    }
    if (hash && valid_alias(token_atom) && valid_profile_hash(hash)) {
        canon[n++] = "00000000";
        canon[n++] = (char *)token_atom;
        canon[n++] = (char *)hash;
        return elevate_with_sudo_canonical(program, n, canon);
    } else if (checked_snprintf(token, sizeof(token), "system:%s", token_atom) != 0) {
        return 3;
    }
    canon[n++] = token;
    if (multi) {
        canon[n++] = "--multi";
        if (multi_count != 1) {
            snprintf(count_buf, sizeof(count_buf), "%d", multi_count);
            canon[n++] = count_buf;
        }
    }
    return elevate_with_sudo_canonical(program, n, canon);
}

static int perform_profile_start(const struct invocation *inv,
                                 const struct store_paths *store,
                                 bool tail,
                                 bool console_mode,
                                 const char *hash,
                                 const char *alias) {
    struct profile p;
    if (load_profile_by_hash(store, hash, &p) != 0) {
        fprintf(stderr, "sigmund: error: profile %s is unavailable\n", hash);
        return 5;
    }
    int rc = perform_start(inv, store, tail, console_mode, p.argc, p.argv, p.binary_path, alias);
    free_profile(&p);
    return rc;
}

static int perform_explicit_start(const struct invocation *inv,
                                  const struct store_paths *store,
                                  bool tail,
                                  bool console_mode,
                                  int argc,
                                  char **argv);

static int cmd_start_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            const struct store_paths *fallback_store,
                            bool tail,
                            bool console_mode,
                            bool multi,
                            int multi_count,
                            int argc,
                            char **argv) {
    if (argc == 1) {
        struct start_profile_target target;
        int rc = resolve_start_profile_target(inv, user_store, system_store, argv[0], &target);
        if (rc < 0) {
            return 5;
        }
        if (rc == 1) {
            int start_rc;
            int starts = multi ? multi_count : 1;
            if (tail && starts > 1) {
                fprintf(stderr, "sigmund: error: --tail cannot follow multiple starts\n");
                free_start_profile_target(&target);
                return 5;
            }
            if (target.needs_elevation) {
                start_rc = 0;
                for (int i = 0; i < starts; i++) {
                    start_rc = elevate_start_token(program,
                                                   tail,
                                                   console_mode,
                                                   target.has_alias ? target.alias : target.hash,
                                                   target.has_alias ? target.hash : NULL,
                                                   false,
                                                   1);
                    if (start_rc != 0) {
                        break;
                    }
                }
            } else {
                if (target.has_alias && !multi) {
                    size_t running = 0;
                    if (count_running_alias(&target.store, target.alias, &running) != 0) {
                        free_start_profile_target(&target);
                        return 3;
                    }
                    if (running > 0) {
                        fprintf(stderr,
                                "sigmund: error: alias '%s' already has a running process; use --multi to start another\n",
                                target.alias);
                        free_start_profile_target(&target);
                        return 6;
                    }
                }
                start_rc = 0;
                for (int i = 0; i < starts; i++) {
                    if (target.has_recipe) {
                        start_rc = perform_start(inv,
                                                 &target.store,
                                                 tail,
                                                 console_mode,
                                                 target.recipe.argc,
                                                 target.recipe.argv,
                                                 target.recipe.binary_path,
                                                 target.has_alias ? target.alias : NULL);
                    } else {
                        start_rc = perform_profile_start(inv,
                                                         &target.store,
                                                         tail,
                                                         console_mode,
                                                         target.hash,
                                                         target.has_alias ? target.alias : NULL);
                    }
                    if (start_rc != 0) {
                        break;
                    }
                }
            }
            free_start_profile_target(&target);
            return start_rc;
        }
        free_start_profile_target(&target);
    }
    if (multi) {
        fprintf(stderr, "sigmund: error: --multi applies only to alias starts\n");
        return 5;
    }
    return perform_explicit_start(inv, fallback_store, tail, console_mode, argc, argv);
}

static bool command_accepts_target_tokens(const char *command) {
    return command && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                       !strcmp(command, "tail") || !strcmp(command, "dump") ||
                       !strcmp(command, "prune") || !strcmp(command, "console"));
}

static int maybe_elevate_requested_system_targets(const char *program,
                                                  const char *command,
                                                  int argc,
                                                  char **argv,
                                                  bool all,
                                                  int *rc_out) {
    if (!command_accepts_target_tokens(command) || argc <= 0) {
        return 0;
    }
    struct store_paths system_store;
    if (init_system_store(&system_store) != 0) {
        return 0;
    }
    char **canon = calloc((size_t)argc * 3 + 3, sizeof(char *));
    char **owned_tokens = calloc((size_t)argc * 3, sizeof(char *));
    if (!canon || !owned_tokens) {
        free(canon);
        free(owned_tokens);
        *rc_out = 3;
        return 1;
    }
    bool changed = false;
    int n = 0;
    canon[n++] = (char *)command;
    for (int i = 0; i < argc; i++) {
        const char *token = argv[i];
        const char *atom = NULL;
        enum id_token_scope scope = parse_id_token(token, &atom);
        if (!strcmp(command, "prune") && strcmp(token, "all") == 0) {
            canon[n++] = argv[i];
            continue;
        }
        if ((scope == ID_TOKEN_PLAIN || scope == ID_TOKEN_SYSTEM) && atom && valid_alias(atom)) {
            char hash[PROFILE_HASH_STR_LEN];
            if (alias_lookup_hash(&system_store, atom, hash) == 0) {
                struct alias_match_list matches;
                if (collect_public_alias_matches(&system_store, atom, &matches) != 0) {
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    *rc_out = 3;
                    return 1;
                }
                const char *selector = NULL;
                char selector_buf[16];
                if (matches.count == 0) {
                    free_alias_match_list(&matches);
                    *rc_out = 0;
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    return 1;
                }
                if (matches.count > 1) {
                    if (!all || !command_all_allowed(command)) {
                        report_alias_ambiguity(command, atom, &matches);
                        free_alias_match_list(&matches);
                        *rc_out = 6;
                        for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                        free(owned_tokens);
                        free(canon);
                        return 1;
                    }
                    selector = "ffffffff";
                } else {
                    snprintf(selector_buf, sizeof(selector_buf), "%s", matches.items[0].id);
                    selector = selector_buf;
                }
                size_t slot = (size_t)i * 3;
                owned_tokens[slot] = strdup(selector);
                owned_tokens[slot + 1] = strdup(atom);
                owned_tokens[slot + 2] = strdup(hash);
                free_alias_match_list(&matches);
                if (!owned_tokens[slot] || !owned_tokens[slot + 1] || !owned_tokens[slot + 2]) {
                    for (int j = 0; j < argc * 3; j++) free(owned_tokens[j]);
                    free(owned_tokens);
                    free(canon);
                    *rc_out = 3;
                    return 1;
                }
                canon[n++] = owned_tokens[slot];
                canon[n++] = owned_tokens[slot + 1];
                canon[n++] = owned_tokens[slot + 2];
                changed = true;
                continue;
            }
        }
        canon[n++] = argv[i];
    }
    if (!changed) {
        free(owned_tokens);
        free(canon);
        return 0;
    }
    *rc_out = elevate_with_sudo_canonical(program, n, canon);
    for (int i = 0; i < argc * 3; i++) {
        free(owned_tokens[i]);
    }
    free(owned_tokens);
    free(canon);
    return 1;
}

static int cmd_alias_action(const struct invocation *inv,
                            const struct store_paths *user_store,
                            const struct store_paths *system_store,
                            const char *program,
                            int argc,
                            char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: sigmund alias <id> <name> [-v]\n");
        return 5;
    }
    const char *target_token = argv[0];
    const char *name = argv[1];
    bool verbose = false;
    if (argc == 3) {
        if (strcmp(argv[2], "-v") != 0 && strcmp(argv[2], "--verbose") != 0) {
            fprintf(stderr, "usage: sigmund alias <id> <name> [-v]\n");
            return 5;
        }
        verbose = true;
    }
    if (!valid_alias(name)) {
        fprintf(stderr, "sigmund: error: invalid alias '%s'\n", name);
        return 5;
    }

    struct resolved_target target;
    if (resolve_target(inv, user_store, system_store, target_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return report_not_found(target_token);
    }
    if (target.needs_elevation) {
        char scoped[8 + PROFILE_HASH_STR_LEN];
        if (checked_snprintf(scoped, sizeof(scoped), "system:%s", target.id) != 0) {
            return 3;
        }
        char *canon[3] = {"alias", scoped, (char *)name};
        return elevate_with_sudo_canonical(program, 3, canon);
    }
    if (target.store.kind == STORE_USER_LOCAL && inv->euid_root) {
        fprintf(stderr, "sigmund: error: create user-local aliases as that user\n");
        return 5;
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        if (ensure_system_store(&target.store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
    } else if (ensure_user_store_for_current_user(&target.store) != 0) {
        die_errno("sigmund: failed to init user storage");
    }

    struct record r;
    char record_path[SIGMUND_PATH_MAX];
    if (load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    char *j = NULL;
    if (read_owned_file_no_symlink(record_path, &j) != 0) {
        return 5;
    }
    char **profile_argv = NULL;
    int profile_argc = 0;
    char binary_path[SIGMUND_PATH_MAX];
    char hash[PROFILE_HASH_STR_LEN];
    int rc = 0;
    if (json_get_argv_alloc(j, &profile_argv, &profile_argc) != 0 ||
        resolve_binary_path(profile_argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "sigmund: error: failed to derive profile from run %s\n", target.id);
        rc = 5;
        goto out;
    }
    char command[256];
    if (format_argv_human(command, sizeof(command), profile_argc, profile_argv) != 0) {
        snprintf(command, sizeof(command), "%s", "?");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        profile_hash_for_argv(binary_path, profile_argc, profile_argv, hash);
        if (write_profile_atomic(&target.store, hash, binary_path, profile_argc, profile_argv) != 0) {
            die_errno("sigmund: failed to write profile");
        }
        if (alias_upsert_hash(&target.store, name, hash) != 0) {
            die_errno("sigmund: failed to write alias");
        }
        if (verbose) {
            sig_note(inv, "sigmund: pinned '%s' -> %s (hash %s)\n", name, command, hash);
        } else {
            sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
        }
    } else {
        if (alias_upsert_recipe(&target.store, name, binary_path, profile_argc, profile_argv) != 0) {
            die_errno("sigmund: failed to write alias");
        }
        sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
    }

out:
    free_argv_alloc(profile_argv, profile_argc);
    free(j);
    return rc;
}

static int print_aliases_for_store(const char *scope, const struct store_paths *store, bool verbose) {
    struct alias_entry *entries = NULL;
    size_t count = 0;
    if (load_aliases(store, &entries, &count) != 0) {
        fprintf(stderr, "sigmund: warning: failed to read %s aliases\n", scope);
        return 5;
    }
    for (size_t i = 0; i < count; i++) {
        char command[96];
        char hash_display[PROFILE_HASH_STR_LEN];
        if (entries[i].has_recipe) {
            if (format_argv_human(command, sizeof(command), entries[i].argc, entries[i].argv) != 0) {
                snprintf(command, sizeof(command), "%s", "?");
            }
        } else {
            snprintf(command, sizeof(command), "%s", "<root-managed>");
        }
        if (entries[i].has_hash) {
            if (verbose) {
                snprintf(hash_display, sizeof(hash_display), "%s", entries[i].hash);
            } else {
                snprintf(hash_display, sizeof(hash_display), "%.12s...", entries[i].hash);
            }
        } else {
            snprintf(hash_display, sizeof(hash_display), "%s", "-");
        }
        printf("%-12s %-6s %-40.40s %s\n", entries[i].name, scope, command, hash_display);
    }
    free_aliases(entries, count);
    return 0;
}

static int cmd_aliases_action(const struct invocation *inv,
                              const struct store_paths *user_store,
                              const struct store_paths *system_store,
                              bool verbose) {
    printf("%-12s %-6s %-40s %s\n", "NAME", "SCOPE", "COMMAND", "HASH");
    int rc = 0;
    if (inv->euid_root) {
        if (print_aliases_for_store("system", system_store, verbose) != 0) {
            rc = 5;
        }
        if (!inv->requested_system && inv->have_sudo_user) {
            struct store_paths sudo_user_store;
            if (init_invoking_user_store(inv, &sudo_user_store) == 0 &&
                print_aliases_for_store("user", &sudo_user_store, verbose) != 0) {
                rc = 5;
            }
        }
        return rc;
    }
    if (print_aliases_for_store("user", user_store, verbose) != 0) {
        rc = 5;
    }
    if (print_aliases_for_store("system", system_store, verbose) != 0) {
        rc = 5;
    }
    return rc;
}

static const char *const grant_action_names[] = {"start", "stop", "kill", "tail", "dump", "prune", "console"};
#define GRANT_ACTION_COUNT ((int)(sizeof(grant_action_names) / sizeof(grant_action_names[0])))

static int parse_grant_subject(const char *input, char *out, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (strcmp(input, "all") == 0 || strcmp(input, "ALL") == 0) {
        return checked_snprintf(out, n, "%s", "ALL");
    }
    const char *name = input;
    bool group = false;
    if (*name == '%') {
        group = true;
        name++;
        if (!*name) {
            return -1;
        }
    }
    size_t len = strlen(name);
    if (len == 0 || len > ALIAS_MAX_LEN) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return -1;
        }
    }
    return checked_snprintf(out, n, "%s%s", group ? "%" : "", name);
}

static int grant_action_index(const char *name) {
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (strcmp(name, grant_action_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int parse_grant_actions(const char *input, bool selected[GRANT_ACTION_COUNT], bool *all_scope) {
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        selected[i] = false;
    }
    *all_scope = false;
    if (!input || !*input) {
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            selected[i] = true;
        }
        *all_scope = true;
        return 0;
    }
    if (strlen(input) > 128) {
        return -1;
    }
    char buf[129];
    snprintf(buf, sizeof(buf), "%s", input);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (!*tok) {
            return -1;
        }
        int idx = grant_action_index(tok);
        if (idx < 0) {
            return -1;
        }
        selected[idx] = true;
    }
    return 0;
}

static int validate_sigmund_self_for_sudoers(const char *program, char *abs_sigmund, size_t n) {
    if (resolve_self_executable_path(program, abs_sigmund, n) != 0) {
        fprintf(stderr, "sigmund: error: cannot determine executable path for sudoers grant\n");
        return -1;
    }
    for (const char *p = abs_sigmund; *p; p++) {
        if (isspace((unsigned char)*p)) {
            fprintf(stderr, "sigmund: error: executable path contains whitespace and cannot be safely managed in sudoers\n");
            return -1;
        }
    }
    struct stat st;
    if (stat(abs_sigmund, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "sigmund: error: executable path is not a regular file: %s\n", abs_sigmund);
        return -1;
    }
    if (st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        fprintf(stderr,
                "sigmund: error: refusing sudoers grant because %s is not root-owned with group/world writes disabled\n",
                abs_sigmund);
        return -1;
    }
    return 0;
}

static const char *sudoers_dir_path(void) {
#ifdef SIGMUND_TESTING
    const char *override = getenv("SIGMUND_TEST_SUDOERS_DIR");
    if (override && *override) {
        return override;
    }
#endif
    return "/etc/sudoers.d";
}

static int resolve_system_alias_hash_for_grant(const struct store_paths *system_store,
                                               const char *alias,
                                               char hash[PROFILE_HASH_STR_LEN]) {
    if (!valid_alias(alias)) {
        return -1;
    }
    if (alias_lookup_hash(system_store, alias, hash) != 0) {
        return -1;
    }
    return profile_exists_in_store(system_store, hash);
}

static int build_action_alternation(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n) {
    size_t off = 0;
    out[0] = '\0';
    bool first = true;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (!selected[i]) {
            continue;
        }
        const char *name = grant_action_names[i];
        size_t need = strlen(name) + (first ? 0 : 1);
        if (off + need + 3 >= n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (!first) {
            out[off++] = '|';
        }
        memcpy(out + off, name, strlen(name));
        off += strlen(name);
        out[off] = '\0';
        first = false;
    }
    if (first) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int build_actions_csv(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n) {
    size_t off = 0;
    out[0] = '\0';
    bool first = true;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (!selected[i]) {
            continue;
        }
        const char *name = grant_action_names[i];
        size_t need = strlen(name) + (first ? 0 : 1);
        if (off + need + 1 >= n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (!first) {
            out[off++] = ',';
        }
        memcpy(out + off, name, strlen(name));
        off += strlen(name);
        out[off] = '\0';
        first = false;
    }
    return first ? -1 : 0;
}

static int build_sudoers_line(char *out,
                              size_t n,
                              const char *subject,
                              const char *abs_sigmund,
                              const bool selected[GRANT_ACTION_COUNT],
                              const char *alias,
                              const char *hash) {
    char verb_alt[128];
    if (build_action_alternation(selected, verb_alt, sizeof(verb_alt)) != 0) {
        return -1;
    }
    return checked_snprintf(out, n,
                            "%s ALL=(root) NOPASSWD: %s ^--system --elevated (%s) [0-9a-f]{8} %s %s$",
                            subject, abs_sigmund, verb_alt, alias, hash);
}

static bool any_grant_action_selected(const bool selected[GRANT_ACTION_COUNT]) {
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (selected[i]) {
            return true;
        }
    }
    return false;
}

static int find_visudo(char *out, size_t n) {
#ifdef SIGMUND_TESTING
    const char *override = getenv("SIGMUND_TEST_VISUDO_PROG");
    if (override && *override) {
        return checked_snprintf(out, n, "%s", override);
    }
#endif
    return resolve_binary_path("visudo", out, n);
}

static int validate_sudoers_candidate(const char *path) {
    char visudo[SIGMUND_PATH_MAX];
    if (find_visudo(visudo, sizeof(visudo)) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execl(visudo, visudo, "-cf", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int subject_file_label_for_grant(const char *subject, char *out, size_t n) {
    if (strcmp(subject, "ALL") == 0) {
        return checked_snprintf(out, n, "%s", "all");
    }
    if (subject[0] == '%') {
        return checked_snprintf(out, n, "group_%s", subject + 1);
    }
    return checked_snprintf(out, n, "%s", subject);
}

static void actions_from_existing_sudoers(const char *existing,
                                          const char *subject,
                                          const char *abs_sigmund,
                                          const char *alias,
                                          const char *hash,
                                          bool selected[GRANT_ACTION_COUNT]) {
    (void)subject;
    (void)abs_sigmund;
    (void)alias;
    (void)hash;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        selected[i] = false;
    }
    if (!existing) {
        return;
    }
    const char *p = strstr(existing, "# actions-list:");
    if (!p) {
        return;
    }
    p += strlen("# actions-list:");
    p = skip_ws(p);
    char buf[256];
    size_t len = 0;
    while (p[len] && p[len] != '\n' && len + 1 < sizeof(buf)) {
        buf[len] = p[len];
        len++;
    }
    buf[len] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = (char *)skip_ws(tok);
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        int idx = grant_action_index(tok);
        if (idx >= 0) {
            selected[idx] = true;
        }
    }
}

static int write_sudoers_template_file(const char *sudoers_path,
                                       const char *target_label,
                                       const char *subject,
                                       const char *abs_sigmund,
                                       const char *hash,
                                       const bool selected[GRANT_ACTION_COUNT],
                                       bool all_scope) {
    char dir[SIGMUND_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sudoers_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    char tmp[SIGMUND_PATH_MAX];
    if (checked_snprintf(tmp, sizeof(tmp), "%s.tmp", sudoers_path) != 0) {
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0440);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0440) != 0 || (geteuid() == 0 && fchown(fd, 0, 0) != 0)) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fputs("# sigmund managed sudoers; use sigmund grant/revoke\n", f);
    fprintf(f, "# target: ");
    json_escape(f, target_label);
    fputc('\n', f);
    fprintf(f, "# hash: ");
    json_escape(f, hash);
    fputc('\n', f);
    fprintf(f, "# actions: %s\n", all_scope ? "ALL" : "explicit");
    char actions_csv[256];
    if (build_actions_csv(selected, actions_csv, sizeof(actions_csv)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "# actions-list: %s\n", actions_csv);
    char line[SIGMUND_PATH_MAX + 256];
    if (build_sudoers_line(line, sizeof(line), subject, abs_sigmund, selected, target_label, hash) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fputs(line, f);
    fputc('\n', f);
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (validate_sudoers_candidate(tmp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, sudoers_path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)fsync_dir_path(dir);
    return 0;
}

static int unlink_sudoers_template_file(const char *sudoers_path) {
    if (unlink(sudoers_path) != 0 && errno != ENOENT) {
        return -1;
    }
    char dir[SIGMUND_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sudoers_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)fsync_dir_path(dir);
    }
    return 0;
}

static int cmd_grant_revoke_action(const struct invocation *inv,
                                   const struct store_paths *system_store,
                                   const char *program,
                                   bool grant,
                                   int argc,
                                   char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n",
                grant ? "grant" : "revoke");
        return 5;
    }
    if (!inv->euid_root) {
        fprintf(stderr, "sigmund: error: %s requires root authority\n", grant ? "grant" : "revoke");
        return 5;
    }
    char abs_sigmund[SIGMUND_PATH_MAX];
    if (validate_sigmund_self_for_sudoers(program, abs_sigmund, sizeof(abs_sigmund)) != 0) {
        return 5;
    }
    char subject[128];
    if (parse_grant_subject(argv[1], subject, sizeof(subject)) != 0) {
        fprintf(stderr, "sigmund: error: invalid sudoers subject '%s'\n", argv[1]);
        return 5;
    }
    bool selected[GRANT_ACTION_COUNT];
    bool all_scope = false;
    if (parse_grant_actions(argc == 3 ? argv[2] : NULL, selected, &all_scope) != 0) {
        fprintf(stderr, "sigmund: error: invalid action list '%s'\n", argc == 3 ? argv[2] : "");
        return 5;
    }
    char hash[PROFILE_HASH_STR_LEN];
    if (resolve_system_alias_hash_for_grant(system_store, argv[0], hash) != 0) {
        fprintf(stderr, "sigmund: error: grant target must be an existing system alias\n");
        return 5;
    }

    const char *target_label = argv[0];
    char subject_label[128];
    if (subject_file_label_for_grant(subject, subject_label, sizeof(subject_label)) != 0) {
        return 3;
    }
    const char *dir = sudoers_dir_path();
    char sudoers_path[SIGMUND_PATH_MAX];
    if (checked_snprintf(sudoers_path, sizeof(sudoers_path), "%s/sigmund_%s_%s", dir, target_label, subject_label) != 0) {
        return 3;
    }

    if (!grant) {
        if (all_scope) {
            if (unlink_sudoers_template_file(sudoers_path) != 0) {
                die_errno("sigmund: failed to remove managed sudoers file");
            }
            sig_note(inv, "sigmund: revoked sudoers entries for %s %s\n", subject, hash);
            return 0;
        }
        char *existing = NULL;
        bool remaining[GRANT_ACTION_COUNT];
        if (read_owned_file_no_symlink(sudoers_path, &existing) != 0) {
            if (errno == ENOENT) {
                sig_note(inv, "sigmund: revoked sudoers entries for %s %s\n", subject, hash);
                return 0;
            }
            die_errno("sigmund: failed to read managed sudoers file");
        }
        actions_from_existing_sudoers(existing, subject, abs_sigmund, target_label, hash, remaining);
        free(existing);
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            if (selected[i]) {
                remaining[i] = false;
            }
        }
        if (!any_grant_action_selected(remaining)) {
            if (unlink_sudoers_template_file(sudoers_path) != 0) {
                die_errno("sigmund: failed to remove managed sudoers file");
            }
        } else if (write_sudoers_template_file(sudoers_path, target_label, subject, abs_sigmund, hash, remaining, false) != 0) {
            die_errno("sigmund: failed to update managed sudoers file");
        }
        sig_note(inv, "sigmund: revoked sudoers entries for %s %s\n", subject, hash);
        return 0;
    }

    if (!all_scope) {
        char *existing = NULL;
        bool merged[GRANT_ACTION_COUNT];
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            merged[i] = selected[i];
        }
        if (read_owned_file_no_symlink(sudoers_path, &existing) == 0) {
            bool existing_actions[GRANT_ACTION_COUNT];
            actions_from_existing_sudoers(existing, subject, abs_sigmund, target_label, hash, existing_actions);
            for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
                merged[i] = merged[i] || existing_actions[i];
            }
            free(existing);
        } else if (errno != ENOENT) {
            die_errno("sigmund: failed to read managed sudoers file");
        }
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            selected[i] = merged[i];
        }
    }

    if (write_sudoers_template_file(sudoers_path, target_label, subject, abs_sigmund, hash, selected, all_scope) != 0) {
        die_errno("sigmund: failed to update managed sudoers file");
    }
    sig_note(inv, "sigmund: granted sudoers entries for %s %s\n", subject, hash);
    return 0;
}

static void usage(void) {
    printf("sigmund %s - more than nohup, less than systemd\n\n"
           "Run a command that outlives your shell, then find it, watch it, and stop it\n"
           "safely later. No daemon, no config.\n\n"
           "USAGE\n"
           "  sigmund <command> [args...]      start a command in the background\n"
           "  sigmund <action>  [target...]    act on a tracked command\n\n"
           "START\n"
           "  sigmund <command>...             start it; prints a short run id\n"
           "  sigmund -f <command>...          start it and stream output\n"
           "  sigmund --console <command>...   start it with an attachable console\n"
           "  sigmund start <alias>            start a pinned alias\n\n"
           "MANAGE\n"
           "  sigmund list   [alias]          show tracked runs (optionally one alias)\n"
           "  sigmund tail   <target>         follow a run's live output\n"
           "  sigmund console <target>        attach to a run's console\n"
           "  sigmund dump   <target>         print a run's log and exit\n"
           "  sigmund stop   <target>         graceful stop (TERM, then KILL)\n"
           "  sigmund kill   <target>         force kill now (KILL)\n"
           "  sigmund prune  [target|all]     clear past run data\n\n"
           "  target = run id, id prefix, or alias name\n\n"
           "MORE\n"
           "  sigmund help profiles           pin a command as a reusable alias\n"
           "  sigmund help access             give another user scoped access\n"
           "  sigmund help targets            id, alias, and scope resolution\n"
           "  sigmund help system             root-managed runs and elevation\n"
           "  sigmund help scripting          exit codes, --print, --quiet, stdout\n"
           "  sigmund help console            attachable PTY consoles\n"
           "  sigmund <action> -h             help for one action\n\n"
           "  sigmund --version\n",
           SIGMUND_VERSION);
}

static int help_profiles(void) {
    printf("sigmund help profiles\n\n"
           "Pin a run's exact command (resolved binary path + argv) under a reusable\n"
           "name.\n\n"
           "  sigmund alias <id> <name>       pin the command behind <id> as <name>\n"
           "  sigmund aliases [-v]            list visible aliases\n"
           "  sigmund start <name>            start a fresh run under that name\n\n"
           "The name is also recorded on runs started as <name>, so later\n"
           "list, tail, console, dump, stop, kill, and prune commands can use <name>. If the command behind\n"
           "<name> is updated later, future starts use the updated command; prior runs\n"
           "remain under the same recorded alias label.\n");
    return 0;
}

static int help_targets(void) {
    printf("sigmund help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = run id, leading id prefix, or alias name\n\n"
           "A run id addresses one run directly, always. An alias resolves among runs\n"
           "recorded under that name, narrowed by the verb: stop/kill/tail look at\n"
           "running runs, console looks at running console-enabled runs, dump looks at\n"
           "logged runs, and prune looks at removable past\n"
           "run data. One match acts. Several matches exit 6 and print candidates;\n"
           "--all resolves that ambiguity for stop, kill, and prune. A known alias with\n"
           "nothing to do exits 0.\n");
    return 0;
}

static int help_access(void) {
    printf("sigmund help access\n\n"
           "Grant another user permission to act on a specific root-managed alias as\n"
           "root, without a password, scoped to one immutable protected profile.\n\n"
           "  sigmund grant  <alias> <user> [actions]\n"
           "  sigmund revoke <alias> <user> [actions]\n\n"
           "actions = any of: start,stop,kill,tail,dump,prune,console   (default: all)\n\n"
           "The <user> field may be a username, %%group, or all. Sigmund stores one\n"
           "managed sudoers file per alias/user pair. The file contains the current\n"
           "protected profile hash for that alias, an anchored action alternation, and\n"
           "an 8-hex run selector slot. If root updates the alias profile and the hash\n"
           "changes, grant rewrites the same managed file via temp file, visudo check,\n"
           "and atomic rename.\n");
    return 0;
}

static int help_system(void) {
    printf("sigmund help system\n\n"
           "Root, sudo, and --system runs use the root-managed store:\n\n"
           "  Linux: /var/lib/sigmund\n"
           "  macOS: /var/db/sigmund\n\n"
           "Private root records, logs, and profiles stay root-only. Normal users see\n"
           "only the redacted public index and public alias dictionary. A normal action\n"
           "on a root-only public target self-elevates through sudo; user-local targets\n"
           "win over root-public collisions.\n\n"
           "  sigmund --system <cmd...>       start in root-managed state\n"
           "  sigmund --system list           list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("sigmund help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(sigmund <cmd...>)          capture the bare 8-hex run id\n"
           "  sigmund stop --print <id>       print kill -TERM -- -<pgid>\n"
           "  sigmund kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known alias with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_console(void) {
    printf("sigmund help console\n\n"
           "Start a run with an attachable PTY console, then reconnect to it later.\n"
           "Console output is still tee'd to the normal log, so tail and dump continue\n"
           "to work.\n\n"
           "  sigmund --console <cmd...>      start with an attachable console\n"
           "  sigmund start <alias> --console start an alias with a console\n"
           "  sigmund console <target>        attach to that console\n\n"
           "Console attach uses socat. If socat is not available, --console refuses\n"
           "before launching the command. Ctrl-] detaches from socat's raw terminal\n"
           "session without asking Sigmund to stop the run.\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: sigmund list [alias] [--iso|-l]\n\nShow all visible runs, optionally filtered by recorded alias label.\n");
    } else if (!strcmp(action, "start")) {
        printf("usage: sigmund start <alias> [--multi [N]] [--console]\n       sigmund start <cmd> [args...]\n\nStart an alias recipe, or use explicit start form for a raw command.\n");
    } else if (!strcmp(action, "stop")) {
        printf("usage: sigmund stop [--print] [--all] <target>...\n\nGracefully stop matching runs with TERM, then KILL if needed.\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: sigmund kill [--print] [--all] <target>...\n\nForce matching runs down with KILL.\n");
    } else if (!strcmp(action, "tail")) {
        printf("usage: sigmund tail <target>\n\nFollow live output for an alias match, or follow an id's log directly.\n");
    } else if (!strcmp(action, "console")) {
        printf("usage: sigmund console <target>\n\nAttach to a running console-enabled run.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: sigmund dump <target>\n\nPrint a run log and exit.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: sigmund prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "alias")) {
        printf("usage: sigmund alias <id> <name> [-v]\n\nPin the command behind a run id as a reusable alias.\n");
    } else if (!strcmp(action, "aliases")) {
        printf("usage: sigmund aliases [-v]\n\nList visible aliases. User aliases show commands; system commands are redacted.\n");
    } else if (!strcmp(action, "grant") || !strcmp(action, "revoke")) {
        printf("usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n\nManage Sigmund-owned sudoers access for a root-managed alias.\n", action);
    } else {
        return -1;
    }
    return 0;
}

static int show_help(const char *topic) {
    if (!topic || !*topic) {
        usage();
        return 0;
    }
    if (!strcmp(topic, "profiles")) return help_profiles();
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "access")) return help_access();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "console")) return help_console();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "sigmund: unknown help topic '%s'\n", topic);
    return 5;
}

static bool is_sigmund_owned_command(const char *s) {
    return s && (!strcmp(s, "list") || !strcmp(s, "stop") || !strcmp(s, "kill") ||
                 !strcmp(s, "tail") || !strcmp(s, "dump") || !strcmp(s, "prune") ||
                 !strcmp(s, "console") ||
                 !strcmp(s, "start") ||
                 !strcmp(s, "alias") || !strcmp(s, "aliases") ||
                 !strcmp(s, "grant") || !strcmp(s, "revoke") ||
                 !strcmp(s, "help"));
}

static bool parse_positive_count(const char *s, int *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < 1 || v > 1000) {
        return false;
    }
    *out = (int)v;
    return true;
}

static int perform_explicit_start(const struct invocation *inv,
                                  const struct store_paths *store,
                                  bool tail,
                                  bool console_mode,
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
        return perform_start(inv, store, tail, console_mode, 3, shell_argv, NULL, NULL);
    }
    return perform_start(inv, store, tail, console_mode, argc, argv, NULL, NULL);
}

static int cmd_elevated_capability_action(const struct invocation *inv,
                                          const struct store_paths *system_store,
                                          const char *command,
                                          bool tail,
                                          bool console_mode,
                                          int sig,
                                          bool graceful,
                                          int argc,
                                          char **argv) {
    if (!inv->euid_root || argc != 3) {
        return -1;
    }
    const char *runid_sel = argv[0];
    const char *alias = argv[1];
    const char *hash = argv[2];
    if (!valid_runid_selector(runid_sel) || !valid_alias(alias) || !valid_profile_hash(hash)) {
        return -1;
    }
    if (verify_system_alias_cap(system_store, alias, hash) != 0) {
        fprintf(stderr, "sigmund: error: capability for '%s' is no longer valid\n", alias);
        return 3;
    }

    if (!strcmp(command, "start")) {
        if (strcmp(runid_sel, "00000000") != 0) {
            fprintf(stderr, "sigmund: error: start capability requires selector 00000000\n");
            return 5;
        }
        return perform_profile_start(inv, system_store, tail, console_mode, hash, alias);
    }

    if (strcmp(runid_sel, "00000000") == 0) {
        fprintf(stderr, "sigmund: error: selector 00000000 is only valid for start\n");
        return 5;
    }

    if (strcmp(runid_sel, "ffffffff") == 0) {
        if (!command_all_allowed(command)) {
            fprintf(stderr, "sigmund: error: selector ffffffff is not valid for %s\n", command);
            return 5;
        }
        struct alias_match_list matches;
        if (collect_private_alias_matches(system_store, alias, command, &matches) != 0) {
            return 3;
        }
        int worst = 0;
        int acted = 0;
        char boot[128] = {0};
        bool have_boot = current_boot_id(boot, sizeof(boot));
        for (size_t i = 0; i < matches.count; i++) {
            int rc = 0;
            if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
                bool already_done = false;
                rc = do_signal_action(system_store,
                                      matches.items[i].id,
                                      sig,
                                      graceful,
                                      &already_done);
                if (rc == 0) {
                    sig_note(inv,
                             "sigmund: %s %s\n",
                             already_done ? matches.items[i].id : (!strcmp(command, "kill") ? "killed" : "stopped"),
                             already_done ? "already exited" : matches.items[i].id);
                }
            } else if (!strcmp(command, "prune")) {
                bool removed = false;
                rc = prune_one_run(system_store, matches.items[i].id, have_boot ? boot : NULL, true, &removed);
                if (removed) {
                    acted++;
                }
            }
            if (rc == 0 && strcmp(command, "prune") != 0) {
                acted++;
            }
            if (rc > worst) {
                worst = rc;
            }
        }
        if (!strcmp(command, "prune") && worst == 0) {
            if (acted > 0) {
                sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n", acted, acted == 1 ? "" : "s", alias);
            } else {
                sig_note(inv, "sigmund: nothing to prune\n");
            }
        } else if (acted == 0 && worst == 0) {
            sig_note(inv, "sigmund: nothing to %s\n", command);
        }
        free_alias_match_list(&matches);
        return worst;
    }

    if (ensure_run_recorded_under_alias(system_store, runid_sel, alias) != 0) {
        fprintf(stderr, "sigmund: error: run %s is not recorded under alias '%s'\n", runid_sel, alias);
        return 3;
    }

    struct record selected_record;
    char selected_path[SIGMUND_PATH_MAX];
    if (load_record_by_id(system_store->record_dir, runid_sel, &selected_record, selected_path, sizeof(selected_path)) != 0) {
        return 5;
    }
    char selected_boot[128] = {0};
    bool have_selected_boot = current_boot_id(selected_boot, sizeof(selected_boot));
    enum run_state selected_state = eval_state(&selected_record, have_selected_boot ? selected_boot : NULL);
    if (!strcmp(command, "console")) {
        return attach_console_record(inv, &selected_record, selected_state);
    }
    if (!record_matches_alias_intent(command, &selected_record, selected_state)) {
        sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }

    if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
        bool already_done = false;
        int rc = do_signal_action(system_store, runid_sel, sig, graceful, &already_done);
        if (rc == 0) {
            if (already_done) {
                sig_note(inv, "sigmund: %s already exited\n", runid_sel);
            } else {
                sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", runid_sel);
            }
        }
        return rc;
    }
    if (!strcmp(command, "prune")) {
        char boot[128] = {0};
        bool have_boot = current_boot_id(boot, sizeof(boot));
        bool removed = false;
        int rc = prune_one_run(system_store, runid_sel, have_boot ? boot : NULL, true, &removed);
        if (rc == 0) {
            sig_note(inv, removed ? "sigmund: pruned 1 past run for '%s'\n" : "sigmund: nothing to prune\n", alias);
        }
        return rc;
    }
    if (!strcmp(command, "tail") || !strcmp(command, "dump")) {
        struct record r;
        char path[SIGMUND_PATH_MAX];
        if (load_record_by_id(system_store->record_dir, runid_sel, &r, path, sizeof(path)) != 0 || !r.has_log) {
            return 5;
        }
        if (!strcmp(command, "tail")) {
            char boot[128] = {0};
            bool have_boot = current_boot_id(boot, sizeof(boot));
            enum run_state st = eval_state(&r, have_boot ? boot : NULL);
            return tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
        }
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            die_errno("sigmund: failed to open log for dump");
        }
        char buf[4096];
        while (1) {
            ssize_t nr = read(fd, buf, sizeof(buf));
            if (nr == 0) {
                break;
            }
            if (nr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                die_errno("sigmund: failed while dumping log");
            }
            if (write_all(STDOUT_FILENO, buf, (size_t)nr) != 0) {
                close(fd);
                die_errno("sigmund: failed writing dumped output");
            }
        }
        close(fd);
        return 0;
    }

    return -1;
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
    bool console_mode = false;
    bool force_raw = false;
    bool all = false;
    bool multi = false;
    bool quiet = false;
    bool print_cmd = false;
    bool list_iso = false;
    int multi_count = 1;

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
        if (!strcmp(argv[argi], "--tail") || !strcmp(argv[argi], "-f")) {
            tail = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--console")) {
            console_mode = true;
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--quiet")) {
            quiet = true;
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
        bool literal_owned_arg = false;
        for (int i = argi; i < argc; i++) {
            if (!literal_owned_arg && !strcmp(argv[i], "--")) {
                literal_owned_arg = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--system")) {
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--elevated")) {
                elevated = true;
                requested_system = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(argv[i], "--quiet")) {
                quiet = true;
                continue;
            }
            if (!literal_owned_arg && command_accepts_target_tokens(command) && !strcmp(argv[i], "--all")) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "stop") || !strcmp(command, "kill")) &&
                !strcmp(argv[i], "--print")) {
                print_cmd = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") &&
                (!strcmp(argv[i], "--tail") || !strcmp(argv[i], "-f"))) {
                tail = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--console")) {
                console_mode = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "list") &&
                (!strcmp(argv[i], "--iso") || !strcmp(argv[i], "-l"))) {
                list_iso = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--multi")) {
                multi = true;
                multi_count = 1;
                if (i + 1 < argc) {
                    int parsed = 0;
                    if (parse_positive_count(argv[i + 1], &parsed)) {
                        multi_count = parsed;
                        i++;
                    }
                }
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && strncmp(argv[i], "--multi=", 8) == 0) {
                multi = true;
                if (!parse_positive_count(argv[i] + 8, &multi_count)) {
                    fprintf(stderr, "sigmund: error: invalid --multi count '%s'\n", argv[i] + 8);
                    free(cmd_argv);
                    return 5;
                }
                continue;
            }
            cmd_argv[cmd_argc++] = argv[i];
        }
    } else {
        command = NULL;
        cmd_argc = argc - argi;
        cmd_argv = argv + argi;
    }

    if (!owned && !force_raw && !tail && !strcmp(argv[argi], "--version")) {
        puts(SIGMUND_VERSION);
        return 0;
    }
    if (!owned && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund help [topic]\n");
            rc = 5;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = show_help(NULL);
        } else {
            rc = show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (console_mode && owned && strcmp(command, "start") != 0) {
        fprintf(stderr, "sigmund: error: --console applies only to starts\n");
        free(cmd_argv);
        return 5;
    }

    struct invocation inv;
    if (detect_invocation(&inv, requested_system, elevated) != 0) {
        die_errno("sigmund: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "sigmund: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && owned && !strcmp(command, "start") && cmd_argc == 1) {
        struct store_paths pre_system_store;
        if (init_system_store(&pre_system_store) == 0) {
            const char *atom = NULL;
            enum id_token_scope start_scope = parse_id_token(cmd_argv[0], &atom);
            if ((start_scope == ID_TOKEN_PLAIN || start_scope == ID_TOKEN_SYSTEM) && atom &&
                (valid_profile_hash(atom) || valid_alias(atom))) {
                char hash[PROFILE_HASH_STR_LEN];
                if (resolve_public_profile_token(&pre_system_store, atom, hash) == 1) {
                    int rc = 0;
                    int starts = multi ? multi_count : 1;
                    for (int i = 0; i < starts; i++) {
                        rc = elevate_start_token(argv[0],
                                                 tail,
                                                 console_mode,
                                                 valid_alias(atom) ? atom : hash,
                                                 valid_alias(atom) ? hash : NULL,
                                                 false,
                                                 1);
                        if (rc != 0) {
                            break;
                        }
                    }
                    free(cmd_argv);
                    return rc;
                }
            }
        }
    }
    if (requested_system && !inv.euid_root && !is_list) {
        int canonical_rc = 0;
        if (owned && maybe_elevate_requested_system_targets(argv[0], command, cmd_argc, cmd_argv, all, &canonical_rc)) {
            free(cmd_argv);
            return canonical_rc;
        }
        int rc = elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
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
                                               !strcmp(command, "prune") || !strcmp(command, "console")))) {
        if (!inv.euid_root) {
            if (ensure_user_store_for_current_user(&user_store) != 0) {
                die_errno("sigmund: failed to init user storage");
            }
        }
    }

    if (inv.elevated && inv.euid_root && owned && cmd_argc == 3 &&
        (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
         !strcmp(command, "tail") || !strcmp(command, "dump") || !strcmp(command, "prune") ||
         !strcmp(command, "console"))) {
        int sig = !strcmp(command, "kill") ? SIGKILL : SIGTERM;
        bool graceful = !strcmp(command, "stop");
        int rc = cmd_elevated_capability_action(&inv, &system_store, command, tail, console_mode, sig, graceful, cmd_argc, cmd_argv);
        if (rc >= 0) {
            free(cmd_argv);
            return rc;
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
        return perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
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
        int rc = cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, multi, multi_count, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund list [alias]\n");
            free(cmd_argv);
            return 5;
        }
        const char *alias_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (alias_filter && !valid_alias(alias_filter)) {
            fprintf(stderr, "sigmund: error: invalid alias '%s'\n", alias_filter);
            free(cmd_argv);
            return 5;
        }
        if (inv.euid_root) {
            rc = cmd_list_system(&system_store, alias_filter, list_iso);
        } else {
            rc = cmd_list_normal(&user_store, &system_store, alias_filter, list_iso);
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "tail")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund tail <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_tail_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund dump <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_dump_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "console")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund console <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_console_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        int rc = cmd_prune_action(&inv, &user_store, &system_store, argv[0], target, all);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "alias")) {
        int rc = cmd_alias_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "aliases")) {
        bool aliases_verbose = false;
        if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "-v") || !strcmp(cmd_argv[0], "--verbose"))) {
            aliases_verbose = true;
        } else if (cmd_argc != 0) {
            fprintf(stderr, "usage: sigmund aliases [-v]\n");
            free(cmd_argv);
            return 5;
        }
        int rc = cmd_aliases_action(&inv, &user_store, &system_store, aliases_verbose);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "grant")) {
        if (ensure_system_store(&system_store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
        int rc = cmd_grant_revoke_action(&inv, &system_store, argv[0], true, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "revoke")) {
        if (ensure_system_store(&system_store) != 0) {
            die_errno("sigmund: failed to init system storage");
        }
        int rc = cmd_grant_revoke_action(&inv, &system_store, argv[0], false, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", cmd_argc, cmd_argv, SIGTERM, true, all, print_cmd);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = cmd_signal_action(&inv, &user_store, &system_store, argv[0], "kill", cmd_argc, cmd_argv, SIGKILL, false, all, print_cmd);
        free(cmd_argv);
        return rc;
    }

    free(cmd_argv);
    usage();
    return 1;
}
