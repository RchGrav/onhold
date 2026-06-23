#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/cli.h"
#include "sigmund/runtime.h"

static int help_profiles(void);
static int help_targets(void);
static int help_access(void);
static int help_system(void);
static int help_scripting(void);
static int help_console(void);
static int help_action(const char *action);

enum {
    SIGMUND_CLI_ALLOW_ALL = 1 << 0,
    SIGMUND_CLI_ALLOW_DDASH = 1 << 1
};

struct sigmund_cli_command_spec {
    const char *name;
    int min_args;
    int max_args; /* -1 means unbounded. */
    unsigned flags;
    const char *usage;
    const char *help_topic;
};

static const struct sigmund_cli_command_spec command_specs[] = {
    {"list", 0, 1, 0, "usage: sigmund list [alias]", "list"},
    {"run", 1, -1, SIGMUND_CLI_ALLOW_DDASH, "usage: mund run [--tail|-f] [--console] -- <cmd> [args...]", "run"},
    {"start", 1, -1, SIGMUND_CLI_ALLOW_DDASH, "usage: sigmund start <alias> [--multi [N]] [--console]\n       sigmund start <cmd> [args...]", "start"},
    {"stop", 1, -1, SIGMUND_CLI_ALLOW_ALL, "usage: sigmund stop [--print] [--all] <target>...", "stop"},
    {"kill", 1, -1, SIGMUND_CLI_ALLOW_ALL, "usage: sigmund kill [--print] [--all] <target>...", "kill"},
    {"tail", 1, 1, 0, "usage: sigmund tail <target>", "tail"},
    {"logs", 1, 1, 0, "usage: mund logs <target>", "logs"},
    {"status", 0, 1, 0, "usage: mund status [profile|target]", "status"},
    {"inspect", 1, 1, 0, "usage: mund inspect <target>", "inspect"},
    {"dump", 1, 1, 0, "usage: sigmund dump <target>", "dump"},
    {"view", 1, -1, 0, "usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N]", "view"},
    {"console", 1, 1, 0, "usage: sigmund console <target>", "console"},
    {"prune", 0, 1, SIGMUND_CLI_ALLOW_ALL, "usage: sigmund prune [target|all] [--all]", "prune"},
    {"alias", 2, 3, 0, "usage: sigmund alias <id> <name> [-v]", "alias"},
    {"aliases", 0, 1, 0, "usage: sigmund aliases [-v]", "aliases"},
    {"profiles", 0, 1, 0, "usage: mund profiles [-v]", "profiles"},
    {"profile", 1, -1, SIGMUND_CLI_ALLOW_DDASH, "usage: mund profile <list|run|start|show|export|import> [args...]", "profile"},
    {"show", 1, 2, 0, "usage: mund show <runs|profiles|running|dormant|failed|stale> [name]", "show"},
    {"clean", 0, 1, SIGMUND_CLI_ALLOW_ALL, "usage: mund clean [target|all]", "clean"},
    {"doctor", 0, 0, 0, "usage: mund doctor", "doctor"},
    {"shell", 0, 0, 0, "usage: mund shell", "shell"},
    {"grant", 2, 3, 0, "usage: sigmund grant <alias> <user> [start,stop,kill,tail,dump,prune,console]", "grant"},
    {"revoke", 2, 3, 0, "usage: sigmund revoke <alias> <user> [start,stop,kill,tail,dump,prune,console]", "revoke"},
    {"help", 0, 1, 0, "usage: sigmund help [topic]", "help"},
};

