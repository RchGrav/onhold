#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"
#include "hold/access.h"
#include "hold/console.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/core.h"

static void print_command_usage_stderr(const char *command);
static int build_cap_request_token(const char *op, bool force, char *out, size_t n);
static bool parse_docker_run_flag(const char *arg,
                                  bool *detach,
                                  bool *interactive,
                                  bool *tty,
                                  bool *privileged);
static int append_env_assignment(const char *arg, char ***env_out, int *envc_out);
static int append_env_file(const char *path, char ***env_out, int *envc_out);
static int run_recipe_matches_profile(const struct hold_store *store,
                                      const char *name,
                                      int argc,
                                      char **argv,
                                      int envc,
                                      char **env,
                                      bool *matched);
static int ensure_named_run_profile(const struct hold_invocation *inv,
                                    const struct hold_store *store,
                                    const char *name,
                                    int argc,
                                    char **argv,
                                    int envc,
                                    char **env);
static bool is_legacy_run_namespace_verb(const char *arg);

static int build_cap_request_token(const char *op, bool force, char *out, size_t n) {
    if (!op || !*op) {
        errno = EINVAL;
        return -1;
    }
    char json[128];
    if (hold_checked_snprintf(json, sizeof(json),
                              "{\"v\":1,\"op\":\"%s\",\"force\":%s}",
                              op, force ? "true" : "false") != 0) {
        return -1;
    }
    return hold_base64url_encode((const unsigned char *)json, strlen(json), out, n);
}

static void print_command_usage_stderr(const char *command) {
    const char *usage = hold_cli_command_usage(command);
    if (usage) {
        fprintf(stderr, "%s\n", usage);
    }
}

static bool parse_docker_run_flag(const char *arg,
                                  bool *detach,
                                  bool *interactive,
                                  bool *tty,
                                  bool *privileged) {
    if (!arg || arg[0] != '-') return false;
    if (!strcmp(arg, "-d") || !strcmp(arg, "--detach")) {
        *detach = true;
        return true;
    }
    if (!strcmp(arg, "-i") || !strcmp(arg, "--interactive")) {
        *interactive = true;
        return true;
    }
    if (!strcmp(arg, "-t") || !strcmp(arg, "--tty")) {
        *tty = true;
        return true;
    }
    if (!strcmp(arg, "-it") || !strcmp(arg, "-ti")) {
        *interactive = true;
        *tty = true;
        return true;
    }
    if (!strcmp(arg, "--privileged")) {
        *privileged = true;
        return true;
    }
    return false;
}

