/* cli/specs.c — command_specs[] and the flag engine that runs off it.
 *
 * One row per spelled verb: aliases are rows whose canon names the dispatch
 * verb they fold onto. Arity, the per-command flag mask, the usage line, and
 * the help prose all live in the row, so parsing, arity guards, usage text,
 * and help are driven from this one table; no verb parses its own flags by
 * hand. */
#include "hold/config.h"
#include <stddef.h>
#include "hold/types.h"
#include "hold/cli.h"
#include "hold/runtime.h"

/* Per-command flag mask: which owned flags a verb consumes. */
enum {
    SPEC_ALL       = 1 << 0, /* --all; -a is accepted wherever --all is */
    SPEC_LIVE      = 1 << 1, /* -l/--live, list's running-only filter */
    SPEC_SCOPE     = 1 << 2, /* -s/--system and -u/--user store scope */
    SPEC_FORCE     = 1 << 3, /* --force (purge family) */
    SPEC_PRINT     = 1 << 4, /* --print (end/stop/kill) */
    SPEC_NOSTREAM  = 1 << 5, /* --no-stream (stats) */
    SPEC_STRICT    = 1 << 6, /* reject any unconsumed flag (list/ps) */
    SPEC_PS_DOCKER = 1 << 7  /* ps speaks only Docker: --system not consumed */
};

struct hold_cli_command_spec {
    const char *name;
    const char *canon;      /* dispatch verb aliases fold onto; NULL = itself */
    int min_args;
    int max_args;           /* -1 means unbounded. */
    unsigned flags;
    const char *usage;
    const char *help_usage; /* help's usage line when it differs; NULL = usage */
    const char *help;       /* prose body; NULL = not a help topic */
};

/* Help prose, shared between a verb and its aliases. Prose is content, not
 * code: only its duplicated usage lines compress (help prints the row's
 * usage line above the body). */
static const char help_list[] =
    "list is Hold's scoped ledger, with a USER column (CALL ID, USER, COMMAND, CREATED,\n"
    "STATUS, PORTS, NAMES). By default you see your own calls, live and past. Scope flags:\n"
    "  -u/--user     your personal calls only (even under sudo: the invoking user's)\n"
    "  -s/--system   the global (root-managed) calls only\n"
    "  -a/--all      both scopes together\n"
    "  -l/--live     narrow any of the above to running calls (composes)\n\n"
    "Global calls a normal user is not entitled to read appear redacted: USER and COMMAND\n"
    "read 'hidden'. Run as root, list defaults to the global calls; -a also walks every\n"
    "user's store. An optional name narrows the listing. (For Docker's running view, see ps.)\n";
static const char help_ps[] =
    "ps is Docker's machine-wide view: running calls (add -a for ended too) across both your\n"
    "own calls and the global ones, rendered as Docker's table without an IMAGE or USER\n"
    "column (CALL ID, COMMAND, CREATED, STATUS, PORTS, NAMES). Global calls you cannot read\n"
    "show a 'hidden' COMMAND. ps speaks only Docker: it takes no scope flags. For Hold's\n"
    "scoped, past-inclusive ledger with a USER column, use list.\n";
static const char help_on[] =
    "Start a guarded shell under Hold's PTY/session wrapper. Hold holds the line: pressing the classic detach sequence Ctrl-P Ctrl-Q puts the current foreground program on hold as a call and returns to the shell. 'hold off' or exit ends the session. (shell is an alias of on.)\n";
static const char help_off[] =
    "End the current 'hold on' session cleanly. Only works inside a hold on session.\n";
static const char help_end[] =
    "End matching calls politely: TERM, then KILL if needed. (stop is an alias.)\n";
static const char help_kill[] =
    "Force matching calls down with KILL.\n";
static const char help_tail[] =
    "Follow a call's live output. Shorthand for hold logs <target> -f.\n";
static const char help_logs[] =
    "Open the log viewer for a call. In a TTY, type directly in the full-screen viewer to filter dynamically; Backspace relaxes the filter, Space excludes lines like the highlighted line, and Ctrl-R resets filters. --replay plays the log back with its recorded timing (Space pause/resume, . fast-forward, , rewind); to a non-TTY it is a plain linear pipe. Non-TTY output stays script-friendly; -p/--print/--plain always dumps plain text.\n";
static const char help_attach[] =
    "Pick a running console/TTY call back up. Detach again with Ctrl-P Ctrl-Q. Start attachable calls with hold -it <cmd>. (console is an alias of attach.)\n";
static const char help_view[] =
    "Internal regression/debug entrypoint for the log viewer engine. The product UX is hold logs <target>, then type inside the full-screen viewer to filter dynamically.\n";
