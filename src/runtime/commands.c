#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/runtime.h"
#include "sigmund/runtime_internal.h"
#include "sigmund/core.h"
#include "sigmund/platform.h"
#include "sigmund/store.h"
#include "sigmund/console.h"
#include "sigmund/access.h"

static int attach_console_record(const struct sigmund_invocation *inv,
                                 const struct sigmund_run_record *r,
                                 enum run_state st);
static int print_aliases_for_store(const char *scope, const struct sigmund_store *store, bool verbose);
static void profile_shell_quote(FILE *f, const char *s);
static int profile_export_transcript(const char *name, const struct sigmund_profile *recipe);
static int profile_export_json(const char *name, const struct sigmund_profile *recipe);
static int profile_shell_split(const char *line, char ***argv_out, int *argc_out);
static int profile_import_transcript(const struct sigmund_store *store, const char *path);
static int profile_import_json(const struct sigmund_store *store, const char *j);
static void profile_free_tokens(char **argv, int argc);

static int attach_console_record(const struct sigmund_invocation *inv,
                                 const struct sigmund_run_record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        sigmund_sig_note(inv, "sigmund: %s has exited - see 'sigmund dump %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        sigmund_sig_note(inv, "sigmund: %s has no console (start with --console)\n", r->id);
        return 0;
    }
    return sigmund_run_native_console(r->console_sock);
}

int sigmund_cmd_console_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              const struct sigmund_store *system_store,
                              const char *program,
                              const char *id_token) {
    struct sigmund_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = sigmund_resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        sigmund_sig_note(inv, "sigmund: nothing to console\n");
        return 0;
    }
    struct sigmund_resolved_target target = targets[0];
    if (target.needs_elevation) {
        rc = sigmund_elevate_with_sudo_targets(program, "console", NULL, &target, 1, false, false);
        free(targets);
        return rc;
    }
    struct sigmund_run_record r;
    char path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128] = {0};
    bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
    enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
    rc = attach_console_record(inv, &r, st);
    free(targets);
    return rc;
}

int sigmund_cmd_alias_action(const struct sigmund_invocation *inv,
                            const struct sigmund_store *user_store,
                            const struct sigmund_store *system_store,
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
    if (!sigmund_valid_alias(name)) {
        fprintf(stderr, "sigmund: error: invalid alias '%s'\n", name);
        return 5;
    }

    struct sigmund_resolved_target target;
    if (sigmund_resolve_target(inv, user_store, system_store, target_token, &target) != 0) {
        return 5;
    }
    if (target.scope == RESOLVE_NOT_FOUND) {
        return sigmund_report_not_found(target_token);
    }
    if (target.needs_elevation) {
        char scoped[8 + PROFILE_HASH_STR_LEN];
        if (sigmund_checked_snprintf(scoped, sizeof(scoped), "system:%s", target.id) != 0) {
            return 3;
        }
        char *canon[3] = {"alias", scoped, (char *)name};
        return sigmund_elevate_with_sudo_canonical(program, 3, canon);
    }
    if (target.store.kind == STORE_USER_LOCAL && inv->euid_root) {
        fprintf(stderr, "sigmund: error: create user-local aliases as that user\n");
        return 5;
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        if (sigmund_ensure_system_store(&target.store) != 0) {
            sigmund_die_errno("sigmund: failed to init system storage");
        }
    } else if (sigmund_ensure_user_store_for_current_user(&target.store) != 0) {
        sigmund_die_errno("sigmund: failed to init user storage");
    }

    struct sigmund_run_record r;
    char record_path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(target.store.record_dir, target.id, &r, record_path, sizeof(record_path)) != 0) {
        return 5;
    }
    char *j = NULL;
    if (sigmund_read_owned_file_no_symlink(record_path, &j) != 0) {
        return 5;
    }
    char **profile_argv = NULL;
    int profile_argc = 0;
    char binary_path[SIGMUND_PATH_MAX];
    char hash[PROFILE_HASH_STR_LEN];
    int rc = 0;
    if (sigmund_json_get_argv_alloc(j, &profile_argv, &profile_argc) != 0 ||
        sigmund_resolve_binary_path(profile_argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "sigmund: error: failed to derive profile from run %s\n", target.id);
        rc = 5;
        goto out;
    }
    char command[256];
    if (sigmund_format_argv_human(command, sizeof(command), profile_argc, profile_argv) != 0) {
        snprintf(command, sizeof(command), "%s", "?");
    }
    if (target.store.kind == STORE_SYSTEM_MANAGED) {
        sigmund_profile_hash_for_argv(binary_path, profile_argc, profile_argv, hash);
        if (sigmund_write_profile_atomic(&target.store, hash, binary_path, profile_argc, profile_argv) != 0) {
            sigmund_die_errno("sigmund: failed to write profile");
        }
        if (sigmund_alias_upsert_hash(&target.store, name, hash) != 0) {
            sigmund_die_errno("sigmund: failed to write alias");
        }
        if (verbose) {
            sigmund_sig_note(inv, "sigmund: pinned '%s' -> %s (hash %s)\n", name, command, hash);
        } else {
            sigmund_sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
        }
    } else {
        if (sigmund_alias_upsert_recipe(&target.store, name, binary_path, profile_argc, profile_argv) != 0) {
            sigmund_die_errno("sigmund: failed to write alias");
        }
        sigmund_sig_note(inv, "sigmund: pinned '%s' -> %s\n", name, command);
    }