static int append_env_assignment(const char *arg, char ***env_out, int *envc_out) {
    const char *eq = arg ? strchr(arg, '=') : NULL;
    if (!arg || !*arg || !eq || eq == arg) {
        fprintf(stderr, "hold: error: expected KEY=VALUE after --env/-e\n");
        return 5;
    }
    size_t key_len = (size_t)(eq - arg);
    char key[256];
    if (key_len >= sizeof(key)) {
        fprintf(stderr, "hold: error: environment key is too long\n");
        return 5;
    }
    memcpy(key, arg, key_len);
    key[key_len] = '\0';
    for (size_t i = 0; key[i]; i++) {
        if (key[i] == '=') {
            fprintf(stderr, "hold: error: invalid environment key\n");
            return 5;
        }
    }
    if (setenv(key, eq + 1, 1) != 0) {
        hold_die_errno("hold: failed to set launch environment");
    }
    char *copy = strdup(arg);
    if (!copy) return 3;
    char **next = realloc(*env_out, ((size_t)*envc_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 3;
    }
    next[*envc_out] = copy;
    *env_out = next;
    (*envc_out)++;
    return 0;
}

static int append_env_file(const char *path, char ***env_out, int *envc_out) {
    if (!path || !*path) {
        fprintf(stderr, "hold: error: --env-file requires a path\n");
        return 5;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "hold: error: cannot read env file '%s': %s\n", path, strerror(errno));
        return 5;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t nr;
    unsigned long lineno = 0;
    int rc = 0;
    while ((nr = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (nr > 0 && (line[nr - 1] == '\n' || line[nr - 1] == '\r')) {
            line[--nr] = '\0';
        }
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        rc = append_env_assignment(p, env_out, envc_out);
        if (rc != 0) {
            fprintf(stderr, "hold: error: invalid --env-file entry at %s:%lu\n", path, lineno);
            break;
        }
    }
    free(line);
    if (ferror(f) && rc == 0) {
        fprintf(stderr, "hold: error: failed reading env file '%s': %s\n", path, strerror(errno));
        rc = 5;
    }
    fclose(f);
    return rc;
}

static int run_recipe_matches_profile(const struct hold_store *store,
                                      const char *name,
                                      int argc,
                                      char **argv,
                                      int envc,
                                      char **env,
                                      bool *matched) {
    *matched = false;
    struct hold_profile recipe;
    if (hold_alias_lookup_recipe(store, name, &recipe) != 0) {
        return 0;
    }
    char binary_path[HOLD_PATH_MAX];
    if (argc <= 0 || hold_resolve_binary_path(argv[0], binary_path, sizeof(binary_path)) != 0) {
        hold_free_profile(&recipe);
        return -1;
    }
    bool same = recipe.argc == argc && recipe.envc == envc && !strcmp(recipe.binary_path, binary_path);
    for (int i = 0; same && i < argc; i++) {
        same = !strcmp(recipe.argv[i], argv[i]);
    }
    for (int i = 0; same && i < envc; i++) {
        same = recipe.env && env && !strcmp(recipe.env[i], env[i]);
    }
    *matched = same;
    hold_free_profile(&recipe);
    return 1;
}

static int ensure_named_run_profile(const struct hold_invocation *inv,
                                    const struct hold_store *store,
                                    const char *name,
                                    int argc,
                                    char **argv,
                                    int envc,
                                    char **env) {
    if (!name) return 0;
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "hold: error: invalid profile name '%s'\n", name);
        return 5;
    }
    if (argc <= 0 || !argv || !argv[0]) {
        fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
        return 5;
    }
    if (inv->euid_root && store->kind == STORE_USER_LOCAL) {
        fprintf(stderr, "hold: error: create user-local profiles as that user\n");
        return 5;
    }
    if (hold_alias_exists_in_store(store, name)) {
        bool matched = false;
        int rc = run_recipe_matches_profile(store, name, argc, argv, envc, env, &matched);
        if (rc < 0) return 5;
        if (!matched) {
            fprintf(stderr,
                    "hold: error: profile '%s' already exists with a different launch recipe; use `hold run %s` or edit it in configuration mode\n",
                    name,
                    name);
            return 5;
        }
        return 0;
    }
    char binary_path[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(argv[0], binary_path, sizeof(binary_path)) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "hold: cannot start '%s': command not found\n", argv[0]);
        } else {
            fprintf(stderr, "hold: cannot start '%s': %s\n", argv[0], strerror(errno));
        }
        return 1;
    }
    if (hold_alias_upsert_recipe_env(store, name, binary_path, argc, argv, envc, env) != 0) {
        hold_die_errno("hold: failed to write profile");
    }
    hold_sig_note(inv, "hold: created profile '%s'\n", name);
    return 0;
}

