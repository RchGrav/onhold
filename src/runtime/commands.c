#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_run_record *r,
                                 enum run_state st);
static int print_aliases_for_store(const char *scope, const struct hold_store *store, bool verbose);
static void profile_shell_quote(FILE *f, const char *s);
static int profile_export_transcript(FILE *out, const char *name, const struct hold_profile *recipe);
static int profile_export_json(FILE *out, const char *name, const struct hold_profile *recipe);
static int profile_shell_split(const char *line, char ***argv_out, int *argc_out);
static int profile_import_transcript(const struct hold_store *store, const char *path, const char *override_name, bool dry_run);
static int profile_import_json(const struct hold_store *store, const char *j, const char *override_name, bool dry_run);
static void profile_free_tokens(char **argv, int argc);
static int profile_write_command_recipe(const struct hold_store *store,
                                        const char *name,
                                        int argc,
                                        char **argv);

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_run_record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        hold_sig_note(inv, "hold: %s has exited - see 'hold dump %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        hold_sig_note(inv, "hold: %s has no console (start with --console)\n", r->id);
        return 0;
    }
    return hold_run_native_console(r->console_sock);
}

int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *program,
                              const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to console\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = hold_elevate_with_sudo_targets(program, "console", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
    rc = attach_console_record(inv, &r, st);
    free(targets);
    return rc;
}

int hold_cmd_alias_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *program,
                            int argc,
                            char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: hold profile save <id> as <name> [-v]\n");
        return 5;
    }
    const char *target_token = argv[0];
    const char *name = argv[1];
    bool verbose = false;
    if (argc == 3) {
        if (strcmp(argv[2], "-v") != 0 && strcmp(argv[2], "--verbose") != 0) {
            fprintf(stderr, "usage: hold profile save <id> as <name> [-v]\n");
            return 5;
        }
        verbose = true;
    }
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "hold: error: invalid profile '%s'\n", name);
        return 5;
    }

    struct hold_resolved_target target;
    if (hold_resolve_target(inv, user_store, system_store, target_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return hold_report_not_found(target_token);
    }
    if (target.needs_elevation) {
        char scoped[8 + PROFILE_HASH_STR_LEN];
        if (hold_checked_snprintf(scoped, sizeof(scoped), "system:%s", target.id) != 0) {
            return 3;
        }
        char *canon[5] = {"profile", "save", scoped, "as", (char *)name};
        return hold_elevate_with_sudo_canonical(program, 5, canon);
    }
    if (target.store.kind == STORE_USER_LOCAL && inv->euid_root) {
        fprintf(stderr, "hold: error: create user-local profiles as that user\n");
        return 5;
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        if (hold_ensure_system_store(&target.store) != 0) {
            hold_die_errno("hold: failed to init system storage");
        }
    } else if (hold_ensure_user_store_for_current_user(&target.store) != 0) {
        hold_die_errno("hold: failed to init user storage");
    }

    struct hold_run_record r;
    char record_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    char *j = NULL;
    if (hold_read_owned_file_no_symlink(record_path, &j) != 0) {
        return 5;
    }
    char **profile_argv = NULL;
    int profile_argc = 0;
    char binary_path[HOLD_PATH_MAX];
    char hash[PROFILE_HASH_STR_LEN];
    int rc = 0;
    if (hold_json_get_argv_alloc(j, &profile_argv, &profile_argc) != 0 ||
        hold_resolve_binary_path(profile_argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "hold: error: failed to derive profile from run %s\n", target.id);
        rc = 5;
        goto out;
    }
    char command[256];
    if (hold_format_argv_human(command, sizeof(command), profile_argc, profile_argv) != 0) {
        snprintf(command, sizeof(command), "%s", "?");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        hold_profile_hash_for_argv(binary_path, profile_argc, profile_argv, hash);
        if (hold_write_profile_atomic(&target.store, hash, binary_path, profile_argc, profile_argv) != 0) {
            hold_die_errno("hold: failed to write profile");
        }
        if (hold_alias_upsert_hash(&target.store, name, hash) != 0) {
            hold_die_errno("hold: failed to write profile");
        }
        if (verbose) {
            hold_sig_note(inv, "hold: pinned '%s' -> %s (hash %s)\n", name, command, hash);
        } else {
            hold_sig_note(inv, "hold: pinned '%s' -> %s\n", name, command);
        }
    } else {
        if (hold_alias_upsert_recipe(&target.store, name, binary_path, profile_argc, profile_argv) != 0) {
            hold_die_errno("hold: failed to write profile");
        }
        hold_sig_note(inv, "hold: pinned '%s' -> %s\n", name, command);
    }

out:
    hold_free_argv_alloc(profile_argv, profile_argc);
    free(j);
    return rc;
}

