#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/core.h"
#include "hold/access.h"

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
    char *ports[CAPTIVE_MAX_ARGS];
    size_t port_count;
    char *volumes[CAPTIVE_MAX_ARGS];
    size_t volume_count;
    char *cap_add[CAPTIVE_MAX_ARGS];
    size_t cap_add_count;
    char *cap_drop[CAPTIVE_MAX_ARGS];
    size_t cap_drop_count;
    bool mode_interactive;
    bool mode_tty;
    bool mode_detach;
    bool allow_multi;
    bool has_restart_policy;
    bool has_restart_delay;
    char restart_policy[64];
    int restart_delay_seconds;
    bool has_log_destination;
    char log_destination[32];
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

static int reject_publish_config(void) {
    fprintf(stderr, "%% Hold is not containerized and does not publish or forward ports; listening ports are observed automatically in hold ps\n");
    return 5;
}

static int reject_volume_config(void) {
    fprintf(stderr, "%% Hold is not containerized and does not mount or remap volumes; pass host paths directly as argv/config paths\n");
    return 5;
}

static void stage_clear(struct captive_profile_stage *stage) {
    for (size_t i = 0; i < stage->arg_count; i++) free(stage->args[i]);
    for (size_t i = 0; i < stage->env_count; i++) free(stage->env[i]);
    for (size_t i = 0; i < stage->port_count; i++) free(stage->ports[i]);
    for (size_t i = 0; i < stage->volume_count; i++) free(stage->volumes[i]);
    for (size_t i = 0; i < stage->cap_add_count; i++) free(stage->cap_add[i]);
    for (size_t i = 0; i < stage->cap_drop_count; i++) free(stage->cap_drop[i]);
    memset(stage, 0, sizeof(*stage));
}

