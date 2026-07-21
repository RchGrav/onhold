#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"
#include "hold/observe.h"

struct list_row {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char owner[64];   /* set only for the root cross-user view's USER column */
    char state[16];
    char created[64];
    char status[96];
    char ports[HOLD_PATH_MAX];
    char cmd[HOLD_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
    bool redacted;    /* a projected global row: COMMAND and owner are hidden */
};

struct list_rows {
    struct list_row *items;
    size_t count;
};

static void free_list_rows(struct list_rows *rows);
static int append_list_row(struct list_rows *rows, const struct list_row *row);
static int compare_list_rows(const void *a, const void *b);
static int collect_list_private(const struct hold_store *store,
                                const char *name_filter,
                                bool running_only,
                                bool system_scope,
                                struct list_rows *rows);
static int collect_list_public(const struct hold_store *store,
                               const char *name_filter,
                               bool running_only,
                               struct list_rows *rows);
static int collect_all_user_stores(const char *name_filter,
                                    bool running_only,
                                    struct list_rows *rows);
static void refresh_system_public_ports(const struct hold_store *store);
static int print_collected_table(struct list_rows *rows, bool with_user);
static void unlink_public_index(const struct hold_store *store, const char *id);
struct prune_sweep_stats;
static void unlink_log_index_for_log(const char *log_path);
static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, struct prune_sweep_stats *stats);

static void free_list_rows(struct list_rows *rows) {
    free(rows->items);
    rows->items = NULL;
    rows->count = 0;
}

static int append_list_row(struct list_rows *rows, const struct list_row *row) {
    struct list_row *next = realloc(rows->items, (rows->count + 1) * sizeof(*rows->items));
    if (!next) {
        return -1;
    }
    rows->items = next;
    rows->items[rows->count++] = *row;
    return 0;
}

static int compare_list_rows(const void *a, const void *b) {
    const struct list_row *ra = (const struct list_row *)a;
    const struct list_row *rb = (const struct list_row *)b;
    if (ra->running != rb->running) {
        return ra->running ? -1 : 1;
    }
    if (ra->start_unix_ns > rb->start_unix_ns) {
        return -1;
    }
    if (ra->start_unix_ns < rb->start_unix_ns) {
        return 1;
    }
    return strcmp(ra->id, rb->id);
}

/* Docker double-quotes COMMAND and ellipsizes it so one long command never
 * blows out the column. Up to PS_CMD_CHARS characters of the command survive;
 * anything longer ends in an ellipsis inside the quotes. */
#define PS_CMD_CHARS 30
#define PS_CMD_CELL (PS_CMD_CHARS + 8) /* quotes + "..." + NUL */

static void quote_command(char out[PS_CMD_CELL], const char *cmd) {
    const char *src = (cmd && *cmd) ? cmd : "?";
    char body[PS_CMD_CHARS + 4];
    size_t len = strlen(src);
    /* ASCII ellipsis keeps byte width equal to display width, so the
     * content-sized columns below never shear on a truncated command. */
    snprintf(body, sizeof(body), "%.*s%s", PS_CMD_CHARS, src, len > PS_CMD_CHARS ? "..." : "");
    snprintf(out, PS_CMD_CELL, "\"%s\"", body);
}

