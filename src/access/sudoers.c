#include "hold/config.h"
#include "hold/types.h"
#include "hold/access.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"

static const char *const grant_action_names[] = {"start", "stop", "kill", "tail", "dump", "prune", "console"};
#define GRANT_ACTION_COUNT ((int)(sizeof(grant_action_names) / sizeof(grant_action_names[0])))

static int parse_grant_subject(const char *input, char *out, size_t n);
static int grant_action_index(const char *name);
static int parse_grant_actions(const char *input, bool selected[GRANT_ACTION_COUNT], bool *all_scope);
static int validate_hold_self_for_sudoers(const char *program, char *abs_hold, size_t n);
static const char *sudoers_dir_path(void);
static int resolve_system_alias_hash_for_grant(const struct hold_store *system_store,
                                               const char *alias,
                                               char hash[PROFILE_HASH_STR_LEN]);
static int build_actions_csv(const bool selected[GRANT_ACTION_COUNT], char *out, size_t n);
static int build_sudoers_line(char *out,
                              size_t n,
                              const char *subject,
                              const char *abs_hold,
                              const bool selected[GRANT_ACTION_COUNT],
                              const char *alias,
                              const char *hash);
static int write_subject_grant_copy(const struct hold_store *system_store,
                                    const char *subject,
                                    const char *profile,
                                    const char *source_hash,
                                    const bool selected[GRANT_ACTION_COUNT],
                                    char grant_hash[PROFILE_HASH_STR_LEN]);
static int unlink_subject_grant_copy(const struct hold_store *system_store,
                                     const char *subject,
                                     const char *profile);
static int parse_subject_grant_actions(const char *json, bool selected[GRANT_ACTION_COUNT]);
static int subject_grant_canonical_digest_json(const char *json,
                                               char hash[PROFILE_HASH_STR_LEN],
                                               struct hold_profile *profile_out,
                                               bool selected[GRANT_ACTION_COUNT]);
static int subject_grant_canonical_digest_path(const char *path, char hash[PROFILE_HASH_STR_LEN]);
static int write_raw_grant_file(const char *path, const char *contents, mode_t mode);
static int restore_subject_grant_snapshot(const struct hold_store *system_store,
                                          const char *subject,
                                          const char *profile,
                                          const char *private_contents,
                                          const char *public_contents);
static bool any_grant_action_selected(const bool selected[GRANT_ACTION_COUNT]);
static int find_visudo(char *out, size_t n);
static int validate_sudoers_candidate(const char *path);
static int subject_file_label_for_grant(const char *subject, char *out, size_t n);
static void actions_from_existing_sudoers(const char *existing,
                                          const char *subject,
                                          const char *abs_hold,
                                          const char *alias,
                                          const char *hash,
                                          bool selected[GRANT_ACTION_COUNT]);

static int grant_subject_path_parts(const char *subject, const char **kind, const char **name) {
    if (!subject || !*subject || strcmp(subject, "ALL") == 0) {
        errno = EINVAL;
        return -1;
    }
    if (subject[0] == '%') {
        if (!subject[1]) {
            errno = EINVAL;
            return -1;
        }
        *kind = "groups";
        *name = subject + 1;
    } else {
        *kind = "users";
        *name = subject;
    }
    return 0;
}

static int grant_private_path(const struct hold_store *system_store,
                              const char *subject,
                              const char *profile,
                              char *path,
                              size_t n) {
    const char *kind = NULL;
    const char *name = NULL;
    if (!system_store || !hold_valid_alias(profile) || grant_subject_path_parts(subject, &kind, &name) != 0) {
        errno = EINVAL;
        return -1;
    }
    return hold_checked_snprintf(path, n, "%s/grants/%s/%s/%s.json", system_store->base, kind, name, profile);
}

static int grant_public_path(const struct hold_store *system_store,
                             const char *subject,
                             const char *profile,
                             char *path,
                             size_t n) {
    const char *kind = NULL;
    const char *name = NULL;
    if (!system_store || !hold_valid_alias(profile) || grant_subject_path_parts(subject, &kind, &name) != 0) {
        errno = EINVAL;
        return -1;
    }
    return hold_checked_snprintf(path, n, "%s/grants/%s/%s/%s.json", system_store->public_dir, kind, name, profile);
}