static int print_aliases_for_store(const char *scope, const struct hold_store *store, bool verbose) {
    struct hold_alias *entries = NULL;
    size_t count = 0;
    if (hold_load_aliases(store, &entries, &count) != 0) {
        fprintf(stderr, "hold: warning: failed to read %s profiles\n", scope);
        return 5;
    }
    for (size_t i = 0; i < count; i++) {
        char command[96];
        char hash_display[PROFILE_HASH_STR_LEN];
        if (entries[i].has_recipe) {
            if (hold_format_argv_human(command, sizeof(command), entries[i].argc, entries[i].argv) != 0) {
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
    hold_free_aliases(entries, count);
    return 0;
}

int hold_cmd_aliases_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              bool verbose) {
    printf("%-12s %-6s %-40s %s\n", "NAME", "SCOPE", "COMMAND", "HASH");
    int rc = 0;
    if (inv->euid_root) {
        if (print_aliases_for_store("system", system_store, verbose) != 0) {
            rc = 5;
        }
        if (!inv->requested_system && inv->have_sudo_user) {
            struct hold_store sudo_user_store;
            if (hold_init_invoking_user_store(inv, &sudo_user_store) == 0 &&
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

static void profile_shell_quote(FILE *f, const char *s) {
    if (!s || !*s) {
        fputs("''", f);
        return;
    }
    bool bare = true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!isalnum(*p) && !strchr("/._-:=,@%+", *p)) {
            bare = false;
            break;
        }
    }
    if (bare) {
        fputs(s, f);
        return;
    }
    fputc('\'', f);
    for (; *s; s++) {
        if (*s == '\'') {
            fputs("'\\''", f);
        } else {
            fputc(*s, f);
        }
    }
    fputc('\'', f);
}

static int profile_export_transcript(FILE *out, const char *name, const struct hold_profile *recipe) {
    fputs("enable\nconfigure terminal\nprofile ", out);
    profile_shell_quote(out, name);
    fputs("\nbinary ", out);
    profile_shell_quote(out, recipe->binary_path);
    fputc('\n', out);
    if (recipe->argc > 1) {
        fputs("argv", out);
    }
    for (int i = 1; i < recipe->argc; i++) {
        fputc(' ', out);
        profile_shell_quote(out, recipe->argv[i]);
    }
    if (recipe->argc > 1) fputc('\n', out);
    for (int i = 0; i < recipe->envc; i++) {
        const char *eq = recipe->env[i] ? strchr(recipe->env[i], '=') : NULL;
        if (!eq || eq == recipe->env[i]) continue;
        fputs("env ", out);
        char key[256];
        size_t key_len = (size_t)(eq - recipe->env[i]);
        if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
        memcpy(key, recipe->env[i], key_len);
        key[key_len] = '\0';
        profile_shell_quote(out, key);
        fputc(' ', out);
        profile_shell_quote(out, eq + 1);
        fputc('\n', out);
    }
    if (recipe->mode_interactive) fputs("interactive\n", out);
    if (recipe->mode_tty) fputs("tty\n", out);
    if (recipe->mode_detach) fputs("detach\n", out);
    if (recipe->allow_multi) fputs("multi\n", out);
    if (recipe->has_restart_policy && recipe->restart_policy[0]) {
        fputs("restart ", out);
        profile_shell_quote(out, recipe->restart_policy);
        fputc('\n', out);
    }
    if (recipe->has_restart_delay) {
        fprintf(out, "restart-delay %d\n", recipe->restart_delay_seconds);
    }
    fputs("commit\nend\nwrite\n", out);
    return ferror(out) ? 3 : 0;
}

static int profile_export_json(FILE *out, const char *name, const struct hold_profile *recipe) {
    fputs("{\n  \"version\": 1,\n  \"name\": \"", out);
    hold_json_escape(out, name);
    fputs("\",\n  \"bin\": \"", out);
    hold_json_escape(out, recipe->binary_path);
    fputs("\",\n  \"args\": ", out);
    hold_write_json_argv(out, recipe->argc, recipe->argv);
    if (recipe->envc > 0 && recipe->env) {
        fputs(",\n  \"env\": ", out);
        hold_write_json_argv(out, recipe->envc, recipe->env);
    }
    if (recipe->portc > 0 && recipe->ports) {
        fputs(",\n  \"ports\": ", out);
        hold_write_json_argv(out, recipe->portc, recipe->ports);
    }
    if (recipe->volumec > 0 && recipe->volumes) {
        fputs(",\n  \"volumes\": ", out);
        hold_write_json_argv(out, recipe->volumec, recipe->volumes);
    }
    if (recipe->mode_interactive || recipe->mode_tty || recipe->mode_detach || recipe->allow_multi) {
        bool wrote = false;
        fputs(",\n  \"mode\": {", out);
        if (recipe->mode_interactive) {
            fputs("\"interactive\": true", out);
            wrote = true;
        }
        if (recipe->mode_tty) {
            fprintf(out, "%s\"tty\": true", wrote ? ", " : "");
            wrote = true;
        }
        if (recipe->mode_detach) {
            fprintf(out, "%s\"detach\": true", wrote ? ", " : "");
            wrote = true;
        }
        if (recipe->allow_multi) {
            fprintf(out, "%s\"multi\": true", wrote ? ", " : "");
        }
        fputs("}", out);
    }
    if (recipe->has_restart_policy && recipe->restart_policy[0]) {
        fputs(",\n  \"restart\": \"", out);
        hold_json_escape(out, recipe->restart_policy);
        fputs("\"", out);
    }
    if (recipe->has_restart_delay) {
        fprintf(out, ",\n  \"restart_delay_seconds\": %d", recipe->restart_delay_seconds);
    }
    fputs("\n}\n", out);
    return ferror(out) ? 3 : 0;
}

static void profile_free_tokens(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int profile_push_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t next_cap = *cap ? *cap * 2 : 32;
        char *next = realloc(*buf, next_cap);
        if (!next) {
            return -1;
        }
        *buf = next;
        *cap = next_cap;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

static int profile_push_token(char ***argv, int *argc, int *cap, char *token) {
    if (*argc == *cap) {
        int next_cap = *cap ? *cap * 2 : 8;
        char **next = realloc(*argv, (size_t)next_cap * sizeof(*next));
        if (!next) {
            return -1;
        }
        *argv = next;
        *cap = next_cap;
    }
    (*argv)[(*argc)++] = token;
    return 0;
}

static int profile_shell_split(const char *line, char ***argv_out, int *argc_out) {
    *argv_out = NULL;
    *argc_out = 0;
    char **argv = NULL;
    int argc = 0, argv_cap = 0;
    const char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p || *p == '#') {
            break;
        }
        char *buf = NULL;
        size_t len = 0, cap = 0;
        bool in_single = false, in_double = false;
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (!in_single && !in_double && (isspace(c) || c == '#')) {
                break;
            }
            if (!in_double && c == '\'') {
                in_single = !in_single;
                p++;
                continue;
            }
            if (!in_single && c == '"') {
                in_double = !in_double;
                p++;
                continue;
            }
            if (!in_single && c == '\\') {
                p++;
                if (!*p) {
                    free(buf);
                    profile_free_tokens(argv, argc);
                    errno = EINVAL;
                    return -1;
                }
                c = (unsigned char)*p;
            }
            if (profile_push_char(&buf, &len, &cap, (char)c) != 0) {
                free(buf);
                profile_free_tokens(argv, argc);
                return -1;
            }
            p++;
        }
        if (in_single || in_double) {
            free(buf);
            profile_free_tokens(argv, argc);
            errno = EINVAL;
            return -1;
        }
        if (profile_push_char(&buf, &len, &cap, '\0') != 0) {
            free(buf);
            profile_free_tokens(argv, argc);
            return -1;
        }
        if (profile_push_token(&argv, &argc, &argv_cap, buf) != 0) {
            free(buf);
            profile_free_tokens(argv, argc);
            return -1;
        }
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '#') {
            break;
        }
    }
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static int profile_import_json(const struct hold_store *store,
                               const char *j,
                               const char *override_name,
                               bool dry_run) {
    char name[ALIAS_MAX_LEN + 1];
    char binary_path[HOLD_PATH_MAX];
    char **argv = NULL;
    char **env = NULL;
    char **ports = NULL;
    char **volumes = NULL;
    int argc = 0;
    int envc = 0;
    int portc = 0;
    int volumec = 0;
    bool mode_interactive = false;
    bool mode_tty = false;
    bool mode_detach = false;
    bool allow_multi = false;
    char restart_policy[64] = {0};
    int restart_delay_seconds = 0;
    int rc = 5;
    if (hold_json_get_str(j, "name", name, sizeof(name)) != 0 || !hold_valid_alias(name) ||
        hold_json_get_args_alloc(j, &argv, &argc) != 0 || argc <= 0) {
        fprintf(stderr, "hold: error: invalid profile JSON\n");
        goto out;
    }
    if (override_name) {
        if (!hold_valid_alias(override_name)) {
            fprintf(stderr, "hold: error: invalid profile name '%s'\n", override_name);
            goto out;
        }
        snprintf(name, sizeof(name), "%s", override_name);
    }
    if (hold_json_get_str(j, "bin", binary_path, sizeof(binary_path)) != 0 &&
        hold_json_get_str(j, "binary_path", binary_path, sizeof(binary_path)) != 0 &&
        hold_resolve_binary_path(argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "hold: error: failed to resolve profile command '%s'\n", argv[0]);
        goto out;
    }
    if (binary_path[0] != '/') {
        fprintf(stderr, "hold: error: invalid profile binary path\n");
        goto out;
    }
    if (hold_normalize_existing_argv_paths_from_cwd(argv, argc, 1, NULL) != 0) {
        hold_die_errno("hold: failed to normalize profile argv paths");
    }
    (void)hold_json_get_env_alloc(j, &env, &envc);
    (void)hold_json_get_ports_alloc(j, &ports, &portc);
    (void)hold_json_get_volumes_alloc(j, &volumes, &volumec);
    (void)hold_json_get_bool(j, "interactive", &mode_interactive);
    (void)hold_json_get_bool(j, "tty", &mode_tty);
    (void)hold_json_get_bool(j, "detach", &mode_detach);
    (void)hold_json_get_bool(j, "multi", &allow_multi);
    (void)hold_json_get_str(j, "restart", restart_policy, sizeof(restart_policy));
    int64_t restart_delay_tmp = 0;
    if (hold_json_get_i64(j, "restart_delay_seconds", &restart_delay_tmp) == 0 && restart_delay_tmp >= 0 && restart_delay_tmp <= INT_MAX) {
        restart_delay_seconds = (int)restart_delay_tmp;
    }
    const char *mode = NULL;
    if (hold_json_find_key(j, "mode", &mode) == 0 && *mode == '{') {
        const char *end = mode;
        if (hold_skip_json_value(&end) == 0 && end > mode) {
            size_t len = (size_t)(end - mode);
            char *copy = malloc(len + 1);
            if (!copy) {
                rc = 3;
                goto out;
            }
            memcpy(copy, mode, len);
            copy[len] = '\0';
            (void)hold_json_get_bool(copy, "interactive", &mode_interactive);
            (void)hold_json_get_bool(copy, "tty", &mode_tty);
            (void)hold_json_get_bool(copy, "detach", &mode_detach);
            (void)hold_json_get_bool(copy, "multi", &allow_multi);
            free(copy);
        }
    }
    if (dry_run) {
        printf("profile %s import dry-run ok\n", name);
        rc = 0;
        goto out;
    }
    if (hold_alias_upsert_recipe_full(store,
                                      name,
                                      binary_path,
                                      argc,
                                      argv,
                                      envc,
                                      env,
                                      portc,
                                      ports,
                                      volumec,
                                      volumes,
                                      mode_interactive,
                                      mode_tty,
                                      mode_detach,
                                      allow_multi,
                                      restart_policy[0] ? restart_policy : NULL,
                                      restart_delay_seconds) != 0) {
        hold_die_errno("hold: failed to import profile");
    }
    rc = 0;
out:
    hold_free_argv_alloc(argv, argc);
    hold_free_argv_alloc(env, envc);
    hold_free_argv_alloc(ports, portc);
    hold_free_argv_alloc(volumes, volumec);
    return rc;
}

static int profile_write_command_recipe(const struct hold_store *store,
                                        const char *name,
                                        int argc,
                                        char **argv) {
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", name ? name : "");
        return 5;
    }
    if (argc <= 0 || !argv || !argv[0] || !*argv[0]) {
        fprintf(stderr, "usage: hold profile <name> set command -- <cmd> [args...]\n");
        return 5;
    }
    char binary_path[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "hold: error: failed to resolve profile command '%s'\n", argv[0]);
        return 5;
    }
    if (hold_normalize_existing_argv_paths_from_cwd(argv, argc, 1, NULL) != 0) {
        hold_die_errno("hold: failed to normalize profile argv paths");
    }
    if (hold_alias_upsert_recipe(store, name, binary_path, argc, argv) != 0) {
        hold_die_errno("hold: failed to update profile");
    }
    return 0;
}

