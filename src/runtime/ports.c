#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/observe.h"

/* hold ports <target>: the listening sockets in use by the call's process
 * group, one per line, in the same "127.0.0.1:8080/tcp" form as the PORTS
 * column. Own calls need no root - the match runs against /proc socket inodes.
 * A call the invoking user cannot read (another user's) exits 3 with a note. */
int hold_cmd_ports_action(const struct hold_invocation *inv,
                          const struct hold_store *user_store,
                          const struct hold_store *system_store,
                          const char *id_token) {
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", id_token, false, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        return hold_report_not_found(id_token);
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
    if (st != STATE_RUNNING) {
        /* An ended call holds no sockets; that is a clean empty result, not an
         * error, so scripts can loop over it. */
        hold_free_run_record(&r);
        free(targets);
        return 0;
    }
    struct hold_port_list ports = {0};
    bool denied = false;
    rc = hold_observe_run_ports(&r, &ports, &denied);
    if (rc != 0) {
        hold_port_list_free(&ports);
        hold_free_run_record(&r);
        free(targets);
        hold_die_errno("hold: failed to read ports");
    }
    int result = 0;
    if (denied && ports.count == 0) {
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(r.id, display_id);
        fprintf(stderr, "hold: error: cannot read the process table for '%s'\n", display_id);
        result = 3;
    } else {
        for (size_t i = 0; i < ports.count; i++) {
            printf("%s\n", ports.items[i]);
        }
    }
    hold_port_list_free(&ports);
    hold_free_run_record(&r);
    free(targets);
    return result;
}