static int ensure_parent_dir(const char *path, mode_t mode) {
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s", path) != 0) return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    if (hold_mkdir_p_mode(dir, mode) != 0 || hold_chmod_dir_no_symlink(dir, mode) != 0) return -1;
    if (geteuid() == 0 && hold_chown_dir_no_symlink_if_root(dir, 0, 0) != 0) return -1;
    return 0;
}

static int write_profile_grant_json_file(const char *path,
                                         const char *subject,
                                         const char *profile,
                                         const struct hold_profile *recipe,
                                         const bool selected[GRANT_ACTION_COUNT]) {
    if (ensure_parent_dir(path, 0700) != 0) return -1;
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s", path) != 0) return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) { errno = EINVAL; return -1; }
    *slash = '\0';
    char tmp[HOLD_PATH_MAX];
    int fd = hold_open_unique_temp(dir, ".grant", 0600, tmp, sizeof(tmp));
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    fputs("{\n", f);
    fputs("  \"schema\": \"hold.subject-grant.v1\",\n", f);
    fputs("  \"subject\": \"", f); hold_json_escape(f, subject); fputs("\",\n", f);
    fputs("  \"profile\": \"", f); hold_json_escape(f, profile); fputs("\",\n", f);
    fputs("  \"binary_path\": \"", f); hold_json_escape(f, recipe->binary_path); fputs("\",\n", f);
    fputs("  \"argv\": ", f); hold_write_json_argv(f, recipe->argc, recipe->argv); fputs(",\n", f);
    fputs("  \"actions\": [", f);
    bool first = true;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (!selected[i]) continue;
        if (!first) fputs(", ", f);
        fputc('"', f); hold_json_escape(f, grant_action_names[i]); fputc('"', f);
        first = false;
    }
    fputs("]\n}\n", f);
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { int saved = errno; unlink(tmp); errno = saved; return -1; }
    (void)hold_fsync_dir_path(dir);
    return 0;
}

static int write_public_grant_hash_file(const char *path,
                                        const char *subject,
                                        const char *profile,
                                        const char *hash,
                                        const bool selected[GRANT_ACTION_COUNT]) {
    if (ensure_parent_dir(path, 0755) != 0) return -1;
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s", path) != 0) return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) { errno = EINVAL; return -1; }
    *slash = '\0';
    char tmp[HOLD_PATH_MAX];
    int fd = hold_open_unique_temp(dir, ".grant-public", 0644, tmp, sizeof(tmp));
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    fputs("{\n", f);
    fputs("  \"schema\": \"hold.subject-grant-public.v1\",\n", f);
    fputs("  \"subject\": \"", f); hold_json_escape(f, subject); fputs("\",\n", f);
    fputs("  \"profile\": \"", f); hold_json_escape(f, profile); fputs("\",\n", f);
    fputs("  \"hash\": \"", f); hold_json_escape(f, hash); fputs("\",\n", f);
    fputs("  \"actions\": [", f);
    bool first = true;
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (!selected[i]) continue;
        if (!first) fputs(", ", f);
        fputc('"', f); hold_json_escape(f, grant_action_names[i]); fputc('"', f);
        first = false;
    }
    fputs("]\n}\n", f);
    if (ferror(f) || fflush(f) != 0 || fsync(fd) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { int saved = errno; unlink(tmp); errno = saved; return -1; }
    (void)hold_fsync_dir_path(dir);
    return 0;
}

static int write_subject_grant_copy(const struct hold_store *system_store,
                                    const char *subject,
                                    const char *profile,
                                    const char *source_hash,
                                    const bool selected[GRANT_ACTION_COUNT],
                                    char grant_hash[PROFILE_HASH_STR_LEN]) {
    if (strcmp(subject, "ALL") == 0) {
        fprintf(stderr, "hold: error: ALL grants require subject-specific 0.4 grant material\n");
        errno = EINVAL;
        return -1;
    }
    struct hold_profile recipe;
    if (hold_load_profile_by_hash(system_store, source_hash, &recipe) != 0) return -1;
    char private_path[HOLD_PATH_MAX];
    char public_path[HOLD_PATH_MAX];
    int rc = -1;
    if (grant_private_path(system_store, subject, profile, private_path, sizeof(private_path)) != 0 ||
        grant_public_path(system_store, subject, profile, public_path, sizeof(public_path)) != 0) {
        goto out;
    }
    if (write_profile_grant_json_file(private_path, subject, profile, &recipe, selected) != 0) goto out;
    if (subject_grant_canonical_digest_path(private_path, grant_hash) != 0) goto out;
    if (write_public_grant_hash_file(public_path, subject, profile, grant_hash, selected) != 0) goto out;
    rc = 0;
out:
    hold_free_profile(&recipe);
    return rc;
}