static int stage_load_recipe(struct captive_profile_stage *stage, const struct hold_store *store) {
    struct hold_profile recipe;
    if (hold_alias_lookup_recipe(store, stage->name, &recipe) != 0) {
        return 0;
    }
    int rc = 0;
    if (hold_checked_snprintf(stage->binary, sizeof(stage->binary), "%s", recipe.recipe.binary_path) != 0) {
        rc = 5;
        goto done;
    }
    if (recipe.recipe.argc > 1) {
        if ((size_t)(recipe.recipe.argc - 1) > CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Existing profile has too many argv tokens for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 1; i < recipe.recipe.argc; i++) {
            stage->args[stage->arg_count] = strdup(recipe.recipe.argv[i]);
            if (!stage->args[stage->arg_count]) {
                rc = 3;
                goto done;
            }
            stage->arg_count++;
        }
    }
    if (recipe.recipe.envc > 0) {
        if ((size_t)recipe.recipe.envc > CAPTIVE_MAX_ENV) {
            fprintf(stderr, "%% Existing profile has too many env entries for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 0; i < recipe.recipe.envc; i++) {
            stage->env[stage->env_count] = strdup(recipe.recipe.env[i]);
            if (!stage->env[stage->env_count]) {
                rc = 3;
                goto done;
            }
            stage->env_count++;
        }
    }
    if (recipe.recipe.portc > 0) {
        if ((size_t)recipe.recipe.portc > CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Existing profile has too many publish entries for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 0; i < recipe.recipe.portc; i++) {
            stage->ports[stage->port_count] = strdup(recipe.recipe.ports[i]);
            if (!stage->ports[stage->port_count]) {
                rc = 3;
                goto done;
            }
            stage->port_count++;
        }
    }
    if (recipe.recipe.volumec > 0) {
        if ((size_t)recipe.recipe.volumec > CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Existing profile has too many volume entries for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 0; i < recipe.recipe.volumec; i++) {
            stage->volumes[stage->volume_count] = strdup(recipe.recipe.volumes[i]);
            if (!stage->volumes[stage->volume_count]) {
                rc = 3;
                goto done;
            }
            stage->volume_count++;
        }
    }
    if (recipe.recipe.cap_addc > 0) {
        if ((size_t)recipe.recipe.cap_addc > CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Existing profile has too many cap-add entries for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 0; i < recipe.recipe.cap_addc; i++) {
            stage->cap_add[stage->cap_add_count] = strdup(recipe.recipe.cap_add[i]);
            if (!stage->cap_add[stage->cap_add_count]) {
                rc = 3;
                goto done;
            }
            stage->cap_add_count++;
        }
    }
    if (recipe.recipe.cap_dropc > 0) {
        if ((size_t)recipe.recipe.cap_dropc > CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Existing profile has too many cap-drop entries for captive editing\n");
            rc = 5;
            goto done;
        }
        for (int i = 0; i < recipe.recipe.cap_dropc; i++) {
            stage->cap_drop[stage->cap_drop_count] = strdup(recipe.recipe.cap_drop[i]);
            if (!stage->cap_drop[stage->cap_drop_count]) {
                rc = 3;
                goto done;
            }
            stage->cap_drop_count++;
        }
    }
    stage->mode_interactive = recipe.recipe.mode_interactive;
    stage->mode_tty = recipe.recipe.mode_tty;
    stage->mode_detach = recipe.recipe.mode_detach;
    stage->allow_multi = recipe.recipe.allow_multi;
    stage->has_restart_policy = recipe.recipe.has_restart_policy;
    stage->has_restart_delay = recipe.recipe.has_restart_delay;
    if (recipe.recipe.has_restart_policy) {
        snprintf(stage->restart_policy, sizeof(stage->restart_policy), "%s", recipe.recipe.restart_policy);
    }
    stage->restart_delay_seconds = recipe.recipe.restart_delay_seconds;
    stage->has_log_destination = recipe.recipe.has_log_destination;
    if (recipe.recipe.has_log_destination) {
        snprintf(stage->log_destination, sizeof(stage->log_destination), "%s", recipe.recipe.log_destination);
    }
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

static const char *prompt_text(const struct captive_session *s, char *buf, size_t buf_len) {
    if (s->mode == CAP_USER_EXEC) return "hold> ";
    if (s->mode == CAP_PRIV_EXEC) return "hold# ";
    if (s->mode == CAP_CONFIG) return "hold(config)# ";
    snprintf(buf, buf_len, "hold(config-profile:%s)# ", s->profile.name[0] ? s->profile.name : "?");
    return buf;
}

static void reset_terminal_input_modes(void) {
    /* A prior fullscreen viewer, terminal multiplexer, or interrupted child can
     * leave mouse tracking / bracketed paste enabled. The captive CLI is a
     * command shell, not a mouse UI, so reset those modes before reading lines
     * to prevent mouse movement from being echoed as literal escape text. */
    static const char reset[] =
        "\033[?1000l"  /* X10/button mouse */
        "\033[?1002l"  /* button-event mouse */
        "\033[?1003l"  /* any-event mouse */
        "\033[?1004l"  /* focus events */
        "\033[?1005l"  /* UTF-8 mouse */
        "\033[?1006l"  /* SGR mouse */
        "\033[?1015l"  /* urxvt mouse */
        "\033[?2004l"; /* bracketed paste */
    ssize_t ignored = write(STDOUT_FILENO, reset, sizeof(reset) - 1);
    (void)ignored;
}

enum captive_input_key {
    CAP_KEY_IGNORE = 0,
    CAP_KEY_LEFT,
    CAP_KEY_RIGHT,
    CAP_KEY_HOME,
    CAP_KEY_END,
    CAP_KEY_DELETE
};

static int read_byte_timeout(unsigned char *out, int timeout_ms) {
    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
        .revents = 0,
    };
    int ready;
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return 0;
    ssize_t n = read(STDIN_FILENO, out, 1);
    return n == 1 ? 1 : 0;
}

static void consume_bytes_timeout(size_t count) {
    unsigned char c;
    for (size_t i = 0; i < count; i++) {
        if (!read_byte_timeout(&c, 25)) return;
    }
}

static enum captive_input_key read_escape_key(void) {
    unsigned char c;
    if (!read_byte_timeout(&c, 25)) return CAP_KEY_IGNORE;

    if (c == 'M') { /* X10 mouse report: ESC M b x y */
        consume_bytes_timeout(3);
        return CAP_KEY_IGNORE;
    }

    if (c == 'O') { /* SS3 cursor/application-keypad mode. */
        if (!read_byte_timeout(&c, 25)) return CAP_KEY_IGNORE;
        if (c == 'D') return CAP_KEY_LEFT;
        if (c == 'C') return CAP_KEY_RIGHT;
        if (c == 'H') return CAP_KEY_HOME;
        if (c == 'F') return CAP_KEY_END;
        return CAP_KEY_IGNORE;
    }

    if (c != '[') return CAP_KEY_IGNORE;
    if (!read_byte_timeout(&c, 25)) return CAP_KEY_IGNORE;

    if (c == '<') { /* SGR mouse report: ESC [ < ... M/m */
        for (size_t i = 0; i < 64; i++) {
            if (!read_byte_timeout(&c, 25)) return CAP_KEY_IGNORE;
            if (c == 'M' || c == 'm') return CAP_KEY_IGNORE;
        }
        return CAP_KEY_IGNORE;
    }
    if (c == 'M') { /* Normal tracking mouse report: ESC [ M b x y */
        consume_bytes_timeout(3);
        return CAP_KEY_IGNORE;
    }

    if (c == 'D') return CAP_KEY_LEFT;
    if (c == 'C') return CAP_KEY_RIGHT;
    if (c == 'H') return CAP_KEY_HOME;
    if (c == 'F') return CAP_KEY_END;
    if (c == 'A' || c == 'B') return CAP_KEY_IGNORE;

    char params[32];
    size_t param_len = 0;
    if ((c >= '0' && c <= '9') || c == ';') {
        params[param_len++] = (char)c;
        for (size_t i = 0; i < sizeof(params) - 1; i++) {
            if (!read_byte_timeout(&c, 25)) return CAP_KEY_IGNORE;
            if ((c >= '0' && c <= '9') || c == ';') {
                params[param_len++] = (char)c;
                continue;
            }
            params[param_len] = '\0';
            if (c == '~') {
                if (!strcmp(params, "1") || !strcmp(params, "7")) return CAP_KEY_HOME;
                if (!strcmp(params, "4") || !strcmp(params, "8")) return CAP_KEY_END;
                if (!strcmp(params, "3")) return CAP_KEY_DELETE;
            }
            if (c == 'D') return CAP_KEY_LEFT;
            if (c == 'C') return CAP_KEY_RIGHT;
            if (c == 'H') return CAP_KEY_HOME;
            if (c == 'F') return CAP_KEY_END;
            return CAP_KEY_IGNORE;
        }
    }
    return CAP_KEY_IGNORE;
}

static void redraw_input_line(const char *prompt, const char *line, size_t len, size_t cursor) {
    fputc('\r', stdout);
    fputs(prompt, stdout);
    fwrite(line, 1, len, stdout);
    fputs("\033[K", stdout);
    if (len > cursor) fprintf(stdout, "\033[%zuD", len - cursor);
    fflush(stdout);
}

static int read_interactive_line(const struct captive_session *s, char *line, size_t line_size) {
    char prompt_buf[128];
    const char *prompt = prompt_text(s, prompt_buf, sizeof(prompt_buf));
    struct termios original;
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        fputs(prompt, stdout);
        fflush(stdout);
        return fgets(line, (int)line_size, stdin) ? 1 : 0;
    }

    struct termios raw = original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        fputs(prompt, stdout);
        fflush(stdout);
        return fgets(line, (int)line_size, stdin) ? 1 : 0;
    }

    size_t len = 0;
    size_t cursor = 0;
    line[0] = '\0';
    fputs(prompt, stdout);
    fflush(stdout);

    int result = 1;
    while (1) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 0) { result = 0; break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            result = 0;
            break;
        }

        if (c == '\r' || c == '\n') {
            fputc('\n', stdout);
            line[len] = '\0';
            break;
        }
        if (c == 3) { /* Ctrl-C: cancel the current line, keep the shell alive. */
            fputs("^C\n", stdout);
            line[0] = '\0';
            break;
        }
        if (c == 26) { /* Ctrl-Z: Cisco-style end to privileged EXEC. */
            fputc('\n', stdout);
            line[0] = (char)26;
            line[1] = '\0';
            break;
        }
        if (c == 4) { /* Ctrl-D */
            if (len == 0) { result = 0; break; }
            if (cursor < len) {
                memmove(line + cursor, line + cursor + 1, len - cursor);
                len--;
                redraw_input_line(prompt, line, len, cursor);
            }
            continue;
        }
        if (c == 1) { cursor = 0; redraw_input_line(prompt, line, len, cursor); continue; } /* Ctrl-A */
        if (c == 5) { cursor = len; redraw_input_line(prompt, line, len, cursor); continue; } /* Ctrl-E */
        if (c == 21) { len = 0; cursor = 0; line[0] = '\0'; redraw_input_line(prompt, line, len, cursor); continue; } /* Ctrl-U */
        if (c == 127 || c == 8) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor, len - cursor + 1);
                cursor--;
                len--;
                redraw_input_line(prompt, line, len, cursor);
            }
            continue;
        }
        if (c == 27) {
            enum captive_input_key key = read_escape_key();
            if (key == CAP_KEY_LEFT && cursor > 0) {
                cursor--;
                fputs("\033[D", stdout);
                fflush(stdout);
            } else if (key == CAP_KEY_RIGHT && cursor < len) {
                cursor++;
                fputs("\033[C", stdout);
                fflush(stdout);
            } else if (key == CAP_KEY_HOME) {
                cursor = 0;
                redraw_input_line(prompt, line, len, cursor);
            } else if (key == CAP_KEY_END) {
                cursor = len;
                redraw_input_line(prompt, line, len, cursor);
            } else if (key == CAP_KEY_DELETE && cursor < len) {
                memmove(line + cursor, line + cursor + 1, len - cursor);
                len--;
                redraw_input_line(prompt, line, len, cursor);
            }
            continue;
        }
        if (c < 32 && c != '\t') continue;
        if (len + 1 >= line_size) {
            fputc('\a', stdout);
            fflush(stdout);
            continue;
        }
        memmove(line + cursor + 1, line + cursor, len - cursor + 1);
        line[cursor] = (char)c;
        cursor++;
        len++;
        redraw_input_line(prompt, line, len, cursor);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    return result;
}

