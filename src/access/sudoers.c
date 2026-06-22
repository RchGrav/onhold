#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/access.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"

static const char *const grant_action_names[] = {"start", "stop", "kill", "tail", "dump", "prune", "console"};
#define GRANT_ACTION_COUNT ((int)(sizeof(grant_action_names) / sizeof(grant_action_names[0])))

static int parse_grant_subject(const char *input, char *out, size_t n);
static int grant_action_index(const char *name);
static int parse_grant_actions(const char *input, bool selected[GRANT_ACTION_COUNT], bool *all_scope);
static int validate_sigmund_self_for_sudoers(const char *program, char *abs_sigmund, size_t n);
static const char *sudoers_dir_path(void);
static int resolve_system_alias_hash_for_grant(const struct store_paths *system_store,
                                               const char *alias,
                                               char hash[PROFILE_HASH_STR_LEN]);
static int build_action_alternation(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n);
static int build_actions_csv(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n);
static int build_sudoers_line(char *out,
                              size_t n,
                              const char *subject,
                              const char *abs_sigmund,
                              const bool selected[GRANT_ACTION_COUNT],
                              const char *alias,
                              const char *hash);
static bool any_grant_action_selected(const bool selected[GRANT_ACTION_COUNT]);
static int find_visudo(char *out, size_t n);
static int validate_sudoers_candidate(const char *path);
static int subject_file_label_for_grant(const char *subject, char *out, size_t n);
static void actions_from_existing_sudoers(const char *existing,
                                          const char *subject,
                                          const char *abs_sigmund,
                                          const char *alias,
                                          const char *hash,
                                          bool selected[GRANT_ACTION_COUNT]);
static int write_sudoers_template_file(const char *sudoers_path,
                                       const char *target_label,
                                       const char *subject,
                                       const char *abs_sigmund,
                                       const char *hash,
                                       const bool selected[GRANT_ACTION_COUNT],
                                       bool all_scope);
static int unlink_sudoers_template_file(const char *sudoers_path);

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

int cmd_grant_revoke_action(const struct invocation *inv,
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