static const char help_purge[] =
    "The one removal verb. With no target it sweeps ended calls; -a includes stale (a state\n"
    "widener, not a scope one). Scope: -u/--user sweeps your personal calls (the default);\n"
    "-s/--system sweeps the global store and, if you are not root, re-execs through sudo so\n"
    "sudo can prompt for the password. A target removes one call; --force removes regardless\n"
    "of state (live or saved). rm, prune, and drop are accepted aliases.\n";
static const char help_inspect[] =
    "Print structured JSON details for a call. For a live call the output includes a Stdio object showing where fds 0/1/2 currently point. Log text belongs to hold logs <target> --plain.\n";
static const char help_ports[] =
    "List the listening sockets in use by the call's process group, one per line (\"127.0.0.1:8080/tcp\"). Your own calls need no root.\n";
static const char help_stats[] =
    "Live resource usage for the call's process group: CPU %, resident memory, and process count, refreshing in place every second until Ctrl-C. Prints a single frame when stdout is not a TTY or with --no-stream.\n";
static const char help_save[] =
    "Protect a call from purge. There is no unsave; use hold purge --force to remove a saved call.\n";
static const char help_rename[] =
    "Rename a call. Live and ended calls are both renameable. Naming a call\ndeclares you want to keep it, so rename also saves it (protects it from\npurge); a targeted `hold purge <name> --force` still removes it.\n";

/* The purge family's help documents the scope flags its arity usage omits. */
static const char usage_purge_help[] =
    "usage: hold purge [<target>] [-a|--all] [-s|--system] [-u|--user] [--force]";
static const char usage_end[] = "usage: hold end [--print] [--all] <target>...";
static const char usage_attach[] = "usage: hold attach <target>";
static const char usage_on[] = "usage: hold on";

#define SPEC_PURGE_FLAGS (SPEC_ALL | SPEC_SCOPE | SPEC_FORCE)

