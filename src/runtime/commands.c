#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_run_record *r,
                                 enum run_state st);

static int attach_console_record(const struct hold_invocation *inv,
                                 const struct hold_run_record *r,
                                 enum run_state st) {
    if (st != STATE_RUNNING) {
        hold_sig_note(inv, "hold: %s has ended - see 'hold logs %s'\n", r->id, r->id);
        return 0;
    }
    if (!r->has_console) {
        hold_sig_note(inv, "hold: %s has no console (start with -it)\n", r->id);
        return 0;
    }
    return hold_run_native_console(r->console_sock, r->log_path, r->id,
                                   r->has_name && r->name[0] ? r->name : NULL);
}

int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "console", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to attach\n");
        return 0;
    }
    struct hold_resolved_target target = targets[0];
    if (target.requires_root) {
        free(targets);
        return hold_report_requires_root(target.id);
    }
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(target.store.record_dir, target.id, &r, path, sizeof(path)) != 0) {
        free(targets);
        return 5;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    enum run_state st = hold_eval_state(&r, boot_id);
    rc = attach_console_record(inv, &r, st);
    hold_free_run_record(&r);
    free(targets);
    return rc;
}

void hold_usage(void) {
    printf("hold %s - more than nohup, less than systemd\n\n"
           "Put a command on hold as a durable call, then list your calls, pick one\n"
           "back up, and end it politely. No daemon, no config server.\n\n"
           "PLACE A CALL\n"
           "  hold <cmd> [args...]         foreground: stream its output\n"
           "  hold -d <cmd> [args...]      detached: print the bare 64-hex call id\n"
           "  hold -it <cmd> [args...]     attached on a PTY (Ctrl-P Ctrl-Q holds it)\n"
           "  hold <id|name>               redial: restart a retained call from its recipe\n"
           "  hold --name web -d <cmd>...  name the call and place it in the background\n\n"
           "SESSION\n"
           "  hold on                      guarded shell; Ctrl-P Ctrl-Q holds a program\n"
           "  hold off                     end the current hold on session\n\n"
           "MANAGE YOUR CALLS\n"
           "  hold list                    your ledger: your calls, live and past\n"
           "  hold ps                      the Docker view: running calls, machine-wide\n"
           "  hold attach <target>         pick the call back up (Ctrl-P Ctrl-Q detaches)\n"
           "  hold logs <target> [-f]      open the log viewer (-p dumps plain text)\n"
           "  hold inspect <target>        structured JSON details\n"
           "  hold ports <target>          listening sockets in use by the call\n"
           "  hold stats <target>          live CPU, memory, and pid usage\n"
           "  hold end <target> [--all]    end the call politely: TERM, then KILL\n"
           "  hold kill <target>           KILL now, when it won't listen\n"
           "  hold save <target>           protect a call from purge\n"
           "  hold rename <target> <name>  name a call; naming it also saves it\n"
           "  hold purge [<target>]        the one removal verb (-a stale, --force even saved)\n\n"
           "  target = call id, id prefix, or call name\n\n"
           "MORE\n"
           "  hold help targets            id and scope resolution\n"
           "  hold help scripting          exit codes, --print, --quiet, stdout\n"
           "  hold <command> -h            help for one command\n\n"
           "  hold --version\n",
           HOLD_VERSION);
}