static int profile_write_full_recipe(const struct hold_store *store,
                                     const char *name,
                                     const char *binary,
                                     int argc,
                                     char **argv,
                                     int envc,
                                     char **env,
                                     bool mode_interactive,
                                     bool mode_tty,
                                     bool mode_detach,
                                     bool allow_multi,
                                     const char *restart_policy,
                                     int restart_delay_seconds) {
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", name ? name : "");
        return 5;
    }
    if (!binary || binary[0] != '/' || argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "hold: error: incomplete profile transcript\n");
        return 5;
    }
    char binary_path[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(binary, binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "hold: error: failed to resolve profile command '%s'\n", binary);
        return 5;
    }
    if (hold_normalize_existing_argv_paths_from_cwd(argv, argc, 1, NULL) != 0) {
        hold_die_errno("hold: failed to normalize profile argv paths");
    }
    if (hold_alias_upsert_recipe_full(store,
                                      name,
                                      binary_path,
                                      argc,
                                      argv,
                                      envc,
                                      env,
                                      0,
                                      NULL,
                                      0,
                                      NULL,
                                      mode_interactive,
                                      mode_tty,
                                      mode_detach,
                                      allow_multi,
                                      restart_policy,
                                      restart_delay_seconds) != 0) {
        hold_die_errno("hold: failed to update profile");
    }
    return 0;
}