static void help_exec(bool priv) {
    printf("Exec commands:\n");
    printf("  attach      Attach to a compatible running instance when supported\n");
    printf("  console     Attach with a PTY (interactive)\n");
    printf("  enable      Enter privileged mode\n");
    printf("  list        List running instances\n");
    printf("  logs        View logs for a run/profile target\n");
    printf("  run         Start an instance from a profile\n");
    printf("  show        Show running system information\n");
    printf("  inspect     Show structured details for a run/profile target\n");
    printf("  stop        Stop a run/profile target\n");
    if (priv) {
        printf("  configure   Enter configuration mode\n");
        printf("  grant       Grant a user capability for a profile\n");
        printf("  revoke      Revoke a user capability\n");
        printf("  kill        Force-kill a run/profile target\n");
        printf("  prune       Remove inactive run data\n");
        printf("  write       Write running config to storage\n");
        printf("  disable     Return to user EXEC mode\n");
    }
    printf("  exit        Close the session\n");
}

static void help_config(void) {
    printf("Configuration commands:\n");
    printf("  profile     Create or edit a profile\n");
    printf("  pattern     Manage the pattern dictionary (reserved)\n");
    printf("  switch      Manage the switchionary (reserved)\n");
    printf("  no          Negate a command\n");
    printf("  default     Reset a command to default\n");
    printf("  exit        Exit configuration mode\n");
    printf("  end         Return to privileged EXEC mode\n");
}

