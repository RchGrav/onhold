#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"
#include "hold/access.h"
#include "hold/console.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/core.h"

static const char *program_basename(const char *path);
static bool invoked_as_hold(const char *path);
static int shell_split_line(const char *line, char ***argv_out, int *argc_out);
static void shell_free_argv(char **argv, int argc);
static int shell_exec_command(const char *program, int argc, char **argv);
static int shell_map_slash_view(char **argv, int argc, char ***mapped_out, int *mapped_argc_out);
static int shell_map_profile_context(const char *name, char **argv, int argc, char ***mapped_out, int *mapped_argc_out);
static int hold_run_captive_shell(const char *program);

static const char *program_basename(const char *path) {
    if (!path || !*path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool invoked_as_hold(const char *path) {
    const char *base = program_basename(path);
    return !strcmp(base, "hold");
}

static void shell_free_argv(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int shell_push_char(char **buf, size_t *len, size_t *cap, char c) {
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

static int shell_push_token(char ***argv, int *argc, int *cap, char *token) {
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

static int shell_split_line(const char *line, char ***argv_out, int *argc_out) {
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
                    shell_free_argv(argv, argc);
                    errno = EINVAL;
                    return -1;
                }
                c = (unsigned char)*p;
            }
            if (shell_push_char(&buf, &len, &cap, (char)c) != 0) {
                free(buf);
                shell_free_argv(argv, argc);
                return -1;
            }
            p++;
        }
        if (in_single || in_double || shell_push_char(&buf, &len, &cap, '\0') != 0) {
            free(buf);
            shell_free_argv(argv, argc);
            errno = EINVAL;
            return -1;
        }
        if (shell_push_token(&argv, &argc, &argv_cap, buf) != 0) {
            free(buf);
            shell_free_argv(argv, argc);
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

static char *shell_strdup(const char *s) {
    char *copy = strdup(s);
    if (!copy) {
        hold_die_errno("hold: shell allocation failed");
    }
    return copy;
}

static int shell_map_slash_view(char **argv, int argc, char ***mapped_out, int *mapped_argc_out) {
    *mapped_out = NULL;
    *mapped_argc_out = 0;
    if (argc <= 0 || argv[0][0] != '/') {
        return 0;
    }
    const char *view = argv[0] + 1;
    const char *mapped_view = NULL;
    if (!strcmp(view, "profiles") || !strcmp(view, "profile")) {
        mapped_view = "profiles";
    } else if (!strcmp(view, "runs") || !strcmp(view, "run")) {
        mapped_view = "runs";
    } else if (!strcmp(view, "running") || !strcmp(view, "active")) {
        mapped_view = "running";
    } else if (!strcmp(view, "dormant") || !strcmp(view, "inactive")) {
        mapped_view = "dormant";
    } else if (!strcmp(view, "failed")) {
        mapped_view = "failed";
    } else if (!strcmp(view, "stale")) {
        mapped_view = "stale";
    } else if (!strcmp(view, "time") || !strcmp(view, "uptime")) {
        mapped_view = view;
    } else {
        fprintf(stderr, "hold: unknown view '/%s'\n", view);
        return 5;
    }
    int out_argc = !strcmp(mapped_view, "profiles") ? argc : argc + 1;
    char **out = calloc((size_t)out_argc, sizeof(*out));
    if (!out) {
        return 3;
    }
    int o = 0;
    if (!strcmp(mapped_view, "profiles")) {
        out[o++] = shell_strdup("profiles");
    } else {
        out[o++] = shell_strdup("show");
        out[o++] = shell_strdup(mapped_view);
    }
    for (int i = 1; i < argc; i++) {
        out[o++] = shell_strdup(argv[i]);
    }
    *mapped_out = out;
    *mapped_argc_out = out_argc;
    return 1;
}

static int shell_map_profile_context(const char *name, char **argv, int argc, char ***mapped_out, int *mapped_argc_out) {
    *mapped_out = NULL;
    *mapped_argc_out = 0;
    if (!name || !*name || argc <= 0) {
        return 0;
    }
    int out_argc = argc + 2;
    char **out = calloc((size_t)out_argc, sizeof(*out));
    if (!out) {
        return 3;
    }
    int o = 0;
    out[o++] = shell_strdup("profile");
    out[o++] = shell_strdup(name);
    for (int i = 0; i < argc; i++) {
        out[o++] = shell_strdup(argv[i]);
    }
    *mapped_out = out;
    *mapped_argc_out = out_argc;
    return 1;
}

static int shell_exec_command(const char *program, int argc, char **argv) {
    char **child_argv = calloc((size_t)argc + 2, sizeof(*child_argv));
    if (!child_argv) {
        return 3;
    }
    child_argv[0] = (char *)program;
    for (int i = 0; i < argc; i++) {
        child_argv[i + 1] = argv[i];
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(child_argv);
        hold_die_errno("hold: shell fork failed");
    }
    if (pid == 0) {
        execv(program, child_argv);
        execvp(program, child_argv);
        fprintf(stderr, "hold: failed to execute %s: %s\n", program, strerror(errno));
        _exit(127);
    }
    free(child_argv);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            hold_die_errno("hold: shell wait failed");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static int hold_run_captive_shell(const char *program) {
    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    char *line = NULL;
    size_t cap = 0;
    int last_rc = 0;
    char profile_ctx[ALIAS_MAX_LEN + 1] = {0};
    if (interactive) {
        printf("hold shell — type help, /profiles, profile <name>, back, or exit\n");
    }
    while (1) {
        if (interactive) {
            if (profile_ctx[0]) {
                printf("hold(profile:%s)> ", profile_ctx);
            } else {
                printf("hold> ");
            }
            fflush(stdout);
        }
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            break;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        char **argv = NULL;
        int argc = 0;
        if (shell_split_line(line, &argv, &argc) != 0) {
            fprintf(stderr, "hold: invalid shell syntax\n");
            last_rc = 5;
            continue;
        }
        if (argc == 0) {
            shell_free_argv(argv, argc);
            continue;
        }
        if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "quit")) {
            shell_free_argv(argv, argc);
            break;
        }
        if (!strcmp(argv[0], "back")) {
            if (argc == 1) {
                profile_ctx[0] = '\0';
                last_rc = 0;
            } else {
                fprintf(stderr, "hold: usage: back\n");
                last_rc = 5;
            }
            shell_free_argv(argv, argc);
            continue;
        }
        if (!strcmp(argv[0], "cd")) {
            if (argc != 2 || chdir(argv[1]) != 0) {
                fprintf(stderr, "hold: cd failed%s%s\n", argc == 2 ? ": " : "", argc == 2 ? strerror(errno) : "usage: cd <dir>");
                last_rc = 5;
            } else {
                last_rc = 0;
            }
            shell_free_argv(argv, argc);
            continue;
        }
        if (!strcmp(argv[0], "profile") && argc == 2) {
            if (!hold_valid_alias(argv[1])) {
                fprintf(stderr, "hold: invalid profile name '%s'\n", argv[1]);
                last_rc = 5;
            } else {
                snprintf(profile_ctx, sizeof(profile_ctx), "%s", argv[1]);
                last_rc = 0;
            }
            shell_free_argv(argv, argc);
            continue;
        }
        char **mapped = NULL;
        int mapped_argc = 0;
        int mapped_rc = 0;
        if (profile_ctx[0] && argv[0][0] != '/') {
            mapped_rc = shell_map_profile_context(profile_ctx, argv, argc, &mapped, &mapped_argc);
        } else {
            mapped_rc = shell_map_slash_view(argv, argc, &mapped, &mapped_argc);
        }
        if (mapped_rc < 0 || mapped_rc > 1) {
            last_rc = mapped_rc;
            shell_free_argv(argv, argc);
            continue;
        }
        if (mapped_rc == 1) {
            last_rc = shell_exec_command(program, mapped_argc, mapped);
            shell_free_argv(mapped, mapped_argc);
        } else {
            last_rc = shell_exec_command(program, argc, argv);
        }
        shell_free_argv(argv, argc);
    }
    free(line);
    return last_rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        if (invoked_as_hold(argv[0]) && isatty(STDIN_FILENO)) {
            return hold_run_captive_shell(argv[0]);
        }
        hold_usage();
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
        hold_usage();
        return 5;
    }

    bool owned = !force_raw && !tail && hold_cli_command_is_parser_owned(argv[argi]);
    const char *command = owned ? argv[argi++] : NULL;
    int cmd_argc = 0;
    char **cmd_argv = NULL;
    bool saw_owned_delimiter = false;

    if (owned) {
        cmd_argv = calloc((size_t)(argc - argi + 1), sizeof(char *));
        if (!cmd_argv) {
            return 3;
        }
        bool literal_owned_arg = false;
        for (int i = argi; i < argc; i++) {
            if (!literal_owned_arg && !strcmp(argv[i], "--")) {
                literal_owned_arg = true;
                saw_owned_delimiter = true;
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
            if (!literal_owned_arg && hold_cli_command_allows_all(command) && !strcmp(argv[i], "--all")) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "stop") || !strcmp(command, "kill")) &&
                !strcmp(argv[i], "--print")) {
                print_cmd = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                (!strcmp(argv[i], "--tail") || !strcmp(argv[i], "-f"))) {
                tail = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) && !strcmp(argv[i], "--console")) {
                console_mode = true;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "list") || !strcmp(command, "status")) &&
                (!strcmp(argv[i], "--iso") || !strcmp(argv[i], "-l"))) {
                list_iso = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--multi")) {
                multi = true;
                multi_count = 1;
                if (i + 1 < argc) {
                    int parsed = 0;
                    if (hold_parse_positive_count(argv[i + 1], &parsed)) {
                        multi_count = parsed;
                        i++;
                    } else if (argv[i + 1][0] != '-') {
                        fprintf(stderr, "hold: error: invalid --multi count '%s'\n", argv[i + 1]);
                        free(cmd_argv);
                        return 5;
                    }
                }
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && strncmp(argv[i], "--multi=", 8) == 0) {
                multi = true;
                if (!hold_parse_positive_count(argv[i] + 8, &multi_count)) {
                    fprintf(stderr, "hold: error: invalid --multi count '%s'\n", argv[i] + 8);
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
        puts(HOLD_VERSION);
        return 0;
    }
    if (!owned && !force_raw && !tail && (!strcmp(argv[argi], "--help") || !strcmp(argv[argi], "-h"))) {
        hold_usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: hold help [topic]\n");
            rc = 5;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = hold_show_help(NULL);
        } else {
            rc = hold_show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && hold_cli_command_is_retired(command)) {
        if (!strcmp(command, "alias")) {
            fprintf(stderr, "hold: error: alias command was removed; use `hold profile save <id> as <name>`\n");
        } else {
            fprintf(stderr, "hold: error: aliases command was removed; use `hold profiles`\n");
        }
        free(cmd_argv);
        return 5;
    }
    if (owned && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = hold_show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (owned && !strcmp(command, "run") && !saw_owned_delimiter) {
        fprintf(stderr, "usage: hold run [--tail|-f] [--console] -- <cmd> [args...]\n");
        free(cmd_argv);
        return 5;
    }
    if (console_mode && owned && strcmp(command, "start") != 0 && strcmp(command, "run") != 0 && strcmp(command, "profile") != 0) {
        fprintf(stderr, "hold: error: --console applies only to starts\n");
        free(cmd_argv);
        return 5;
    }
    if (owned) {
        int arity_rc = hold_validate_owned_command_arity(command, cmd_argc);
        if (arity_rc != 0) {
            free(cmd_argv);
            return arity_rc;
        }
    }

    struct hold_invocation inv;
    if (hold_detect_invocation(&inv, requested_system, elevated) != 0) {
        hold_die_errno("hold: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "hold: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    if (owned && !strcmp(command, "logs")) {
        command = cmd_argc == 1 ? "tail" : "view";
    }
    if (owned && !strcmp(command, "inspect")) command = "dump";
    if (owned && !strcmp(command, "status")) command = "list";
    if (owned && !strcmp(command, "clean")) command = "prune";

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && owned && !strcmp(command, "start") && cmd_argc == 1) {
        struct hold_store pre_system_store;
        if (hold_init_system_store(&pre_system_store) == 0) {
            const char *atom = NULL;
            enum id_token_scope start_scope = hold_parse_id_token(cmd_argv[0], &atom);
            if ((start_scope == ID_TOKEN_PLAIN || start_scope == ID_TOKEN_SYSTEM) && atom &&
                (hold_valid_profile_hash(atom) || hold_valid_alias(atom))) {
                char hash[PROFILE_HASH_STR_LEN];
                if (hold_resolve_public_profile_token(&pre_system_store, atom, hash) == 1) {
                    int rc = 0;
                    int starts = multi ? multi_count : 1;
                    for (int i = 0; i < starts; i++) {
                        rc = hold_elevate_start_token(argv[0],
                                                 tail,
                                                 console_mode,
                                                 hold_valid_alias(atom) ? atom : hash,
                                                 hold_valid_alias(atom) ? hash : NULL,
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
        if (owned && hold_maybe_elevate_requested_system_targets(argv[0], command, cmd_argc, cmd_argv, all, &canonical_rc)) {
            free(cmd_argv);
            return canonical_rc;
        }
        int rc = hold_elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
        if (owned) {
            free(cmd_argv);
        }
        return rc;
    }

    struct hold_store user_store;
    struct hold_store system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (hold_init_system_store(&system_store) != 0) {
        hold_die_errno("hold: failed to resolve system storage");
    }

    if (!inv.euid_root || is_list || (owned && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                                               !strcmp(command, "tail") || !strcmp(command, "dump") ||
                                               !strcmp(command, "view") || !strcmp(command, "prune") ||
                                               !strcmp(command, "console") || !strcmp(command, "profile") ||
                                               !strcmp(command, "show")))) {
        if (!inv.euid_root) {
            if (hold_ensure_user_store_for_current_user(&user_store) != 0) {
                hold_die_errno("hold: failed to init user storage");
            }
        }
    }

    if (inv.elevated && inv.euid_root && owned && cmd_argc == 3 &&
        (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
         !strcmp(command, "tail") || !strcmp(command, "dump") || !strcmp(command, "prune") ||
         !strcmp(command, "console"))) {
        int sig = !strcmp(command, "kill") ? SIGKILL : SIGTERM;
        bool graceful = !strcmp(command, "stop");
        int rc = hold_cmd_elevated_capability_action(&inv, &system_store, command, tail, console_mode, sig, graceful, cmd_argc, cmd_argv);
        if (rc >= 0) {
            free(cmd_argv);
            return rc;
        }
    }

    if (owned && !strcmp(command, "run")) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        int rc = hold_perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
        free(cmd_argv);
        return rc;
    }

    if (owned && !strcmp(command, "shell")) {
        free(cmd_argv);
        return hold_run_captive_shell(argv[0]);
    }

    if (!owned) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        return hold_perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
    }

    if (!strcmp(command, "doctor")) {
        printf("hold doctor\n");
        printf("version: %s\n", HOLD_VERSION);
        printf("user_store: %s\n", user_store.base[0] ? user_store.base : "(not initialized)");
        printf("system_store: %s\n", system_store.base);
        free(cmd_argv);
        return 0;
    }

    if (!strcmp(command, "show")) {
        const char *view = cmd_argv[0];
        const char *filter = cmd_argc > 1 ? cmd_argv[1] : NULL;
        int rc = 0;
        if (!strcmp(view, "profiles")) {
            rc = hold_cmd_aliases_action(&inv, &user_store, &system_store, false);
        } else if (!strcmp(view, "runs") || !strcmp(view, "running") || !strcmp(view, "active") ||
                   !strcmp(view, "dormant") || !strcmp(view, "inactive") || !strcmp(view, "failed") ||
                   !strcmp(view, "stale") || !strcmp(view, "time") || !strcmp(view, "uptime")) {
            if (filter && !hold_valid_alias(filter)) {
                fprintf(stderr, "hold: error: invalid profile '%s'\n", filter);
                free(cmd_argv);
                return 5;
            }
            rc = inv.euid_root ? hold_cmd_list_system(&system_store, filter, list_iso)
                               : hold_cmd_list_normal(&user_store, &system_store, filter, list_iso);
        } else {
            fprintf(stderr, "usage: hold show <runs|profiles|running|dormant|failed|stale> [name]\n");
            rc = 5;
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "profile")) {
        const char *sub = cmd_argv[0];
        if (!strcmp(sub, "list") || !strcmp(sub, "ls")) {
            bool verbose = cmd_argc == 2 && (!strcmp(cmd_argv[1], "-v") || !strcmp(cmd_argv[1], "--verbose"));
            if (cmd_argc > 2 || (cmd_argc == 2 && !verbose)) {
                fprintf(stderr, "usage: hold profile list [-v]\n");
                free(cmd_argv);
                return 5;
            }
            int rc = hold_cmd_aliases_action(&inv, &user_store, &system_store, verbose);
            free(cmd_argv);
            return rc;
        }

        if (!strcmp(sub, "save")) {
            bool verbose = false;
            if (cmd_argc == 5 && (!strcmp(cmd_argv[4], "-v") || !strcmp(cmd_argv[4], "--verbose"))) {
                verbose = true;
            } else if (cmd_argc != 4) {
                fprintf(stderr, "usage: hold profile save <id> as <name> [-v]\n");
                free(cmd_argv);
                return 5;
            }
            if (strcmp(cmd_argv[2], "as")) {
                fprintf(stderr, "usage: hold profile save <id> as <name> [-v]\n");
                free(cmd_argv);
                return 5;
            }
            char *save_argv[3];
            save_argv[0] = cmd_argv[1];
            save_argv[1] = cmd_argv[3];
            save_argv[2] = verbose ? cmd_argv[4] : NULL;
            int rc = hold_cmd_alias_action(&inv, &user_store, &system_store, argv[0], verbose ? 3 : 2, save_argv);
            free(cmd_argv);
            return rc;
        }
        bool sub_is_profile_name = hold_valid_alias(sub) &&
            strcmp(sub, "list") && strcmp(sub, "ls") && strcmp(sub, "run") &&
            strcmp(sub, "start") && strcmp(sub, "show") && strcmp(sub, "save") && strcmp(sub, "export") &&
            strcmp(sub, "import");
        if (sub_is_profile_name) {
            const char *name = sub;
            if (cmd_argc < 2) {
                fprintf(stderr, "usage: hold profile <name> <show|start|run|set|export> [args...]\n");
                free(cmd_argv);
                return 5;
            }
            const char *op = cmd_argv[1];
            if (!strcmp(op, "create")) {
                if (cmd_argc < 3) {
                    fprintf(stderr, "usage: hold profile <name> create -- <cmd> [args...]\n");
                    free(cmd_argv);
                    return 5;
                }
                int rc = hold_cmd_profile_create_command(&inv, &user_store, name, cmd_argc - 2, cmd_argv + 2);
                free(cmd_argv);
                return rc;
            }
            if (!strcmp(op, "set")) {
                if (cmd_argc < 4 || strcmp(cmd_argv[2], "command")) {
                    fprintf(stderr, "usage: hold profile <name> set command -- <cmd> [args...]\n");
                    free(cmd_argv);
                    return 5;
                }
                int rc = hold_cmd_profile_set_command(&inv, &user_store, name, cmd_argc - 3, cmd_argv + 3);
                free(cmd_argv);
                return rc;
            }
            if (!strcmp(op, "export")) {
                char *export_argv[3];
                int export_argc = 2;
                export_argv[0] = "export";
                export_argv[1] = (char *)name;
                if (cmd_argc == 3 && (!strcmp(cmd_argv[2], "--json") || !strcmp(cmd_argv[2], "--format=json"))) {
                    export_argv[2] = "--json";
                    export_argc = 3;
                } else if (cmd_argc == 4 && !strcmp(cmd_argv[2], "--format") && !strcmp(cmd_argv[3], "json")) {
                    export_argv[2] = "--json";
                    export_argc = 3;
                } else if (cmd_argc == 4 && !strcmp(cmd_argv[2], "--format") && !strcmp(cmd_argv[3], "cli")) {
                    export_argc = 2;
                } else if (cmd_argc == 3 && !strcmp(cmd_argv[2], "--format=cli")) {
                    export_argc = 2;
                } else if (cmd_argc != 2) {
                    fprintf(stderr, "usage: hold profile <name> export [--format cli|json]\n");
                    free(cmd_argv);
                    return 5;
                }
                int rc = hold_cmd_profile_action(&inv, &user_store, export_argc, export_argv);
                free(cmd_argv);
                return rc;
            }
            if (!strcmp(op, "show") && cmd_argc == 2) {
                char *show_argv[2];
                show_argv[0] = "export";
                show_argv[1] = (char *)name;
                int rc = hold_cmd_profile_action(&inv, &user_store, 2, show_argv);
                free(cmd_argv);
                return rc;
            }
            if (!strcmp(op, "delete") && cmd_argc == 2) {
                int rc = hold_cmd_profile_delete(&inv, &user_store, name);
                free(cmd_argv);
                return rc;
            }
            if (!strcmp(op, "rename")) {
                if (cmd_argc != 3) {
                    fprintf(stderr, "usage: hold profile <name> rename <new-name>\n");
                    free(cmd_argv);
                    return 5;
                }
                int rc = hold_cmd_profile_rename(&inv, &user_store, name, cmd_argv[2]);
                free(cmd_argv);
                return rc;
            }
            if ((!strcmp(op, "run") || !strcmp(op, "start")) && cmd_argc >= 2) {
                char **rewritten = calloc((size_t)cmd_argc, sizeof(*rewritten));
                if (!rewritten) {
                    free(cmd_argv);
                    return 3;
                }
                rewritten[0] = (char *)op;
                rewritten[1] = (char *)name;
                for (int i = 2; i < cmd_argc; i++) {
                    rewritten[i] = cmd_argv[i];
                }
                free(cmd_argv);
                cmd_argv = rewritten;
                sub = cmd_argv[0];
            } else {
                fprintf(stderr, "usage: hold profile <name> <show|start|run|set|export> [args...]\n");
                free(cmd_argv);
                return 5;
            }
        }
        if (!strcmp(sub, "run") || !strcmp(sub, "start")) {
            if (cmd_argc < 2) {
                fprintf(stderr, "usage: hold profile run <name> [--multi [N]] [--tail|-f] [--console]\n");
                free(cmd_argv);
                return 5;
            }
            const char *name = cmd_argv[1];
            if (!hold_valid_alias(name)) {
                fprintf(stderr, "hold: error: invalid profile '%s'\n", name);
                free(cmd_argv);
                return 5;
            }
            bool p_tail = tail, p_console = console_mode, p_multi = false;
            int p_multi_count = 1;
            for (int i = 2; i < cmd_argc; i++) {
                if (!strcmp(cmd_argv[i], "--tail") || !strcmp(cmd_argv[i], "-f")) {
                    p_tail = true;
                } else if (!strcmp(cmd_argv[i], "--console")) {
                    p_console = true;
                } else if (!strcmp(cmd_argv[i], "--multi")) {
                    p_multi = true;
                    p_multi_count = 1;
                    if (i + 1 < cmd_argc) {
                        int parsed = 0;
                        if (hold_parse_positive_count(cmd_argv[i + 1], &parsed)) {
                            p_multi_count = parsed;
                            i++;
                        } else if (cmd_argv[i + 1][0] != '-') {
                            fprintf(stderr, "hold: error: invalid --multi count '%s'\n", cmd_argv[i + 1]);
                            free(cmd_argv);
                            return 5;
                        }
                    }
                } else if (strncmp(cmd_argv[i], "--multi=", 8) == 0) {
                    p_multi = true;
                    if (!hold_parse_positive_count(cmd_argv[i] + 8, &p_multi_count)) {
                        fprintf(stderr, "hold: error: invalid --multi count '%s'\n", cmd_argv[i] + 8);
                        free(cmd_argv);
                        return 5;
                    }
                } else {
                    fprintf(stderr, "usage: hold profile run <name> [--multi [N]] [--tail|-f] [--console]\n");
                    free(cmd_argv);
                    return 5;
                }
            }
            char *start_argv[1];
            start_argv[0] = (char *)name;
            struct hold_store start_store;
            if (hold_ensure_start_store_for_command(&inv, requested_system, true, "start", 1, start_argv, &start_store) != 0) {
                hold_die_errno("hold: failed to init start storage");
            }
            int rc = hold_cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, p_tail, p_console, p_multi, p_multi_count, 1, start_argv);
            free(cmd_argv);
            return rc;
        }
        if (!strcmp(sub, "show")) {
            if (cmd_argc != 2 || !hold_valid_alias(cmd_argv[1])) {
                fprintf(stderr, "usage: hold profile show <name>\n");
                free(cmd_argv);
                return 5;
            }
            int rc = hold_cmd_aliases_action(&inv, &user_store, &system_store, true);
            free(cmd_argv);
            return rc;
        }
        if (!strcmp(sub, "export") || !strcmp(sub, "import")) {
            int rc = hold_cmd_profile_action(&inv, &user_store, cmd_argc, cmd_argv);
            free(cmd_argv);
            return rc;
        }
        fprintf(stderr, "usage: hold profile <list|run|start|save|show|export|import|<name> set> [args...]\n");
        free(cmd_argv);
        return 5;
    }

    if (!strcmp(command, "start")) {
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, true, command, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, true, command, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        int rc = hold_cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, multi, multi_count, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: hold list [profile]\n");
            free(cmd_argv);
            return 5;
        }
        const char *alias_filter = cmd_argc == 1 ? cmd_argv[0] : NULL;
        if (alias_filter && !hold_valid_alias(alias_filter)) {
            fprintf(stderr, "hold: error: invalid profile '%s'\n", alias_filter);
            free(cmd_argv);
            return 5;
        }
        if (inv.euid_root) {
            rc = hold_cmd_list_system(&system_store, alias_filter, list_iso);
        } else {
            rc = hold_cmd_list_normal(&user_store, &system_store, alias_filter, list_iso);
        }
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "tail")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: hold tail <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_tail_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: hold dump <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_dump_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "view")) {
        int rc = hold_cmd_view_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "console")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: hold console <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_console_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        int rc = hold_cmd_prune_action(&inv, &user_store, &system_store, argv[0], target, all);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "profiles")) {
        bool profiles_verbose = false;
        if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "-v") || !strcmp(cmd_argv[0], "--verbose"))) {
            profiles_verbose = true;
        } else if (cmd_argc != 0) {
            fprintf(stderr, "usage: hold profiles [-v]\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_aliases_action(&inv, &user_store, &system_store, profiles_verbose);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "grant")) {
        if (hold_ensure_system_store(&system_store) != 0) {
            hold_die_errno("hold: failed to init system storage");
        }
        int rc = hold_cmd_grant_revoke_action(&inv, &system_store, argv[0], true, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "revoke")) {
        if (hold_ensure_system_store(&system_store) != 0) {
            hold_die_errno("hold: failed to init system storage");
        }
        int rc = hold_cmd_grant_revoke_action(&inv, &system_store, argv[0], false, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = hold_cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", cmd_argc, cmd_argv, SIGTERM, true, all, print_cmd);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = hold_cmd_signal_action(&inv, &user_store, &system_store, argv[0], "kill", cmd_argc, cmd_argv, SIGKILL, false, all, print_cmd);
        free(cmd_argv);
        return rc;
    }

    free(cmd_argv);
    hold_usage();
    return 1;
}
