#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/core.h"

#define CAPTIVE_MAX_TOKENS 128
#define CAPTIVE_MAX_ARGS 128
#define CAPTIVE_MAX_ENV 128

enum captive_mode {
    CAP_USER_EXEC = 0,
    CAP_PRIV_EXEC,
    CAP_CONFIG,
    CAP_PROFILE
};

struct captive_profile_stage {
    char name[ALIAS_MAX_LEN + 1];
    char binary[HOLD_PATH_MAX];
    char *args[CAPTIVE_MAX_ARGS];
    size_t arg_count;
    char *env[CAPTIVE_MAX_ENV];
    size_t env_count;
    bool mode_interactive;
    bool mode_tty;
    bool mode_detach;
    bool dirty;
};

struct captive_session {
    const struct hold_invocation *inv;
    const struct hold_store *user_store;
    const struct hold_store *system_store;
    const char *program;
    enum captive_mode mode;
    struct captive_profile_stage profile;
};

static void stage_clear(struct captive_profile_stage *stage) {
    for (size_t i = 0; i < stage->arg_count; i++) free(stage->args[i]);
    for (size_t i = 0; i < stage->env_count; i++) free(stage->env[i]);
    memset(stage, 0, sizeof(*stage));
}

static int stage_load_recipe(struct captive_profile_stage *stage, const struct hold_store *store) {
    struct hold_profile recipe;
    if (hold_alias_lookup_recipe(store, stage->name, &recipe) != 0) {
        return 0;
    }
    int rc = 0;
    if (hold_checked_snprintf(stage->binary, sizeof(stage->binary), "%s", recipe.binary_path) != 0) {
        rc = 5;
        goto done;
    }
    if (recipe.argc > 1) {
        if ((size_t)(recipe.argc - 1) > CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Existing profile has too many argv tokens for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 1; i < recipe.argc; i++) {
            stage->args[stage->arg_count] = strdup(recipe.argv[i]);
            if (!stage->args[stage->arg_count]) {
                rc = 3;
                goto done;
            }
            stage->arg_count++;
        }
    }
    if (recipe.envc > 0) {
        if ((size_t)recipe.envc > CAPTIVE_MAX_ENV) {
            fprintf(stderr, "%% Existing profile has too many env entries for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 0; i < recipe.envc; i++) {
            stage->env[stage->env_count] = strdup(recipe.env[i]);
            if (!stage->env[stage->env_count]) {
                rc = 3;
                goto done;
            }
            stage->env_count++;
        }
    }
    stage->mode_interactive = recipe.mode_interactive;
    stage->mode_tty = recipe.mode_tty;
    stage->mode_detach = recipe.mode_detach;
    stage->dirty = false;
done:
    hold_free_profile(&recipe);
    if (rc != 0) stage_clear(stage);
    return rc;
}

static int stage_set_name(struct captive_profile_stage *stage, const struct hold_store *store, const char *name) {
    stage_clear(stage);
    if (!hold_valid_alias(name)) {
        fprintf(stderr, "%% Invalid profile name '%s'\n", name ? name : "");
        return 5;
    }
    snprintf(stage->name, sizeof(stage->name), "%s", name);
    return stage_load_recipe(stage, store);
}

static int split_line(char *line, char **tokens, int *count) {
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '\n' || *p == '\r') break;
        if (n >= CAPTIVE_MAX_TOKENS) {
            fprintf(stderr, "%% Too many tokens\n");
            return 5;
        }
        char *out = p;
        char *in = p;
        tokens[n++] = out;
        char quote = 0;
        while (*in) {
            char c = *in++;
            if (quote) {
                if (c == quote) {
                    quote = 0;
                } else if (c == '\\' && quote != '\'' && *in) {
                    *out++ = *in++;
                } else {
                    *out++ = c;
                }
                continue;
            }
            if (c == '\n' || c == '\r' || isspace((unsigned char)c)) {
                break;
            }
            if (c == '\'' || c == '"') {
                quote = c;
                continue;
            }
            if (c == '\\' && *in) {
                *out++ = *in++;
                continue;
            }
            *out++ = c;
        }
        if (quote) {
            fprintf(stderr, "%% Unterminated quote\n");
            return 5;
        }
        *out = '\0';
        p = in;
    }
    *count = n;
    return 0;
}