/* Seconds elapsed since a past instant given in unix nanoseconds. */
static int64_t seconds_since(int64_t ref_unix_ns) {
    struct timespec now;
    if (ref_unix_ns <= 0 || clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    return now_ns > ref_unix_ns ? (now_ns - ref_unix_ns) / 1000000000LL : 0;
}

/* The instant a call last stopped running: its recorded end time when we have
 * one, otherwise its start (a good enough anchor for short-lived calls and the
 * behavior Hold shipped before the ended timestamp was parseable). */
static int64_t ended_reference_ns(const struct hold_run_record *r) {
    int64_t ns = 0;
    if (r->has_ended_at && hold_parse_rfc3339_utc_to_ns(r->ended_at, &ns)) {
        return ns;
    }
    return r->start_unix_ns;
}

static void format_status(enum run_state st, const struct hold_run_record *r, char *out, size_t n) {
    char human[48];
    switch (st) {
    case STATE_RUNNING:
        hold_format_duration_human(seconds_since(r->start_unix_ns), human, sizeof(human));
        snprintf(out, n, "Up %s", human);
        break;
    case STATE_EXITED:
        hold_format_duration_human(seconds_since(ended_reference_ns(r)), human, sizeof(human));
        if (r->has_exit_code) {
            snprintf(out, n, "Exited (%d) %s ago", r->exit_code, human);
        } else if (r->has_term_signal) {
            snprintf(out, n, "Exited (%d) %s ago", 128 + r->term_signal, human);
        } else {
            snprintf(out, n, "Exited (?) %s ago", human);
        }
        break;
    case STATE_FAILED:
        hold_format_duration_human(seconds_since(ended_reference_ns(r)), human, sizeof(human));
        snprintf(out, n, "Failed %s ago", human);
        break;
    case STATE_STALE:
        /* Stale is Hold-specific: the call's boot id no longer matches, so it
         * cannot be running. Report the age without a false exit story. */
        hold_format_duration_human(seconds_since(r->start_unix_ns), human, sizeof(human));
        snprintf(out, n, "Stale %s", human);
        break;
    default:
        snprintf(out, n, "Unknown");
        break;
    }
}

/* The USER column value for a private record. A system-scope record (root's
 * global view) is attributed to its recorded invoking user, falling back to
 * "root" when none was recorded. A personal record is attributed to the
 * account that owns it, resolved from the record's uid via /etc/passwd, with
 * the numeric uid as an honest fallback when no passwd entry exists. */
static void resolve_user_label(const struct hold_run_record *r, bool system_scope, char *out, size_t n) {
    if (system_scope) {
        if (r->has_invocation && r->invoked_by_user[0]) {
            snprintf(out, n, "%.*s", (int)(n - 1), r->invoked_by_user);
        } else {
            snprintf(out, n, "root");
        }
        return;
    }
    struct hold_passwd_entry pw;
    if (hold_lookup_passwd_by_uid(r->uid, &pw) == 0 && pw.name[0]) {
        snprintf(out, n, "%.*s", (int)(n - 1), pw.name);
    } else {
        snprintf(out, n, "%u", (unsigned)r->uid);
    }
}

/* Records from a private store (the caller's own, a peer's, or root's global
 * store). running_only narrows to live calls; otherwise the full ledger, live
 * and past. system_scope selects how the USER column is attributed. */
static int collect_list_private(const struct hold_store *store,
                                const char *name_filter,
                                bool running_only,
                                bool system_scope,
                                struct list_rows *rows) {
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0) {
            fprintf(stderr, "hold: warning: skipping corrupt record %s\n", e->d_name);
            continue;
        }
        if (!hold_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            fprintf(stderr, "hold: warning: skipping corrupt record %s\n", e->d_name);
            hold_free_run_record(&r);
            continue;
        }
        if (name_filter && (!r.has_name || strcmp(r.name, name_filter) != 0)) {
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, boot_id);
        if (running_only && st != STATE_RUNNING) {
            hold_free_run_record(&r);
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(r.id, display_id);
        snprintf(row.id, sizeof(row.id), "%s", display_id);
        resolve_user_label(&r, system_scope, row.owner, sizeof(row.owner));
        if (r.has_name && r.name[0]) snprintf(row.name, sizeof(row.name), "%s", r.name);
        snprintf(row.state, sizeof(row.state), "%s", hold_state_str(st));
        row.start_unix_ns = r.start_unix_ns;
        row.running = st == STATE_RUNNING;
        int64_t created_ns = r.has_created_at && r.created_unix_ns > 0 ? r.created_unix_ns : r.start_unix_ns;
        snprintf(row.cmd, sizeof(row.cmd), "%s", r.cmdline[0] ? r.cmdline : "?");
        /* CREATED is humanized Docker-style: "About a minute ago", "2 days ago". */
        char human[48];
        hold_format_duration_human(seconds_since(created_ns), human, sizeof(human));
        snprintf(row.created, sizeof(row.created), "%s ago", human);
        format_status(st, &r, row.status, sizeof(row.status));
        if (r.saved) {
            /* Surface protection where the eye already rests: a suffix on
             * STATUS, not a new column. */
            size_t used = strlen(row.status);
            snprintf(row.status + used, sizeof(row.status) - used, " (saved)");
        }
        if (st == STATE_RUNNING) {
            hold_observe_run_ports_column(&r, row.ports, sizeof(row.ports));
        }
        if (append_list_row(rows, &row) != 0) {
            hold_free_run_record(&r);
            closedir(d);
            return -1;
        }
        hold_free_run_record(&r);
    }
    closedir(d);
    return 0;
}