static int profile_import_transcript(const struct hold_store *store,
                                     const char *path,
                                     const char *override_name,
                                     bool dry_run) {
    char *text = NULL;
    if (hold_read_small_file(path, &text) != 0) {
        fprintf(stderr, "hold: error: failed to read profile transcript '%s'\n", path);
        return 5;
    }
    const char *p = hold_skip_ws(text);
    if (*p == '{') {
        int json_rc = profile_import_json(store, text, override_name, dry_run);
        free(text);
        return json_rc;
    }

    char name[ALIAS_MAX_LEN + 1] = {0};
    char binary[HOLD_PATH_MAX] = {0};
    char **args = NULL;
    int arg_count = 0;
    int arg_cap = 0;
    char **env = NULL;
    int envc = 0;
    int env_cap = 0;
    char **cmd_argv = NULL;
    int cmd_argc = 0;
    bool mode_interactive = false;
    bool mode_tty = false;
    bool mode_detach = false;
    bool allow_multi = false;
    char restart_policy[64] = {0};
    int restart_delay_seconds = 0;
    bool saw_save = false;
    int rc = 5;

    char *cursor = text;
    while (*cursor) {
        char *line = cursor;
        char *nl = strchr(cursor, '\n');
        if (nl) {
            *nl = '\0';
            cursor = nl + 1;
        } else {
            cursor += strlen(cursor);
        }
        char **tokens = NULL;
        int ntokens = 0;
        if (profile_shell_split(line, &tokens, &ntokens) != 0) {
            fprintf(stderr, "hold: error: invalid profile transcript syntax\n");
            goto out;
        }
        if (ntokens == 0) {
            profile_free_tokens(tokens, ntokens);
            continue;
        }
        if (!strcmp(tokens[0], "profile") && ntokens == 2) {
            if (!hold_valid_alias(tokens[1])) {
                fprintf(stderr, "hold: error: invalid profile name '%s'\n", tokens[1]);
                profile_free_tokens(tokens, ntokens);
                goto out;
            }
            snprintf(name, sizeof(name), "%s", tokens[1]);
        } else if (!strcmp(tokens[0], "enable") && ntokens == 1) {
            /* Cisco-style transcript prelude; no-op for file import. */
        } else if (!strcmp(tokens[0], "configure") && ntokens == 2 && !strcmp(tokens[1], "terminal")) {
            /* Cisco-style transcript prelude; no-op for file import. */
        } else if (!strcmp(tokens[0], "binary") && ntokens == 2) {
            if (tokens[1][0] != '/') {
                fprintf(stderr, "hold: error: profile binary must be absolute\n");
                profile_free_tokens(tokens, ntokens);
                goto out;
            }
            snprintf(binary, sizeof(binary), "%s", tokens[1]);
        } else if (!strcmp(tokens[0], "argv") && ntokens >= 2) {
            for (int i = 1; i < ntokens; i++) {
                char *copy = strdup(tokens[i]);
                if (!copy || profile_push_token(&args, &arg_count, &arg_cap, copy) != 0) {
                    free(copy);
                    profile_free_tokens(tokens, ntokens);
                    goto out;
                }
            }
        } else if (!strcmp(tokens[0], "env") && (ntokens == 2 || ntokens == 3)) {
            char assignment[4096];
            if (ntokens == 2) {
                if (!strchr(tokens[1], '=') || strlen(tokens[1]) >= sizeof(assignment)) {
                    fprintf(stderr, "hold: error: invalid env command in profile transcript\n");
                    profile_free_tokens(tokens, ntokens);
                    goto out;
                }
                memcpy(assignment, tokens[1], strlen(tokens[1]) + 1);
            } else {
                if (!tokens[1][0] || strchr(tokens[1], '=') ||
                    hold_checked_snprintf(assignment, sizeof(assignment), "%s=%s", tokens[1], tokens[2]) != 0) {
                    fprintf(stderr, "hold: error: invalid env command in profile transcript\n");
                    profile_free_tokens(tokens, ntokens);
                    goto out;
                }
            }
            char *copy = strdup(assignment);
            if (!copy || profile_push_token(&env, &envc, &env_cap, copy) != 0) {
                free(copy);
                profile_free_tokens(tokens, ntokens);
                goto out;
            }
        } else if (!strcmp(tokens[0], "interactive") && ntokens == 1) {
            mode_interactive = true;
        } else if ((!strcmp(tokens[0], "tty") || !strcmp(tokens[0], "console")) && ntokens == 1) {
            mode_tty = true;
        } else if (!strcmp(tokens[0], "detach") && ntokens == 1) {
            mode_detach = true;
        } else if (!strcmp(tokens[0], "multi") && ntokens == 1) {
            allow_multi = true;
        } else if (!strcmp(tokens[0], "restart") && ntokens == 2) {
            snprintf(restart_policy, sizeof(restart_policy), "%s", tokens[1]);
        } else if ((!strcmp(tokens[0], "restart-delay") || !strcmp(tokens[0], "restart_delay_seconds")) && ntokens == 2) {
            char *endptr = NULL;
            long delay = strtol(tokens[1], &endptr, 10);
            if (!endptr || *endptr || delay < 0 || delay > INT_MAX) {
                fprintf(stderr, "hold: error: invalid restart-delay in profile transcript\n");
                profile_free_tokens(tokens, ntokens);
                goto out;
            }
            restart_delay_seconds = (int)delay;
        } else if (!strcmp(tokens[0], "no") && ntokens == 2 && !strcmp(tokens[1], "interactive")) {
            mode_interactive = false;
        } else if (!strcmp(tokens[0], "no") && ntokens == 2 && (!strcmp(tokens[1], "tty") || !strcmp(tokens[1], "console"))) {
            mode_tty = false;
        } else if (!strcmp(tokens[0], "no") && ntokens == 2 && !strcmp(tokens[1], "detach")) {
            mode_detach = false;
        } else if (!strcmp(tokens[0], "no") && ntokens == 2 && !strcmp(tokens[1], "multi")) {
            allow_multi = false;
        } else if (!strcmp(tokens[0], "default") && ntokens == 2 && !strcmp(tokens[1], "multi")) {
            allow_multi = false;
        } else if (!strcmp(tokens[0], "no") && ntokens == 2 && !strcmp(tokens[1], "restart")) {
            restart_policy[0] = '\0';
            restart_delay_seconds = 0;
        } else if (!strcmp(tokens[0], "set") && ntokens >= 4 &&
                   !strcmp(tokens[1], "command") && !strcmp(tokens[2], "--")) {
            profile_free_tokens(cmd_argv, cmd_argc);
            cmd_argc = ntokens - 3;
            cmd_argv = calloc((size_t)cmd_argc, sizeof(*cmd_argv));
            if (!cmd_argv) {
                profile_free_tokens(tokens, ntokens);
                goto out;
            }
            for (int i = 0; i < cmd_argc; i++) {
                cmd_argv[i] = strdup(tokens[i + 3]);
                if (!cmd_argv[i]) {
                    profile_free_tokens(tokens, ntokens);
                    goto out;
                }
            }
        } else if ((!strcmp(tokens[0], "save") || !strcmp(tokens[0], "commit")) && ntokens == 1) {
            saw_save = true;
        } else if ((!strcmp(tokens[0], "end") || !strcmp(tokens[0], "write")) && ntokens == 1) {
            /* Cisco-style transcript trailer; no-op for file import. */
        } else {
            fprintf(stderr, "hold: error: unsupported profile transcript command '%s'\n", tokens[0]);
            profile_free_tokens(tokens, ntokens);
            goto out;
        }
        profile_free_tokens(tokens, ntokens);
    }

    if (override_name) {
        if (!hold_valid_alias(override_name)) {
            fprintf(stderr, "hold: error: invalid profile name '%s'\n", override_name);
            goto out;
        }
        snprintf(name, sizeof(name), "%s", override_name);
    }
    if (!saw_save || name[0] == '\0') {
        fprintf(stderr, "hold: error: incomplete profile transcript\n");
        goto out;
    }
    if (cmd_argv) {
        if (dry_run) {
            if (!hold_valid_alias(name) || cmd_argc <= 0 || !cmd_argv || !cmd_argv[0] || !*cmd_argv[0]) {
                fprintf(stderr, "hold: error: incomplete profile transcript\n");
                goto out;
            }
            char binary_path[HOLD_PATH_MAX];
            if (hold_resolve_binary_path(cmd_argv[0], binary_path, sizeof(binary_path)) != 0) {
                fprintf(stderr, "hold: error: failed to resolve profile command '%s'\n", cmd_argv[0]);
                goto out;
            }
            if (hold_normalize_existing_argv_paths_from_cwd(cmd_argv, cmd_argc, 1, NULL) != 0) {
                hold_die_errno("hold: failed to normalize profile argv paths");
            }
            printf("profile %s import dry-run ok\n", name);
            rc = 0;
        } else {
            rc = profile_write_command_recipe(store, name, cmd_argc, cmd_argv);
        }
    } else {
        if (!binary[0]) {
            fprintf(stderr, "hold: error: incomplete profile transcript\n");
            goto out;
        }
        int argc = arg_count + 1;
        char **argv = calloc((size_t)argc + 1, sizeof(*argv));
        if (!argv) {
            rc = 3;
            goto out;
        }
        argv[0] = binary;
        for (int i = 0; i < arg_count; i++) argv[i + 1] = args[i];
        if (dry_run) {
            if (!hold_valid_alias(name) || binary[0] != '/' || argc <= 0 || !argv[0]) {
                fprintf(stderr, "hold: error: incomplete profile transcript\n");
                free(argv);
                goto out;
            }
            char binary_path[HOLD_PATH_MAX];
            if (hold_resolve_binary_path(binary, binary_path, sizeof(binary_path)) != 0) {
                fprintf(stderr, "hold: error: failed to resolve profile command '%s'\n", binary);
                free(argv);
                goto out;
            }
            if (hold_normalize_existing_argv_paths_from_cwd(argv, argc, 1, NULL) != 0) {
                hold_die_errno("hold: failed to normalize profile argv paths");
            }
            printf("profile %s import dry-run ok\n", name);
            rc = 0;
        } else {
            rc = profile_write_full_recipe(store, name, binary, argc, argv, envc, env, mode_interactive, mode_tty, mode_detach, allow_multi, restart_policy[0] ? restart_policy : NULL, restart_delay_seconds);
        }
        free(argv);
    }

out:
    profile_free_tokens(args, arg_count);
    profile_free_tokens(env, envc);
    profile_free_tokens(cmd_argv, cmd_argc);
    free(text);
    return rc;
}