static void print_prompt(const struct captive_session *s) {
    if (s->mode == CAP_USER_EXEC) {
        printf("hold> ");
    } else if (s->mode == CAP_PRIV_EXEC) {
        printf("hold# ");
    } else if (s->mode == CAP_CONFIG) {
        printf("hold(config)# ");
    } else {
        printf("hold(config-profile:%s)# ", s->profile.name);
    }
    fflush(stdout);
}

static void help_exec(bool priv) {
    printf("Exec commands:\n");
    printf("  enable      Enter privileged mode\n");
    if (priv) printf("  configure   Enter configuration mode\n");
    printf("  run         Start an instance from a profile\n");
    printf("  show        Show system information\n");
    printf("  list        List running instances\n");
    printf("  exit        Close the session\n");
    if (priv) printf("  disable     Return to user EXEC mode\n");
}

static void help_config(void) {
    printf("Configuration commands:\n");
    printf("  profile     Create or edit a profile\n");
    printf("  no          Negate a command\n");
    printf("  default     Reset a command to default\n");
    printf("  exit        Exit configuration mode\n");
    printf("  end         Return to privileged EXEC mode\n");
}

static void help_profile(void) {
    printf("Profile configuration commands:\n");
    printf("  binary      Set the target executable\n");
    printf("  argv        Append base argv tokens\n");
    printf("  env         Set an environment variable\n");
    printf("  interactive Keep stdin open when profile runs\n");
    printf("  tty         Allocate a pseudo-TTY when profile runs\n");
    printf("  console     Alias for tty\n");
    printf("  detach      Run profile in background by default\n");
    printf("  no env      Remove environment variables\n");
    printf("  no argv     Clear base argv tokens\n");
    printf("  no interactive|tty|console|detach\n");
    printf("  info        Show staged profile state\n");
    printf("  commit      Validate and save profile\n");
    printf("  exit        Return to global config mode\n");
    printf("  end         Return to privileged EXEC mode\n");
}

static int cmd_show(struct captive_session *s, int argc, char **argv) {
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "?"))) {
        printf("  runs       Show running instances\n");
        printf("  profiles   Show profile names\n");
        printf("  version    Show hold version\n");
        return 0;
    }
    if (!strcmp(argv[1], "version")) {
        printf("hold version %s\n", HOLD_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "profiles")) {
        return hold_cmd_aliases_action(s->inv, s->user_store, s->system_store, false);
    }
    if (!strcmp(argv[1], "runs") || !strcmp(argv[1], "running")) {
        return s->inv->euid_root ? hold_cmd_list_system(s->system_store, NULL, false)
                                 : hold_cmd_list_normal(s->user_store, s->system_store, NULL, false);
    }
    fprintf(stderr, "%% Unknown show command '%s'\n", argv[1]);
    return 5;
}

static int cmd_run(struct captive_session *s, int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "%% Usage: run <profile>\n");
        return 5;
    }
    return hold_cmd_start_action(s->inv,
                                 s->user_store,
                                 s->system_store,
                                 s->program,
                                 s->inv->euid_root ? s->system_store : s->user_store,
                                 false,
                                 false,
                                 false,
                                 1,
                                 1,
                                 &argv[1]);
}

static void profile_info(const struct captive_profile_stage *stage) {
    printf("  profile   : %s\n", stage->name);
    printf("  binary    : %s\n", stage->binary[0] ? stage->binary : "-");
    printf("  argv      :");
    if (stage->arg_count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < stage->arg_count; i++) printf(" %s", stage->args[i]);
    }
    printf("\n");
    printf("  env       :");
    if (stage->env_count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < stage->env_count; i++) printf(" %s", stage->env[i]);
    }
    printf("\n");
    printf("  modes     :%s%s%s%s\n",
           stage->mode_interactive ? " interactive" : "",
           stage->mode_tty ? " tty" : "",
           stage->mode_detach ? " detach" : "",
           (!stage->mode_interactive && !stage->mode_tty && !stage->mode_detach) ? " -" : "");
    printf("  staged    : %s\n", stage->dirty ? "uncommitted changes present" : "clean");
}

