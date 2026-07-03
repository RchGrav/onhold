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
static int build_cap_request_token(const char *op, bool force, bool detach, char *out, size_t n);
static bool parse_docker_run_flag(const char *arg,
                                  bool *detach,
                                  bool *interactive,
                                  bool *tty,
                                  bool *privileged);
static int append_env_assignment(const char *arg, char ***env_out, int *envc_out);
static int append_string_option(const char *what, const char *arg, char ***items_out, int *count_out);
static int append_string_option(const char *what, const char *arg, char ***items_out, int *count_out) {
    if (!arg || !*arg) {
        fprintf(stderr, "hold: error: %s requires a value\n", what);
        return 5;
    }
    char *copy = strdup(arg);
    if (!copy) return 3;
    char **next = realloc(*items_out, ((size_t)*count_out + 1) * sizeof(*next));
    if (!next) {
        free(copy);
        return 3;
    }
    next[*count_out] = copy;
    *items_out = next;
    (*count_out)++;
    return 0;
}

static int reject_publish_option(void);
static int reject_volume_option(void);
static int parse_restart_policy_arg(const char *arg, char out[64]);
static int parse_restart_delay_arg(const char *arg, int *out);
static bool restart_policy_arg_enabled(const char *arg);
static int append_env_file(const char *path, char ***env_out, int *envc_out);
static int configure_detach_keys_option(const char *value);
static bool token_names_existing_profile(const struct hold_store *user_store,
                                         const struct hold_store *system_store,
                                         const char *token);
static bool token_names_detached_profile(const struct hold_store *user_store,
                                         const struct hold_store *system_store,
                                         const char *token);
static bool is_legacy_run_namespace_verb(const char *arg);
static bool command_supports_multiplicity(const char *command);

struct cli_run_options {
    bool detach;
    bool interactive;
    bool tty;
    bool privileged;
    bool auto_remove;
    const char *name;
    char **env;
    int envc;
    char **ports;
    int portc;
    char **volumes;
    int volumec;
    char **cap_add;
    int cap_addc;
    char **cap_drop;
    int cap_dropc;
    char restart_policy[64];
    int restart_delay_seconds;
    bool restart_delay_seen;
    const char *log_destination;
};

static void free_run_options(struct cli_run_options *run) {
    if (!run) return;
    hold_free_argv_alloc(run->env, run->envc);
    hold_free_argv_alloc(run->ports, run->portc);
    hold_free_argv_alloc(run->volumes, run->volumec);
    hold_free_argv_alloc(run->cap_add, run->cap_addc);
    hold_free_argv_alloc(run->cap_drop, run->cap_dropc);
    memset(run, 0, sizeof(*run));
}

static struct hold_start_options start_options_from_run(const struct cli_run_options *run,
                                                        bool tail,
                                                        bool console_mode,
                                                        bool interactive_stdin,
                                                        int argc,
                                                        char **argv) {
    return (struct hold_start_options){
        .tail = tail,
        .console_mode = console_mode,
        .auto_remove = run->auto_remove,
        .interactive_stdin = interactive_stdin,
        .argc = argc,
        .argv = argv,
        .run_name = run->name,
        .envc = run->envc,
        .env = run->env,
        .portc = run->portc,
        .ports = run->ports,
        .volumec = run->volumec,
        .volumes = run->volumes,
        .cap_addc = run->cap_addc,
        .cap_add = run->cap_add,
        .cap_dropc = run->cap_dropc,
        .cap_drop = run->cap_drop,
        .restart_policy = run->restart_policy[0] ? run->restart_policy : NULL,
        .restart_delay_seconds = run->restart_delay_seconds,
        .log_destination = run->log_destination,
    };
}

