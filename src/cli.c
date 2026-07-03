#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"

static int help_targets(void);
static int help_system(void);
static int help_scripting(void);
static int help_action(const char *action);

enum {
    HOLD_CLI_ALLOW_ALL = 1 << 0,
    HOLD_CLI_ALLOW_DDASH = 1 << 1
};

struct hold_cli_command_spec {
    const char *name;
    int min_args;
    int max_args; /* -1 means unbounded. */
    unsigned flags;
    const char *usage;
    const char *help_topic;
};

static const struct hold_cli_command_spec command_specs[] = {
    {"list", 0, 1, 0, "usage: hold list [name]", "list"},
    {"ps", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold ps [-a|--all]", "ps"},
    {"on", 0, 0, 0, "usage: hold on", "on"},
    {"off", 0, 0, 0, "usage: hold off", "off"},
    {"shell", 0, 0, 0, "usage: hold shell", "shell"},
    {"end", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold end [--print] [--all] <target>...", "end"},
    {"stop", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold stop [--print] [--all] <target>...", "stop"},
    {"kill", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold kill [--print] [--all] <target>...", "kill"},
    {"tail", 1, 1, 0, "usage: hold tail <target>", "tail"},
    {"logs", 1, -1, 0, "usage: hold logs <target> [--follow|-f] [--tail|-n N] [--plain|-p|--interactive]", "logs"},
    {"inspect", 1, 1, 0, "usage: hold inspect <target>", "inspect"},
    {"__view", 1, -1, 0, "usage: hold __view <target> [internal viewer test options]", "__view"},
    {"attach", 1, 1, 0, "usage: hold attach <target>", "attach"},
    {"console", 1, 1, 0, "usage: hold console <target>", "console"},
    {"purge", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold purge [<target>] [-a|--all] [--force]", "purge"},
    {"prune", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold purge [<target>] [-a|--all] [--force]", "prune"},
    {"rm", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold purge [<target>] [--force]", "rm"},
    {"drop", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold purge [<target>] [-a|--all] [--force]", "drop"},
    {"save", 1, 1, 0, "usage: hold save <target>", "save"},
    {"rename", 2, 2, 0, "usage: hold rename <target> <name>", "rename"},
    {"help", 0, 1, 0, "usage: hold help [topic]", "help"},
};

static const struct hold_cli_command_spec *find_public_command_spec(const char *s) {
    if (!s) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        if (!strcmp(command_specs[i].name, s)) {
            return &command_specs[i];
        }
    }
    return NULL;
}

static int help_targets(void) {
    printf("hold help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = call id, leading id prefix, or call name\n\n"
           "A call id addresses one call directly, always. A call name resolves to the\n"
           "single call recorded under that name, narrowed by the verb: end/kill/logs\n"
           "look at running or logged calls, inspect looks at retained records, and purge\n"
           "looks at removable past call data.\n");
    return 0;
}

static int help_system(void) {
    printf("hold help system\n\n"
           "Root and sudo calls use the root-managed store:\n\n"
           "  Linux: /var/lib/hold\n"
           "  macOS: /var/db/hold\n\n"
           "Private root records and logs stay root-only. Normal users see only the\n"
           "redacted public index. Acting on a root-managed target requires root;\n"
           "user-local targets win over root-public collisions.\n\n"
           "  sudo hold <cmd...>           start in root-managed state\n"
           "  sudo hold list               list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("hold help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(hold -d <cmd...>)       capture the bare 64-hex call id\n"
           "  hold end --print <id>        print kill -TERM -- -<pgid>\n"
           "  hold kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known name with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: hold list [name] [--iso|-l]\n\nShow all visible calls, optionally filtered by name.\n");
    } else if (!strcmp(action, "ps")) {
        printf("usage: hold ps [-a|--all]\n\nDocker-shaped call listing. Shows Hold call IDs and names.\n");
    } else if (!strcmp(action, "on") || !strcmp(action, "shell")) {
        printf("usage: hold on\n\nStart a guarded shell under Hold's PTY/session wrapper. Hold holds the line: pressing the classic detach sequence Ctrl-P Ctrl-Q puts the current foreground program on hold as a call and returns to the shell. 'hold off' or exit ends the session. (shell is an alias of on.)\n");
    } else if (!strcmp(action, "off")) {
        printf("usage: hold off\n\nEnd the current 'hold on' session cleanly. Only works inside a hold on session.\n");
    } else if (!strcmp(action, "end") || !strcmp(action, "stop")) {
        printf("usage: hold end [--print] [--all] <target>...\n\nEnd matching calls politely: TERM, then KILL if needed. (stop is an alias.)\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: hold kill [--print] [--all] <target>...\n\nForce matching calls down with KILL.\n");
    } else if (!strcmp(action, "tail")) {
        printf("usage: hold tail <target>\n\nFollow a call's live output. Shorthand for hold logs <target> -f.\n");
    } else if (!strcmp(action, "logs")) {
        printf("usage: hold logs <target> [--follow|-f] [--tail|-n N] [--plain|-p|--interactive]\n\nOpen the log viewer for a call. In a TTY, type directly in the full-screen viewer to filter dynamically; Backspace relaxes the filter, Space excludes lines like the highlighted line, and Ctrl-R resets filters. Non-TTY output stays script-friendly; -p/--print/--plain always dumps plain text.\n");
    } else if (!strcmp(action, "console") || !strcmp(action, "attach")) {
        printf("usage: hold attach <target>\n\nPick a running console/TTY call back up. Detach again with Ctrl-P Ctrl-Q. Start attachable calls with hold -it <cmd>. (console is an alias of attach.)\n");
    } else if (!strcmp(action, "__view")) {
        printf("usage: hold __view <target> [internal viewer test options]\n\nInternal regression/debug entrypoint for the log viewer engine. The product UX is hold logs <target>, then type inside the full-screen viewer to filter dynamically.\n");
    } else if (!strcmp(action, "purge") || !strcmp(action, "prune") || !strcmp(action, "rm") || !strcmp(action, "drop")) {
        printf("usage: hold purge [<target>] [-a|--all] [--force]\n\nThe one removal verb. With no target it sweeps ended calls (-a includes stale); a target removes one call; --force removes regardless of state (live or saved). rm, prune, and drop are accepted aliases.\n");
    } else if (!strcmp(action, "inspect")) {
        printf("usage: hold inspect <target>\n\nPrint structured JSON details for a call. Log text belongs to hold logs <target> --plain.\n");
    } else if (!strcmp(action, "save")) {
        printf("usage: hold save <target>\n\nProtect a call from purge. There is no unsave; use hold purge --force to remove a saved call.\n");
    } else if (!strcmp(action, "rename")) {
        printf("usage: hold rename <target> <name>\n\nRename a call. Live and ended calls are both renameable.\n");
    } else {
        return -1;
    }
    return 0;
}

int hold_show_help(const char *topic) {
    if (!topic || !*topic) {
        hold_usage();
        return 0;
    }
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "hold: unknown help topic '%s'\n", topic);
    return 5;
}

bool hold_cli_command_is_parser_owned(const char *s) {
    return find_public_command_spec(s) != NULL;
}

bool hold_cli_command_is_public(const char *s) {
    return find_public_command_spec(s) != NULL;
}

bool hold_cli_command_allows_all(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec && (spec->flags & HOLD_CLI_ALLOW_ALL);
}

const char *hold_cli_command_usage(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec ? spec->usage : NULL;
}

int hold_validate_owned_command_arity(const char *command, int argc) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    if (!spec) {
        return 0;
    }
    if (argc < spec->min_args || (spec->max_args >= 0 && argc > spec->max_args)) {
        fprintf(stderr, "%s\n", spec->usage);
        return 5;
    }
    return 0;
}
