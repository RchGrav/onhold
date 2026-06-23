#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/cli.h"
#include "sigmund/runtime.h"
#include "sigmund/access.h"
#include "sigmund/console.h"
#include "sigmund/store.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

static const char *program_basename(const char *path);
static bool invoked_as_mund(const char *path);
static int shell_split_line(const char *line, char ***argv_out, int *argc_out);
static void shell_free_argv(char **argv, int argc);
static int shell_exec_command(const char *program, int argc, char **argv);
static int shell_map_slash_view(char **argv, int argc, char ***mapped_out, int *mapped_argc_out);
static int sigmund_run_captive_shell(const char *program);

static const char *program_basename(const char *path) {
    if (!path || !*path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool invoked_as_mund(const char *path) {
    const char *base = program_basename(path);
    return !strcmp(base, "mund");
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
        sigmund_die_errno("sigmund: shell allocation failed");
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
        fprintf(stderr, "mund: unknown view '/%s'\n", view);
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
        sigmund_die_errno("sigmund: shell fork failed");
    }
    if (pid == 0) {
        execv(program, child_argv);
        execvp(program, child_argv);
        fprintf(stderr, "mund: failed to execute %s: %s\n", program, strerror(errno));
        _exit(127);
    }
    free(child_argv);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            sigmund_die_errno("sigmund: shell wait failed");
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

static int sigmund_run_captive_shell(const char *program) {
    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    char *line = NULL;
    size_t cap = 0;
    int last_rc = 0;
    if (interactive) {
        printf("mund shell — type help, /profiles, /runs, or exit\n");
    }
    while (1) {
        if (interactive) {
            printf("mund> ");
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
            fprintf(stderr, "mund: invalid shell syntax\n");
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
        if (!strcmp(argv[0], "cd")) {
            if (argc != 2 || chdir(argv[1]) != 0) {
                fprintf(stderr, "mund: cd failed%s%s\n", argc == 2 ? ": " : "", argc == 2 ? strerror(errno) : "usage: cd <dir>");
                last_rc = 5;
            } else {
                last_rc = 0;
            }
            shell_free_argv(argv, argc);
            continue;
        }
        char **mapped = NULL;
        int mapped_argc = 0;
        int mapped_rc = shell_map_slash_view(argv, argc, &mapped, &mapped_argc);
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
        if (invoked_as_mund(argv[0]) && isatty(STDIN_FILENO)) {
            return sigmund_run_captive_shell(argv[0]);
        }
        sigmund_usage();
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
        sigmund_usage();
        return 5;
    }

    bool owned = !force_raw && !tail && sigmund_is_sigmund_owned_command(argv[argi]);
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
            if (!literal_owned_arg && sigmund_cli_command_allows_all(command) && !strcmp(argv[i], "--all")) {
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
                    if (sigmund_parse_positive_count(argv[i + 1], &parsed)) {
                        multi_count = parsed;
                        i++;
                    } else if (argv[i + 1][0] != '-') {
                        fprintf(stderr, "sigmund: error: invalid --multi count '%s'\n", argv[i + 1]);
                        free(cmd_argv);
                        return 5;
                    }
                }
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "start") && strncmp(argv[i], "--multi=", 8) == 0) {
                multi = true;
                if (!sigmund_parse_positive_count(argv[i] + 8, &multi_count)) {
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
        sigmund_usage();
        return 0;
    }
    if (owned && !strcmp(command, "help")) {
        int rc = 0;
        if (cmd_argc > 1) {
            fprintf(stderr, "usage: sigmund help [topic]\n");
            rc = 5;
        } else if (cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
            rc = sigmund_show_help(NULL);
        } else {
            rc = sigmund_show_help(cmd_argc == 1 ? cmd_argv[0] : NULL);
        }
        free(cmd_argv);
        return rc;
    }
    if (owned && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = sigmund_show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (console_mode && owned && strcmp(command, "start") != 0 && strcmp(command, "run") != 0 && strcmp(command, "profile") != 0) {
        fprintf(stderr, "sigmund: error: --console applies only to starts\n");
        free(cmd_argv);
        return 5;
    }
    if (owned) {
        int arity_rc = sigmund_validate_owned_command_arity(command, cmd_argc);
        if (arity_rc != 0) {
            free(cmd_argv);
            return arity_rc;
        }
    }

    struct sigmund_invocation inv;
    if (sigmund_detect_invocation(&inv, requested_system, elevated) != 0) {
        sigmund_die_errno("sigmund: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "sigmund: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    if (owned && !strcmp(command, "logs")) command = "tail";
    if (owned && !strcmp(command, "inspect")) command = "dump";
    if (owned && !strcmp(command, "status")) command = "list";
    if (owned && !strcmp(command, "profiles")) command = "aliases";
    if (owned && !strcmp(command, "clean")) command = "prune";

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && owned && !strcmp(command, "start") && cmd_argc == 1) {
        struct sigmund_store pre_system_store;
        if (sigmund_init_system_store(&pre_system_store) == 0) {
            const char *atom = NULL;
            enum id_token_scope start_scope = sigmund_parse_id_token(cmd_argv[0], &atom);
            if ((start_scope == ID_TOKEN_PLAIN || start_scope == ID_TOKEN_SYSTEM) && atom &&
                (sigmund_valid_profile_hash(atom) || sigmund_valid_alias(atom))) {
                char hash[PROFILE_HASH_STR_LEN];
                if (sigmund_resolve_public_profile_token(&pre_system_store, atom, hash) == 1) {
                    int rc = 0;
                    int starts = multi ? multi_count : 1;
                    for (int i = 0; i < starts; i++) {
                        rc = sigmund_elevate_start_token(argv[0],
                                                 tail,
                                                 console_mode,
                                                 sigmund_valid_alias(atom) ? atom : hash,
                                                 sigmund_valid_alias(atom) ? hash : NULL,
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
        if (owned && sigmund_maybe_elevate_requested_system_targets(argv[0], command, cmd_argc, cmd_argv, all, &canonical_rc)) {
            free(cmd_argv);
            return canonical_rc;
        }
        int rc = sigmund_elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
        if (owned) {
            free(cmd_argv);
        }
        return rc;
    }

    struct sigmund_store user_store;
    struct sigmund_store system_store;
    memset(&user_store, 0, sizeof(user_store));
    if (sigmund_init_system_store(&system_store) != 0) {
        sigmund_die_errno("sigmund: failed to resolve system storage");
    }

    if (!inv.euid_root || is_list || (owned && (!strcmp(command, "stop") || !strcmp(command, "kill") ||
                                               !strcmp(command, "tail") || !strcmp(command, "dump") ||
                                               !strcmp(command, "view") || !strcmp(command, "prune") ||
                                               !strcmp(command, "console") || !strcmp(command, "aliases") ||
                                               !strcmp(command, "profile") || !strcmp(command, "show")))) {
        if (!inv.euid_root) {
            if (sigmund_ensure_user_store_for_current_user(&user_store) != 0) {
                sigmund_die_errno("sigmund: failed to init user storage");
            }
        }
    }

    if (inv.elevated && inv.euid_root && owned && cmd_argc == 3 &&
        (!strcmp(command, "start") || !strcmp(command, "stop") || !strcmp(command, "kill") ||
         !strcmp(command, "tail") || !strcmp(command, "dump") || !strcmp(command, "prune") ||
         !strcmp(command, "console"))) {
        int sig = !strcmp(command, "kill") ? SIGKILL : SIGTERM;
        bool graceful = !strcmp(command, "stop");
        int rc = sigmund_cmd_elevated_capability_action(&inv, &system_store, command, tail, console_mode, sig, graceful, cmd_argc, cmd_argv);
        if (rc >= 0) {
            free(cmd_argv);
            return rc;
        }
    }

    if (owned && !strcmp(command, "run")) {
        struct sigmund_store start_store;
        if (sigmund_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                sigmund_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                sigmund_die_errno("sigmund: failed to init invoking-user storage");
            }
            sigmund_die_errno("sigmund: failed to init start storage");
        }
        int rc = sigmund_perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
        free(cmd_argv);
        return rc;
    }

    if (owned && !strcmp(command, "shell")) {
        free(cmd_argv);
        return sigmund_run_captive_shell(argv[0]);
    }

    if (!owned) {
        struct sigmund_store start_store;
        if (sigmund_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                sigmund_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                sigmund_die_errno("sigmund: failed to init invoking-user storage");
            }
            sigmund_die_errno("sigmund: failed to init start storage");
        }
        return sigmund_perform_start(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL);
    }

    if (!strcmp(command, "doctor")) {
        printf("mund doctor\n");
        printf("version: %s\n", SIGMUND_VERSION);
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
            rc = sigmund_cmd_aliases_action(&inv, &user_store, &system_store, false);
        } else if (!strcmp(view, "runs") || !strcmp(view, "running") || !strcmp(view, "active") ||
                   !strcmp(view, "dormant") || !strcmp(view, "inactive") || !strcmp(view, "failed") ||
                   !strcmp(view, "stale") || !strcmp(view, "time") || !strcmp(view, "uptime")) {
            if (filter && !sigmund_valid_alias(filter)) {
                fprintf(stderr, "sigmund: error: invalid profile '%s'\n", filter);
                free(cmd_argv);
                return 5;
            }
            rc = inv.euid_root ? sigmund_cmd_list_system(&system_store, filter, list_iso)
                               : sigmund_cmd_list_normal(&user_store, &system_store, filter, list_iso);
        } else {
            fprintf(stderr, "usage: mund show <runs|profiles|running|dormant|failed|stale> [name]\n");
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
                fprintf(stderr, "usage: mund profile list [-v]\n");
                free(cmd_argv);
                return 5;
            }
            int rc = sigmund_cmd_aliases_action(&inv, &user_store, &system_store, verbose);
            free(cmd_argv);
            return rc;
        }
        if (!strcmp(sub, "run") || !strcmp(sub, "start")) {
            if (cmd_argc < 2) {
                fprintf(stderr, "usage: mund profile run <name> [--multi [N]] [--tail|-f] [--console]\n");
                free(cmd_argv);
                return 5;
            }
            const char *name = cmd_argv[1];
            if (!sigmund_valid_alias(name)) {
                fprintf(stderr, "sigmund: error: invalid profile '%s'\n", name);
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
                        if (sigmund_parse_positive_count(cmd_argv[i + 1], &parsed)) {
                            p_multi_count = parsed;
                            i++;
                        } else if (cmd_argv[i + 1][0] != '-') {
                            fprintf(stderr, "sigmund: error: invalid --multi count '%s'\n", cmd_argv[i + 1]);
                            free(cmd_argv);
                            return 5;
                        }
                    }
                } else if (strncmp(cmd_argv[i], "--multi=", 8) == 0) {
                    p_multi = true;
                    if (!sigmund_parse_positive_count(cmd_argv[i] + 8, &p_multi_count)) {
                        fprintf(stderr, "sigmund: error: invalid --multi count '%s'\n", cmd_argv[i] + 8);
                        free(cmd_argv);
                        return 5;
                    }
                } else {
                    fprintf(stderr, "usage: mund profile run <name> [--multi [N]] [--tail|-f] [--console]\n");
                    free(cmd_argv);
                    return 5;
                }
            }
            char *start_argv[1];
            start_argv[0] = (char *)name;
            struct sigmund_store start_store;
            if (sigmund_ensure_start_store_for_command(&inv, requested_system, true, "start", 1, start_argv, &start_store) != 0) {
                sigmund_die_errno("sigmund: failed to init start storage");
            }
            int rc = sigmund_cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, p_tail, p_console, p_multi, p_multi_count, 1, start_argv);
            free(cmd_argv);
            return rc;
        }
        if (!strcmp(sub, "show")) {
            if (cmd_argc != 2 || !sigmund_valid_alias(cmd_argv[1])) {
                fprintf(stderr, "usage: mund profile show <name>\n");
                free(cmd_argv);
                return 5;
            }
            int rc = sigmund_cmd_aliases_action(&inv, &user_store, &system_store, true);
            free(cmd_argv);
            return rc;
        }
        if (!strcmp(sub, "export") || !strcmp(sub, "import")) {
            int rc = sigmund_cmd_profile_action(&inv, &user_store, cmd_argc, cmd_argv);
            free(cmd_argv);
            return rc;
        }
        fprintf(stderr, "usage: mund profile <list|run|start|show|export|import> [args...]\n");
        free(cmd_argv);
        return 5;
    }

    if (!strcmp(command, "start")) {
        struct sigmund_store start_store;
        if (sigmund_ensure_start_store_for_command(&inv, requested_system, true, command, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                sigmund_start_target_is_within_invoking_home(&inv, true, command, cmd_argc, cmd_argv)) {
                sigmund_die_errno("sigmund: failed to init invoking-user storage");
            }
            sigmund_die_errno("sigmund: failed to init start storage");
        }
        int rc = sigmund_cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, multi, multi_count, cmd_argc, cmd_argv);
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
        if (alias_filter && !sigmund_valid_alias(alias_filter)) {
            fprintf(stderr, "sigmund: error: invalid alias '%s'\n", alias_filter);
            free(cmd_argv);
            return 5;
        }
        if (inv.euid_root) {
            rc = sigmund_cmd_list_system(&system_store, alias_filter, list_iso);
        } else {
            rc = sigmund_cmd_list_normal(&user_store, &system_store, alias_filter, list_iso);
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
        int rc = sigmund_cmd_tail_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "dump")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund dump <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = sigmund_cmd_dump_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "view")) {
        int rc = sigmund_cmd_view_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "console")) {
        if (cmd_argc < 1) {
            fprintf(stderr, "usage: sigmund console <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = sigmund_cmd_console_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "prune")) {
        const char *target = cmd_argc > 0 ? cmd_argv[0] : NULL;
        int rc = sigmund_cmd_prune_action(&inv, &user_store, &system_store, argv[0], target, all);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "alias")) {
        int rc = sigmund_cmd_alias_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
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
        int rc = sigmund_cmd_aliases_action(&inv, &user_store, &system_store, aliases_verbose);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "grant")) {
        if (sigmund_ensure_system_store(&system_store) != 0) {
            sigmund_die_errno("sigmund: failed to init system storage");
        }
        int rc = sigmund_cmd_grant_revoke_action(&inv, &system_store, argv[0], true, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "revoke")) {
        if (sigmund_ensure_system_store(&system_store) != 0) {
            sigmund_die_errno("sigmund: failed to init system storage");
        }
        int rc = sigmund_cmd_grant_revoke_action(&inv, &system_store, argv[0], false, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "stop")) {
        int rc = sigmund_cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", cmd_argc, cmd_argv, SIGTERM, true, all, print_cmd);
        free(cmd_argv);
        return rc;
    }
    if (!strcmp(command, "kill")) {
        int rc = sigmund_cmd_signal_action(&inv, &user_store, &system_store, argv[0], "kill", cmd_argc, cmd_argv, SIGKILL, false, all, print_cmd);
        free(cmd_argv);
        return rc;
    }

    free(cmd_argv);
    sigmund_usage();
    return 1;
}