static bool is_legacy_run_namespace_verb(const char *arg) {
    static const char *verbs[] = {
        "stop", "kill", "logs", "tail", "dump", "inspect", "console", "rm", "prune",
        "profile", "show", "status", "ps", "list"
    };
    if (!arg) return false;
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++) {
        if (!strcmp(arg, verbs[i])) return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        struct hold_invocation inv;
        if (hold_detect_invocation(&inv, false, false) != 0) {
            hold_die_errno("hold: failed to resolve invocation context");
        }
        struct hold_store user_store, system_store;
        memset(&user_store, 0, sizeof(user_store));
        if (hold_init_system_store(&system_store) != 0) {
            hold_die_errno("hold: failed to resolve system storage");
        }
        if (!inv.euid_root && hold_ensure_user_store_for_current_user(&user_store) != 0) {
            hold_die_errno("hold: failed to init user storage");
        }
        return hold_cmd_captive_action(&inv, inv.euid_root ? &system_store : &user_store, &system_store, argv[0]);
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
    bool rm_force = false;
    bool docker_detach = false;
    bool docker_interactive = false;
    bool docker_tty = false;
    bool docker_privileged = false;
    const char *docker_name = NULL;
    char **docker_env = NULL;
    int docker_envc = 0;
    int multi_count = 1;

    while (argi < argc) {
        if (parse_docker_run_flag(argv[argi], &docker_detach, &docker_interactive, &docker_tty, &docker_privileged)) {
            if (docker_privileged) {
                requested_system = true;
            }
            if (docker_tty) {
                console_mode = true;
            }
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--name")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                return 5;
            }
            docker_name = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (!strcmp(argv[argi], "-e") || !strcmp(argv[argi], "--env")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                return 5;
            }
            int env_rc = append_env_assignment(argv[argi + 1], &docker_env, &docker_envc);
            if (env_rc != 0) { free(docker_env); return env_rc; }
            argi += 2;
            continue;
        }
        if (!strncmp(argv[argi], "--env=", 6)) {
            int env_rc = append_env_assignment(argv[argi] + 6, &docker_env, &docker_envc);
            if (env_rc != 0) { free(docker_env); return env_rc; }
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--env-file")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                return 5;
            }
            int env_rc = append_env_file(argv[argi + 1], &docker_env, &docker_envc);
            if (env_rc != 0) { free(docker_env); return env_rc; }
            argi += 2;
            continue;
        }
        if (!strncmp(argv[argi], "--env-file=", 11)) {
            int env_rc = append_env_file(argv[argi] + 11, &docker_env, &docker_envc);
            if (env_rc != 0) { free(docker_env); return env_rc; }
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "-p") || !strcmp(argv[argi], "--publish") ||
            !strcmp(argv[argi], "-v") || !strcmp(argv[argi], "--volume") ||
            !strcmp(argv[argi], "--detach-keys") || !strcmp(argv[argi], "--restart") ||
            !strcmp(argv[argi], "--restart-delay")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                return 5;
            }
            argi += 2;
            continue;
        }
        if (!strncmp(argv[argi], "--publish=", 10) || !strncmp(argv[argi], "--volume=", 9) ||
            !strncmp(argv[argi], "--detach-keys=", 14) || !strncmp(argv[argi], "--restart=", 10) ||
            !strncmp(argv[argi], "--restart-delay=", 16)) {
            argi++;
            continue;
        }
        if (!strcmp(argv[argi], "--rm")) {
            argi++;
            continue;
        }
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
            if (!literal_owned_arg && !strcmp(command, "ps") && !strcmp(argv[i], "-a")) {
                all = true;
                continue;
            }
            if (!literal_owned_arg && !strcmp(command, "rm") && !strcmp(argv[i], "--force")) {
                rm_force = true;
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
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                parse_docker_run_flag(argv[i], &docker_detach, &docker_interactive, &docker_tty, &docker_privileged)) {
                if (docker_privileged) {
                    requested_system = true;
                }
                if (docker_tty) {
                    console_mode = true;
                }
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                !strcmp(argv[i], "--name")) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                    free(cmd_argv);
                    return 5;
                }
                docker_name = argv[++i];
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--env"))) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                    free(cmd_argv);
                    return 5;
                }
                int env_rc = append_env_assignment(argv[++i], &docker_env, &docker_envc);
                if (env_rc != 0) {
                    free(cmd_argv);
                    free(docker_env);
                    return env_rc;
                }
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                !strncmp(argv[i], "--env=", 6)) {
                int env_rc = append_env_assignment(argv[i] + 6, &docker_env, &docker_envc);
                if (env_rc != 0) {
                    free(cmd_argv);
                    free(docker_env);
                    return env_rc;
                }
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                !strcmp(argv[i], "--env-file")) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                    free(cmd_argv);
                    return 5;
                }
                int env_rc = append_env_file(argv[++i], &docker_env, &docker_envc);
                if (env_rc != 0) {
                    free(cmd_argv);
                    free(docker_env);
                    return env_rc;
                }
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                !strncmp(argv[i], "--env-file=", 11)) {
                int env_rc = append_env_file(argv[i] + 11, &docker_env, &docker_envc);
                if (env_rc != 0) {
                    free(cmd_argv);
                    free(docker_env);
                    return env_rc;
                }
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--publish") ||
                 !strcmp(argv[i], "-v") || !strcmp(argv[i], "--volume") ||
                 !strcmp(argv[i], "--detach-keys") || !strcmp(argv[i], "--restart") ||
                 !strcmp(argv[i], "--restart-delay"))) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
                    free(cmd_argv);
                    return 5;
                }
                i++;
                continue;
            }
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run")) &&
                (!strncmp(argv[i], "--publish=", 10) || !strncmp(argv[i], "--volume=", 9) ||
                 !strncmp(argv[i], "--detach-keys=", 14) || !strncmp(argv[i], "--restart=", 10) ||
                 !strncmp(argv[i], "--restart-delay=", 16) || !strcmp(argv[i], "--rm"))) {
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
            if (!literal_owned_arg && !strcmp(command, "start") && !strcmp(argv[i], "--force")) {
                multi = true;
                multi_count = 1;
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

    bool cap_run_form = owned && !strcmp(command, "run") && !saw_owned_delimiter &&
                        cmd_argc == 4 && !strcmp(cmd_argv[1], "--cap");

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
    if (owned && !saw_owned_delimiter && cmd_argc == 1 && (!strcmp(cmd_argv[0], "--help") || !strcmp(cmd_argv[0], "-h"))) {
        int rc = hold_show_help(command);
        free(cmd_argv);
        return rc;
    }
    if (owned && !strcmp(command, "run") && !saw_owned_delimiter &&
        cmd_argc >= 2 && is_legacy_run_namespace_verb(cmd_argv[1])) {
        fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]; use -- <cmd> when a command conflicts with Hold syntax\n");
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
    if (docker_detach) {
        tail = false;
    }
    if (docker_interactive && !docker_tty) {
        /* Parsed for Docker-shaped CLI compatibility; non-PTY stdin plumbing is a later runner slice. */
    }
    if (inv.elevated && !inv.euid_root) {
        fprintf(stderr, "hold: internal error: --elevated without root authority\n");
        if (owned) {
            free(cmd_argv);
        }
        return 3;
    }

    if (owned && !strcmp(command, "logs")) {
        command = "__view";
    }
    if (owned && !strcmp(command, "ps")) command = "list";
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
                    if (hold_valid_alias(atom)) {
                        struct passwd *pw = getpwuid(geteuid());
                        char subject[128];
                        if (pw && pw->pw_name && *pw->pw_name &&
                            hold_checked_snprintf(subject, sizeof(subject), "%s", pw->pw_name) == 0) {
                            char grant_hash[PROFILE_HASH_STR_LEN];
                            if (hold_subject_grant_hash_for(&pre_system_store, subject, atom, grant_hash) == 0) {
                                char token[1024];
                                if (build_cap_request_token("start", multi, token, sizeof(token)) != 0) {
                                    free(cmd_argv);
                                    return 3;
                                }
                                char *canon[5] = {"run", (char *)atom, "--cap", grant_hash, token};
                                int rc = hold_elevate_with_sudo_direct(argv[0], 5, canon);
                                free(cmd_argv);
                                return rc;
                            }
                        }
                    }
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
                                               !strcmp(command, "__view") || !strcmp(command, "prune") ||
                                               !strcmp(command, "console") || !strcmp(command, "profile") ||
                                               !strcmp(command, "show") || !strcmp(command, "rm") ||
                                               !strcmp(command, "shell")))) {
        if (!inv.euid_root) {
            if (hold_ensure_user_store_for_current_user(&user_store) != 0) {
                hold_die_errno("hold: failed to init user storage");
            }
        }
    }

    if (cap_run_form && inv.euid_root) {
        int rc = hold_cmd_cap_request_action(&inv, &system_store, tail, console_mode, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc >= 0 ? rc : 5;
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
        int rc;
        if (docker_name) {
            rc = ensure_named_run_profile(&inv, &start_store, docker_name, cmd_argc, cmd_argv, docker_envc, docker_env);
            if (rc == 0) {
                rc = hold_perform_start_with_env(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, docker_name, docker_envc, docker_env);
            }
        } else if (saw_owned_delimiter) {
            rc = hold_perform_start_with_env(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL, docker_envc, docker_env);
        } else {
            rc = hold_cmd_start_action(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, multi, multi_count, cmd_argc, cmd_argv);
        }
        free(cmd_argv);
        free(docker_env);
        return rc;
    }

    if (owned && !strcmp(command, "shell")) {
        int rc = hold_cmd_shell_action(&inv, inv.euid_root ? &system_store : &user_store);
        free(cmd_argv);
        return rc;
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
        if (docker_name) {
            int rc = ensure_named_run_profile(&inv, &start_store, docker_name, cmd_argc, cmd_argv, docker_envc, docker_env);
            if (rc != 0) {
                free(docker_env);
                return rc;
            }
            rc = hold_perform_start_with_env(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, docker_name, docker_envc, docker_env);
            free(docker_env);
            return rc;
        }
        int rc = hold_perform_start_with_env(&inv, &start_store, tail, console_mode, cmd_argc, cmd_argv, NULL, NULL, docker_envc, docker_env);
        free(docker_env);
        return rc;
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
            fprintf(stderr, "hold: error: profile %s command was removed; use `hold profiles%s`\n",
                    sub, verbose ? " -v" : "");
            free(cmd_argv);
            return 5;
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
                print_command_usage_stderr("profile");
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
                print_command_usage_stderr("profile");
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
        print_command_usage_stderr("profile");
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
    if (!strcmp(command, "inspect")) {
        if (cmd_argc != 1) {
            fprintf(stderr, "usage: hold inspect <target>\n");
            free(cmd_argv);
            return 5;
        }
        int rc = hold_cmd_inspect_action(&inv, &user_store, &system_store, argv[0], cmd_argv[0]);
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
    if (!strcmp(command, "__view")) {
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
    if (!strcmp(command, "rm")) {
        const char *target = cmd_argv[0];
        int rc = 0;
        if (!rm_force && !inv.euid_root && hold_valid_alias(target) && hold_alias_exists_in_store(&user_store, target)) {
            rc = hold_cmd_profile_delete(&inv, &user_store, target);
            free(cmd_argv);
            return rc;
        }
        if (rm_force) {
            rc = hold_cmd_signal_action(&inv, &user_store, &system_store, argv[0], "stop", 1, cmd_argv, SIGTERM, true, false, false);
            if (rc != 0) {
                free(cmd_argv);
                return rc;
            }
        }
        rc = hold_cmd_prune_action(&inv, &user_store, &system_store, argv[0], target, false);
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
