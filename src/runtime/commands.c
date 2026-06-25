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
static int profile_export_transcript(const char *name, const struct hold_profile *recipe);
static int profile_export_json(const char *name, const struct hold_profile *recipe);
static int profile_shell_split(const char *line, char ***argv_out, int *argc_out);
static int profile_import_transcript(const struct hold_store *store, const char *path);
static int profile_import_json(const struct hold_store *store, const char *j);
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

static int profile_export_transcript(const char *name, const struct hold_profile *recipe) {
    fputs("profile ", stdout);
    profile_shell_quote(stdout, name);
    fputs("\nset command --", stdout);
    for (int i = 0; i < recipe->argc; i++) {
        fputc(' ', stdout);
        profile_shell_quote(stdout, recipe->argv[i]);
    }
    fputs("\nsave\n", stdout);
    return ferror(stdout) ? 3 : 0;
}

static int profile_export_json(const char *name, const struct hold_profile *recipe) {
    fputs("{\n  \"version\": 1,\n  \"name\": \"", stdout);
    hold_json_escape(stdout, name);
    fputs("\",\n  \"bin\": \"", stdout);
    hold_json_escape(stdout, recipe->binary_path);
    fputs("\",\n  \"args\": ", stdout);
    hold_write_json_argv(stdout, recipe->argc, recipe->argv);
    fputs("\n}\n", stdout);
    return ferror(stdout) ? 3 : 0;
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

static int profile_import_json(const struct hold_store *store, const char *j) {
    char name[ALIAS_MAX_LEN + 1];
    char binary_path[HOLD_PATH_MAX];
    char **argv = NULL;
    int argc = 0;
    int rc = 5;
    if (hold_json_get_str(j, "name", name, sizeof(name)) != 0 || !hold_valid_alias(name) ||
        hold_json_get_args_alloc(j, &argv, &argc) != 0 || argc <= 0) {
        fprintf(stderr, "hold: error: invalid profile JSON\n");
        goto out;
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
    if (hold_alias_upsert_recipe(store, name, binary_path, argc, argv) != 0) {
        hold_die_errno("hold: failed to import profile");
    }
    rc = 0;
out:
    hold_free_argv_alloc(argv, argc);
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
    if (hold_alias_upsert_recipe(store, name, binary_path, argc, argv) != 0) {
        hold_die_errno("hold: failed to update profile");
    }
    return 0;
}

static int profile_import_transcript(const struct hold_store *store, const char *path) {
    char *text = NULL;
    if (hold_read_small_file(path, &text) != 0) {
        fprintf(stderr, "hold: error: failed to read profile transcript '%s'\n", path);
        return 5;
    }
    const char *p = hold_skip_ws(text);
    if (*p == '{') {
        int json_rc = profile_import_json(store, text);
        free(text);
        return json_rc;
    }

    char name[ALIAS_MAX_LEN + 1] = {0};
    char **cmd_argv = NULL;
    int cmd_argc = 0;
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
        } else if (!strcmp(tokens[0], "save") && ntokens == 1) {
            saw_save = true;
        } else {
            fprintf(stderr, "hold: error: unsupported profile transcript command '%s'\n", tokens[0]);
            profile_free_tokens(tokens, ntokens);
            goto out;
        }
        profile_free_tokens(tokens, ntokens);
    }

    if (!saw_save || name[0] == '\0' || cmd_argc <= 0 || !cmd_argv) {
        fprintf(stderr, "hold: error: incomplete profile transcript\n");
        goto out;
    }
    rc = profile_write_command_recipe(store, name, cmd_argc, cmd_argv);

out:
    profile_free_tokens(cmd_argv, cmd_argc);
    free(text);
    return rc;
}