static void help_profile(void) {
    printf("Profile configuration commands:\n");
    printf("  binary        Set the target executable\n");
    printf("  argv          Append base argv tokens\n");
    printf("  env           Set an environment variable\n");
    printf("  publish       Unsupported: ports are observed in hold ps\n");
    printf("  volume        Unsupported: pass host paths directly\n");
    printf("  alias         Define a profile-local alias (reserved)\n");
    printf("  param         Expose a validated optional parameter (reserved)\n");
    printf("  multi         Allow multiple concurrent instances\n");
    printf("  interactive   Keep stdin open for profile runs\n");
    printf("  tty           Allocate a pseudo-TTY for profile runs\n");
    printf("  console       Alias for tty\n");
    printf("  detach        Run profile in background by default\n");
    printf("  cap-add       Add a Linux capability to the profile launch config\n");
    printf("  cap-drop      Drop a Linux capability from the profile launch config\n");
    printf("  restart       Set restart policy (no|always|unless-stopped|on-failure[:N])\n");
    printf("  restart-delay Set restart delay seconds\n");
    printf("  log-destination Mirror logs to syslog, or reset to local/json-file\n");
    printf("  pty-shim      Start under the PTY shim (reserved)\n");
    printf("  no            Negate a command\n");
    printf("  default       Reset a command to default\n");
    printf("  info          Show staged profile state\n");
    printf("  commit        Validate and save profile\n");
    printf("  exit          Return to global config mode\n");
    printf("  end           Return to privileged EXEC mode\n");
}

static int cmd_show(struct captive_session *s, int argc, char **argv) {
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "?"))) {
        printf("  aliases    Show public alias table\n");
        printf("  runs       Show running instances\n");
        printf("  profiles   Show profile names and redacted metadata\n");
        printf("  version    Show hold version\n");
        return 0;
    }
    if (!strcmp(argv[1], "version")) {
        printf("hold version %s\n", HOLD_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "profiles") || !strcmp(argv[1], "aliases")) {
        return hold_cmd_aliases_action(s->inv, s->user_store, s->system_store, false);
    }
    if (!strcmp(argv[1], "runs") || !strcmp(argv[1], "running")) {
        return s->inv->euid_root ? hold_cmd_list_system(s->system_store, NULL, false)
                                 : hold_cmd_list_normal(s->user_store, s->system_store, NULL, false);
    }
    fprintf(stderr, "%% Unknown show command '%s'\n", argv[1]);
    return 5;
}

static int cmd_write(void) {
    printf("Building configuration...\n");
    printf("[OK]\n");
    return 0;
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
    printf("  publish   :");
    if (stage->port_count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < stage->port_count; i++) printf(" %s", stage->ports[i]);
    }
    printf("\n");
    printf("  volume    :");
    if (stage->volume_count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < stage->volume_count; i++) printf(" %s", stage->volumes[i]);
    }
    printf("\n");
    printf("  cap-add   :");
    if (stage->cap_add_count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < stage->cap_add_count; i++) printf(" %s", stage->cap_add[i]);
    }
    printf("\n");
    printf("  cap-drop  :");
    if (stage->cap_drop_count == 0) {
        printf(" -");
    } else {
        for (size_t i = 0; i < stage->cap_drop_count; i++) printf(" %s", stage->cap_drop[i]);
    }
    printf("\n");
    printf("  modes     :%s%s%s%s%s\n",
           stage->mode_interactive ? " interactive" : "",
           stage->mode_tty ? " tty" : "",
           stage->mode_detach ? " detach" : "",
           stage->allow_multi ? " multi" : "",
           (!stage->mode_interactive && !stage->mode_tty && !stage->mode_detach && !stage->allow_multi) ? " -" : "");
    printf("  restart   : %s", stage->has_restart_policy ? stage->restart_policy : "-");
    if (stage->has_restart_delay) printf(" delay=%d", stage->restart_delay_seconds);
    printf("\n");
    printf("  log-dest  : %s\n", stage->has_log_destination ? stage->log_destination : "-");
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