/* The redacted global projection every user may see: the public index only,
 * never the root-private records. The command line and the owner do not
 * survive -- both read the literal "hidden", which says exists-but-not-yours-
 * to-see rather than "-"'s none. A row carries the call id, its generated name
 * if the entry has one, the honest projected status, CREATED, and the ports
 * root last observed. */
static int collect_list_public(const struct hold_store *store,
                               const char *name_filter,
                               bool running_only,
                               struct list_rows *rows) {
    DIR *d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= ID_STR_LEN) {
            continue;
        }
        char file_id[ID_STR_LEN];
        memcpy(file_id, e->d_name, len - 5);
        file_id[len - 5] = '\0';
        if (!hold_valid_id(file_id)) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_public_index pi;
        if (hold_load_public_index(path, &pi) != 0 || strcmp(pi.id, file_id) != 0) {
            continue;
        }
        if (name_filter && (!pi.has_name || strcmp(pi.name, name_filter) != 0)) {
            continue;
        }
        bool running = pi.state_hint[0] && strcmp(pi.state_hint, "running") == 0;
        if (running_only && !running) {
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(pi.id, display_id);
        snprintf(row.id, sizeof(row.id), "%s", display_id);
        snprintf(row.owner, sizeof(row.owner), "hidden");
        if (pi.has_name && pi.name[0]) snprintf(row.name, sizeof(row.name), "%s", pi.name);
        snprintf(row.state, sizeof(row.state), "%s", pi.state_hint[0] ? pi.state_hint : "unknown");
        row.running = running;
        row.redacted = true;
        row.start_unix_ns = 0;
        snprintf(row.ports, sizeof(row.ports), "%s", pi.observed_ports);
        /* Projected rows speak the table's Docker phrasing too: "Up <age>"
         * from the published start time when the hint says running. */
        int64_t started_ns = 0;
        if (running && pi.started_at[0] && hold_parse_rfc3339_utc_to_ns(pi.started_at, &started_ns)) {
            char up_human[48];
            hold_format_duration_human(seconds_since(started_ns), up_human, sizeof(up_human));
            snprintf(row.status, sizeof(row.status), "Up %s", up_human);
        } else if (running) {
            snprintf(row.status, sizeof(row.status), "Up");
        } else {
            snprintf(row.status, sizeof(row.status), "%s", pi.state_hint[0] ? pi.state_hint : "Unknown");
        }
        /* The table never shows a raw ISO stamp: humanize the public index's
         * created_at, falling back to "-" when unparseable. */
        int64_t created_at_ns = 0;
        if (hold_parse_rfc3339_utc_to_ns(pi.created_at, &created_at_ns)) {
            char human[48];
            hold_format_duration_human(seconds_since(created_at_ns), human, sizeof(human));
            snprintf(row.created, sizeof(row.created), "%s ago", human);
        } else {
            snprintf(row.created, sizeof(row.created), "-");
        }
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

/* The Docker-shaped call table, content-sized like `docker ps`: a first pass
 * measures every column from the actual rows (never a fixed printf width that
 * shears a long value), a second pass prints. Columns are separated by a
 * two-space gutter; each column is at least as wide as its header; the final
 * NAMES column is never padded, so no line carries trailing spaces.
 *
 * with_user adds the USER column in Docker's IMAGE slot (second, after CALL
 * ID). It is exclusive to `list`: `ps` speaks only Docker, which has no USER
 * concept, and does not repurpose IMAGE's position -- ps simply omits it. */
static int print_collected_table(struct list_rows *rows, bool with_user) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }

    char (*cmds)[PS_CMD_CELL] = NULL;
    if (rows->count > 0) {
        cmds = malloc(rows->count * sizeof(*cmds));
        if (!cmds) {
            return 3;
        }
    }

    size_t w_id = strlen("CALL ID");
    size_t w_user = strlen("USER");
    size_t w_cmd = strlen("COMMAND");
    size_t w_created = strlen("CREATED");
    size_t w_status = strlen("STATUS");
    size_t w_ports = strlen("PORTS");
    for (size_t i = 0; i < rows->count; i++) {
        const struct list_row *r = &rows->items[i];
        /* A projected global row has no command to show: the COMMAND cell is a
         * bare "hidden" (never quoted like a real command, never leaked). */
        if (r->redacted) {
            snprintf(cmds[i], sizeof(cmds[i]), "%s", "hidden");
        } else {
            quote_command(cmds[i], r->cmd);
        }
        const char *created = r->created[0] ? r->created : "-";
        const char *status = r->status[0] ? r->status : r->state;
        if (strlen(r->id) > w_id) w_id = strlen(r->id);
        if (strlen(cmds[i]) > w_cmd) w_cmd = strlen(cmds[i]);
        if (strlen(created) > w_created) w_created = strlen(created);
        if (strlen(status) > w_status) w_status = strlen(status);
        if (strlen(r->ports) > w_ports) w_ports = strlen(r->ports);
        if (with_user && strlen(r->owner) > w_user) w_user = strlen(r->owner);
    }

    if (with_user) {
        printf("%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
               (int)w_id, "CALL ID",
               (int)w_user, "USER",
               (int)w_cmd, "COMMAND",
               (int)w_created, "CREATED",
               (int)w_status, "STATUS",
               (int)w_ports, "PORTS",
               "NAMES");
    } else {
        printf("%-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
               (int)w_id, "CALL ID",
               (int)w_cmd, "COMMAND",
               (int)w_created, "CREATED",
               (int)w_status, "STATUS",
               (int)w_ports, "PORTS",
               "NAMES");
    }
    for (size_t i = 0; i < rows->count; i++) {
        const struct list_row *r = &rows->items[i];
        if (with_user) {
            printf("%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
                   (int)w_id, r->id,
                   (int)w_user, r->owner[0] ? r->owner : "-",
                   (int)w_cmd, cmds[i],
                   (int)w_created, r->created[0] ? r->created : "-",
                   (int)w_status, r->status[0] ? r->status : r->state,
                   (int)w_ports, r->ports,
                   r->name[0] ? r->name : "-");
        } else {
            printf("%-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
                   (int)w_id, r->id,
                   (int)w_cmd, cmds[i],
                   (int)w_created, r->created[0] ? r->created : "-",
                   (int)w_status, r->status[0] ? r->status : r->state,
                   (int)w_ports, r->ports,
                   r->name[0] ? r->name : "-");
        }
    }
    free(cmds);
    return 0;
}