static int profile_append_args(struct captive_profile_stage *stage, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "%% Usage: argv <token> [token...]\n");
        return 5;
    }
    for (int i = 1; i < argc; i++) {
        if (stage->arg_count >= CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Too many argv tokens\n");
            return 5;
        }
        stage->args[stage->arg_count] = strdup(argv[i]);
        if (!stage->args[stage->arg_count]) return 3;
        stage->arg_count++;
    }
    stage->dirty = true;
    return 0;
}

static bool env_key_matches(const char *assignment, const char *key) {
    size_t key_len = key ? strlen(key) : 0;
    return assignment && key && strncmp(assignment, key, key_len) == 0 && assignment[key_len] == '=';
}

static int profile_set_env(struct captive_profile_stage *stage, int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "%% Usage: env <KEY> <VALUE> or env <KEY=VALUE>\n");
        return 5;
    }
    char assignment[4096];
    if (argc == 2) {
        const char *eq = strchr(argv[1], '=');
        if (!eq || eq == argv[1]) {
            fprintf(stderr, "%% Usage: env <KEY> <VALUE> or env <KEY=VALUE>\n");
            return 5;
        }
        if (strlen(argv[1]) >= sizeof(assignment)) return 5;
        memcpy(assignment, argv[1], strlen(argv[1]) + 1);
    } else {
        if (!argv[1] || !*argv[1] || strchr(argv[1], '=')) {
            fprintf(stderr, "%% Invalid environment key\n");
            return 5;
        }
        if (hold_checked_snprintf(assignment, sizeof(assignment), "%s=%s", argv[1], argv[2] ? argv[2] : "") != 0) {
            return 5;
        }
    }
    const char *eq = strchr(assignment, '=');
    size_t key_len = eq ? (size_t)(eq - assignment) : 0;
    char key[256];
    if (key_len == 0 || key_len >= sizeof(key)) {
        fprintf(stderr, "%% Invalid environment key\n");
        return 5;
    }
    memcpy(key, assignment, key_len);
    key[key_len] = '\0';
    for (size_t i = 0; i < stage->env_count; i++) {
        if (env_key_matches(stage->env[i], key)) {
            char *copy = strdup(assignment);
            if (!copy) return 3;
            free(stage->env[i]);
            stage->env[i] = copy;
            stage->dirty = true;
            return 0;
        }
    }
    if (stage->env_count >= CAPTIVE_MAX_ENV) {
        fprintf(stderr, "%% Too many env entries\n");
        return 5;
    }
    stage->env[stage->env_count] = strdup(assignment);
    if (!stage->env[stage->env_count]) return 3;
    stage->env_count++;
    stage->dirty = true;
    return 0;
}

static int profile_no_env(struct captive_profile_stage *stage, int argc, char **argv) {
    if (argc == 2) {
        for (size_t i = 0; i < stage->env_count; i++) free(stage->env[i]);
        memset(stage->env, 0, sizeof(stage->env));
        stage->env_count = 0;
        stage->dirty = true;
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "%% Usage: no env [KEY]\n");
        return 5;
    }
    for (size_t i = 0; i < stage->env_count; i++) {
        if (env_key_matches(stage->env[i], argv[2])) {
            free(stage->env[i]);
            for (size_t j = i + 1; j < stage->env_count; j++) stage->env[j - 1] = stage->env[j];
            stage->env[--stage->env_count] = NULL;
            stage->dirty = true;
            return 0;
        }
    }
    return 0;
}

