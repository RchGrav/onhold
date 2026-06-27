#include "hold/config.h"
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"

static int help_profiles(void);
static int help_targets(void);
static int help_access(void);
static int help_system(void);
static int help_scripting(void);
static int help_console(void);
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
    {"list", 0, 1, 0, "usage: hold list [profile]", "list"},
    {"ps", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold ps [-a|--all]", "ps"},
    {"run", 1, -1, HOLD_CLI_ALLOW_DDASH, "usage: hold run [run-options] <cmd|profile> [args...]", "run"},
    {"start", 1, -1, HOLD_CLI_ALLOW_DDASH, "usage: hold start <profile> [--force|--multi N] [--console]\n       hold start <cmd> [args...]", "start"},
    {"stop", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold stop [--print] [--all] <target>...", "stop"},
    {"kill", 1, -1, HOLD_CLI_ALLOW_ALL, "usage: hold kill [--print] [--all] <target>...", "kill"},
    {"tail", 1, 1, 0, "usage: hold tail <target>", "tail"},
    {"logs", 1, -1, 0, "usage: hold logs <target> [--follow|-f] [--tail|-n N] [--plain|--interactive]", "logs"},
    {"status", 0, 1, 0, "usage: hold status [profile|target]", "status"},
    {"inspect", 1, 1, 0, "usage: hold inspect <target>", "inspect"},
    {"dump", 1, 1, 0, "usage: hold dump <target>", "dump"},
    {"__view", 1, -1, 0, "usage: hold __view <target> [internal viewer test options]", "__view"},
    {"console", 1, 1, 0, "usage: hold console <target>", "console"},
    {"prune", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold prune [target|all] [--all]", "prune"},
    {"rm", 1, 1, 0, "usage: hold rm [--force] <inactive-runid|profile>", "rm"},
    {"profiles", 0, 1, 0, "usage: hold profiles [-v]", "profiles"},
    {"profile", 1, -1, HOLD_CLI_ALLOW_DDASH, "usage: hold profile <name> <show|start|run|create|set|export|rename|delete> [args...]\n       hold profile <run|start|save|show|export|import> [args...]", "profile"},
    {"export", 1, -1, 0, "usage: hold export <profile> [as <file>] [--format cli|json]", "export"},
    {"import", 1, -1, 0, "usage: hold import <file> [as <profile>] [--dry-run|--yes]", "import"},
    {"show", 1, 2, 0, "usage: hold show <runs|profiles|running|dormant|failed|stale> [name]", "show"},
    {"clean", 0, 1, HOLD_CLI_ALLOW_ALL, "usage: hold clean [target|all]", "clean"},
    {"doctor", 0, 0, 0, "usage: hold doctor", "doctor"},
    {"shell", 0, 0, 0, "usage: hold shell", "shell"},
    {"grant", 2, 3, 0, "usage: hold grant <profile> <user> [start,stop,kill,tail,dump,prune,console]", "grant"},
    {"revoke", 2, 3, 0, "usage: hold revoke <profile> <user> [start,stop,kill,tail,dump,prune,console]", "revoke"},
    {"help", 0, 1, 0, "usage: hold help [topic]", "help"},
};

static const char *retired_command_names[] = {
    "alias",
    "aliases",
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

static bool is_retired_command_name(const char *s) {
    if (!s) {
        return false;
    }
    for (size_t i = 0; i < sizeof(retired_command_names) / sizeof(retired_command_names[0]); i++) {
        if (!strcmp(retired_command_names[i], s)) {
            return true;
        }
    }
    return false;
}

static int help_profiles(void) {
    printf("hold help profiles\n\n"
           "Pin a run's exact command (resolved binary path + argv) under a reusable\n"
           "name.\n\n"
           "  hold profile <name> create -- <cmd>  create/update a saved command recipe\n"
           "  hold profile save <id> as <name>   save a recent run as a profile\n"
           "  hold profiles [-v]           list visible profiles\n"
           "  hold export <name>           export a Cisco IOS-style transcript\n"
           "  hold export <name> as FILE   write transcript to FILE\n"
           "  hold import FILE as <name>   import/update a user-local profile\n"
           "  hold run <name>              start a fresh run under that name\n\n"
           "The name is also recorded on runs started as <name>, so later\n"
           "ps, logs, inspect, stop, kill, rm, and prune commands can use <name>.\n"
           "If the command behind <name> is updated later, future starts use the updated command; prior runs\n"
           "remain under the same recorded profile label.\n");
    return 0;
}

static int help_targets(void) {
    printf("hold help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = run id, leading id prefix, or profile name\n\n"
           "A run id addresses one run directly, always. A profile name resolves among runs\n"
           "recorded under that name, narrowed by the verb: stop/kill/logs look at\n"
           "running or logged runs, inspect looks at retained records, and prune looks at removable past\n"
           "run data. One match acts. Several matches exit 6 and print candidates;\n"
           "--all resolves that ambiguity for stop, kill, and prune. A known profile with\n"
           "nothing to do exits 0.\n");
    return 0;
}

static int help_access(void) {
    printf("hold help access\n\n"
           "Grant another user permission to act on a specific root-managed profile as\n"
           "root, without a password, scoped to one immutable protected profile.\n\n"
           "  hold grant  <profile> <user> [actions]\n"
           "  hold revoke <profile> <user> [actions]\n\n"
           "actions = any of: start,stop,kill,tail,dump,prune,console   (default: all)\n\n"
           "The <user> field may be a username, %%group, or all. On Hold stores one\n"
           "managed sudoers file per profile/user pair. The file contains the current\n"
           "protected profile hash for that profile, an anchored action alternation, and\n"
           "a 12-hex run selector slot. If root updates the profile and the hash\n"
           "changes, grant rewrites the same managed file via temp file, visudo check,\n"
           "and atomic rename.\n");
    return 0;
}

static int help_system(void) {
    printf("hold help system\n\n"
           "Root, sudo, and --system runs use the root-managed store:\n\n"
           "  Linux: /var/lib/hold\n"
           "  macOS: /var/db/hold\n\n"
           "Private root records, logs, and profiles stay root-only. Normal users see\n"
           "only the redacted public index and public profile dictionary. A normal action\n"
           "on a root-only public target self-elevates through sudo; user-local targets\n"
           "win over root-public collisions.\n\n"
           "  hold --system <cmd...>       start in root-managed state\n"
           "  hold --system list           list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("hold help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(hold -d <cmd...>)       capture the bare 12-hex run id\n"
           "  hold stop --print <id>       print kill -TERM -- -<pgid>\n"
           "  hold kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known profile with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_console(void) {
    printf("hold help console\n\n"
           "Start a run with an attachable PTY console, then reconnect to it later.\n"
           "Console output is still tee'd to the normal log, so logs continue\n"
           "to work.\n\n"
           "  hold -t <cmd...>             start with an attachable PTY console\n"
           "  hold run -it <profile>       start/reconnect a profile with a TTY\n"
           "  hold console <target>        IOS/operator attach command when needed\n\n"
           "Console attach is native: On Hold saves your terminal, enters an alternate\n"
           "screen for interactive attaches, forwards terminal size changes to the PTY,\n"
           "and restores your original screen on exit. Ctrl-P Ctrl-Q detaches without asking\n"
           "On Hold to stop the run.\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: hold list [profile] [--iso|-l]\n\nShow all visible runs, optionally filtered by recorded profile label.\n");
    } else if (!strcmp(action, "ps")) {
        printf("usage: hold ps [-a|--all]\n\nDocker-shaped run listing. Shows Hold run IDs; profile-backed runs include their profile label where available.\n");
    } else if (!strcmp(action, "start")) {
        printf("usage: hold start <profile> [--force|--multi N] [--console]\n       hold start <cmd> [args...]\n\nStart a saved profile recipe, or use explicit start form for a raw command.\n");
    } else if (!strcmp(action, "stop")) {
        printf("usage: hold stop [--print] [--all] <target>...\n\nGracefully stop matching runs with TERM, then KILL if needed.\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: hold kill [--print] [--all] <target>...\n\nForce matching runs down with KILL.\n");
    } else if (!strcmp(action, "run")) {
        printf("usage: hold run [run-options] <cmd|profile> [args...]\n\nDocker-shaped launch. Without -d, Hold starts the run and follows its log in the foreground.\nCommon options:\n  -d, --detach          run in the background and print the run ID\n  -i, --interactive     keep non-PTY stdin open\n  -t, --tty             allocate Hold's PTY/console path\n  -e, --env KEY=VALUE   set launch environment\n      --env-file FILE   load KEY=VALUE launch environment lines\n  -p, --publish SPEC    record published-port metadata\n  -v, --volume SPEC     record volume/path metadata\n      --rm              remove run record/log after exit\n      --restart POLICY  restart rule: no|always|unless-stopped|on-failure[:N]\n      --restart-delay N delay seconds between restart attempts\n      --force           start one additional instance of a running profile\n      --multi N         start N instances of a profile\n      --name PROFILE    create/label a profile from this launch recipe\n      --detach-keys SEQ set TTY detach keys (default ctrl-p,ctrl-q)\n      --privileged      request Hold's elevated/root-managed path\nUse -- before a command whose name conflicts with a Hold command or option.\n");
    } else if (!strcmp(action, "tail") || !strcmp(action, "logs")) {
        if (!strcmp(action, "logs")) {
            printf("usage: hold logs <target> [--follow|-f] [--tail|-n N] [--plain|--interactive]\n\nOpen the log viewer for a run. In a TTY, type directly in the full-screen viewer to filter dynamically; Backspace relaxes the filter, Space excludes lines like the highlighted line, and Ctrl-R resets filters. Non-TTY output stays script-friendly.\n");
        } else {
            printf("usage: hold tail <target>\n\nFollow live output for a profile match, or follow an id's log directly.\n");
        }
    } else if (!strcmp(action, "console")) {
        printf("usage: hold console <target>\n\nAttach to a running console-enabled run. Shell examples should prefer Docker-shaped `hold run -it <profile>` or `hold -it <cmd>`.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: hold logs <target> --plain\n\nThe public 0.4 log-text command is `hold logs <target> --plain`; structured details are `hold inspect <target>`.\n");
    } else if (!strcmp(action, "__view")) {
        printf("usage: hold __view <target> [internal viewer test options]\n\nInternal regression/debug entrypoint for the log viewer engine. The product UX is hold logs <target>, then type inside the full-screen viewer to filter dynamically.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: hold prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "rm")) {
        printf("usage: hold rm [--force] <inactive-runid|profile>\n\nRemove an inactive run record/log or delete a user-local profile. With --force, stop and remove one concrete active run ID.\n");
    } else if (!strcmp(action, "profiles")) {
        printf("usage: hold profiles [-v]\n\nList visible profiles. User profiles show commands; system commands are redacted.\n");
    } else if (!strcmp(action, "export")) {
        printf("usage: hold export <profile> [as <file>] [--format cli|json]\n\nExport a profile as the Cisco IOS-style command transcript an operator would type in captive configuration mode. Use `as <file>` to write the transcript instead of stdout.\n");
    } else if (!strcmp(action, "import")) {
        printf("usage: hold import <file> [as <profile>] [--dry-run|--yes]\n\nImport a Cisco IOS-style profile transcript into canonical user-local profile storage. `as <profile>` overrides the transcript profile name; --dry-run validates without writing.\n");
    } else if (!strcmp(action, "profile")) {
        printf("usage: hold profile <name> <show|start|run|create|set|export|rename|delete> [args...]\n       hold profile <run|start|save|show|export|import> [args...]\n\nWork with profile definitions and profile-backed runs. The name-first editor supports:\n  hold profile web create -- /usr/bin/python3 -m http.server 9000\n  hold profile web set command -- /usr/bin/python3 -m http.server 9000\n  hold profile web export [--format cli|json]\n  hold profile save <runid> as web [-v]\n  hold profiles [-v]\n  hold profile web rename api\n  hold profile api delete\nImport/export supports Cisco IOS-style transcripts:\n  enable\n  configure terminal\n  profile web\n  binary /usr/bin/python3\n  argv -m http.server 9000\n  commit\n  end\n  write\n");
    } else if (!strcmp(action, "status")) {
        printf("usage: hold status [profile|target]\n\nShow runs, optionally narrowed by profile/target.\n");
    } else if (!strcmp(action, "inspect")) {
        printf("usage: hold inspect <target>\n\nPrint structured JSON details for a run/profile target. Log text belongs to `hold logs <target> --plain`.\n");
    } else if (!strcmp(action, "show")) {
        printf("usage: hold show <runs|profiles|running|dormant|failed|stale> [name]\n\nNavigate alternate views of the same runtime tree.\n");
    } else if (!strcmp(action, "clean")) {
        printf("usage: hold clean [target|all]\n\nClear removable past run data.\n");
    } else if (!strcmp(action, "doctor")) {
        printf("usage: hold doctor\n\nCheck local On Hold/Hold paths and build identity.\n");
    } else if (!strcmp(action, "shell")) {
        printf("usage: hold shell\n\nStart an ordinary user shell under Hold's PTY/session wrapper. Typing `exit` returns without creating a runid. Pressing the classic detach sequence Ctrl-P Ctrl-Q captures the current foreground process group as a Hold run and returns to the caller.\n");
    } else if (!strcmp(action, "grant") || !strcmp(action, "revoke")) {
        printf("usage: hold %s <profile> <user> [start,stop,kill,tail,dump,prune,console]\n\nManage On Hold-owned sudoers access for a root-managed profile.\n", action);
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
    if (!strcmp(topic, "profiles")) return help_profiles();
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "access")) return help_access();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "console")) return help_console();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "hold: unknown help topic '%s'\n", topic);
    return 5;
}

bool hold_cli_command_is_parser_owned(const char *s) {
    return find_public_command_spec(s) != NULL || is_retired_command_name(s);
}

bool hold_cli_command_is_public(const char *s) {
    return find_public_command_spec(s) != NULL;
}

bool hold_cli_command_allows_all(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec && (spec->flags & HOLD_CLI_ALLOW_ALL);
}

bool hold_cli_command_is_retired(const char *s) {
    return is_retired_command_name(s);
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

bool hold_parse_positive_count(const char *s, int *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < 1 || v > 1000) {
        return false;
    }
    *out = (int)v;
    return true;
}