static void profile_clear_metadata(char **items, size_t *count, bool *dirty) {
    for (size_t i = 0; i < *count; i++) free(items[i]);
    memset(items, 0, sizeof(items[0]) * CAPTIVE_MAX_ARGS);
    *count = 0;
    *dirty = true;
}

static int profile_append_metadata(char **items, size_t *count, const char *kind, int argc, char **argv, bool *dirty) {
    if (argc < 2) {
        fprintf(stderr, "%% Usage: %s <value> [value...]\n", kind);
        return 5;
    }
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || !*argv[i]) {
            fprintf(stderr, "%% Usage: %s <value> [value...]\n", kind);
            return 5;
        }
        if (*count >= CAPTIVE_MAX_ARGS) {
            fprintf(stderr, "%% Too many %s entries\n", kind);
            return 5;
        }
        items[*count] = strdup(argv[i]);
        if (!items[*count]) return 3;
        (*count)++;
    }
    *dirty = true;
    return 0;
}

static int profile_remove_metadata(char **items, size_t *count, const char *kind, int argc, char **argv, bool *dirty) {
    if (argc == 2 || (argc == 3 && !strcmp(argv[2], "all"))) {
        profile_clear_metadata(items, count, dirty);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "%% Usage: no %s [spec|all]\n", kind);
        return 5;
    }
    for (size_t i = 0; i < *count; i++) {
        if (!strcmp(items[i], argv[2])) {
            free(items[i]);
            for (size_t j = i + 1; j < *count; j++) items[j - 1] = items[j];
            items[--(*count)] = NULL;
            *dirty = true;
            return 0;
        }
    }
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

static void profile_clear_argv(struct captive_profile_stage *stage) {
    for (size_t i = 0; i < stage->arg_count; i++) free(stage->args[i]);
    memset(stage->args, 0, sizeof(stage->args));
    stage->arg_count = 0;
    stage->dirty = true;
}

static int unsupported_reserved_profile_command(const char *name) {
    fprintf(stderr, "%% '%s' is reserved for the expanded profile schema and is not persisted by this build\n", name);
    return 5;
}

static int captive_parse_restart_policy(const char *arg, char out[64]) {
    if (!arg || !*arg) {
        fprintf(stderr, "%% Usage: restart <no|always|unless-stopped|on-failure[:N]>\n");
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
                fprintf(stderr, "%% Invalid restart retry count '%s'\n", arg + 11);
                return 5;
            }
        }
        snprintf(out, 64, "%s", arg);
        return 0;
    }
    fprintf(stderr, "%% Invalid restart policy '%s'\n", arg);
    return 5;
}

static int profile_set_restart(struct captive_profile_stage *stage, int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "%% Usage: restart <no|always|unless-stopped|on-failure[:N]>\n");
        return 5;
    }
    char policy[64];
    int rc = captive_parse_restart_policy(argv[1], policy);
    if (rc != 0) return rc;
    if (!strcmp(policy, "no")) {
        stage->restart_policy[0] = '\0';
        stage->has_restart_policy = false;
        stage->restart_delay_seconds = 0;
        stage->has_restart_delay = false;
    } else {
        snprintf(stage->restart_policy, sizeof(stage->restart_policy), "%s", policy);
        stage->has_restart_policy = true;
    }
    stage->dirty = true;
    return 0;
}

static int profile_set_restart_delay(struct captive_profile_stage *stage, int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "%% Usage: restart-delay <seconds>\n");
        return 5;
    }
    char *end = NULL;
    long n = strtol(argv[1], &end, 10);
    if (!end || *end || n < 0 || n > INT_MAX) {
        fprintf(stderr, "%% Invalid restart-delay '%s'\n", argv[1]);
        return 5;
    }
    if (!stage->has_restart_policy) {
        fprintf(stderr, "%% restart-delay requires a restart policy\n");
        return 5;
    }
    stage->restart_delay_seconds = (int)n;
    stage->has_restart_delay = n > 0;
    stage->dirty = true;
    return 0;
}

static void profile_clear_restart(struct captive_profile_stage *stage) {
    stage->restart_policy[0] = '\0';
    stage->has_restart_policy = false;
    stage->restart_delay_seconds = 0;
    stage->has_restart_delay = false;
    stage->dirty = true;
}