static int profile_set_mode(struct captive_profile_stage *stage, const char *name, bool enabled) {
    bool *slot = NULL;
    if (!strcmp(name, "interactive")) {
        slot = &stage->mode_interactive;
    } else if (!strcmp(name, "tty") || !strcmp(name, "console")) {
        slot = &stage->mode_tty;
    } else if (!strcmp(name, "detach")) {
        slot = &stage->mode_detach;
    } else {
        return 1;
    }
    if (*slot != enabled) {
        *slot = enabled;
        stage->dirty = true;
    }
    return 0;
}

static int profile_commit(struct captive_session *s) {
    struct captive_profile_stage *stage = &s->profile;
    if (!stage->binary[0]) {
        fprintf(stderr, "%% Profile requires a binary before commit\n");
        return 5;
    }
    char binary_path[HOLD_PATH_MAX];
    if (hold_resolve_binary_path(stage->binary, binary_path, sizeof(binary_path)) != 0) {
        fprintf(stderr, "%% Cannot resolve binary '%s': %s\n", stage->binary, strerror(errno));
        return 5;
    }
    int argc = (int)stage->arg_count + 1;
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) return 3;
    argv[0] = binary_path;
    for (size_t i = 0; i < stage->arg_count; i++) argv[i + 1] = stage->args[i];
    if (hold_alias_upsert_recipe_full(s->user_store,
                                      stage->name,
                                      binary_path,
                                      argc,
                                      argv,
                                      (int)stage->env_count,
                                      stage->env,
                                      0,
                                      NULL,
                                      0,
                                      NULL,
                                      stage->mode_interactive,
                                      stage->mode_tty,
                                      stage->mode_detach,
                                      NULL,
                                      0) != 0) {
        free(argv);
        hold_die_errno("hold: failed to commit profile");
    }
    free(argv);
    stage->dirty = false;
    printf("Profile committed.\n");
    return 0;
}

static int handle_profile(struct captive_session *s, int argc, char **argv) {
    if (argc == 0) return 0;
    if (!strcmp(argv[0], "?") || !strcmp(argv[0], "help")) {
        help_profile();
        return 0;
    }
    if (!strcmp(argv[0], "exit")) {
        stage_clear(&s->profile);
        s->mode = CAP_CONFIG;
        return 0;
    }
    if (!strcmp(argv[0], "end")) {
        stage_clear(&s->profile);
        s->mode = CAP_PRIV_EXEC;
        return 0;
    }
    if (!strcmp(argv[0], "binary")) {
        if (argc != 2 || argv[1][0] != '/') {
            fprintf(stderr, "%% Usage: binary <absolute-path>\n");
            return 5;
        }
        snprintf(s->profile.binary, sizeof(s->profile.binary), "%s", argv[1]);
        s->profile.dirty = true;
        return 0;
    }
    if (!strcmp(argv[0], "argv")) return profile_append_args(&s->profile, argc, argv);
    if (!strcmp(argv[0], "env")) return profile_set_env(&s->profile, argc, argv);
    if (argc == 1 && profile_set_mode(&s->profile, argv[0], true) == 0) return 0;
    if (!strcmp(argv[0], "no") && argc >= 2 && !strcmp(argv[1], "env")) return profile_no_env(&s->profile, argc, argv);
    if (!strcmp(argv[0], "no") && argc == 2) {
        int mode_rc = profile_set_mode(&s->profile, argv[1], false);
        if (mode_rc == 0) return 0;
    }
    if (!strcmp(argv[0], "no") && argc == 2 && !strcmp(argv[1], "argv")) {
        for (size_t i = 0; i < s->profile.arg_count; i++) free(s->profile.args[i]);
        memset(s->profile.args, 0, sizeof(s->profile.args));
        s->profile.arg_count = 0;
        s->profile.dirty = true;
        return 0;
    }
    if (!strcmp(argv[0], "info") || !strcmp(argv[0], "show")) {
        profile_info(&s->profile);
        return 0;
    }
    if (!strcmp(argv[0], "commit")) return profile_commit(s);
    fprintf(stderr, "%% Unknown profile command '%s'\n", argv[0]);
    return 5;
}