static int profile_export_to_path(const char *path,
                                  bool json,
                                  const char *name,
                                  const struct hold_profile *recipe) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "hold: error: failed to open export path '%s': %s\n", path, strerror(errno));
        return 3;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        int saved_errno = errno;
        close(fd);
        fprintf(stderr, "hold: error: failed to open export stream '%s': %s\n", path, strerror(saved_errno));
        return 3;
    }
    int rc = json ? profile_export_json(out, name, recipe) : profile_export_transcript(out, name, recipe);
    if (fclose(out) != 0 && rc == 0) {
        fprintf(stderr, "hold: error: failed to write export path '%s': %s\n", path, strerror(errno));
        rc = 3;
    }
    return rc;
}

int hold_cmd_profile_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              int argc,
                              char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n       hold import <file> [as <profile>] [--dry-run|--yes]\n");
        return 5;
    }
    if (inv->euid_root) {
        fprintf(stderr, "hold: error: profile import/export is user-local; run it as the profile owner\n");
        return 5;
    }
    if (!strcmp(argv[0], "export")) {
        bool json = false;
        const char *path = NULL;
        if (argc < 2) {
            fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n");
            return 5;
        }
        const char *name = argv[1];
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "--format=json")) {
                json = true;
            } else if (!strcmp(argv[i], "--format=cli")) {
                json = false;
            } else if (!strcmp(argv[i], "--format")) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n");
                    return 5;
                }
                if (!strcmp(argv[i + 1], "json")) {
                    json = true;
                } else if (!strcmp(argv[i + 1], "cli")) {
                    json = false;
                } else {
                    fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n");
                    return 5;
                }
                i++;
            } else if (!strcmp(argv[i], "as")) {
                if (path || i + 1 >= argc) {
                    fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n");
                    return 5;
                }
                path = argv[++i];
            } else {
                fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n");
                return 5;
            }
        }
        if (!hold_valid_alias(name)) {
            fprintf(stderr, "hold: error: invalid profile name '%s'\n", name);
            return 5;
        }
        struct hold_profile recipe;
        if (hold_alias_lookup_recipe(user_store, name, &recipe) != 0) {
            fprintf(stderr, "hold: error: profile '%s' not found\n", name);
            return 5;
        }
        int rc = path ? profile_export_to_path(path, json, name, &recipe)
                      : (json ? profile_export_json(stdout, name, &recipe)
                              : profile_export_transcript(stdout, name, &recipe));
        hold_free_profile(&recipe);
        return rc;
    }
    if (!strcmp(argv[0], "import")) {
        if (argc < 2) {
            fprintf(stderr, "usage: hold import <file> [as <profile>] [--dry-run|--yes]\n");
            return 5;
        }
        const char *path = argv[1];
        const char *override_name = NULL;
        bool dry_run = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "as")) {
                if (override_name || i + 1 >= argc) {
                    fprintf(stderr, "usage: hold import <file> [as <profile>] [--dry-run|--yes]\n");
                    return 5;
                }
                override_name = argv[++i];
            } else if (!strcmp(argv[i], "--dry-run")) {
                dry_run = true;
            } else if (!strcmp(argv[i], "--yes")) {
                /* Non-interactive CLI import applies immediately; --yes is accepted for scripts. */
            } else {
                fprintf(stderr, "usage: hold import <file> [as <profile>] [--dry-run|--yes]\n");
                return 5;
            }
        }
        return profile_import_transcript(user_store, path, override_name, dry_run);
    }
    fprintf(stderr, "usage: hold export <profile> [as <file>] [--format cli|json]\n       hold import <file> [as <profile>] [--dry-run|--yes]\n");
    return 5;
}