/* The home root the cross-user scan walks. A test override keeps the scan
 * exercisable without writing under a real /home. */
static const char *user_home_root(void) {
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_TEST_HOME_ROOT");
    if (override && *override) {
        return override;
    }
#endif
    return "/home";
}

/* Enumerate every user's personal store under the home root. Each row's USER
 * comes from the record's own uid (see resolve_user_label), so a home named
 * differently from its account still attributes honestly. Unreadable stores
 * are simply skipped. */
static int collect_all_user_stores(const char *name_filter,
                                    bool running_only,
                                    struct list_rows *rows) {
    const char *home_root = user_home_root();
    DIR *d = opendir(home_root);
    if (!d) {
        return 0;
    }
    int rc = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char home[HOLD_PATH_MAX];
        if (hold_checked_snprintf(home, sizeof(home), "%s/%s", home_root, e->d_name) != 0) {
            continue;
        }
        struct hold_store store;
        if (hold_init_user_store_from_home(home, &store) != 0) {
            continue;
        }
        if (collect_list_private(&store, name_filter, running_only, false, rows) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}

/* Refresh the public port projection of every live global call. Root can read
 * the private records and observe the process group; non-root cannot, so this
 * is where the PORTS a user sees in `list -a` come from. Best-effort and
 * eventually consistent: a failure to observe or rewrite just leaves the last
 * projection in place. */
static void refresh_system_public_ports(const struct hold_store *store) {
    if (store->kind != STORE_SYSTEM_MANAGED) {
        return;
    }
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return;
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0) {
            continue;
        }
        if (!hold_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            hold_free_run_record(&r);
            continue;
        }
        if (hold_eval_state(&r, boot_id) == STATE_RUNNING) {
            /* Only listening TCP and bound UDP sockets are observed here (the
             * hold_observe_run_ports filter); outbound connections are never
             * published, so the projection cannot leak who a call talks to. */
            char ports[HOLD_PATH_MAX];
            hold_observe_run_ports_column(&r, ports, sizeof(ports));
            (void)hold_write_public_index_atomic(store, &r, ports);
        }
        hold_free_run_record(&r);
    }
    closedir(d);
}