static const struct hold_cli_command_spec command_specs[] = {
    {"list", NULL, 0, 1, SPEC_ALL | SPEC_LIVE | SPEC_SCOPE | SPEC_STRICT,
     "usage: hold list [-a|--all] [-s|--system] [-u|--user] [-l|--live] [name]", NULL, help_list},
    {"ps", NULL, 0, 1, SPEC_ALL | SPEC_STRICT | SPEC_PS_DOCKER,
     "usage: hold ps [-a|--all] [name]", NULL, help_ps},
    {"on", NULL, 0, 0, 0, usage_on, NULL, help_on},
    {"off", NULL, 0, 0, 0, "usage: hold off", NULL, help_off},
    {"shell", "on", 0, 0, 0, "usage: hold shell", usage_on, help_on},
    {"end", NULL, 1, -1, SPEC_ALL | SPEC_PRINT, usage_end, NULL, help_end},
    {"stop", "end", 1, -1, SPEC_ALL | SPEC_PRINT,
     "usage: hold stop [--print] [--all] <target>...", usage_end, help_end},
    {"kill", NULL, 1, -1, SPEC_ALL | SPEC_PRINT,
     "usage: hold kill [--print] [--all] <target>...", NULL, help_kill},
    {"tail", NULL, 1, 1, 0, "usage: hold tail <target>", NULL, help_tail},
    {"logs", "__view", 1, -1, 0,
     "usage: hold logs <target> [--follow|-f] [--replay] [--tail|-n N] [--plain|-p|--interactive]", NULL, help_logs},
    {"inspect", NULL, 1, 1, 0, "usage: hold inspect <target>", NULL, help_inspect},
    {"ports", NULL, 1, 1, 0, "usage: hold ports <target>", NULL, help_ports},
    {"stats", NULL, 1, 1, SPEC_NOSTREAM, "usage: hold stats <target> [--no-stream]", NULL, help_stats},
    {"__view", NULL, 1, -1, 0,
     "usage: hold __view <target> [internal viewer test options]", NULL, help_view},
    {"attach", NULL, 1, 1, 0, usage_attach, NULL, help_attach},
    {"console", "attach", 1, 1, 0, "usage: hold console <target>", usage_attach, help_attach},
    {"purge", NULL, 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"prune", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"rm", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [--force]", usage_purge_help, help_purge},
    {"drop", "purge", 0, 1, SPEC_PURGE_FLAGS,
     "usage: hold purge [<target>] [-a|--all] [--force]", usage_purge_help, help_purge},
    {"save", NULL, 1, 1, 0, "usage: hold save <target>", NULL, help_save},
    {"rename", NULL, 2, 2, 0, "usage: hold rename <target> <name>", NULL, help_rename},
    {"help", NULL, 0, 1, 0, "usage: hold help [topic]", NULL, NULL},
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

/* The spelled flags the owned sweep can consume; each spec's mask says which
 * apply. -a rides SPEC_ALL: it is accepted wherever --all is. */
static const struct owned_flag {
    const char *shortf; /* NULL when the flag has no short spelling */
    const char *longf;
    unsigned bit;
    size_t out; /* offsetof the bool it sets in hold_cli_owned_opts */
} owned_flags[] = {
    {"-a", "--all", SPEC_ALL, offsetof(struct hold_cli_owned_opts, all)},
    {"-l", "--live", SPEC_LIVE, offsetof(struct hold_cli_owned_opts, live_only)},
    {"-s", "--system", SPEC_SCOPE, offsetof(struct hold_cli_owned_opts, system_scope)},
    {"-u", "--user", SPEC_SCOPE, offsetof(struct hold_cli_owned_opts, user_scope)},
    {NULL, "--force", SPEC_FORCE, offsetof(struct hold_cli_owned_opts, force)},
    {NULL, "--print", SPEC_PRINT, offsetof(struct hold_cli_owned_opts, print_cmd)},
    {NULL, "--no-stream", SPEC_NOSTREAM, offsetof(struct hold_cli_owned_opts, no_stream)},
};

/* Sweep an owned command's argv: consume the flags its spec allows (plus the
 * global --system/--quiet), collect everything else into cmd_argv. A literal
 * `--` passes the rest through untouched. Strict verbs (list/ps) reject any
 * flag they did not consume rather than misreading it as a name filter.
 * Returns 0, or 1 after printing the unknown-flag error and usage. */
int hold_cli_collect_owned_args(const char *command, int argc, char **argv,
                                char **cmd_argv, int *cmd_argc,
                                struct hold_cli_owned_opts *opts) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(command);
    unsigned allowed = spec ? spec->flags : 0;
    bool literal = false;
    for (int i = 0; i < argc; i++) {
        char *arg = argv[i];
        if (!literal) {
            if (!strcmp(arg, "--")) {
                literal = true;
                opts->saw_delimiter = true;
                continue;
            }
            /* --system selects the root-managed store for every owned verb
             * except ps, which speaks only Docker and takes no scope flag. */
            if (!(allowed & SPEC_PS_DOCKER) && !strcmp(arg, "--system")) {
                opts->requested_system = true;
                continue;
            }
            if (!strcmp(arg, "--quiet")) {
                opts->quiet = true;
                continue;
            }
            const struct owned_flag *f = NULL;
            for (size_t j = 0; j < sizeof(owned_flags) / sizeof(owned_flags[0]); j++) {
                if ((owned_flags[j].shortf && !strcmp(arg, owned_flags[j].shortf)) ||
                    !strcmp(arg, owned_flags[j].longf)) {
                    f = &owned_flags[j];
                    break;
                }
            }
            if (f && (allowed & f->bit)) {
                *(bool *)((char *)opts + f->out) = true;
                continue;
            }
            if ((allowed & SPEC_STRICT) && arg[0] == '-') {
                fprintf(stderr, "hold: error: unknown flag '%s'\n", arg);
                if (spec) {
                    fprintf(stderr, "%s\n", spec->usage);
                }
                return 1;
            }
        }
        cmd_argv[(*cmd_argc)++] = arg;
    }
    return 0;
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
           "redacted public index, where a call's USER and COMMAND read 'hidden'.\n"
           "Acting on a root-managed target requires root; user-local targets win\n"
           "over root-public collisions.\n\n"
           "  sudo hold <cmd...>           start in root-managed state\n"
           "  sudo hold list               list the global calls (authoritative)\n"
           "  sudo hold list -a            global calls plus every user's calls\n"
           "  hold list -s                 the redacted global view, as a normal user\n"
           "  hold purge -s                sweep the global store (re-execs via sudo)\n");
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

int hold_show_help(const char *topic) {
    if (!topic || !*topic) {
        hold_usage();
        return 0;
    }
    if (!strcmp(topic, "targets")) return help_targets();
    if (!strcmp(topic, "system")) return help_system();
    if (!strcmp(topic, "scripting")) return help_scripting();
    const struct hold_cli_command_spec *spec = find_public_command_spec(topic);
    if (spec && spec->help) {
        printf("%s\n\n%s", spec->help_usage ? spec->help_usage : spec->usage, spec->help);
        return 0;
    }
    fprintf(stderr, "hold: unknown help topic '%s'\n", topic);
    return 5;
}

bool hold_cli_command_is_parser_owned(const char *s) {
    return find_public_command_spec(s) != NULL;
}

const char *hold_cli_command_usage(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    return spec ? spec->usage : NULL;
}

const char *hold_cli_command_canon(const char *s) {
    const struct hold_cli_command_spec *spec = find_public_command_spec(s);
    if (!spec) return s;
    return spec->canon ? spec->canon : spec->name;
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