static int subject_grant_has_action(const char *json, const char *required_action) {
    if (!required_action || !*required_action) {
        return 0;
    }
    const char *v = NULL;
    if (hold_json_find_key(json, "actions", &v) != 0 || *v != '[') {
        return -1;
    }
    v = hold_skip_ws(v + 1);
    while (*v && *v != ']') {
        char action[32];
        if (hold_parse_json_string(v, action, sizeof(action), &v) != 0) {
            return -1;
        }
        if (strcmp(action, required_action) == 0) {
            return 0;
        }
        v = hold_skip_ws(v);
        if (*v == ',') {
            v = hold_skip_ws(v + 1);
        } else if (*v != ']') {
            return -1;
        }
    }
    return -1;
}

static int parse_subject_grant_actions(const char *json, bool selected[GRANT_ACTION_COUNT]) {
    const char *v = NULL;
    if (hold_json_find_key(json, "actions", &v) != 0 || *v != '[') {
        return -1;
    }
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        selected[i] = false;
    }
    v = hold_skip_ws(v + 1);
    while (*v && *v != ']') {
        char action[32];
        if (hold_parse_json_string(v, action, sizeof(action), &v) != 0) {
            return -1;
        }
        int idx = grant_action_index(action);
        if (idx < 0) {
            return -1;
        }
        selected[idx] = true;
        v = hold_skip_ws(v);
        if (*v == ',') {
            v = hold_skip_ws(v + 1);
        } else if (*v != ']') {
            return -1;
        }
    }
    if (*v != ']') {
        return -1;
    }
    return any_grant_action_selected(selected) ? 0 : -1;
}

static int subject_grant_canonical_digest_json(const char *json,
                                               char hash[PROFILE_HASH_STR_LEN],
                                               struct hold_profile *profile_out,
                                               bool selected[GRANT_ACTION_COUNT]) {
    char schema[64];
    char subject[128];
    char profile[ALIAS_MAX_LEN + 1];
    char binary_path[HOLD_PATH_MAX];
    char **argv = NULL;
    int argc = 0;
    bool actions[GRANT_ACTION_COUNT];
    struct sha256_ctx ctx;
    unsigned char digest[32];

    if (hold_json_get_str(json, "schema", schema, sizeof(schema)) != 0 ||
        strcmp(schema, "hold.subject-grant.v1") != 0 ||
        hold_json_get_str(json, "subject", subject, sizeof(subject)) != 0 ||
        hold_json_get_str(json, "profile", profile, sizeof(profile)) != 0 ||
        !hold_valid_alias(profile) ||
        hold_json_get_str(json, "binary_path", binary_path, sizeof(binary_path)) != 0 ||
        binary_path[0] != '/' ||
        hold_json_get_argv_alloc(json, &argv, &argc) != 0 ||
        parse_subject_grant_actions(json, actions) != 0) {
        hold_free_argv_alloc(argv, argc);
        errno = EINVAL;
        return -1;
    }

    hold_sha256_init(&ctx);
    hold_sha256_update_nul_field(&ctx, "hold-subject-grant-canonical-v1");
    hold_sha256_update_nul_field(&ctx, "schema");
    hold_sha256_update_nul_field(&ctx, schema);
    hold_sha256_update_nul_field(&ctx, "subject");
    hold_sha256_update_nul_field(&ctx, subject);
    hold_sha256_update_nul_field(&ctx, "profile");
    hold_sha256_update_nul_field(&ctx, profile);
    hold_sha256_update_nul_field(&ctx, "binary_path");
    hold_sha256_update_nul_field(&ctx, binary_path);
    hold_sha256_update_nul_field(&ctx, "argv");
    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d", argc);
    hold_sha256_update_nul_field(&ctx, count_buf);
    for (int i = 0; i < argc; i++) {
        char idx_buf[32];
        snprintf(idx_buf, sizeof(idx_buf), "%d", i);
        hold_sha256_update_nul_field(&ctx, idx_buf);
        hold_sha256_update_nul_field(&ctx, argv[i]);
    }
    hold_sha256_update_nul_field(&ctx, "actions");
    for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
        if (actions[i]) {
            hold_sha256_update_nul_field(&ctx, grant_action_names[i]);
        }
    }
    hold_sha256_final(&ctx, digest);
    hold_hex_encode(digest, sizeof(digest), hash, PROFILE_HASH_STR_LEN);

    if (profile_out) {
        memset(profile_out, 0, sizeof(*profile_out));
        if (hold_checked_snprintf(profile_out->binary_path, sizeof(profile_out->binary_path), "%s", binary_path) != 0) {
            hold_free_argv_alloc(argv, argc);
            return -1;
        }
        profile_out->argv = argv;
        profile_out->argc = argc;
        argv = NULL;
        argc = 0;
    }
    if (selected) {
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            selected[i] = actions[i];
        }
    }
    hold_free_argv_alloc(argv, argc);
    return 0;
}