int hold_cmd_profile_set_command(const struct hold_invocation *inv,
                                    const struct hold_store *user_store,
                                    const char *name,
                                    int argc,
                                    char **argv) {
    if (inv->euid_root) {
        fprintf(stderr, "hold: error: profile editing is user-local; run it as the profile owner\n");
        return 5;
    }
    return profile_write_command_recipe(user_store, name, argc, argv);
}

int hold_cmd_profile_create_command(const struct hold_invocation *inv,
                                       const struct hold_store *user_store,
                                       const char *name,
                                       int argc,
                                       char **argv) {
    if (inv->euid_root) {
        fprintf(stderr, "hold: error: profile editing is user-local; run it as the profile owner\n");
        return 5;
    }
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", name ? name : "");
        return 5;
    }
    if (hold_alias_exists_in_store(user_store, name)) {
        fprintf(stderr, "hold: error: profile '%s' already exists\n", name);
        return 5;
    }
    return profile_write_command_recipe(user_store, name, argc, argv);
}

int hold_cmd_profile_delete(const struct hold_invocation *inv,
                               const struct hold_store *user_store,
                               const char *name) {
    if (inv->euid_root) {
        fprintf(stderr, "hold: error: profile editing is user-local; run it as the profile owner\n");
        return 5;
    }
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", name ? name : "");
        return 5;
    }
    bool deleted = false;
    if (hold_alias_delete(user_store, name, &deleted) != 0) {
        hold_die_errno("hold: failed to delete profile");
    }
    if (!deleted) {
        fprintf(stderr, "hold: error: profile '%s' not found\n", name);
        return 5;
    }
    return 0;
}