static const struct sigmund_cli_command_spec *find_command_spec(const char *s) {
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

static int help_profiles(void) {
    printf("sigmund help profiles\n\n"
           "Pin a run's exact command (resolved binary path + argv) under a reusable\n"
           "name.\n\n"
           "  sigmund alias <id> <name>       pin the command behind <id> as <name>\n"
           "  sigmund aliases [-v]            list visible aliases\n"
           "  sigmund profile export <name>   export a typed-shell transcript\n"
           "  sigmund profile import <file>   import/update a user-local profile\n"
           "  sigmund start <name>            start a fresh run under that name\n\n"
           "The name is also recorded on runs started as <name>, so later\n"
           "list, tail, console, dump, stop, kill, and prune commands can use <name>. If the command behind\n"
           "<name> is updated later, future starts use the updated command; prior runs\n"
           "remain under the same recorded alias label.\n");
    return 0;
}

static int help_targets(void) {
    printf("sigmund help targets\n\n"
           "  <target>          resolve in the current context\n"
           "  user:<target>     force user-local lookup\n"
           "  system:<target>   force root-managed lookup\n\n"
           "target = run id, leading id prefix, or alias name\n\n"
           "A run id addresses one run directly, always. An alias resolves among runs\n"
           "recorded under that name, narrowed by the verb: stop/kill/tail look at\n"
           "running runs, console looks at running console-enabled runs, dump looks at\n"
           "logged runs, and prune looks at removable past\n"
           "run data. One match acts. Several matches exit 6 and print candidates;\n"
           "--all resolves that ambiguity for stop, kill, and prune. A known alias with\n"
           "nothing to do exits 0.\n");
    return 0;
}

static int help_access(void) {
    printf("sigmund help access\n\n"
           "Grant another user permission to act on a specific root-managed alias as\n"
           "root, without a password, scoped to one immutable protected profile.\n\n"
           "  sigmund grant  <alias> <user> [actions]\n"
           "  sigmund revoke <alias> <user> [actions]\n\n"
           "actions = any of: start,stop,kill,tail,dump,prune,console   (default: all)\n\n"
           "The <user> field may be a username, %%group, or all. Sigmund stores one\n"
           "managed sudoers file per alias/user pair. The file contains the current\n"
           "protected profile hash for that alias, an anchored action alternation, and\n"
           "an 8-hex run selector slot. If root updates the alias profile and the hash\n"
           "changes, grant rewrites the same managed file via temp file, visudo check,\n"
           "and atomic rename.\n");
    return 0;
}

static int help_system(void) {
    printf("sigmund help system\n\n"
           "Root, sudo, and --system runs use the root-managed store:\n\n"
           "  Linux: /var/lib/sigmund\n"
           "  macOS: /var/db/sigmund\n\n"
           "Private root records, logs, and profiles stay root-only. Normal users see\n"
           "only the redacted public index and public alias dictionary. A normal action\n"
           "on a root-only public target self-elevates through sudo; user-local targets\n"
           "win over root-public collisions.\n\n"
           "  sigmund --system <cmd...>       start in root-managed state\n"
           "  sigmund --system list           list authoritative root records\n");
    return 0;
}

static int help_scripting(void) {
    printf("sigmund help scripting\n\n"
           "stdout is for machine data. Human banners, confirmations, warnings, and\n"
           "errors go to stderr; --quiet suppresses normal human status.\n\n"
           "  id=$(sigmund <cmd...>)          capture the bare 8-hex run id\n"
           "  sigmund stop --print <id>       print kill -TERM -- -<pgid>\n"
           "  sigmund kill --print <id>       print kill -KILL -- -<pgid>\n\n"
           "Exit codes:\n"
           "  0  success (includes known alias with nothing to do)\n"
           "  1  usage / generic error\n"
           "  2  refused for safety\n"
           "  3  permission denied or storage/security failure\n"
           "  4  signal delivery failed\n"
           "  5  target not found or invalid target\n"
           "  6  must disambiguate\n");
    return 0;
}

static int help_console(void) {
    printf("sigmund help console\n\n"
           "Start a run with an attachable PTY console, then reconnect to it later.\n"
           "Console output is still tee'd to the normal log, so tail and dump continue\n"
           "to work.\n\n"
           "  sigmund --console <cmd...>      start with an attachable console\n"
           "  sigmund start <alias> --console start an alias with a console\n"
           "  sigmund console <target>        attach to that console\n\n"
           "Console attach is native: Sigmund saves your terminal, enters an alternate\n"
           "screen for interactive attaches, forwards terminal size changes to the PTY,\n"
           "and restores your original screen on exit. Ctrl-] detaches without asking\n"
           "Sigmund to stop the run.\n");
    return 0;
}

static int help_action(const char *action) {
    if (!strcmp(action, "list")) {
        printf("usage: sigmund list [alias] [--iso|-l]\n\nShow all visible runs, optionally filtered by recorded alias label.\n");
    } else if (!strcmp(action, "start")) {
        printf("usage: sigmund start <alias> [--multi [N]] [--console]\n       sigmund start <cmd> [args...]\n\nStart an alias recipe, or use explicit start form for a raw command.\n");
    } else if (!strcmp(action, "stop")) {
        printf("usage: sigmund stop [--print] [--all] <target>...\n\nGracefully stop matching runs with TERM, then KILL if needed.\n");
    } else if (!strcmp(action, "kill")) {
        printf("usage: sigmund kill [--print] [--all] <target>...\n\nForce matching runs down with KILL.\n");
    } else if (!strcmp(action, "run")) {
        printf("usage: mund run [--tail|-f] [--console] -- <cmd> [args...]\n\nStart an ad-hoc command explicitly. The -- delimiter keeps command args unambiguous.\n");
    } else if (!strcmp(action, "tail") || !strcmp(action, "logs")) {
        printf("usage: %s <target>\n\nFollow live output for a profile match, or follow an id's log directly.\n", !strcmp(action, "logs") ? "mund logs" : "sigmund tail");
    } else if (!strcmp(action, "console")) {
        printf("usage: sigmund console <target>\n\nAttach to a running console-enabled run.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: sigmund dump <target>\n\nPrint a run log and exit.\n");
    } else if (!strcmp(action, "view")) {
        printf("usage: mund view <target> [--filter TEXT] [--similar TEXT] [--limit N] [--debug-stats]\n\nPrint the first lazily discovered matching log lines. --filter is literal; --similar may be repeated with example lines.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: sigmund prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "alias")) {
        printf("usage: sigmund alias <id> <name> [-v]\n\nPin the command behind a run id as a reusable alias.\n");
    } else if (!strcmp(action, "aliases") || !strcmp(action, "profiles")) {
        printf("usage: %s [-v]\n\nList visible profiles. User profiles show commands; system commands are redacted.\n", !strcmp(action, "profiles") ? "mund profiles" : "sigmund aliases");
    } else if (!strcmp(action, "profile")) {
        printf("usage: mund profile <list|run|start|show|export|import> [args...]\n\nWork with profile definitions and profile-backed runs. Import/export supports typed-shell transcripts:\n  profile <name>\n  set command -- <argv...>\n  save\n");
    } else if (!strcmp(action, "status")) {
        printf("usage: mund status [profile|target]\n\nShow runs, optionally narrowed by profile/target.\n");
    } else if (!strcmp(action, "inspect")) {
        printf("usage: mund inspect <target>\n\nPrint a target log/record-oriented inspection view.\n");
    } else if (!strcmp(action, "show")) {
        printf("usage: mund show <runs|profiles|running|dormant|failed|stale> [name]\n\nNavigate alternate views of the same runtime tree.\n");
    } else if (!strcmp(action, "clean")) {
        printf("usage: mund clean [target|all]\n\nClear removable past run data.\n");
    } else if (!strcmp(action, "doctor")) {
        printf("usage: mund doctor\n\nCheck local Sigmund/Mund paths and build identity.\n");
    } else if (!strcmp(action, "shell")) {
        printf("usage: mund shell\n\nEnter the captive operator shell. Slash views map to normal commands: /profiles, /runs, /running, /stale.\n");
    } else if (!strcmp(action, "grant") || !strcmp(action, "revoke")) {
        printf("usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n\nManage Sigmund-owned sudoers access for a root-managed alias.\n", action);
    } else {
        return -1;
    }
    return 0;
}

int sigmund_show_help(const char *topic) {
    if (!topic || !*topic) {
        sigmund_usage();
        return 0;
    }
    if (!strcmp(topic, "profiles")) return help_profiles();
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "access")) return help_access();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    if (!strcmp(topic, "console")) return help_console();
    if (help_action(topic) == 0) return 0;
    fprintf(stderr, "sigmund: unknown help topic '%s'\n", topic);
    return 5;
}

bool sigmund_is_sigmund_owned_command(const char *s) {
    return find_command_spec(s) != NULL;
}

bool sigmund_cli_command_allows_all(const char *s) {
    const struct sigmund_cli_command_spec *spec = find_command_spec(s);
    return spec && (spec->flags & SIGMUND_CLI_ALLOW_ALL);
}

const char *sigmund_cli_command_usage(const char *s) {
    const struct sigmund_cli_command_spec *spec = find_command_spec(s);
    return spec ? spec->usage : NULL;
}

int sigmund_validate_owned_command_arity(const char *command, int argc) {
    const struct sigmund_cli_command_spec *spec = find_command_spec(command);
    if (!spec) {
        return 0;
    }
    if (argc < spec->min_args || (spec->max_args >= 0 && argc > spec->max_args)) {
        fprintf(stderr, "%s\n", spec->usage);
        return 5;
    }
    return 0;
}

bool sigmund_parse_positive_count(const char *s, int *out) {
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