static int subject_grant_canonical_digest_path(const char *path, char hash[PROFILE_HASH_STR_LEN]) {
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(path, &j) != 0) {
        return -1;
    }
    int rc = subject_grant_canonical_digest_json(j, hash, NULL, NULL);
    free(j);
    return rc;
}

static int write_raw_grant_file(const char *path, const char *contents, mode_t mode) {
    if (!contents) {
        if (unlink(path) != 0 && errno != ENOENT) {
            return -1;
        }
        return 0;
    }
    if (ensure_parent_dir(path, mode == 0644 ? 0755 : 0700) != 0) {
        return -1;
    }
    char dir[HOLD_PATH_MAX];
    if (hold_checked_snprintf(dir, sizeof(dir), "%s", path) != 0) return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) { errno = EINVAL; return -1; }
    *slash = '\0';
    char tmp[HOLD_PATH_MAX];
    int fd = hold_open_unique_temp(dir, ".grant-restore", mode, tmp, sizeof(tmp));
    if (fd < 0) return -1;
    if (fchmod(fd, mode) != 0 || (geteuid() == 0 && fchown(fd, 0, 0) != 0)) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    size_t len = strlen(contents);
    if (hold_write_all(fd, contents, len) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    (void)hold_fsync_dir_path(dir);
    return 0;
}

static int restore_subject_grant_snapshot(const struct hold_store *system_store,
                                          const char *subject,
                                          const char *profile,
                                          const char *private_contents,
                                          const char *public_contents) {
    char private_path[HOLD_PATH_MAX];
    char public_path[HOLD_PATH_MAX];
    if (grant_private_path(system_store, subject, profile, private_path, sizeof(private_path)) != 0 ||
        grant_public_path(system_store, subject, profile, public_path, sizeof(public_path)) != 0) {
        return -1;
    }
    int rc_private = write_raw_grant_file(private_path, private_contents, 0600);
    int rc_public = write_raw_grant_file(public_path, public_contents, 0644);
    return (rc_private == 0 && rc_public == 0) ? 0 : -1;
}

static int unlink_subject_grant_copy(const struct hold_store *system_store,
                                     const char *subject,
                                     const char *profile) {
    char private_path[HOLD_PATH_MAX];
    char public_path[HOLD_PATH_MAX];
    if (grant_private_path(system_store, subject, profile, private_path, sizeof(private_path)) == 0) {
        if (unlink(private_path) != 0 && errno != ENOENT) return -1;
    }
    if (grant_public_path(system_store, subject, profile, public_path, sizeof(public_path)) == 0) {
        if (unlink(public_path) != 0 && errno != ENOENT) return -1;
    }
    return 0;
}