int hold_cmd_profile_rename(const struct hold_invocation *inv,
                               const struct hold_store *user_store,
                               const char *old_name,
                               const char *new_name) {
    if (inv->euid_root) {
        fprintf(stderr, "hold: error: profile editing is user-local; run it as the profile owner\n");
        return 5;
    }
    if (!hold_valid_alias(old_name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", old_name ? old_name : "");
        return 5;
    }
    if (!hold_valid_alias(new_name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", new_name ? new_name : "");
        return 5;
    }
    if (hold_alias_rename(user_store, old_name, new_name) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "hold: error: profile '%s' not found\n", old_name);
            return 5;
        }
        if (errno == EEXIST) {
            fprintf(stderr, "hold: error: profile '%s' already exists\n", new_name);
            return 5;
        }
        hold_die_errno("hold: failed to rename profile");
    }
    return 0;
}

void hold_usage(void) {
    printf("hold %s - more than nohup, less than systemd\n\n"
           "Run a command or profile under a durable run ID, then list it, watch it,\n"
           "inspect it, and stop it later. No daemon, no config server.\n\n"
           "USAGE\n"
           "  hold [run-options] <cmd> [args...]      Docker-shaped ad-hoc launch\n"
           "  hold run [run-options] <cmd|profile>   Docker-shaped launch/profile run\n"
           "  hold <command> [args...]                manage runs and profiles\n\n"
           "RUN\n"
           "  hold <command>...                       foreground run, streaming logs\n"
           "  hold run <command>...                   foreground run, streaming logs\n"
           "  hold -d <command>...                    detach/background and print run ID\n"
           "  hold -it <command>...                   allocate Hold's PTY/console path\n"
           "  hold run --name web -d <command>...     create/label a profile and run it\n"
           "  hold run -it web                        run or reconnect to profile TTY\n\n"
           "MANAGE\n"
           "  hold ps [-a]                            list active (-a: retained inactive too)\n"
           "  hold logs <target> [-f] [-n N]          open/filter logs\n"
           "  hold logs <target> --plain              print log text and exit\n"
           "  hold inspect <target>                   print structured JSON details\n"
           "  hold stop <target> [--all]              graceful stop (TERM, then KILL)\n"
           "  hold kill <target>                      force kill now (KILL)\n"
           "  hold rm [--force] <target>              remove inactive run/profile; force active runid\n"
           "  hold prune [target|all]                 clear inactive past run data\n\n"
           "PROFILES / CONFIG\n"
           "  hold                                   enter Cisco IOS-style captive CLI\n"
           "  hold shell                             capture/adopt from a real system shell\n"
           "  hold profile save <id> as <name>        save a run as a profile\n"
           "  hold export <name> [as <file>]          export IOS-style profile transcript\n"
           "  hold import <file> as <name>            import IOS-style profile transcript\n"
           "  hold profiles [-v]                      list visible profiles\n\n"
           "  target = run id, id prefix, or safely singular profile name\n\n"
           "MORE\n"
           "  hold help profiles           save commands as reusable profiles\n"
           "  hold help access             give another user scoped access\n"
           "  hold help targets            id, profile, and scope resolution\n"
           "  hold help system             root-managed runs and elevation\n"
           "  hold help scripting          exit codes, --print, --quiet, stdout\n"
           "  hold <command> -h            help for one command\n\n"
           "  hold --version\n",
           HOLD_VERSION);
}

static int json_request_get_op(const char *json, char *op, size_t op_n, bool *force) {
    int64_t v = 0;
    if (hold_json_get_i64(json, "v", &v) != 0 || v != 1 ||
        hold_json_get_str(json, "op", op, op_n) != 0) {
        return -1;
    }
    bool parsed_force = false;
    if (hold_json_get_bool(json, "force", &parsed_force) != 0) {
        parsed_force = false;
    }
    *force = parsed_force;
    return 0;
}

int hold_cmd_cap_request_action(const struct hold_invocation *inv,
                                const struct hold_store *system_store,
                                bool tail,
                                bool console_mode,
                                int argc,
                                char **argv) {
    if (!inv || !inv->euid_root || argc != 4) {
        return -1;
    }
    const char *profile = argv[0];
    const char *cap_flag = argv[1];
    const char *hash = argv[2];
    const char *encoded = argv[3];
    if (!hold_valid_alias(profile) || strcmp(cap_flag, "--cap") != 0 || !hold_valid_profile_hash(hash) || strlen(encoded) > 768) {
        return -1;
    }
    unsigned char decoded[2048];
    size_t decoded_len = 0;
    if (hold_base64url_decode(encoded, decoded, sizeof(decoded), &decoded_len) != 0) {
        fprintf(stderr, "hold: error: malformed capability request\n");
        return 5;
    }
    if (decoded_len >= sizeof(decoded)) {
        fprintf(stderr, "hold: error: oversized capability request\n");
        return 5;
    }
    decoded[decoded_len] = '\0';
    char op[32];
    bool force = false;
    if (json_request_get_op((const char *)decoded, op, sizeof(op), &force) != 0) {
        fprintf(stderr, "hold: error: invalid capability request\n");
        return 5;
    }
    const char *subject = (inv->have_sudo_user && inv->invoking_user[0]) ? inv->invoking_user : "root";
    struct hold_profile granted;
    if (hold_load_subject_grant_profile(system_store, subject, profile, hash, op, &granted) != 0) {
        fprintf(stderr, "hold: error: capability for '%s' is no longer valid\n", profile);
        return 3;
    }
    int rc = 0;
    if (!strcmp(op, "start")) {
        (void)force;
        rc = hold_perform_start_with_env(inv, system_store, tail, console_mode, granted.argc, granted.argv, granted.binary_path, profile, granted.envc, granted.env);
    } else {
        fprintf(stderr, "hold: error: unsupported capability operation '%s'\n", op);
        rc = 5;
    }
    hold_free_profile(&granted);
    return rc;
}

