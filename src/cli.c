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

static int help_profiles(void) {
    printf("sigmund help profiles\n\n"
           "Pin a run's exact command (resolved binary path + argv) under a reusable\n"
           "name.\n\n"
           "  sigmund alias <id> <name>       pin the command behind <id> as <name>\n"
           "  sigmund aliases [-v]            list visible aliases\n"
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
    } else if (!strcmp(action, "tail")) {
        printf("usage: sigmund tail <target>\n\nFollow live output for an alias match, or follow an id's log directly.\n");
    } else if (!strcmp(action, "console")) {
        printf("usage: sigmund console <target>\n\nAttach to a running console-enabled run.\n");
    } else if (!strcmp(action, "dump")) {
        printf("usage: sigmund dump <target>\n\nPrint a run log and exit.\n");
    } else if (!strcmp(action, "prune")) {
        printf("usage: sigmund prune [target|all] [--all]\n\nClear removable past run data. Running valid runs are never pruned.\n");
    } else if (!strcmp(action, "alias")) {
        printf("usage: sigmund alias <id> <name> [-v]\n\nPin the command behind a run id as a reusable alias.\n");
    } else if (!strcmp(action, "aliases")) {
        printf("usage: sigmund aliases [-v]\n\nList visible aliases. User aliases show commands; system commands are redacted.\n");
    } else if (!strcmp(action, "grant") || !strcmp(action, "revoke")) {
        printf("usage: sigmund %s <alias> <user> [start,stop,kill,tail,dump,prune,console]\n\nManage Sigmund-owned sudoers access for a root-managed alias.\n", action);
    } else {
        return -1;
    }
    return 0;
}

int show_help(const char *topic) {
    if (!topic || !*topic) {
        usage();
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

bool is_sigmund_owned_command(const char *s) {
    return s && (!strcmp(s, "list") || !strcmp(s, "stop") || !strcmp(s, "kill") ||
                 !strcmp(s, "tail") || !strcmp(s, "dump") || !strcmp(s, "prune") ||
                 !strcmp(s, "console") ||
                 !strcmp(s, "start") ||
                 !strcmp(s, "alias") || !strcmp(s, "aliases") ||
                 !strcmp(s, "grant") || !strcmp(s, "revoke") ||
                 !strcmp(s, "help"));
}

bool parse_positive_count(const char *s, int *out) {
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