int hold_subject_grant_hash_for(const struct hold_store *system_store,
                                const char *subject,
                                const char *profile,
                                char hash[PROFILE_HASH_STR_LEN]) {
    char path[HOLD_PATH_MAX];
    if (grant_public_path(system_store, subject, profile, path, sizeof(path)) != 0) return -1;
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(path, &j) != 0) return -1;
    int rc = hold_json_get_str(j, "hash", hash, PROFILE_HASH_STR_LEN);
    free(j);
    if (rc != 0 || !hold_valid_profile_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int hold_load_subject_grant_profile(const struct hold_store *system_store,
                                      const char *subject,
                                      const char *profile,
                                      const char *expected_hash,
                                      const char *required_action,
                                      struct hold_profile *profile_out) {
    memset(profile_out, 0, sizeof(*profile_out));
    if (!hold_valid_profile_hash(expected_hash)) { errno = EINVAL; return -1; }
    char path[HOLD_PATH_MAX];
    if (grant_private_path(system_store, subject, profile, path, sizeof(path)) != 0) return -1;
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(path, &j) != 0) return -1;
    char subject_in[128], profile_in[ALIAS_MAX_LEN + 1];
    char actual[PROFILE_HASH_STR_LEN];
    bool selected[GRANT_ACTION_COUNT];
    int rc = -1;
    if (subject_grant_canonical_digest_json(j, actual, profile_out, selected) != 0 ||
        strcmp(actual, expected_hash) != 0) {
        errno = EPERM;
        goto out;
    }
    if (hold_json_get_str(j, "subject", subject_in, sizeof(subject_in)) != 0 || strcmp(subject_in, subject) != 0 ||
        hold_json_get_str(j, "profile", profile_in, sizeof(profile_in)) != 0 || strcmp(profile_in, profile) != 0 ||
        subject_grant_has_action(j, required_action) != 0) {
        errno = EINVAL;
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        hold_free_profile(profile_out);
    }
    free(j);
    return rc;
}

static int write_sudoers_template_file(const char *sudoers_path,
                                       const char *target_label,
                                       const char *subject,
                                       const char *abs_hold,
                                       const char *hash,
                                       const bool selected[GRANT_ACTION_COUNT],
                                       bool all_scope);
static int unlink_sudoers_template_file(const char *sudoers_path);