static int profile_set_log_destination(struct captive_profile_stage *stage, int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "%% Usage: log-destination <syslog|local|json-file>\n");
        return 5;
    }
    if (!strcmp(argv[1], "syslog")) {
        snprintf(stage->log_destination, sizeof(stage->log_destination), "%s", argv[1]);
        stage->has_log_destination = true;
        stage->dirty = true;
        return 0;
    }
    if (!strcmp(argv[1], "local") || !strcmp(argv[1], "json-file")) {
        stage->log_destination[0] = '\0';
        stage->has_log_destination = false;
        stage->dirty = true;
        return 0;
    }
    fprintf(stderr, "%% Unsupported log destination '%s'\n", argv[1]);
    return 5;
}

static void profile_clear_log_destination(struct captive_profile_stage *stage) {
    stage->log_destination[0] = '\0';
    stage->has_log_destination = false;
    stage->dirty = true;
}

static int profile_set_mode(struct captive_profile_stage *stage, const char *name, bool enabled) {
    bool *slot = NULL;
    if (!strcmp(name, "interactive")) {
        slot = &stage->mode_interactive;
    } else if (!strcmp(name, "tty") || !strcmp(name, "console")) {
        slot = &stage->mode_tty;
    } else if (!strcmp(name, "detach")) {
        slot = &stage->mode_detach;
    } else if (!strcmp(name, "multi")) {
        slot = &stage->allow_multi;
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
                                      (int)stage->port_count,
                                      stage->ports,
                                      (int)stage->volume_count,
                                      stage->volumes,
                                      (int)stage->cap_add_count,
                                      stage->cap_add,
                                      (int)stage->cap_drop_count,
                                      stage->cap_drop,
                                      stage->mode_interactive,
                                      stage->mode_tty,
                                      stage->mode_detach,
                                      stage->allow_multi,
                                      stage->has_restart_policy ? stage->restart_policy : NULL,
                                      stage->has_restart_delay ? stage->restart_delay_seconds : 0,
                                      stage->has_log_destination ? stage->log_destination : NULL) != 0) {
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
    if (argc == 2 && !strcmp(argv[1], "?")) {
        if (!strcmp(argv[0], "binary")) {
            printf("  WORD       Absolute path to the executable\n");
            return 0;
        }
        if (!strcmp(argv[0], "argv")) {
            printf("  WORD       Argument token to append\n");
            return 0;
        }
        if (!strcmp(argv[0], "env")) {
            printf("  WORD       KEY VALUE or KEY=VALUE\n");
            return 0;
        }
        if (!strcmp(argv[0], "publish")) {
            printf("  unsupported: Hold does not publish/forward ports; use host networking and hold ps\n");
            return 0;
        }
        if (!strcmp(argv[0], "volume")) {
            printf("  unsupported: Hold does not mount/remap paths; pass host paths directly\n");
            return 0;
        }
        if (!strcmp(argv[0], "param")) {
            printf("  WORD       Long flag to expose (e.g. --port)\n");
            return 0;
        }
        if (!strcmp(argv[0], "cap-add") || !strcmp(argv[0], "cap_add")) {
            printf("  WORD       Capability to add, e.g. NET_BIND_SERVICE\n");
            return 0;
        }
        if (!strcmp(argv[0], "cap-drop") || !strcmp(argv[0], "cap_drop")) {
            printf("  WORD       Capability to drop, or ALL\n");
            return 0;
        }
        if (!strcmp(argv[0], "restart")) {
            printf("  WORD       no | always | unless-stopped | on-failure[:N]\n");
            return 0;
        }
        if (!strcmp(argv[0], "restart-delay")) {
            printf("  WORD       Delay in seconds before restart\n");
            return 0;
        }
        if (!strcmp(argv[0], "log-destination") || !strcmp(argv[0], "log_destination")) {
            printf("  WORD       syslog | local | json-file\n");
            return 0;
        }
        if (!strcmp(argv[0], "no")) {
            printf("  env         Remove an environment variable or all env\n");
            printf("  publish     Clear legacy publish metadata\n");
            printf("  volume      Clear legacy volume metadata\n");
            printf("  cap-add     Clear one or all added capabilities\n");
            printf("  cap-drop    Clear one or all dropped capabilities\n");
            printf("  argv        Clear base argv tokens\n");
            printf("  interactive Disable stdin-open profile mode\n");
            printf("  tty         Disable pseudo-TTY profile mode\n");
            printf("  console     Alias for no tty\n");
            printf("  detach      Disable detached default profile mode\n");
            printf("  multi       Disable multi-instance profile mode\n");
            printf("  restart     Clear restart policy and delay\n");
            printf("  restart-delay Clear restart delay only\n");
            printf("  log-destination Clear log mirroring\n");
            return 0;
        }
        if (!strcmp(argv[0], "default")) {
            printf("  argv        Clear base argv tokens\n");
            printf("  env         Clear environment entries\n");
            printf("  publish     Clear legacy publish metadata\n");
            printf("  volume      Clear legacy volume metadata\n");
            printf("  cap-add     Clear added capabilities\n");
            printf("  cap-drop    Clear dropped capabilities\n");
            printf("  interactive Reset stdin-open mode to default\n");
            printf("  tty         Reset pseudo-TTY mode to default\n");
            printf("  console     Alias for default tty\n");
            printf("  detach      Reset detached mode to default\n");
            printf("  multi       Reset multi-instance mode to default\n");
            printf("  restart     Reset restart policy and delay to default\n");
            printf("  restart-delay Reset restart delay to default\n");
            printf("  log-destination Reset log destination to local/json-file\n");
            return 0;
        }
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
    if (!strcmp(argv[0], "publish")) {
        return reject_publish_config();
    }
    if (!strcmp(argv[0], "volume")) {
        return reject_volume_config();
    }
    if (!strcmp(argv[0], "cap-add") || !strcmp(argv[0], "cap_add")) {
        return profile_append_metadata(s->profile.cap_add, &s->profile.cap_add_count, "cap-add", argc, argv, &s->profile.dirty);
    }
    if (!strcmp(argv[0], "cap-drop") || !strcmp(argv[0], "cap_drop")) {
        return profile_append_metadata(s->profile.cap_drop, &s->profile.cap_drop_count, "cap-drop", argc, argv, &s->profile.dirty);
    }
    if (!strcmp(argv[0], "restart")) return profile_set_restart(&s->profile, argc, argv);
    if (!strcmp(argv[0], "restart-delay") || !strcmp(argv[0], "restart_delay_seconds")) return profile_set_restart_delay(&s->profile, argc, argv);
    if (!strcmp(argv[0], "log-destination") || !strcmp(argv[0], "log_destination")) return profile_set_log_destination(&s->profile, argc, argv);
    if (!strcmp(argv[0], "alias") || !strcmp(argv[0], "param") || !strcmp(argv[0], "pty-shim")) {
        return unsupported_reserved_profile_command(argv[0]);
    }
    if (argc == 1 && profile_set_mode(&s->profile, argv[0], true) == 0) return 0;
    if (!strcmp(argv[0], "no") && argc >= 2 && !strcmp(argv[1], "env")) return profile_no_env(&s->profile, argc, argv);
    if (!strcmp(argv[0], "no") && argc >= 2 && !strcmp(argv[1], "publish")) {
        return profile_remove_metadata(s->profile.ports, &s->profile.port_count, "publish", argc, argv, &s->profile.dirty);
    }
    if (!strcmp(argv[0], "no") && argc >= 2 && !strcmp(argv[1], "volume")) {
        return profile_remove_metadata(s->profile.volumes, &s->profile.volume_count, "volume", argc, argv, &s->profile.dirty);
    }
    if (!strcmp(argv[0], "no") && argc >= 2 && (!strcmp(argv[1], "cap-add") || !strcmp(argv[1], "cap_add"))) {
        return profile_remove_metadata(s->profile.cap_add, &s->profile.cap_add_count, "cap-add", argc, argv, &s->profile.dirty);
    }
    if (!strcmp(argv[0], "no") && argc >= 2 && (!strcmp(argv[1], "cap-drop") || !strcmp(argv[1], "cap_drop"))) {
        return profile_remove_metadata(s->profile.cap_drop, &s->profile.cap_drop_count, "cap-drop", argc, argv, &s->profile.dirty);
    }
    if (!strcmp(argv[0], "no") && argc == 2 && !strcmp(argv[1], "argv")) {
        profile_clear_argv(&s->profile);
        return 0;
    }
    if (!strcmp(argv[0], "no") && argc == 2 && !strcmp(argv[1], "restart")) {
        profile_clear_restart(&s->profile);
        return 0;
    }
    if (!strcmp(argv[0], "no") && argc == 2 && (!strcmp(argv[1], "restart-delay") || !strcmp(argv[1], "restart_delay_seconds"))) {
        s->profile.restart_delay_seconds = 0;
        s->profile.has_restart_delay = false;
        s->profile.dirty = true;
        return 0;
    }
    if (!strcmp(argv[0], "no") && argc == 2 && (!strcmp(argv[1], "log-destination") || !strcmp(argv[1], "log_destination"))) {
        profile_clear_log_destination(&s->profile);
        return 0;
    }
    if (!strcmp(argv[0], "no") && argc == 2) {
        int mode_rc = profile_set_mode(&s->profile, argv[1], false);
        if (mode_rc == 0) return 0;
        if (!strcmp(argv[1], "alias") || !strcmp(argv[1], "param") || !strcmp(argv[1], "pty-shim")) {
            return unsupported_reserved_profile_command(argv[1]);
        }
    }
    if (!strcmp(argv[0], "default") && argc == 2) {
        if (!strcmp(argv[1], "argv")) {
            profile_clear_argv(&s->profile);
            return 0;
        }
        if (!strcmp(argv[1], "env")) return profile_no_env(&s->profile, 2, argv);
        if (!strcmp(argv[1], "publish")) {
            profile_clear_metadata(s->profile.ports, &s->profile.port_count, &s->profile.dirty);
            return 0;
        }
        if (!strcmp(argv[1], "volume")) {
            profile_clear_metadata(s->profile.volumes, &s->profile.volume_count, &s->profile.dirty);
            return 0;
        }
        if (!strcmp(argv[1], "cap-add") || !strcmp(argv[1], "cap_add")) {
            profile_clear_metadata(s->profile.cap_add, &s->profile.cap_add_count, &s->profile.dirty);
            return 0;
        }
        if (!strcmp(argv[1], "cap-drop") || !strcmp(argv[1], "cap_drop")) {
            profile_clear_metadata(s->profile.cap_drop, &s->profile.cap_drop_count, &s->profile.dirty);
            return 0;
        }
        if (!strcmp(argv[1], "restart")) {
            profile_clear_restart(&s->profile);
            return 0;
        }
        if (!strcmp(argv[1], "restart-delay") || !strcmp(argv[1], "restart_delay_seconds")) {
            s->profile.restart_delay_seconds = 0;
            s->profile.has_restart_delay = false;
            s->profile.dirty = true;
            return 0;
        }
        if (!strcmp(argv[1], "log-destination") || !strcmp(argv[1], "log_destination")) {
            profile_clear_log_destination(&s->profile);
            return 0;
        }
        int mode_rc = profile_set_mode(&s->profile, argv[1], false);
        if (mode_rc == 0) return 0;
        if (!strcmp(argv[1], "alias") || !strcmp(argv[1], "param") || !strcmp(argv[1], "pty-shim")) {
            return unsupported_reserved_profile_command(argv[1]);
        }
        fprintf(stderr, "%% Unknown default target '%s'\n", argv[1]);
        return 5;
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
    if (!strcmp(argv[0], "write")) {
        if (!priv) {
            fprintf(stderr, "%% write requires privileged EXEC mode\n");
            return 5;
        }
        return cmd_write();
    }
    if (!strcmp(argv[0], "list") || !strcmp(argv[0], "ps")) {
        return s->inv->euid_root ? hold_cmd_list_system(s->system_store, NULL, false)
                                 : hold_cmd_list_normal(s->user_store, s->system_store, NULL, false);
    }
    if (!strcmp(argv[0], "run")) return cmd_run(s, argc, argv);
    if (!strcmp(argv[0], "logs")) return hold_cmd_view_action(s->inv, s->user_store, s->system_store, s->program, argc - 1, argv + 1);
    if (!strcmp(argv[0], "inspect")) {
        if (argc != 2) {
            fprintf(stderr, "%% Usage: %s <target>\n", argv[0]);
            return 5;
        }
        return hold_cmd_inspect_action(s->inv, s->user_store, s->system_store, s->program, argv[1]);
    }
    if (!strcmp(argv[0], "console") || !strcmp(argv[0], "attach")) {
        if (argc != 2) {
            fprintf(stderr, "%% Usage: %s <target>\n", argv[0]);
            return 5;
        }
        return hold_cmd_console_action(s->inv, s->user_store, s->system_store, s->program, argv[1]);
    }
    if (!strcmp(argv[0], "stop")) {
        if (argc != 2) {
            fprintf(stderr, "%% Usage: stop <target>\n");
            return 5;
        }
        return hold_cmd_signal_action(s->inv, s->user_store, s->system_store, s->program, "stop", 1, argv + 1, SIGTERM, true, false, false);
    }
    if (priv && !strcmp(argv[0], "kill")) {
        if (argc != 2) {
            fprintf(stderr, "%% Usage: kill <target>\n");
            return 5;
        }
        return hold_cmd_signal_action(s->inv, s->user_store, s->system_store, s->program, "kill", 1, argv + 1, SIGKILL, false, false, false);
    }
    if (priv && !strcmp(argv[0], "prune")) {
        if (argc > 2) {
            fprintf(stderr, "%% Usage: prune [target|all]\n");
            return 5;
        }
        const char *target = argc == 2 && strcmp(argv[1], "all") ? argv[1] : NULL;
        bool all = argc == 2 && !strcmp(argv[1], "all");
        return hold_cmd_prune_action(s->inv, s->user_store, s->system_store, s->program, target, all);
    }
    if (priv && (!strcmp(argv[0], "grant") || !strcmp(argv[0], "revoke"))) {
        return hold_cmd_grant_revoke_action(s->inv, s->system_store, s->program, !strcmp(argv[0], "grant"), argc - 1, argv + 1);
    }
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
    if (interactive) reset_terminal_input_modes();
    while (1) {
        if (interactive) {
            reset_terminal_input_modes();
            if (!read_interactive_line(&s, line, sizeof(line))) break;
        } else {
            if (!fgets(line, sizeof(line), stdin)) break;
        }
        if (!interactive && line[0] == '\0') continue;
        size_t len = strlen(line);
        if (!interactive && len > 0 && line[len - 1] != '\n' && !feof(stdin)) {
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