/* list is Hold's scoped ledger. The requested scope resolves against privilege:
 * a normal user's default is their own store, root's default is the global one.
 * SYSTEM shows the global store (real for root, redacted for a normal user);
 * USER shows personal calls only, even under sudo; BOTH (-a) unions the two.
 * Every list view carries the USER column. */
int hold_cmd_list(const struct hold_invocation *inv,
                    const struct hold_store *user_store,
                    const struct hold_store *system_store,
                    const char *name_filter,
                    enum hold_list_scope scope,
                    bool live_only) {
    if (scope == HOLD_LIST_SCOPE_DEFAULT) {
        scope = inv->euid_root ? HOLD_LIST_SCOPE_SYSTEM : HOLD_LIST_SCOPE_USER;
    }
    struct list_rows rows = {0};
    int rc = 0;
    switch (scope) {
    case HOLD_LIST_SCOPE_USER:
        if (collect_list_private(user_store, name_filter, live_only, false, &rows) != 0) {
            rc = 3;
        }
        break;
    case HOLD_LIST_SCOPE_SYSTEM:
        if (inv->euid_root) {
            refresh_system_public_ports(system_store);
            if (collect_list_private(system_store, name_filter, live_only, true, &rows) != 0) {
                rc = 3;
            }
        } else if (collect_list_public(system_store, name_filter, live_only, &rows) != 0) {
            rc = 3;
        }
        break;
    case HOLD_LIST_SCOPE_BOTH:
        if (inv->euid_root) {
            refresh_system_public_ports(system_store);
            if (collect_list_private(system_store, name_filter, live_only, true, &rows) != 0 ||
                collect_all_user_stores(name_filter, live_only, &rows) != 0) {
                rc = 3;
            }
        } else if (collect_list_private(user_store, name_filter, live_only, false, &rows) != 0 ||
                   collect_list_public(system_store, name_filter, live_only, &rows) != 0) {
            rc = 3;
        }
        break;
    case HOLD_LIST_SCOPE_DEFAULT:
        break; /* resolved above */
    }
    if (rc == 0) {
        rc = print_collected_table(&rows, true);
    }
    free_list_rows(&rows);
    return rc;
}

/* ps is Docker's machine-wide, running-first view. Docker has no user/system
 * split -- one daemon, one namespace -- so a faithful ps shows everything
 * running on the machine and speaks only Docker: no scope flags, no USER
 * column. A normal user sees their own calls in full and the global calls
 * redacted; root sees the global store directly. -a adds ended calls. */
int hold_cmd_ps(const struct hold_invocation *inv,
                  const struct hold_store *user_store,
                  const struct hold_store *system_store,
                  const char *name_filter,
                  bool all) {
    bool running_only = !all;
    struct list_rows rows = {0};
    int rc = 0;
    if (inv->euid_root) {
        refresh_system_public_ports(system_store);
        if (collect_list_private(system_store, name_filter, running_only, true, &rows) != 0) {
            rc = 3;
        }
    } else if (collect_list_private(user_store, name_filter, running_only, false, &rows) != 0 ||
               collect_list_public(system_store, name_filter, running_only, &rows) != 0) {
        rc = 3;
    }
    if (rc == 0) {
        rc = print_collected_table(&rows, false);
    }
    free_list_rows(&rows);
    return rc;
}

static void unlink_public_index(const struct hold_store *store, const char *id) {
    if (store->kind != STORE_SYSTEM_MANAGED || !id || !*id) {
        return;
    }
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0) {
        unlink(path);
    }
}

static void unlink_log_index_for_log(const char *log_path) {
    char idx[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx, sizeof(idx)) == 0) {
        unlink(idx);
    }
}

int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) {
        return 5;
    }
    enum run_state st = hold_eval_state(&r, boot ? boot : NULL);
    bool prunable = (st == STATE_EXITED || st == STATE_FAILED || (allow_stale && st == STATE_STALE));
    if (!prunable) {
        fprintf(stderr, "hold: error: call %s is %s and cannot be purged\n", id, hold_state_str(st));
        hold_free_run_record(&r);
        return 2;
    }
    unlink(path);
    if (r.has_log) {
        unlink_log_index_for_log(r.log_path);
        unlink(r.log_path);
    }
    if (r.has_console) {
        unlink(r.console_sock);
    }
    unlink_public_index(store, id);
    if (removed) {
        *removed = true;
    }
    hold_free_run_record(&r);
    return 0;
}