out:
    sigmund_free_argv_alloc(profile_argv, profile_argc);
    free(j);
    return rc;
}

static int print_aliases_for_store(const char *scope, const struct sigmund_store *store, bool verbose) {
    struct sigmund_alias *entries = NULL;
    size_t count = 0;
    if (sigmund_load_aliases(store, &entries, &count) != 0) {
        fprintf(stderr, "sigmund: warning: failed to read %s aliases\n", scope);
        return 5;
    }
    for (size_t i = 0; i < count; i++) {
        char command[96];
        char hash_display[PROFILE_HASH_STR_LEN];
        if (entries[i].has_recipe) {
            if (sigmund_format_argv_human(command, sizeof(command), entries[i].argc, entries[i].argv) != 0) {
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
    sigmund_free_aliases(entries, count);
    return 0;
}

int sigmund_cmd_aliases_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              const struct sigmund_store *system_store,
                              bool verbose) {
    printf("%-12s %-6s %-40s %s\n", "NAME", "SCOPE", "COMMAND", "HASH");
    int rc = 0;
    if (inv->euid_root) {
        if (print_aliases_for_store("system", system_store, verbose) != 0) {
            rc = 5;
        }
        if (!inv->requested_system && inv->have_sudo_user) {
            struct sigmund_store sudo_user_store;
            if (sigmund_init_invoking_user_store(inv, &sudo_user_store) == 0 &&
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

static int profile_export_transcript(const char *name, const struct sigmund_profile *recipe) {
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

static int profile_export_json(const char *name, const struct sigmund_profile *recipe) {
    fputs("{\n  \"version\": 1,\n  \"name\": \"", stdout);
    sigmund_json_escape(stdout, name);
    fputs("\",\n  \"bin\": \"", stdout);
    sigmund_json_escape(stdout, recipe->binary_path);
    fputs("\",\n  \"args\": ", stdout);
    sigmund_write_json_argv(stdout, recipe->argc, recipe->argv);
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

static int profile_import_json(const struct sigmund_store *store, const char *j) {
    char name[ALIAS_MAX_LEN + 1];
    char binary_path[SIGMUND_PATH_MAX];
    char **argv = NULL;
    int argc = 0;
    int rc = 5;
    if (sigmund_json_get_str(j, "name", name, sizeof(name)) != 0 || !sigmund_valid_alias(name) ||
        sigmund_json_get_args_alloc(j, &argv, &argc) != 0 || argc <= 0) {
        fprintf(stderr, "sigmund: error: invalid profile JSON\n");
        goto out;
    }
    if (sigmund_json_get_str(j, "bin", binary_path, sizeof(binary_path)) != 0 &&
        sigmund_json_get_str(j, "binary_path", binary_path, sizeof(binary_path)) != 0 &&
        sigmund_resolve_binary_path(argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "sigmund: error: failed to resolve profile command '%s'\n", argv[0]);
        goto out;
    }
    if (binary_path[0] != '/') {
        fprintf(stderr, "sigmund: error: invalid profile binary path\n");
        goto out;
    }
    if (sigmund_alias_upsert_recipe(store, name, binary_path, argc, argv) != 0) {
        sigmund_die_errno("sigmund: failed to import profile");
    }
    rc = 0;
out:
    sigmund_free_argv_alloc(argv, argc);
    return rc;
}

static int profile_import_transcript(const struct sigmund_store *store, const char *path) {
    char *text = NULL;
    if (sigmund_read_small_file(path, &text) != 0) {
        fprintf(stderr, "sigmund: error: failed to read profile transcript '%s'\n", path);
        return 5;
    }
    const char *p = sigmund_skip_ws(text);
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
            fprintf(stderr, "sigmund: error: invalid profile transcript syntax\n");
            goto out;
        }
        if (ntokens == 0) {
            profile_free_tokens(tokens, ntokens);
            continue;
        }
        if (!strcmp(tokens[0], "profile") && ntokens == 2) {
            if (!sigmund_valid_alias(tokens[1])) {
                fprintf(stderr, "sigmund: error: invalid profile name '%s'\n", tokens[1]);
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
            fprintf(stderr, "sigmund: error: unsupported profile transcript command '%s'\n", tokens[0]);
            profile_free_tokens(tokens, ntokens);
            goto out;
        }
        profile_free_tokens(tokens, ntokens);
    }

    if (!saw_save || name[0] == '\0' || cmd_argc <= 0 || !cmd_argv) {
        fprintf(stderr, "sigmund: error: incomplete profile transcript\n");
        goto out;
    }
    char binary_path[SIGMUND_PATH_MAX];
    if (sigmund_resolve_binary_path(cmd_argv[0], binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "sigmund: error: failed to resolve profile command '%s'\n", cmd_argv[0]);
        goto out;
    }
    if (sigmund_alias_upsert_recipe(store, name, binary_path, cmd_argc, cmd_argv) != 0) {
        sigmund_die_errno("sigmund: failed to import profile");
    }
    rc = 0;

out:
    profile_free_tokens(cmd_argv, cmd_argc);
    free(text);
    return rc;
}

int sigmund_cmd_profile_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              int argc,
                              char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: sigmund profile export <name> [--json]\n       sigmund profile import <file>\n");
        return 5;
    }
    if (inv->euid_root) {
        fprintf(stderr, "sigmund: error: profile import/export is user-local; run it as the profile owner\n");
        return 5;
    }
    if (!strcmp(argv[0], "export")) {
        bool json = false;
        if (argc == 3 && !strcmp(argv[2], "--json")) {
            json = true;
        } else if (argc != 2) {
            fprintf(stderr, "usage: sigmund profile export <name> [--json]\n");
            return 5;
        }
        const char *name = argv[1];
        if (!sigmund_valid_alias(name)) {
            fprintf(stderr, "sigmund: error: invalid profile name '%s'\n", name);
            return 5;
        }
        struct sigmund_profile recipe;
        if (sigmund_alias_lookup_recipe(user_store, name, &recipe) != 0) {
            fprintf(stderr, "sigmund: error: profile '%s' not found\n", name);
            return 5;
        }
        int rc = json ? profile_export_json(name, &recipe) : profile_export_transcript(name, &recipe);
        sigmund_free_profile(&recipe);
        return rc;
    }
    if (!strcmp(argv[0], "import")) {
        if (argc != 2) {
            fprintf(stderr, "usage: sigmund profile import <file>\n");
            return 5;
        }
        return profile_import_transcript(user_store, argv[1]);
    }
    fprintf(stderr, "usage: sigmund profile export <name> [--json]\n       sigmund profile import <file>\n");
    return 5;
}

void sigmund_usage(void) {
    printf("sigmund %s - more than nohup, less than systemd\n\n"
           "Run a command that outlives your shell, then find it, watch it, and stop it\n"
           "safely later. No daemon, no config.\n\n"
           "USAGE\n"
           "  mund run -- <cmd> [args...]      start a command explicitly\n"
           "  mund <action> [target...]        act on a tracked command\n"
           "  sigmund <command> [args...]      compatibility raw start\n\n"
           "START\n"
           "  mund run -- <command>...         start it; prints a short run id\n"
           "  sigmund <command>...             start it; prints a short run id\n"
           "  sigmund -f <command>...          start it and stream output\n"
           "  sigmund --console <command>...   start it with an attachable console\n"
           "  sigmund start <alias>            start a pinned alias\n\n"
           "MANAGE\n"
           "  mund status [profile]          show tracked runs (optionally one profile)\n"
           "  mund logs   <target>            follow a run's live output\n"
           "  mund profile export <name>      print a typed-shell profile config\n"
           "  mund profile import <file>      import a typed-shell or JSON profile config\n"
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

int sigmund_cmd_elevated_capability_action(const struct sigmund_invocation *inv,
                                          const struct sigmund_store *system_store,
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
    if (!sigmund_valid_runid_selector(runid_sel) || !sigmund_valid_alias(alias) || !sigmund_valid_profile_hash(hash)) {
        return -1;
    }
    if (sigmund_verify_system_alias_cap(system_store, alias, hash) != 0) {
        fprintf(stderr, "sigmund: error: capability for '%s' is no longer valid\n", alias);
        return 3;
    }

    if (!strcmp(command, "start")) {
        if (strcmp(runid_sel, "00000000") != 0) {
            fprintf(stderr, "sigmund: error: start capability requires selector 00000000\n");
            return 5;
        }
        return sigmund_perform_profile_start(inv, system_store, tail, console_mode, hash, alias);
    }

    if (strcmp(runid_sel, "00000000") == 0) {
        fprintf(stderr, "sigmund: error: selector 00000000 is only valid for start\n");
        return 5;
    }

    if (strcmp(runid_sel, "ffffffff") == 0) {
        if (!sigmund_command_all_allowed(command)) {
            fprintf(stderr, "sigmund: error: selector ffffffff is not valid for %s\n", command);
            return 5;
        }
        struct alias_match_list matches;
        if (sigmund_collect_private_alias_matches(system_store, alias, command, &matches) != 0) {
            return 3;
        }
        int worst = 0;
        int acted = 0;
        char boot[128] = {0};
        bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
        for (size_t i = 0; i < matches.count; i++) {
            int rc = 0;
            if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
                bool already_done = false;
                rc = sigmund_do_signal_action(system_store,
                                      matches.items[i].id,
                                      sig,
                                      graceful,
                                      &already_done);
                if (rc == 0) {
                    sigmund_sig_note(inv,
                             "sigmund: %s %s\n",
                             already_done ? matches.items[i].id : (!strcmp(command, "kill") ? "killed" : "stopped"),
                             already_done ? "already exited" : matches.items[i].id);
                }
            } else if (!strcmp(command, "prune")) {
                bool removed = false;
                rc = sigmund_prune_one_run(system_store, matches.items[i].id, have_boot ? boot : NULL, true, &removed);
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
                sigmund_sig_note(inv, "sigmund: pruned %d past run%s for '%s'\n", acted, acted == 1 ? "" : "s", alias);
            } else {
                sigmund_sig_note(inv, "sigmund: nothing to prune\n");
            }
        } else if (acted == 0 && worst == 0) {
            sigmund_sig_note(inv, "sigmund: nothing to %s\n", command);
        }
        sigmund_free_alias_match_list(&matches);
        return worst;
    }

    if (sigmund_ensure_run_recorded_under_alias(system_store, runid_sel, alias) != 0) {
        fprintf(stderr, "sigmund: error: run %s is not recorded under alias '%s'\n", runid_sel, alias);
        return 3;
    }

    struct sigmund_run_record selected_record;
    char selected_path[SIGMUND_PATH_MAX];
    if (sigmund_load_record_by_id(system_store->record_dir, runid_sel, &selected_record, selected_path, sizeof(selected_path)) != 0) {
        return 5;
    }
    char selected_boot[128] = {0};
    bool have_selected_boot = sigmund_current_boot_id(selected_boot, sizeof(selected_boot));
    enum run_state selected_state = sigmund_eval_state(&selected_record, have_selected_boot ? selected_boot : NULL);
    if (!strcmp(command, "console")) {
        return attach_console_record(inv, &selected_record, selected_state);
    }
    if (!sigmund_record_matches_alias_intent(command, &selected_record, selected_state)) {
        sigmund_sig_note(inv, "sigmund: nothing to %s\n", command);
        return 0;
    }

    if (!strcmp(command, "stop") || !strcmp(command, "kill")) {
        bool already_done = false;
        int rc = sigmund_do_signal_action(system_store, runid_sel, sig, graceful, &already_done);
        if (rc == 0) {
            if (already_done) {
                sigmund_sig_note(inv, "sigmund: %s already exited\n", runid_sel);
            } else {
                sigmund_sig_note(inv, "sigmund: %s %s\n", !strcmp(command, "kill") ? "killed" : "stopped", runid_sel);
            }
        }
        return rc;
    }
    if (!strcmp(command, "prune")) {
        char boot[128] = {0};
        bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
        bool removed = false;
        int rc = sigmund_prune_one_run(system_store, runid_sel, have_boot ? boot : NULL, true, &removed);
        if (rc == 0) {
            sigmund_sig_note(inv, removed ? "sigmund: pruned 1 past run for '%s'\n" : "sigmund: nothing to prune\n", alias);
        }
        return rc;
    }
    if (!strcmp(command, "tail") || !strcmp(command, "dump")) {
        struct sigmund_run_record r;
        char path[SIGMUND_PATH_MAX];
        if (sigmund_load_record_by_id(system_store->record_dir, runid_sel, &r, path, sizeof(path)) != 0 || !r.has_log) {
            return 5;
        }
        if (!strcmp(command, "tail")) {
            char boot[128] = {0};
            bool have_boot = sigmund_current_boot_id(boot, sizeof(boot));
            enum run_state st = sigmund_eval_state(&r, have_boot ? boot : NULL);
            return sigmund_tail_log_until_exit(&r, st == STATE_RUNNING, st == STATE_RUNNING);
        }
        int fd = open(r.log_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            sigmund_die_errno("sigmund: failed to open log for dump");
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
                sigmund_die_errno("sigmund: failed while dumping log");
            }
            if (sigmund_write_all(STDOUT_FILENO, buf, (size_t)nr) != 0) {
                close(fd);
                sigmund_die_errno("sigmund: failed writing dumped output");
            }
        }
        close(fd);
        return 0;
    }

    return -1;
}