static int build_cap_request_token(const char *op, bool force, bool detach, char *out, size_t n) {
    if (!op || !*op) {
        errno = EINVAL;
        return -1;
    }
    char json[160];
    if (detach) {
        if (hold_checked_snprintf(json, sizeof(json),
                                  "{\"v\":1,\"op\":\"%s\",\"force\":%s,\"detach\":true}",
                                  op, force ? "true" : "false") != 0) {
            return -1;
        }
    } else if (hold_checked_snprintf(json, sizeof(json),
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

static bool command_supports_multiplicity(const char *command) {
    return command && (!strcmp(command, "start") || !strcmp(command, "run"));
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

static int parse_restart_policy_arg(const char *arg, char out[64]) {
    if (!arg || !*arg) {
        fprintf(stderr, "hold: error: --restart requires a policy\n");
        return 5;
    }
    if (!strcmp(arg, "no") || !strcmp(arg, "always") || !strcmp(arg, "unless-stopped")) {
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    if (!strncmp(arg, "on-failure", 10) && (arg[10] == '\0' || arg[10] == ':')) {
        if (arg[10] == ':') {
            char *end = NULL;
            long n = strtol(arg + 11, &end, 10);
            if (!end || *end || n < 0 || n > INT_MAX) {
                fprintf(stderr, "hold: error: invalid --restart on-failure retry count '%s'\n", arg + 11);
                return 5;
            }
        }
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    fprintf(stderr, "hold: error: invalid --restart policy '%s'\n", arg);
    return 5;
}

static bool restart_policy_arg_enabled(const char *arg) {
    return arg && *arg && strcmp(arg, "no") != 0;
}

static int parse_restart_delay_arg(const char *arg, int *out) {
    if (!arg || !*arg) {
        fprintf(stderr, "hold: error: --restart-delay requires seconds\n");
        return 5;
    }
    char *end = NULL;
    long n = strtol(arg, &end, 10);
    if (!end || *end || n < 0 || n > INT_MAX) {
        fprintf(stderr, "hold: error: invalid --restart-delay '%s'\n", arg);
        return 5;
    }
    *out = (int)n;
    return 0;
}

static int detach_key_token_value(const char *token, size_t len, unsigned char *out) {
    if (!token || len == 0 || !out) return -1;
    if (len == 1) {
        *out = (unsigned char)token[0];
        return 0;
    }
    if (len == 2 && token[0] == '^' && token[1] >= '@' && token[1] <= '_') {
        *out = (unsigned char)(token[1] - '@');
        return 0;
    }
    if (len == 6 &&
        tolower((unsigned char)token[0]) == 'c' &&
        tolower((unsigned char)token[1]) == 't' &&
        tolower((unsigned char)token[2]) == 'r' &&
        tolower((unsigned char)token[3]) == 'l' &&
        token[4] == '-' &&
        token[5] >= '@' && token[5] <= '_') {
        *out = (unsigned char)(token[5] - '@');
        return 0;
    }
    if (len == 6 &&
        tolower((unsigned char)token[0]) == 'c' &&
        tolower((unsigned char)token[1]) == 't' &&
        tolower((unsigned char)token[2]) == 'r' &&
        tolower((unsigned char)token[3]) == 'l' &&
        token[4] == '-' &&
        token[5] >= 'a' && token[5] <= 'z') {
        *out = (unsigned char)(token[5] - 'a' + 1);
        return 0;
    }
    return -1;
}

static int configure_detach_keys_option(const char *value) {
    if (!value || !*value) {
        fprintf(stderr, "hold: error: --detach-keys requires a key sequence\n");
        return 5;
    }
    unsigned char keys[8];
    size_t nkeys = 0;
    const char *p = value;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        if (nkeys >= sizeof(keys) || detach_key_token_value(start, (size_t)(p - start), &keys[nkeys]) != 0) {
            fprintf(stderr, "hold: error: invalid --detach-keys sequence '%s'\n", value);
            return 5;
        }
        nkeys++;
    }
    if (nkeys == 0 || hold_console_set_detach_keys(keys, nkeys) != 0) {
        fprintf(stderr, "hold: error: invalid --detach-keys sequence '%s'\n", value);
        return 5;
    }
    return 0;
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

static int reject_publish_option(void) {
    fprintf(stderr,
            "hold: error: Hold is not containerized and does not publish or forward ports; listening ports are observed automatically and shown in `hold ps`\n");
    return 5;
}

static int reject_volume_option(void) {
    fprintf(stderr,
            "hold: error: Hold is not containerized and does not mount or remap volumes; pass host paths directly (prefer absolute paths) or save them in the profile command argv\n");
    return 5;
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

static bool token_names_existing_profile(const struct hold_store *user_store,
                                         const struct hold_store *system_store,
                                         const char *token) {
    if (!token || !*token) return false;
    if (hold_valid_profile_hash(token)) return true;
    if (!hold_valid_alias(token)) return false;
    if (user_store && hold_alias_exists_in_store(user_store, token)) return true;
    if (system_store && hold_alias_exists_in_store(system_store, token)) return true;
    return false;
}

static bool token_names_detached_profile(const struct hold_store *user_store,
                                         const struct hold_store *system_store,
                                         const char *token) {
    const char *atom = NULL;
    enum id_token_scope scope = hold_parse_id_token(token, &atom);
    if (scope == ID_TOKEN_INVALID || !atom || !hold_valid_alias(atom)) {
        return false;
    }
    struct hold_profile recipe;
    if ((scope == ID_TOKEN_PLAIN || scope == ID_TOKEN_USER) && user_store && user_store->base[0] &&
        hold_alias_lookup_recipe(user_store, atom, &recipe) == 0) {
        bool detached = recipe.recipe.mode_detach;
        hold_free_profile(&recipe);
        return detached;
    }
    if ((scope == ID_TOKEN_PLAIN || scope == ID_TOKEN_SYSTEM) && system_store && system_store->base[0]) {
        char hash[PROFILE_HASH_STR_LEN];
        if (hold_resolve_public_profile_token(system_store, atom, hash) == 1 &&
            hold_load_profile_by_hash(system_store, hash, &recipe) == 0) {
            bool detached = recipe.recipe.mode_detach;
            hold_free_profile(&recipe);
            return detached;
        }
    }
    return false;
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

static int set_log_destination(struct cli_run_options *run, const char *value) {
    if (!value || !*value) {
        fprintf(stderr, "hold: error: --log-destination requires a destination\n");
        return 5;
    }
    if (strcmp(value, "syslog") && strcmp(value, "json-file") && strcmp(value, "local")) {
        fprintf(stderr, "hold: error: unsupported log destination '%s'\n", value);
        return 5;
    }
    run->log_destination = (!strcmp(value, "syslog")) ? value : NULL;
    return 0;
}

static int parse_run_options(int argc,
                             char **argv,
                             int *index,
                             struct cli_run_options *run,
                             bool *requested_system,
                             bool *tail,
                             bool *console_mode,
                             bool *parsed) {
    const char *arg = argv[*index];
    *parsed = true;
    if (parse_docker_run_flag(arg, &run->detach, &run->interactive, &run->tty, &run->privileged)) {
        if (run->privileged) *requested_system = true;
        if (run->tty) *console_mode = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--name")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        run->name = argv[*index + 1];
        *index += 2;
        return 0;
    }
    if (!strcmp(arg, "-e") || !strcmp(arg, "--env")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = append_env_assignment(argv[*index + 1], &run->env, &run->envc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--env=", 6)) {
        int rc = append_env_assignment(arg + 6, &run->env, &run->envc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--env-file")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = append_env_file(argv[*index + 1], &run->env, &run->envc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--env-file=", 11)) {
        int rc = append_env_file(arg + 11, &run->env, &run->envc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "-p") || !strcmp(arg, "--publish") ||
        !strcmp(arg, "-P") || !strcmp(arg, "--publish-all") ||
        !strncmp(arg, "--publish=", 10)) {
        return reject_publish_option();
    }
    if (!strcmp(arg, "-v") || !strcmp(arg, "--volume") || !strncmp(arg, "--volume=", 9)) {
        return reject_volume_option();
    }
    if (!strcmp(arg, "--cap-add")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "hold: error: --cap-add requires a capability\n");
            return 5;
        }
        int rc = append_string_option("--cap-add", argv[*index + 1], &run->cap_add, &run->cap_addc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--cap-add=", 10)) {
        int rc = append_string_option("--cap-add", arg + 10, &run->cap_add, &run->cap_addc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--cap-drop")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "hold: error: --cap-drop requires a capability\n");
            return 5;
        }
        int rc = append_string_option("--cap-drop", argv[*index + 1], &run->cap_drop, &run->cap_dropc);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--cap-drop=", 11)) {
        int rc = append_string_option("--cap-drop", arg + 11, &run->cap_drop, &run->cap_dropc);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--detach-keys")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = configure_detach_keys_option(argv[*index + 1]);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--detach-keys=", 14)) {
        int rc = configure_detach_keys_option(arg + 14);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--restart")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = parse_restart_policy_arg(argv[*index + 1], run->restart_policy);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--restart=", 10)) {
        int rc = parse_restart_policy_arg(arg + 10, run->restart_policy);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--restart-delay")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]\n");
            return 5;
        }
        int rc = parse_restart_delay_arg(argv[*index + 1], &run->restart_delay_seconds);
        if (rc != 0) return rc;
        run->restart_delay_seen = true;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--restart-delay=", 16)) {
        int rc = parse_restart_delay_arg(arg + 16, &run->restart_delay_seconds);
        if (rc != 0) return rc;
        run->restart_delay_seen = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--rm")) {
        run->auto_remove = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--log-destination")) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "hold: error: --log-destination requires a destination\n");
            return 5;
        }
        int rc = set_log_destination(run, argv[*index + 1]);
        if (rc != 0) return rc;
        *index += 2;
        return 0;
    }
    if (!strncmp(arg, "--log-destination=", 18)) {
        int rc = set_log_destination(run, arg + 18);
        if (rc != 0) return rc;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--tail") || !strcmp(arg, "-f")) {
        *tail = true;
        (*index)++;
        return 0;
    }
    if (!strcmp(arg, "--console")) {
        *console_mode = true;
        (*index)++;
        return 0;
    }
    *parsed = false;
    return 0;
}

int hold_cli_main(int argc, char **argv) {
    if (argc < 2) {
        /* Docker parity: bare invocation prints help. The captive CLI is `hold cli`. */
        hold_usage();
        return 0;
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
    struct cli_run_options run = {0};
    int multi_count = 1;

    while (argi < argc) {
        bool parsed_run_option = false;
        int option_rc = parse_run_options(argc, argv, &argi, &run, &requested_system,
                                          &tail, &console_mode, &parsed_run_option);
        if (option_rc != 0) {
            free_run_options(&run);
            return option_rc;
        }
        if (parsed_run_option) {
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
            if (!literal_owned_arg && !strcmp(argv[i], "-a") &&
                (!strcmp(command, "ps") || !strcmp(command, "prune") || !strcmp(command, "clean"))) {
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
            if (!literal_owned_arg && (!strcmp(command, "start") || !strcmp(command, "run"))) {
                bool parsed_run_option = false;
                int option_index = i;
                int option_rc = parse_run_options(argc, argv, &option_index, &run, &requested_system,
                                                  &tail, &console_mode, &parsed_run_option);
                if (option_rc != 0) {
                    free(cmd_argv);
                    free_run_options(&run);
                    return option_rc;
                }
                if (parsed_run_option) {
                    i = option_index - 1;
                    continue;
                }
            }
            if (!literal_owned_arg && (!strcmp(command, "list") || !strcmp(command, "status")) &&
                (!strcmp(argv[i], "--iso") || !strcmp(argv[i], "-l"))) {
                list_iso = true;
                continue;
            }
            if (!literal_owned_arg && command_supports_multiplicity(command) && !strcmp(argv[i], "--force")) {
                multi = true;
                multi_count = 1;
                continue;
            }
            if (!literal_owned_arg && command_supports_multiplicity(command) && !strcmp(argv[i], "--multi")) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "hold: error: --multi requires a positive count\n");
                    free(cmd_argv);
                    return 5;
                }
                int parsed = 0;
                if (!hold_parse_positive_count(argv[i + 1], &parsed)) {
                    fprintf(stderr, "hold: error: invalid --multi count '%s'\n", argv[i + 1]);
                    free(cmd_argv);
                    return 5;
                }
                multi = true;
                multi_count = parsed;
                i++;
                continue;
            }
            if (!literal_owned_arg && command_supports_multiplicity(command) && strncmp(argv[i], "--multi=", 8) == 0) {
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
    if (owned && !strcmp(command, "run") && !saw_owned_delimiter && cmd_argc >= 1 &&
        (is_legacy_run_namespace_verb(cmd_argv[0]) ||
         (cmd_argc >= 2 && is_legacy_run_namespace_verb(cmd_argv[1])))) {
        fprintf(stderr, "usage: hold run [run-options] <cmd|profile> [args...]; use -- <cmd> when a command conflicts with Hold syntax\n");
        free(cmd_argv);
        free_run_options(&run);
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
    if (run.restart_delay_seen && !restart_policy_arg_enabled(run.restart_policy)) {
        fprintf(stderr, "hold: error: --restart-delay requires --restart with an active policy\n");
        if (owned) free(cmd_argv);
        free_run_options(&run);
        return 5;
    }

    struct hold_invocation inv;
    if (hold_detect_invocation(&inv, requested_system, elevated) != 0) {
        hold_die_errno("hold: failed to resolve invocation context");
    }
    inv.quiet = quiet;
    inv.docker_run = owned && !strcmp(command, "run");
    bool run_tail_implicit = false;
    if ((owned && !strcmp(command, "run")) && !run.detach && (!console_mode || run.tty)) {
        if (!tail) {
            run_tail_implicit = true;
        }
        tail = true;
    }
    if (run.detach) {
        tail = false;
    }
    bool interactive_stdin = run.interactive && !run.tty;
    if (inv.elevated && !inv.euid_root) {
        int rc = hold_elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, run.auto_remove, interactive_stdin, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
        if (owned) {
            free(cmd_argv);
        }
        return rc;
    }

    bool docker_ps_command = owned && !strcmp(command, "ps");
    if (owned && !strcmp(command, "logs")) {
        command = "__view";
    }
    if (owned && !strcmp(command, "attach")) {
        command = "console";
    }
    if (docker_ps_command) command = "list";
    if (owned && !strcmp(command, "status")) command = "list";
    if (owned && !strcmp(command, "clean")) command = "prune";

    bool is_list = owned && !strcmp(command, "list");
    if (requested_system && !inv.euid_root && owned && command_supports_multiplicity(command) && cmd_argc == 1) {
        struct hold_store pre_system_store;
        if (hold_init_system_store(&pre_system_store) == 0) {
            const char *atom = NULL;
            enum id_token_scope start_scope = hold_parse_id_token(cmd_argv[0], &atom);
            if ((start_scope == ID_TOKEN_PLAIN || start_scope == ID_TOKEN_SYSTEM) && atom &&
                (hold_valid_profile_hash(atom) || hold_valid_alias(atom))) {
                char hash[PROFILE_HASH_STR_LEN];
                if (hold_resolve_public_profile_token(&pre_system_store, atom, hash) == 1) {
                    if (hold_valid_alias(atom)) {
                        struct hold_passwd_entry pw;
                        char subject[128];
                        if (hold_lookup_passwd_by_uid(geteuid(), &pw) == 0 && pw.name[0] &&
                            hold_checked_snprintf(subject, sizeof(subject), "%s", pw.name) == 0) {
                            char grant_hash[PROFILE_HASH_STR_LEN];
                            if (hold_subject_grant_hash_for(&pre_system_store, subject, atom, grant_hash) == 0) {
                                char token[1024];
                                if (build_cap_request_token("start", multi, !tail, token, sizeof(token)) != 0) {
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
                                                 interactive_stdin,
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
        int rc = hold_elevate_with_sudo_parsed(argv[0], owned, command, tail, console_mode, run.auto_remove, interactive_stdin, all, print_cmd, multi, multi_count, force_raw, cmd_argc, cmd_argv);
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
                                               !strcmp(command, "commit") || !strcmp(command, "shell")))) {
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
        bool run_tail = tail;
        if (run_tail_implicit && !console_mode && !interactive_stdin && cmd_argc == 1 &&
            token_names_detached_profile(&user_store, &system_store, cmd_argv[0])) {
            run_tail = false;
        }
        if (run.name && !saw_owned_delimiter && run.envc == 0 && run.portc == 0 && run.volumec == 0 && run.cap_addc == 0 && run.cap_dropc == 0) {
            rc = hold_cmd_start_action_name_options(&inv, &user_store, &system_store, argv[0], &start_store,
                                                    run_tail, console_mode, run.auto_remove, interactive_stdin,
                                                    multi, multi_count,
                                                    run.restart_policy[0] ? run.restart_policy : NULL,
                                                    run.restart_delay_seconds,
                                                    run.name,
                                                    run.log_destination,
                                                    false,
                                                    cmd_argc, cmd_argv);
        } else if (run.name) {
            struct hold_start_options opts = start_options_from_run(&run, run_tail, console_mode,
                                                                    interactive_stdin, cmd_argc, cmd_argv);
            rc = hold_perform_start_options(&inv, &start_store, &opts);
        } else if (saw_owned_delimiter || run.envc > 0 || run.portc > 0 || run.volumec > 0 || run.cap_addc > 0 || run.cap_dropc > 0 || run.log_destination) {
            if (!saw_owned_delimiter && cmd_argc == 1 && token_names_existing_profile(&user_store, &system_store, cmd_argv[0]) &&
                (run.envc > 0 || run.portc > 0 || run.volumec > 0 || run.cap_addc > 0 || run.cap_dropc > 0)) {
                fprintf(stderr,
                        "hold: error: -e runtime overrides for existing profiles are not supported yet; edit the profile or use -- to force an executable\n");
                rc = 5;
            } else if (!saw_owned_delimiter && cmd_argc == 1 && token_names_existing_profile(&user_store, &system_store, cmd_argv[0]) &&
                       run.log_destination) {
                rc = hold_cmd_start_action_name_options(&inv, &user_store, &system_store, argv[0], &start_store,
                                                        run_tail, console_mode, run.auto_remove, interactive_stdin,
                                                        multi, multi_count,
                                                        run.restart_policy[0] ? run.restart_policy : NULL,
                                                        run.restart_delay_seconds,
                                                        NULL,
                                                        run.log_destination,
                                                        false,
                                                        cmd_argc, cmd_argv);
            } else if (!saw_owned_delimiter && multi) {
                fprintf(stderr, "hold: error: --multi applies only to profile starts\n");
                rc = 5;
            } else {
                char *shell_argv[4];
                int start_argc = cmd_argc;
                char **start_argv = cmd_argv;
                if (!saw_owned_delimiter && cmd_argc == 1) {
                    shell_argv[0] = "sh";
                    shell_argv[1] = "-c";
                    shell_argv[2] = cmd_argv[0];
                    shell_argv[3] = NULL;
                    start_argc = 3;
                    start_argv = shell_argv;
                }
                struct hold_start_options opts = start_options_from_run(&run, run_tail, console_mode,
                                                                        interactive_stdin, start_argc, start_argv);
                rc = hold_perform_start_options(&inv, &start_store, &opts);
            }
        } else {
            rc = hold_cmd_start_action_name_options(&inv, &user_store, &system_store, argv[0], &start_store,
                                                    run_tail, console_mode, run.auto_remove, interactive_stdin,
                                                    multi, multi_count,
                                                    run.restart_policy[0] ? run.restart_policy : NULL,
                                                    run.restart_delay_seconds,
                                                    NULL,
                                                    run.log_destination,
                                                    true,
                                                    cmd_argc, cmd_argv);
        }
        free(cmd_argv);
        free_run_options(&run);
        return rc;
    }

    if (owned && !strcmp(command, "shell")) {
        int rc = hold_cmd_shell_action(&inv, inv.euid_root ? &system_store : &user_store);
        free(cmd_argv);
        return rc;
    }

    if (owned && !strcmp(command, "cli")) {
        int rc = hold_cmd_captive_action(&inv, inv.euid_root ? &system_store : &user_store, &system_store, argv[0]);
        free(cmd_argv);
        return rc;
    }

    if (owned && !strcmp(command, "commit")) {
        /* Docker parity: commit turns a run into a profile, like container -> image. */
        int rc = hold_cmd_alias_action(&inv, &user_store, &system_store, argv[0], cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (owned && (!strcmp(command, "export") || !strcmp(command, "import"))) {
        char **profile_argv = calloc((size_t)cmd_argc + 2, sizeof(*profile_argv));
        if (!profile_argv) {
            free(cmd_argv);
            return 3;
        }
        profile_argv[0] = (char *)command;
        for (int i = 0; i < cmd_argc; i++) {
            profile_argv[i + 1] = cmd_argv[i];
        }
        int rc = hold_cmd_profile_action(&inv, &user_store, cmd_argc + 1, profile_argv);
        free(profile_argv);
        free(cmd_argv);
        return rc;
    }

    if (!owned) {
        if (!force_raw && cmd_argc == 1 && token_names_existing_profile(&user_store, &system_store, cmd_argv[0])) {
            struct hold_store start_store;
            if (hold_ensure_start_store_for_command(&inv, requested_system, true, "start", cmd_argc, cmd_argv, &start_store) != 0) {
                if ((inv.euid_root || requested_system) &&
                    hold_start_target_is_within_invoking_home(&inv, true, "start", cmd_argc, cmd_argv)) {
                    hold_die_errno("hold: failed to init invoking-user storage");
                }
                hold_die_errno("hold: failed to init start storage");
            }
            int rc = hold_cmd_start_action_options(&inv,
                                                   &user_store,
                                                   &system_store,
                                                   argv[0],
                                                   &start_store,
                                                   tail,
                                                   console_mode,
                                                   run.auto_remove,
                                                   interactive_stdin,
                                                   multi,
                                                   multi_count,
                                                   run.restart_policy[0] ? run.restart_policy : NULL,
                                                   run.restart_delay_seconds,
                                                   cmd_argc,
                                                   cmd_argv);
            free_run_options(&run);
            return rc;
        }
        struct hold_store start_store;
        if (hold_ensure_start_store_for_command(&inv, requested_system, false, NULL, cmd_argc, cmd_argv, &start_store) != 0) {
            if ((inv.euid_root || requested_system) &&
                hold_start_target_is_within_invoking_home(&inv, false, NULL, cmd_argc, cmd_argv)) {
                hold_die_errno("hold: failed to init invoking-user storage");
            }
            hold_die_errno("hold: failed to init start storage");
        }
        if (run.name) {
            struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                    interactive_stdin, cmd_argc, cmd_argv);
            int rc = hold_perform_start_options(&inv, &start_store, &opts);
            free_run_options(&run);
            return rc;
        }
        struct hold_start_options opts = start_options_from_run(&run, tail, console_mode,
                                                                interactive_stdin, cmd_argc, cmd_argv);
        int rc = hold_perform_start_options(&inv, &start_store, &opts);
        free_run_options(&run);
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
            if (strcmp(op, "run") && strcmp(op, "start")) {
                bool p_detach = false, p_interactive = false, p_tty = false, p_privileged = false;
                bool p_allow_multi = false;
                char **p_env = NULL;
                int p_envc = 0;
                char **p_volumes = NULL;
                int p_volumec = 0;
                char **p_cap_add = NULL;
                int p_cap_addc = 0;
                char **p_cap_drop = NULL;
                int p_cap_dropc = 0;
                char p_restart_policy[64] = {0};
                int p_restart_delay_seconds = 0;
                bool p_restart_delay_seen = false;
                const char *p_log_destination = NULL;
                int pi = 1;
                int parse_rc = 0;
                for (; pi < cmd_argc; pi++) {
                    const char *a = cmd_argv[pi];
                    if (!strcmp(a, "--")) {
                        pi++;
                        break;
                    }
                    if (parse_docker_run_flag(a, &p_detach, &p_interactive, &p_tty, &p_privileged)) {
                        if (p_privileged) {
                            fprintf(stderr, "hold: error: --privileged does not apply to user-local profile definitions\n");
                            parse_rc = 5;
                            break;
                        }
                        continue;
                    }
                    if (!strcmp(a, "--name")) {
                        fprintf(stderr, "hold: error: profile names are positional: hold profile <name> ...\n");
                        parse_rc = 5;
                        break;
                    }
                    if (!strcmp(a, "-e") || !strcmp(a, "--env")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --env/-e requires KEY=VALUE\n");
                            parse_rc = 5;
                            break;
                        }
                        parse_rc = append_env_assignment(cmd_argv[++pi], &p_env, &p_envc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strncmp(a, "--env=", 6)) {
                        parse_rc = append_env_assignment(a + 6, &p_env, &p_envc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strcmp(a, "--env-file")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --env-file requires a path\n");
                            parse_rc = 5;
                            break;
                        }
                        parse_rc = append_env_file(cmd_argv[++pi], &p_env, &p_envc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strncmp(a, "--env-file=", 11)) {
                        parse_rc = append_env_file(a + 11, &p_env, &p_envc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strcmp(a, "-p") || !strcmp(a, "--publish") ||
                        !strcmp(a, "-P") || !strcmp(a, "--publish-all") ||
                        !strncmp(a, "--publish=", 10)) {
                        parse_rc = reject_publish_option();
                        break;
                    }
                    if (!strcmp(a, "-v") || !strcmp(a, "--volume")) {
                        parse_rc = reject_volume_option();
                        break;
                    }
                    if (!strncmp(a, "--volume=", 9)) {
                        parse_rc = reject_volume_option();
                        break;
                    }
                    if (!strcmp(a, "--cap-add")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --cap-add requires a capability\n");
                            parse_rc = 5;
                            break;
                        }
                        parse_rc = append_string_option("--cap-add", cmd_argv[++pi], &p_cap_add, &p_cap_addc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strncmp(a, "--cap-add=", 10)) {
                        parse_rc = append_string_option("--cap-add", a + 10, &p_cap_add, &p_cap_addc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strcmp(a, "--cap-drop")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --cap-drop requires a capability\n");
                            parse_rc = 5;
                            break;
                        }
                        parse_rc = append_string_option("--cap-drop", cmd_argv[++pi], &p_cap_drop, &p_cap_dropc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strncmp(a, "--cap-drop=", 11)) {
                        parse_rc = append_string_option("--cap-drop", a + 11, &p_cap_drop, &p_cap_dropc);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strcmp(a, "--restart")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --restart requires a policy\n");
                            parse_rc = 5;
                            break;
                        }
                        parse_rc = parse_restart_policy_arg(cmd_argv[++pi], p_restart_policy);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strncmp(a, "--restart=", 10)) {
                        parse_rc = parse_restart_policy_arg(a + 10, p_restart_policy);
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strcmp(a, "--restart-delay")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --restart-delay requires seconds\n");
                            parse_rc = 5;
                            break;
                        }
                        parse_rc = parse_restart_delay_arg(cmd_argv[++pi], &p_restart_delay_seconds);
                        p_restart_delay_seen = parse_rc == 0;
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strncmp(a, "--restart-delay=", 16)) {
                        parse_rc = parse_restart_delay_arg(a + 16, &p_restart_delay_seconds);
                        p_restart_delay_seen = parse_rc == 0;
                        if (parse_rc != 0) break;
                        continue;
                    }
                    if (!strcmp(a, "--log-destination")) {
                        if (pi + 1 >= cmd_argc) {
                            fprintf(stderr, "hold: error: --log-destination requires a destination\n");
                            parse_rc = 5;
                            break;
                        }
                        p_log_destination = cmd_argv[++pi];
                        if (strcmp(p_log_destination, "syslog") && strcmp(p_log_destination, "json-file") && strcmp(p_log_destination, "local")) {
                            fprintf(stderr, "hold: error: unsupported log destination '%s'\n", p_log_destination);
                            parse_rc = 5;
                            break;
                        }
                        if (!strcmp(p_log_destination, "json-file") || !strcmp(p_log_destination, "local")) p_log_destination = NULL;
                        continue;
                    }
                    if (!strncmp(a, "--log-destination=", 18)) {
                        p_log_destination = a + 18;
                        if (strcmp(p_log_destination, "syslog") && strcmp(p_log_destination, "json-file") && strcmp(p_log_destination, "local")) {
                            fprintf(stderr, "hold: error: unsupported log destination '%s'\n", p_log_destination);
                            parse_rc = 5;
                            break;
                        }
                        if (!strcmp(p_log_destination, "json-file") || !strcmp(p_log_destination, "local")) p_log_destination = NULL;
                        continue;
                    }
                    if (!strcmp(a, "--allow-multi")) {
                        p_allow_multi = true;
                        continue;
                    }
                    if (!strcmp(a, "--multi")) {
                        p_allow_multi = true;
                        if (pi + 1 < cmd_argc) {
                            int ignored = 0;
                            if (hold_parse_positive_count(cmd_argv[pi + 1], &ignored)) {
                                pi++;
                            }
                        }
                        continue;
                    }
                    if (!strncmp(a, "--multi=", 8)) {
                        int ignored = 0;
                        if (!hold_parse_positive_count(a + 8, &ignored)) {
                            fprintf(stderr, "hold: error: invalid --multi count '%s'\n", a + 8);
                            parse_rc = 5;
                            break;
                        }
                        p_allow_multi = true;
                        continue;
                    }
                    if (!strcmp(a, "--rm") || !strcmp(a, "--detach-keys") || !strncmp(a, "--detach-keys=", 14)) {
                        fprintf(stderr, "hold: error: %s is a run-time option and is not stored by profile definitions yet\n", a);
                        parse_rc = 5;
                        break;
                    }
                    if (a[0] == '-') {
                        fprintf(stderr, "hold: error: unsupported profile option '%s'\n", a);
                        parse_rc = 5;
                        break;
                    }
                    break;
                }
                if (parse_rc == 0 && p_restart_delay_seen && !restart_policy_arg_enabled(p_restart_policy)) {
                    fprintf(stderr, "hold: error: --restart-delay requires --restart with an active policy\n");
                    parse_rc = 5;
                }
                int rc = parse_rc;
                if (rc == 0) {
                    if (pi >= cmd_argc) {
                        fprintf(stderr, "usage: hold profile <name> [profile-options] [--] <cmd> [args...]\n");
                        rc = 5;
                    } else {
                        rc = hold_cmd_profile_define_command(&inv,
                                                             &user_store,
                                                             name,
                                                             cmd_argc - pi,
                                                             cmd_argv + pi,
                                                             p_envc,
                                                             p_env,
                                                             p_volumec,
                                                             p_volumes,
                                                             p_cap_addc,
                                                             p_cap_add,
                                                             p_cap_dropc,
                                                             p_cap_drop,
                                                             p_interactive,
                                                             p_tty,
                                                             p_detach,
                                                             p_allow_multi,
                                                             p_restart_policy[0] ? p_restart_policy : NULL,
                                                             p_restart_delay_seconds,
                                                             p_log_destination);
                    }
                }
                hold_free_argv_alloc(p_env, p_envc);
                hold_free_argv_alloc(p_volumes, p_volumec);
                hold_free_argv_alloc(p_cap_add, p_cap_addc);
                hold_free_argv_alloc(p_cap_drop, p_cap_dropc);
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
                fprintf(stderr, "usage: hold profile run <name> [--multi N] [--tail|-f] [--console]\n");
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
                    if (i + 1 >= cmd_argc) {
                        fprintf(stderr, "hold: error: --multi requires a positive count\n");
                        free(cmd_argv);
                        return 5;
                    }
                    int parsed = 0;
                    if (!hold_parse_positive_count(cmd_argv[i + 1], &parsed)) {
                        fprintf(stderr, "hold: error: invalid --multi count '%s'\n", cmd_argv[i + 1]);
                        free(cmd_argv);
                        return 5;
                    }
                    p_multi = true;
                    p_multi_count = parsed;
                    i++;
                } else if (strncmp(cmd_argv[i], "--multi=", 8) == 0) {
                    p_multi = true;
                    if (!hold_parse_positive_count(cmd_argv[i] + 8, &p_multi_count)) {
                        fprintf(stderr, "hold: error: invalid --multi count '%s'\n", cmd_argv[i] + 8);
                        free(cmd_argv);
                        return 5;
                    }
                } else {
                    fprintf(stderr, "usage: hold profile run <name> [--multi N] [--tail|-f] [--console]\n");
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
            int rc = hold_cmd_start_action_options(&inv, &user_store, &system_store, argv[0], &start_store, p_tail, p_console, run.auto_remove, interactive_stdin, p_multi, p_multi_count, run.restart_policy[0] ? run.restart_policy : NULL, run.restart_delay_seconds, 1, start_argv);
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
        int rc = hold_cmd_start_action_options(&inv, &user_store, &system_store, argv[0], &start_store, tail, console_mode, run.auto_remove, interactive_stdin, multi, multi_count, run.restart_policy[0] ? run.restart_policy : NULL, run.restart_delay_seconds, cmd_argc, cmd_argv);
        free(cmd_argv);
        return rc;
    }

    if (!strcmp(command, "list")) {
        int rc;
        if (docker_ps_command) {
            if (cmd_argc != 0) {
                fprintf(stderr, "usage: hold ps [-a|--all]\n");
                free(cmd_argv);
                return 5;
            }
            rc = inv.euid_root ? hold_cmd_ps_system(&system_store, all)
                               : hold_cmd_ps_normal(&user_store, &system_store, all);
            free(cmd_argv);
            return rc;
        }
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
        if (target && target[0] == '-') {
            fprintf(stderr, "hold: error: unknown flag '%s'\n", target);
            print_command_usage_stderr("prune");
            free(cmd_argv);
            return 1;
        }
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