struct prune_sweep_stats {
    int removed;
    int kept_live;
    int kept_stale;
    int kept_saved;
};

/* One orphan sweep: any artifact in dir with the given suffix whose id has
 * no record left is removed (a public entry, log, or console socket whose
 * record is gone must never resurrect a call). is_log also drops the
 * .log.idx sidecar beside each removed log. */
static void sweep_orphaned_artifacts(const struct hold_store *store,
                                     const char *dir,
                                     const char *suffix,
                                     bool is_log,
                                     struct prune_sweep_stats *stats) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    const struct dirent *e;
    size_t suffix_len = strlen(suffix);
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, suffix)) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= suffix_len) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - suffix_len;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], artifact_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            hold_checked_snprintf(artifact_path, sizeof(artifact_path), "%s/%s", dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            if (is_log) {
                unlink_log_index_for_log(artifact_path);
            }
            unlink(artifact_path);
            stats->removed++;
        }
    }
    closedir(d);
}


static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, struct prune_sweep_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    const struct dirent *e;
    DIR *d = opendir(store->record_dir);
    if (!d) {
        goto sweep_logs;
    }
    while ((e = readdir(d))) {
        char file_id[ID_HEX_LEN + 1];
        if (!hold_record_json_filename_id(e->d_name, file_id, sizeof(file_id))) {
            continue;
        }
        char path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->record_dir, e->d_name) != 0) {
            continue;
        }
        struct hold_run_record r;
        if (hold_load_record(path, &r) != 0) {
            unlink(path);
            continue;
        }
        if (!hold_valid_record(&r) || strcmp(r.id, file_id) != 0) {
            unlink(path);
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, boot_id);
        /* The sweep accounts for everything it sees: live calls are end's
         * business, stale needs -a, and saved calls need a targeted --force. */
        if (st == STATE_RUNNING) {
            stats->kept_live++;
        } else if (st == STATE_STALE && !include_stale) {
            stats->kept_stale++;
        } else if (r.saved && (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE)) {
            stats->kept_saved++;
        } else if (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE) {
            unlink(path);
            if (r.has_log) {
                unlink(r.log_path);
            }
            if (r.has_console) {
                unlink(r.console_sock);
            }
            unlink_public_index(store, r.id);
            char display_id[ID_DISPLAY_HEX_LEN + 1];
            hold_run_id_display(r.id, display_id);
            /* Docker prune prints what it deletes; so do we. */
            printf("%s%s%s\n", display_id, r.has_name && r.name[0] ? "  " : "", r.has_name && r.name[0] ? r.name : "");
            stats->removed++;
        }
        hold_free_run_record(&r);
    }
    closedir(d);

sweep_logs:
    sweep_orphaned_artifacts(store, store->log_dir, ".log", true, stats);
    sweep_orphaned_artifacts(store, store->console_dir, ".sock", false, stats);
    sweep_orphaned_artifacts(store, store->public_dir, ".json", false, stats);
    return 0;
}