static int handle_config(struct captive_session *s, int argc, char **argv) {
    if (argc == 0) return 0;
    if (!strcmp(argv[0], "?") || !strcmp(argv[0], "help")) {
        help_config();
        return 0;
    }
    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "end")) {
        s->mode = CAP_PRIV_EXEC;
        return 0;
    }
    if (!strcmp(argv[0], "profile")) {
        if (argc == 1 || (argc == 2 && !strcmp(argv[1], "?"))) {
            printf("  WORD       Name of the profile to create or edit\n");
            return 0;
        }
        int rc = stage_set_name(&s->profile, s->user_store, argv[1]);
        if (rc == 0) s->mode = CAP_PROFILE;
        return rc;
    }
    fprintf(stderr, "%% Unknown configuration command '%s'\n", argv[0]);
    return 5;
}

static int handle_exec(struct captive_session *s, int argc, char **argv) {
    if (argc == 0) return 0;
    bool priv = s->mode == CAP_PRIV_EXEC;
    if (!strcmp(argv[0], "?") || !strcmp(argv[0], "help")) {
        help_exec(priv);
        return 0;
    }
    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "quit")) return 1000;
    if (!strcmp(argv[0], "enable")) {
        s->mode = CAP_PRIV_EXEC;
        return 0;
    }
    if (priv && !strcmp(argv[0], "disable")) {
        s->mode = CAP_USER_EXEC;
        return 0;
    }
    if (priv && !strcmp(argv[0], "configure")) {
        if (argc == 2 && !strcmp(argv[1], "?")) {
            printf("  terminal   Configure from the terminal\n");
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "terminal")) {
            printf("Enter configuration commands, one per line. End with CNTL/Z.\n");
            s->mode = CAP_CONFIG;
            return 0;
        }
        fprintf(stderr, "%% Usage: configure terminal\n");
        return 5;
    }
    if (!strcmp(argv[0], "show")) return cmd_show(s, argc, argv);
    if (!strcmp(argv[0], "list") || !strcmp(argv[0], "ps")) {
        return s->inv->euid_root ? hold_cmd_list_system(s->system_store, NULL, false)
                                 : hold_cmd_list_normal(s->user_store, s->system_store, NULL, false);
    }
    if (!strcmp(argv[0], "run")) return cmd_run(s, argc, argv);
    fprintf(stderr, "%% Unknown command '%s'\n", argv[0]);
    return 5;
}

int hold_cmd_captive_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *program) {
    struct captive_session s;
    memset(&s, 0, sizeof(s));
    s.inv = inv;
    s.user_store = user_store;
    s.system_store = system_store;
    s.program = program;
    s.mode = CAP_USER_EXEC;

    char line[4096];
    int last_rc = 0;
    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    while (1) {
        if (interactive) print_prompt(&s);
        if (!fgets(line, sizeof(line), stdin)) break;
        if (!interactive && line[0] == '\0') continue;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' && !feof(stdin)) {
            fprintf(stderr, "%% Input line too long\n");
            int ch;
            while ((ch = getchar()) != EOF && ch != '\n') {}
            last_rc = 5;
            continue;
        }
        char *tokens[CAPTIVE_MAX_TOKENS];
        int argc = 0;
        int rc = split_line(line, tokens, &argc);
        if (rc == 0) {
            if (argc == 1 && tokens[0][0] == 26) {
                s.mode = CAP_PRIV_EXEC;
                continue;
            }
            if (s.mode == CAP_CONFIG) rc = handle_config(&s, argc, tokens);
            else if (s.mode == CAP_PROFILE) rc = handle_profile(&s, argc, tokens);
            else rc = handle_exec(&s, argc, tokens);
        }
        if (rc == 1000) break;
        if (rc != 0) last_rc = rc;
    }
    stage_clear(&s.profile);
    return last_rc == 1000 ? 0 : last_rc;
}