int hold_cmd_profile_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              int argc,
                              char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: hold profile export <name> [--json]\n       hold profile import <file>\n");
        return 5;
    }
    if (inv->euid_root) {
        fprintf(stderr, "hold: error: profile import/export is user-local; run it as the profile owner\n");
        return 5;
    }
    if (!strcmp(argv[0], "export")) {
        bool json = false;
        if (argc == 3 && !strcmp(argv[2], "--json")) {
            json = true;
        } else if (argc != 2) {
            fprintf(stderr, "usage: hold profile export <name> [--json]\n");
            return 5;
        }
        const char *name = argv[1];
        if (!hold_valid_alias(name)) {
            fprintf(stderr, "hold: error: invalid profile name '%s'\n", name);
            return 5;
        }
        struct hold_profile recipe;
        if (hold_alias_lookup_recipe(user_store, name, &recipe) != 0) {
            fprintf(stderr, "hold: error: profile '%s' not found\n", name);
            return 5;
        }
        int rc = json ? profile_export_json(name, &recipe) : profile_export_transcript(name, &recipe);
        hold_free_profile(&recipe);
        return rc;
    }
    if (!strcmp(argv[0], "import")) {
        if (argc != 2) {
            fprintf(stderr, "usage: hold profile import <file>\n");
            return 5;
        }
        return profile_import_transcript(user_store, argv[1]);
    }
    fprintf(stderr, "usage: hold profile export <name> [--json]\n       hold profile import <file>\n");
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
           "Run a command that outlives your shell, then find it, watch it, and stop it\n"
           "safely later. No daemon, no config.\n\n"
           "USAGE\n"
           "  hold run -- <cmd> [args...]      start a command explicitly\n"
           "  hold <action> [target...]        act on a tracked command\n"
           "  hold <command> [args...]          direct ad-hoc start\n\n"
           "START\n"
           "  hold run -- <command>...         start it; prints a short run id\n"
           "  hold <command>...                 start it; prints a short run id\n"
           "  hold -f <command>...              start it and stream output\n"
           "  hold --console <command>...       start it with an attachable console\n"
           "  hold start <profile>             start a saved profile\n\n"
           "MANAGE\n"
           "  hold status [profile]             show tracked runs (optionally one profile)\n"
           "  hold logs   <target>              open a run's log viewer\n"
           "  hold profile save <id> as <name>  save a recorded run as a profile\n"
           "  hold profiles [-v]                list visible profiles\n"
           "  hold profile export <name>        print a typed-shell profile config\n"
           "  hold profile import <file>        import a typed-shell or JSON profile config\n"
           "  hold list   [profile]             show tracked runs (optionally one profile)\n"
           "  hold tail   <target>              follow a run's live output\n"
           "  hold console <target>             attach to a run's console\n"
           "  hold dump   <target>              print a run's log and exit\n"
           "  hold stop   <target>              graceful stop (TERM, then KILL)\n"
           "  hold kill   <target>              force kill now (KILL)\n"
           "  hold prune  [target|all]          clear past run data\n\n"
           "  target = run id, id prefix, or profile name\n\n"
           "MORE\n"
           "  hold help profiles           save commands as reusable profiles\n"
           "  hold help access             give another user scoped access\n"
           "  hold help targets            id, profile, and scope resolution\n"
           "  hold help system             root-managed runs and elevation\n"
           "  hold help scripting          exit codes, --print, --quiet, stdout\n"
           "  hold help console            attachable PTY consoles\n"
           "  hold <action> -h             help for one action\n\n"
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
        rc = hold_perform_start(inv, system_store, tail, console_mode, granted.argc, granted.argv, granted.binary_path, profile);
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
        if (strcmp(runid_sel, "00000000") != 0) {
            fprintf(stderr, "hold: error: start capability requires selector 00000000\n");
            return 5;
        }
        return hold_perform_profile_start(inv, system_store, tail, console_mode, hash, alias);
    }

    if (strcmp(runid_sel, "00000000") == 0) {
        fprintf(stderr, "hold: error: selector 00000000 is only valid for start\n");
        return 5;
    }

    if (strcmp(runid_sel, "ffffffff") == 0) {
        if (!hold_command_all_allowed(command)) {
            fprintf(stderr, "hold: error: selector ffffffff is not valid for %s\n", command);
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