int hold_cmd_purge_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *target_token,
                            bool all,
                            bool force,
                            bool system_scope) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        /* A no-target sweep only clears already-ended calls and skips saved
         * ones; --force adds nothing here (it never mass-ends live calls). The
         * scope is explicit: -s/--system sweeps the global store (root only,
         * non-root is re-execed through sudo before reaching here), everything
         * else sweeps the caller's personal store. */
        const struct hold_store *store = system_scope ? system_store : user_store;
        struct prune_sweep_stats stats;
        bool include_stale = all || force || (target_token && strcmp(target_token, "all") == 0);
        int rc = cmd_prune_store_all(store, include_stale, &stats);
        fflush(stdout); /* purged-call lines print before the summary note */
        if (rc == 0) {
            /* Account for everything the sweep saw, so "purged 6" while the
             * visible list still has entries is never a mystery. */
            /* snprintf returns the would-have-written length; clamp so a
             * (theoretical) truncation can never wrap `sizeof(kept) - off`
             * into a huge size on the next append. */
            char kept[128] = "";
            size_t off = 0;
            int w;
            if (stats.kept_live > 0) {
                w = snprintf(kept + off, sizeof(kept) - off, "%s%d live", off ? ", " : "; kept ", stats.kept_live);
                if (w > 0) off = (size_t)w >= sizeof(kept) - off ? sizeof(kept) - 1 : off + (size_t)w;
            }
            if (stats.kept_stale > 0) {
                w = snprintf(kept + off, sizeof(kept) - off, "%s%d stale", off ? ", " : "; kept ", stats.kept_stale);
                if (w > 0) off = (size_t)w >= sizeof(kept) - off ? sizeof(kept) - 1 : off + (size_t)w;
            }
            if (stats.kept_saved > 0) {
                w = snprintf(kept + off, sizeof(kept) - off, "%s%d saved", off ? ", " : "; kept ", stats.kept_saved);
                if (w > 0) off = (size_t)w >= sizeof(kept) - off ? sizeof(kept) - 1 : off + (size_t)w;
            }
            const char *hint = stats.kept_stale > 0 ? " (purge -a sweeps stale)" : "";
            if (stats.removed > 0) {
                hold_sig_note(inv, "hold: purged %d past call%s%s%s\n",
                              stats.removed, stats.removed == 1 ? "" : "s", kept, hint);
            } else {
                hold_sig_note(inv, "hold: nothing to purge%s%s\n", kept, hint);
            }
        }
        return rc;
    }
    /* Resolve the target regardless of run state (inspect intent) so protection
     * and --force removal apply uniformly to live and ended calls. */
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "inspect", target_token, all, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        return hold_report_not_found(target_token);
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            rc = hold_report_requires_root(targets[i].id);
            free(targets);
            return rc;
        }
    }
    char boot[128];
    const char *boot_id = hold_boot_id_or_null(boot);
    /* Saved calls are protected: a targeted purge without --force refuses and
     * echoes the exact command the user meant, ready to copy. */
    if (!force) {
        for (int i = 0; i < ntargets; i++) {
            struct hold_run_record r;
            char rp[HOLD_PATH_MAX];
            if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) != 0) {
                continue;
            }
            bool saved = r.saved;
            char display_id[ID_DISPLAY_HEX_LEN + 1];
            hold_run_id_display(r.id, display_id);
            const char *label = r.has_name ? r.name : display_id;
            if (saved) {
                fprintf(stderr,
                        "hold: '%s' is saved \xe2\x80\x94 purging a saved call requires --force\n"
                        "  hold purge %s --force\n",
                        label, target_token);
                hold_free_run_record(&r);
                free(targets);
                return 2;
            }
            hold_free_run_record(&r);
        }
    }
    int worst = 0;
    int removed_count = 0;
    for (int i = 0; i < ntargets; i++) {
        /* --force ends a still-live call before removal; without it a live call
         * is refused below by hold_prune_one_run. */
        if (force) {
            struct hold_run_record r;
            char rp[HOLD_PATH_MAX];
            if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) == 0) {
                enum run_state st = hold_eval_state(&r, boot_id);
                hold_free_run_record(&r);
                if (st == STATE_RUNNING) {
                    char *one[1] = { targets[i].id };
                    int stop_rc = hold_cmd_signal_action(inv, user_store, system_store, "stop", 1, one,
                                                         SIGTERM, true, false, false);
                    if (stop_rc != 0) {
                        if (stop_rc > worst) worst = stop_rc;
                        continue;
                    }
                }
            }
        }
        bool removed = false;
        rc = hold_prune_one_run(&targets[i].store, targets[i].id, boot_id, true, &removed);
        if (removed) {
            removed_count++;
        }
        if (rc > worst) {
            worst = rc;
        }
    }
    if (worst == 0) {
        if (removed_count > 0) {
            const char *atom = NULL;
            enum id_token_scope token_scope = hold_parse_id_token(target_token, &atom);
            bool target_looks_like_alias = (token_scope != ID_TOKEN_INVALID && atom &&
                                            hold_valid_alias(atom) && !hold_valid_id_prefix(atom));
            if (target_looks_like_alias) {
                hold_sig_note(inv, "hold: purged %d past call%s for '%s'\n",
                         removed_count, removed_count == 1 ? "" : "s", atom);
            } else {
                hold_sig_note(inv, "hold: purged %d past call%s\n", removed_count, removed_count == 1 ? "" : "s");
            }
        } else {
            hold_sig_note(inv, "hold: nothing to purge\n");
        }
    }
    free(targets);
    return worst;
}
