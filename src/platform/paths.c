#include "hold/config.h"
#include "hold/types.h"
#include "hold/platform.h"
#include "hold/core.h"

static int realpath_copy(const char *path, char *out, size_t n) {
    char *resolved = realpath(path, NULL);
    if (!resolved) return -1;
    int rc = hold_checked_snprintf(out, n, "%s", resolved);
    free(resolved);
    return rc;
}

/* Copy a passwd field iff present; truncation is fatal (ENAMETOOLONG). */
static int copy_field(char *dst, size_t n, const char *src) {
    return (src && *src) ? hold_checked_snprintf(dst, n, "%s", src) : 0;
}

int hold_lookup_passwd_by_uid(uid_t uid, struct hold_passwd_entry *out) {
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));

    /* getpwuid consults the platform's account database (OpenDirectory on
     * macOS, NSS on glibc); real macOS users never appear in /etc/passwd.
     * The file parse below stays as the fallback for static Linux builds,
     * where glibc's NSS lookup can be unavailable. */
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && *pw->pw_name) {
        if (copy_field(out->name, sizeof(out->name), pw->pw_name) != 0 ||
            copy_field(out->home, sizeof(out->home), pw->pw_dir) != 0 ||
            copy_field(out->shell, sizeof(out->shell), pw->pw_shell) != 0) return -1;
        return 0;
    }

    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        /* name:passwd:uid:gid:gecos:home:shell — require all seven fields. */
        char *fields[7] = {0}, *p = line;
        for (int i = 0; i < 7 && p; i++) {
            fields[i] = p;
            if ((p = strchr(p, ':'))) *p++ = '\0';
        }
        if (!fields[6] || !*fields[0] || !*fields[2]) continue;
        errno = 0;
        char *end = NULL;
        uintmax_t v = strtoumax(fields[2], &end, 10);
        if (errno != 0 || !end || *end != '\0' || (uintmax_t)(uid_t)v != v || (uid_t)v != uid) continue;
        int rc = copy_field(out->name, sizeof(out->name), fields[0]) |
                 copy_field(out->home, sizeof(out->home), fields[5]) |
                 copy_field(out->shell, sizeof(out->shell), fields[6]);
        fclose(fp);
        if (rc != 0) errno = ENAMETOOLONG;
        return rc != 0 ? -1 : 0;
    }
    fclose(fp);
    errno = ENOENT;
    return -1;
}

int hold_resolve_binary_path(const char *argv0, char *out, size_t n) {
    if (!argv0 || !*argv0) { errno = EINVAL; return -1; }
    if (strchr(argv0, '/')) return realpath_copy(argv0, out, n);
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path;; p = strchr(p, ':') + 1) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len >= HOLD_PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        char candidate[HOLD_PATH_MAX];
        if (len > 0 &&
            hold_checked_snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)len, p, argv0) == 0 &&
            access(candidate, X_OK) == 0 && realpath_copy(candidate, out, n) == 0) return 0;
        if (!colon) break;
    }
    errno = ENOENT;
    return -1;
}

int hold_resolve_existing_path_from_cwd(const char *token, const char *cwd, char *out, size_t n) {
    if (!token || !*token || !out || n == 0) { errno = EINVAL; return -1; }
    char candidate[HOLD_PATH_MAX];
    if (token[0] != '/' && cwd && *cwd) {
        if (hold_checked_snprintf(candidate, sizeof(candidate), "%s/%s", cwd, token) != 0) return -1;
        token = candidate;
    }
    return realpath_copy(token, out, n);
}

int hold_normalize_existing_argv_paths_from_cwd(char **argv, int argc, int first_arg, const char *cwd) {
    if (argc < 0 || (argc > 0 && !argv) || first_arg < 0) { errno = EINVAL; return -1; }
    for (int i = first_arg; i < argc; i++) {
        /* Rewrite bare tokens, or the path part of --flag=path; only tokens
         * resolving to an EXISTING path change — all else stays byte-identical. */
        const char *arg = argv[i], *path_part = arg;
        size_t prefix_len = 0;
        if (!arg || !*arg) continue;
        if (arg[0] == '-') {
            const char *eq = strchr(arg, '=');
            if (!eq || !eq[1]) continue;
            prefix_len = (size_t)(eq + 1 - arg);
            path_part = eq + 1;
        }
        char resolved[HOLD_PATH_MAX];
        if (hold_resolve_existing_path_from_cwd(path_part, cwd, resolved, sizeof(resolved)) != 0 ||
            strcmp(path_part, resolved) == 0) continue;
        size_t resolved_len = strlen(resolved);
        char *copy = malloc(prefix_len + resolved_len + 1);
        if (!copy) return -1;
        memcpy(copy, arg, prefix_len);
        memcpy(copy + prefix_len, resolved, resolved_len + 1);
        free(argv[i]);
        argv[i] = copy;
    }
    return 0;
}

bool hold_path_is_within_dir(const char *path, const char *dir) {
    char resolved_dir[HOLD_PATH_MAX];
    if (!path || !*path || !dir || !*dir || !realpath(dir, resolved_dir)) return false;
    size_t len = strlen(resolved_dir);
    while (len > 1 && resolved_dir[len - 1] == '/') resolved_dir[--len] = '\0';
    return strcmp(path, resolved_dir) == 0 ||
           (strncmp(path, resolved_dir, len) == 0 && path[len] == '/');
}

int hold_resolve_self_executable_path(const char *argv0, char *out, size_t n) {
    if (!out || n == 0) { errno = EINVAL; return -1; }
    out[0] = '\0';
    char self[HOLD_PATH_MAX], resolved[HOLD_PATH_MAX];
#if defined(__linux__)
    ssize_t got = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (got > 0) {
        self[got] = '\0';
        return hold_checked_snprintf(out, n, "%s", self);
    }
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(self);
    if (_NSGetExecutablePath(self, &size) == 0 && realpath(self, resolved))
        return hold_checked_snprintf(out, n, "%s", resolved);
#endif
    if (argv0 && strchr(argv0, '/') && realpath(argv0, resolved))
        return hold_checked_snprintf(out, n, "%s", resolved);
    errno = ENOENT;
    return -1;
}
