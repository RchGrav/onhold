#include "sigmund/config.h"
#include "sigmund/types.h"
#include "sigmund/cli.h"
#include "sigmund/runtime.h"
#include "sigmund/access.h"
#include "sigmund/console.h"
#include "sigmund/store.h"
#include "sigmund/platform.h"
#include "sigmund/core.h"

int main(int argc, char **argv) {
    if (argc < 2) {
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
                                               !strcmp(command, "prune") || !strcmp(command, "console") ||
                                               !strcmp(command, "aliases") || !strcmp(command, "profile") ||
                                               !strcmp(command, "show")))) {
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