int hold_cmd_elevated_capability_action(const struct hold_invocation *inv,
                                          const struct hold_store *system_store,
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
    if (!hold_valid_runid_selector(runid_sel) || !hold_valid_alias(alias) || !hold_valid_profile_hash(hash)) {
        return -1;
    }
    if (hold_verify_system_alias_cap(system_store, alias, hash) != 0) {
        fprintf(stderr, "hold: error: capability for '%s' is no longer valid\n", alias);
        return 3;
    }

    if (!strcmp(command, "start")) {
        if (strcmp(runid_sel, "000000000000") != 0) {
            fprintf(stderr, "hold: error: start capability requires selector 000000000000\n");
            return 5;
        }
        return hold_perform_profile_start(inv, system_store, tail, console_mode, hash, alias);
    }

    if (strcmp(runid_sel, "000000000000") == 0) {
        fprintf(stderr, "hold: error: selector 000000000000 is only valid for start\n");
        return 5;
    }

    if (strcmp(runid_sel, "ffffffffffff") == 0) {
        if (!hold_command_all_allowed(command)) {
            fprintf(stderr, "hold: error: selector ffffffffffff is not valid for %s\n", command);
            return 5;
        }
        struct alias_match_list matches;
        if (hold_collect_private_alias_matches(system_store, alias, command, &matches) != 0) {
            return 3;
        }
        int worst = 0;
        int acted = 0;
        char boot[128] = {0};
        bool have_boot = hold_current_boot_id(boot, sizeof(boot));
        for (size_t i = 0; i < matches.count; i++) {
            int rc = 0;
            if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
                bool already_done = false;
                rc = hold_do_signal_action(system_store,
                                      matches.items[i].id,
                                      sig,
                                      graceful,
                                      &already_done);
                if (rc == 0) {
                    hold_sig_note(inv,
                             "hold: %s %s\n",
                             already_done ? matches.items[i].id : (!strcmp(command, "kill") ? "killed" : "stopped"),
                             already_done ? "already exited" : matches.items[i].id);
                }
            } else if (!strcmp(command, "prune")) {
                bool removed = false;
                rc = hold_prune_one_run(system_store, matches.items[i].id, have_boot ? boot : NULL, true, &removed);
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
                hold_sig_note(inv, "hold: pruned %d past run%s for '%s'\n", acted, acted == 1 ? "" : "s", alias);
            } else {
                hold_sig_note(inv, "hold: nothing to prune\n");
            }
        } else if (acted == 0 && worst == 0) {
            hold_sig_note(inv, "hold: nothing to %s\n", command);
        }
        hold_free_alias_match_list(&matches);
        return worst;
    }

    if (hold_ensure_run_recorded_under_alias(system_store, runid_sel, alias) != 0) {
        fprintf(stderr, "hold: error: run %s is not recorded under profile '%s'\n", runid_sel, alias);
        return 3;
    }

    struct hold_run_record selected_record;
    char selected_path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(system_store->record_dir, runid_sel, &selected_record, selected_path, sizeof(selected_path)) != 0) {
        return 5;
    }
    char selected_boot[128] = {0};
    bool have_selected_boot = hold_current_boot_id(selected_boot, sizeof(selected_boot));
    enum run_state selected_state = hold_eval_state(&selected_record, have_selected_boot ? selected_boot : NULL);
    if (!strcmp(command, "console")) {
        return attach_console_record(inv, &selected_record, selected_state);
    }
    if (!hold_record_matches_alias_intent(command, &selected_record, selected_state)) {
        hold_sig_note(inv, "hold: nothing to %s\n", command);
        return 0;
    }

    if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
        bool already_done = false;
        int rc = hold_do_signal_action(system_store, runid_sel, sig, graceful, &already_done);
        if (rc == 0) {
            if (already_done) {
                hold_sig_note(inv, "hold: %s already exited\n", runid_sel);
            } else {
                hold_sig_note(inv, "hold: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", runid_sel);
            }
        }
        return rc;
    }
    if (!strcmp(command, "prune")) {
        char boot[128] = {0};
        bool have_boot = hold_current_boot_id(boot, sizeof(boot));
        bool removed = false;
        int rc = hold_prune_one_run(system_store, runid_sel, have_boot ? boot : NULL, true, &removed);
        if (rc == 0) {
            hold_sig_note(inv, removed ? "hold: pruned 1 past run for '%s'\n" : "hold: nothing to prune\n", alias);
        }
        return rc;
    }
    if (!strcmp(command, "tail") || !strcmp(command, "dump")) {
        struct hold_run_record r;
        char path[HOLD_PATH_MAX];
        if (hold_load_record_by_id(system_store->record_dir, runid_sel, &r, path, sizeof(path)) != 0 || !r.has_log) {
            return 5;
        }
        if (!strcmp(command, "tail")) {
            char boot[128] = {0};
            bool have_boot = hold_current_boot_id(boot, sizeof(boot));
            enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
            return hold_tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
        }
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            hold_die_errno("hold: failed to open log for dump");
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
                hold_die_errno("hold: failed while dumping log");
            }
            if (hold_write_all(STDOUT_FILENO, buf, (size_t)nr) != 0) {
                close(fd);
                hold_die_errno("hold: failed writing dumped output");
            }
        }
        close(fd);
        return 0;
    }

    return -1;
}
