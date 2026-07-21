#include "hold/config.h"
#include "hold/types.h"
#include "hold/platform.h"
#include "hold/core.h"

static int realpath_copy(const char *path, char *out, size_t n);

static int realpath_copy(const char *path, char *out, size_t n) {
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        return -1;
    }
    int rc = hold_checked_snprintf(out, n, "%s", resolved);
    free(resolved);
    return rc;
}

static bool parse_passwd_uid_field(const char *s, uid_t *out) {
    if (!s || !*s || !out) return false;
    errno = 0;
    char *end = NULL;
    uintmax_t v = strtoumax(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    uid_t narrowed = (uid_t)v;
    if ((uintmax_t)narrowed != v) return false;
    *out = narrowed;
    return true;
}

int hold_lookup_passwd_by_uid(uid_t uid, struct hold_passwd_entry *out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    /* getpwuid consults the platform's account database (OpenDirectory on
     * macOS, NSS on glibc); real macOS users never appear in /etc/passwd.
     * The file parse below stays as the fallback for static Linux builds,
     * where glibc's NSS lookup can be unavailable. */
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && *pw->pw_name) {
        if (hold_checked_snprintf(out->name, sizeof(out->name), "%s", pw->pw_name) != 0) return -1;
        if (pw->pw_dir && *pw->pw_dir &&
            hold_checked_snprintf(out->home, sizeof(out->home), "%s", pw->pw_dir) != 0) return -1;
        if (pw->pw_shell && *pw->pw_shell &&
            hold_checked_snprintf(out->shell, sizeof(out->shell), "%s", pw->pw_shell) != 0) return -1;
        return 0;
    }

    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;

    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        char *fields[7] = {0};
        char *p = line;
        for (int i = 0; i < 7; i++) {
            fields[i] = p;
            char *colon = strchr(p, ':');
            if (!colon) {
                if (i != 6) fields[0] = NULL;
                break;
            }
            *colon = '\0';
            p = colon + 1;
        }
        if (!fields[0] || !fields[2] || !*fields[0]) continue;

        uid_t parsed_uid = 0;
        if (!parse_passwd_uid_field(fields[2], &parsed_uid) || parsed_uid != uid) continue;

        int rc = 0;
        if (fields[0] && *fields[0]) {
            rc |= hold_checked_snprintf(out->name, sizeof(out->name), "%s", fields[0]);
        }
        if (fields[5] && *fields[5]) {
            rc |= hold_checked_snprintf(out->home, sizeof(out->home), "%s", fields[5]);
        }
        if (fields[6] && *fields[6]) {
            rc |= hold_checked_snprintf(out->shell, sizeof(out->shell), "%s", fields[6]);
        }
        fclose(fp);
        if (rc != 0) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    fclose(fp);
    errno = ENOENT;
    return -1;
}

int hold_resolve_binary_path(const char *argv0, char *out, size_t n) {
    if (!argv0 || !*argv0) {
        errno = EINVAL;
        return -1;
    }
    if (strchr(argv0, '/')) {
        return realpath_copy(argv0, out, n);
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len == 0) {
            if (!colon) {
                break;
            }
            p = colon + 1;
            continue;
        }
        char dir[HOLD_PATH_MAX];
        if (len >= sizeof(dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(dir, p, len);
        dir[len] = '\0';
        char candidate[HOLD_PATH_MAX], resolved[HOLD_PATH_MAX];
        if (hold_checked_snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0) == 0 &&
            access(candidate, X_OK) == 0 && realpath_copy(candidate, resolved, sizeof(resolved)) == 0) {
            return hold_checked_snprintf(out, n, "%s", resolved);
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

int hold_resolve_existing_path_from_cwd(const char *token, const char *cwd, char *out, size_t n) {
    if (!token || !*token || !out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    if (token[0] == '/') {
        return realpath_copy(token, out, n);
    }
    if (cwd && *cwd) {
        char candidate[HOLD_PATH_MAX];
        if (hold_checked_snprintf(candidate, sizeof(candidate), "%s/%s", cwd, token) != 0) {
            return -1;
        }
        return realpath_copy(candidate, out, n);
    }
    return realpath_copy(token, out, n);
}

int hold_normalize_existing_argv_paths_from_cwd(char **argv, int argc, int first_arg, const char *cwd) {
    if (argc < 0 || (argc > 0 && !argv) || first_arg < 0) {
        errno = EINVAL;
        return -1;
    }
    for (int i = first_arg; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg || !*arg) {
            continue;
        }
        const char *path_part = arg;
        size_t prefix_len = 0;
        if (arg[0] == '-') {
            const char *eq = strchr(arg, '=');
            if (!eq || !eq[1]) {
                continue;
            }
            prefix_len = (size_t)(eq + 1 - arg);
            path_part = eq + 1;
        }
        char resolved[HOLD_PATH_MAX];
        if (hold_resolve_existing_path_from_cwd(path_part, cwd, resolved, sizeof(resolved)) != 0) {
            continue;
        }
        if (prefix_len == 0) {
            if (!strcmp(arg, resolved)) {
                continue;
            }
            char *copy = strdup(resolved);
            if (!copy) {
                return -1;
            }
            free(argv[i]);
            argv[i] = copy;
        } else {
            if (!strcmp(path_part, resolved)) {
                continue;
            }
            size_t resolved_len = strlen(resolved);
            if (prefix_len >= SIZE_MAX - resolved_len) {
                errno = ENAMETOOLONG;
                return -1;
            }
            char *copy = malloc(prefix_len + resolved_len + 1);
            if (!copy) {
                return -1;
            }
            memcpy(copy, arg, prefix_len);
            memcpy(copy + prefix_len, resolved, resolved_len + 1);
            free(argv[i]);
            argv[i] = copy;
        }
    }
    return 0;
}

bool hold_path_is_within_dir(const char *path, const char *dir) {
    if (!path || !*path || !dir || !*dir) {
        return false;
    }
    char resolved_dir[HOLD_PATH_MAX];
    if (!realpath(dir, resolved_dir)) {
        return false;
    }
    size_t len = strlen(resolved_dir);
    while (len > 1 && resolved_dir[len - 1] == '/') {
        resolved_dir[--len] = '\0';
    }
    return strcmp(path, resolved_dir) == 0 ||
           (strncmp(path, resolved_dir, len) == 0 && path[len] == '/');
}

int hold_resolve_self_executable_path(const char *argv0, char *out, size_t n) {
    if (!out || n == 0) {
        errno = EINVAL;
        return -1;
    }
    out[0] = '\0';
#if defined(__linux__)
    char proc_path[HOLD_PATH_MAX];
    ssize_t got = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
    if (got > 0) {
        proc_path[got] = '\0';
        return hold_checked_snprintf(out, n, "%s", proc_path);
    }
#endif
#if defined(__APPLE__)
    char mac_path[HOLD_PATH_MAX];
    uint32_t mac_size = (uint32_t)sizeof(mac_path);
    if (_NSGetExecutablePath(mac_path, &mac_size) == 0) {
        char resolved[HOLD_PATH_MAX];
        if (realpath(mac_path, resolved)) {
            return hold_checked_snprintf(out, n, "%s", resolved);
        }
    }
#endif
    if (argv0 && strchr(argv0, '/')) {
        char resolved[HOLD_PATH_MAX];
        if (realpath(argv0, resolved)) {
            return hold_checked_snprintf(out, n, "%s", resolved);
        }
    }
    errno = ENOENT;
    return -1;
}