static int parse_grant_subject(const char *input, char *out, size_t n) {
    if (!input || !*input) {
        return -1;
    }
    if (strcmp(input, "all") == 0 || strcmp(input, "ALL") == 0) {
        return hold_checked_snprintf(out, n, "%s", "ALL");
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
    return hold_checked_snprintf(out, n, "%s%s", group ? "%" : "", name);
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

static int validate_hold_self_for_sudoers(const char *program, char *abs_hold, size_t n) {
    if (hold_resolve_self_executable_path(program, abs_hold, n) != 0) {
        fprintf(stderr, "hold: error: cannot determine executable path for sudoers grant\n");
        return -1;
    }
    for (const char *p = abs_hold; *p; p++) {
        if (isspace((unsigned char)*p)) {
            fprintf(stderr, "hold: error: executable path contains whitespace and cannot be safely managed in sudoers\n");
            return -1;
        }
    }
    struct stat st;
    if (stat(abs_hold, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "hold: error: executable path is not a regular file: %s\n", abs_hold);
        return -1;
    }
    if (st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        fprintf(stderr,
                "hold: error: refusing sudoers grant because %s is not root-owned with group/world writes disabled\n",
                abs_hold);
        return -1;
    }
    return 0;
}

static const char *sudoers_dir_path(void) {
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_TEST_SUDOERS_DIR");
    if (override && *override) {
        return override;
    }
#endif
    return "/etc/sudoers.d";
}

static int resolve_system_alias_hash_for_grant(const struct hold_store *system_store,
                                               const char *alias,
                                               char hash[PROFILE_HASH_STR_LEN]) {
    if (!hold_valid_alias(alias)) {
        return -1;
    }
    if (hold_alias_lookup_hash(system_store, alias, hash) != 0) {
        return -1;
    }
    return hold_profile_exists_in_store(system_store, hash);
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
                              const char *abs_hold,
                              const bool selected[GRANT_ACTION_COUNT],
                              const char *alias,
                              const char *hash) {
    (void)selected;
    return hold_checked_snprintf(out, n,
                            "%s ALL=(root) NOPASSWD: %s ^run %s --cap %s [A-Za-z0-9_-]{1,768}$",
                            subject, abs_hold, alias, hash);
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
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_TEST_VISUDO_PROG");
    if (override && *override) {
        return hold_checked_snprintf(out, n, "%s", override);
    }
#endif
    return hold_resolve_binary_path("visudo", out, n);
}

static int validate_sudoers_candidate(const char *path) {
    char visudo[HOLD_PATH_MAX];
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
        return hold_checked_snprintf(out, n, "%s", "all");
    }
    if (subject[0] == '%') {
        return hold_checked_snprintf(out, n, "group_%s", subject + 1);
    }
    return hold_checked_snprintf(out, n, "%s", subject);
}

static void actions_from_existing_sudoers(const char *existing,
                                          const char *subject,
                                          const char *abs_hold,
                                          const char *alias,
                                          const char *hash,
                                          bool selected[GRANT_ACTION_COUNT]) {
    (void)subject;
    (void)abs_hold;
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
    p = hold_skip_ws(p);
    char buf[256];
    size_t len = 0;
    while (p[len] && p[len] != '\n' && len + 1 < sizeof(buf)) {
        buf[len] = p[len];
        len++;
    }
    buf[len] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = (char *)hold_skip_ws(tok);
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
                                       const char *abs_hold,
                                       const char *hash,
                                       const bool selected[GRANT_ACTION_COUNT],
                                       bool all_scope) {
    char dir[HOLD_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sudoers_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    char tmp[HOLD_PATH_MAX];
    if (hold_checked_snprintf(tmp, sizeof(tmp), "%s.tmp", sudoers_path) != 0) {
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
    fputs("# hold managed sudoers; use hold grant/revoke\n", f);
    fprintf(f, "# target: ");
    hold_json_escape(f, target_label);
    fputc('\n', f);
    fprintf(f, "# hash: ");
    hold_json_escape(f, hash);
    fputc('\n', f);
    fprintf(f, "# actions: %s\n", all_scope ? "ALL" : "explicit");
    char actions_csv[256];
    if (build_actions_csv(selected, actions_csv, sizeof(actions_csv)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fprintf(f, "# actions-list: %s\n", actions_csv);
    char line[HOLD_PATH_MAX + 256];
    if (build_sudoers_line(line, sizeof(line), subject, abs_hold, selected, target_label, hash) != 0) {
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
    (void)hold_fsync_dir_path(dir);
    return 0;
}

static int unlink_sudoers_template_file(const char *sudoers_path) {
    if (unlink(sudoers_path) != 0 && errno != ENOENT) {
        return -1;
    }
    char dir[HOLD_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sudoers_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)hold_fsync_dir_path(dir);
    }
    return 0;
}

int hold_cmd_grant_revoke_action(const struct hold_invocation *inv,
                                   const struct hold_store *system_store,
                                   const char *program,
                                   bool grant,
                                   int argc,
                                   char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: hold %s <profile> <user> [start,stop,kill,tail,dump,prune,console]\n",
                grant ? "grant" : "revoke");
        return 5;
    }
    if (!inv->euid_root) {
        fprintf(stderr, "hold: error: %s requires root authority\n", grant ? "grant" : "revoke");
        return 5;
    }
    char abs_hold[HOLD_PATH_MAX];
    if (validate_hold_self_for_sudoers(program, abs_hold, sizeof(abs_hold)) != 0) {
        return 5;
    }
    char subject[128];
    if (parse_grant_subject(argv[1], subject, sizeof(subject)) != 0) {
        fprintf(stderr, "hold: error: invalid sudoers subject '%s'\n", argv[1]);
        return 5;
    }
    bool selected[GRANT_ACTION_COUNT];
    bool all_scope = false;
    if (parse_grant_actions(argc == 3 ? argv[2] : NULL, selected, &all_scope) != 0) {
        fprintf(stderr, "hold: error: invalid action list '%s'\n", argc == 3 ? argv[2] : "");
        return 5;
    }
    char source_hash[PROFILE_HASH_STR_LEN];
    if (resolve_system_alias_hash_for_grant(system_store, argv[0], source_hash) != 0) {
        fprintf(stderr, "hold: error: grant target must be an existing system profile\n");
        return 5;
    }

    const char *target_label = argv[0];
    char subject_label[128];
    if (subject_file_label_for_grant(subject, subject_label, sizeof(subject_label)) != 0) {
        return 3;
    }
    const char *dir = sudoers_dir_path();
    char sudoers_path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(sudoers_path, sizeof(sudoers_path), "%s/hold_%s_%s", dir, target_label, subject_label) != 0) {
        return 3;
    }
    bool had_existing_sudoers = access(sudoers_path, F_OK) == 0;
    char old_private_path[HOLD_PATH_MAX];
    char old_public_path[HOLD_PATH_MAX];
    char *old_private = NULL;
    char *old_public = NULL;
    if (had_existing_sudoers) {
        if (grant_private_path(system_store, subject, target_label, old_private_path, sizeof(old_private_path)) != 0 ||
            grant_public_path(system_store, subject, target_label, old_public_path, sizeof(old_public_path)) != 0) {
            return 3;
        }
        if (hold_read_owned_file_no_symlink(old_private_path, &old_private) != 0 && errno != ENOENT) {
            hold_die_errno("hold: failed to snapshot private grant");
        }
        if (hold_read_owned_file_no_symlink(old_public_path, &old_public) != 0 && errno != ENOENT) {
            free(old_private);
            hold_die_errno("hold: failed to snapshot public grant");
        }
    }

    if (!grant) {
        if (all_scope) {
            if (unlink_sudoers_template_file(sudoers_path) != 0) {
                free(old_private);
                free(old_public);
                hold_die_errno("hold: failed to remove managed sudoers file");
            }
            (void)unlink_subject_grant_copy(system_store, subject, target_label);
            hold_sig_note(inv, "hold: revoked sudoers entries for %s %s\n", subject, source_hash);
            free(old_private);
            free(old_public);
            return 0;
        }
        char *existing = NULL;
        bool remaining[GRANT_ACTION_COUNT];
        if (hold_read_owned_file_no_symlink(sudoers_path, &existing) != 0) {
            if (errno == ENOENT) {
                hold_sig_note(inv, "hold: revoked sudoers entries for %s %s\n", subject, source_hash);
                free(old_private);
                free(old_public);
                return 0;
            }
            free(old_private);
            free(old_public);
            hold_die_errno("hold: failed to read managed sudoers file");
        }
        actions_from_existing_sudoers(existing, subject, abs_hold, target_label, source_hash, remaining);
        free(existing);
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            if (selected[i]) {
                remaining[i] = false;
            }
        }
        if (!any_grant_action_selected(remaining)) {
            if (unlink_sudoers_template_file(sudoers_path) != 0) {
                free(old_private);
                free(old_public);
                hold_die_errno("hold: failed to remove managed sudoers file");
            }
            (void)unlink_subject_grant_copy(system_store, subject, target_label);
        } else {
            char grant_hash[PROFILE_HASH_STR_LEN];
            if (write_subject_grant_copy(system_store, subject, target_label, source_hash, remaining, grant_hash) != 0 ||
                write_sudoers_template_file(sudoers_path, target_label, subject, abs_hold, grant_hash, remaining, false) != 0) {
                if (had_existing_sudoers) {
                    (void)restore_subject_grant_snapshot(system_store, subject, target_label, old_private, old_public);
                } else {
                    (void)unlink_subject_grant_copy(system_store, subject, target_label);
                }
                free(old_private);
                free(old_public);
                hold_die_errno("hold: failed to update managed sudoers file");
            }
        }
        hold_sig_note(inv, "hold: revoked sudoers entries for %s %s\n", subject, source_hash);
        free(old_private);
        free(old_public);
        return 0;
    }

    if (!all_scope) {
        char *existing = NULL;
        bool merged[GRANT_ACTION_COUNT];
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            merged[i] = selected[i];
        }
        if (hold_read_owned_file_no_symlink(sudoers_path, &existing) == 0) {
            bool existing_actions[GRANT_ACTION_COUNT];
            actions_from_existing_sudoers(existing, subject, abs_hold, target_label, source_hash, existing_actions);
            for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
                merged[i] = merged[i] || existing_actions[i];
            }
            free(existing);
        } else if (errno != ENOENT) {
            free(old_private);
            free(old_public);
            hold_die_errno("hold: failed to read managed sudoers file");
        }
        for (int i = 0; i < GRANT_ACTION_COUNT; i++) {
            selected[i] = merged[i];
        }
    }

    char grant_hash[PROFILE_HASH_STR_LEN];
    if (write_subject_grant_copy(system_store, subject, target_label, source_hash, selected, grant_hash) != 0 ||
        write_sudoers_template_file(sudoers_path, target_label, subject, abs_hold, grant_hash, selected, all_scope) != 0) {
        if (had_existing_sudoers) {
            (void)restore_subject_grant_snapshot(system_store, subject, target_label, old_private, old_public);
        } else {
            (void)unlink_subject_grant_copy(system_store, subject, target_label);
        }
        free(old_private);
        free(old_public);
        hold_die_errno("hold: failed to update managed sudoers file");
    }
    hold_sig_note(inv, "hold: granted sudoers entries for %s %s\n", subject, grant_hash);
    free(old_private);
    free(old_public);
    return 0;
}
