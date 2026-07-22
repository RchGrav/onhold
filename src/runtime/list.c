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

/* ---- the row model ----------------------------------------------------- */

struct list_row {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char owner[64];   /* set only for views that print the USER column */
    char state[16];
    char created[64];
    char status[96];
    char ports[HOLD_PATH_MAX];
    char cmd[HOLD_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
    bool redacted;    /* a projected global row: COMMAND and owner are hidden */
};

struct list_rows { struct list_row *items; size_t count; };

static void free_list_rows(struct list_rows *rows) {
    free(rows->items);
    rows->items = NULL;
    rows->count = 0;
}

static int append_list_row(struct list_rows *rows, const struct list_row *row) {
    struct list_row *next = realloc(rows->items, (rows->count + 1) * sizeof(*rows->items));
    if (!next) return -1;
    rows->items = next;
    rows->items[rows->count++] = *row;
    return 0;
}

/* Running first, then newest start, then id: the pinned ledger order. */
static int compare_list_rows(const void *a, const void *b) {
    const struct list_row *ra = (const struct list_row *)a;
    const struct list_row *rb = (const struct list_row *)b;
    if (ra->running != rb->running) return ra->running ? -1 : 1;
    if (ra->start_unix_ns != rb->start_unix_ns) return ra->start_unix_ns > rb->start_unix_ns ? -1 : 1;
    return strcmp(ra->id, rb->id);
}

/* Docker double-quotes COMMAND and ellipsizes it so one long command never
 * blows out the column: up to PS_CMD_CHARS characters survive, anything longer
 * ends in an ASCII ellipsis inside the quotes (byte width == display width, so
 * the content-sized columns below never shear on a truncated command). */
#define PS_CMD_CHARS 30
#define PS_CMD_CELL (PS_CMD_CHARS + 8) /* quotes + "..." + NUL */

static void quote_command(char out[PS_CMD_CELL], const char *cmd) {
    const char *src = (cmd && *cmd) ? cmd : "?";
    char body[PS_CMD_CHARS + 4];
    size_t len = strlen(src);
    snprintf(body, sizeof(body), "%.*s%s", PS_CMD_CHARS, src, len > PS_CMD_CHARS ? "..." : "");
    snprintf(out, PS_CMD_CELL, "\"%s\"", body);
}

/* Seconds elapsed since a past instant given in unix nanoseconds. */
static int64_t seconds_since(int64_t ref_unix_ns) {
    struct timespec now;
    if (ref_unix_ns <= 0 || clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
    int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    return now_ns > ref_unix_ns ? (now_ns - ref_unix_ns) / 1000000000LL : 0;
}

/* Humanized Docker-style age of a past instant: "About a minute", "2 days". */
static void format_age(int64_t ref_unix_ns, char *out, size_t n) {
    hold_format_duration_human(seconds_since(ref_unix_ns), out, n);
}

/* The instant a call last stopped running: its recorded end time when we have
 * one, otherwise its start (a good enough anchor for short-lived calls). */
static int64_t ended_reference_ns(const struct hold_run_record *r) {
    int64_t ns = 0;
    if (r->has_ended_at && hold_parse_rfc3339_utc_to_ns(r->ended_at, &ns)) return ns;
    return r->start_unix_ns;
}

static void format_status(enum run_state st, const struct hold_run_record *r, char *out, size_t n) {
    char human[48];
    int64_t anchor = (st == STATE_EXITED || st == STATE_FAILED) ? ended_reference_ns(r) : r->start_unix_ns;
    format_age(anchor, human, sizeof(human));
    switch (st) {
    case STATE_RUNNING: snprintf(out, n, "Up %s", human); break;
    case STATE_EXITED:
        if (r->has_exit_code) snprintf(out, n, "Exited (%d) %s ago", r->exit_code, human);
        else if (r->has_term_signal) snprintf(out, n, "Exited (%d) %s ago", 128 + r->term_signal, human);
        else snprintf(out, n, "Exited (?) %s ago", human);
        break;
    case STATE_FAILED: snprintf(out, n, "Failed %s ago", human); break;
    /* Stale is Hold-specific: the boot id no longer matches, so the call
     * cannot be running. Report the age without a false exit story. */
    case STATE_STALE: snprintf(out, n, "Stale %s", human); break;
    default: snprintf(out, n, "Unknown"); break;
    }
}

/* The USER column for a private record: a system-scope record is attributed to
 * its recorded invoking user (fallback "root"); a personal record to the
 * account owning its uid, with the numeric uid as an honest fallback. */
static void resolve_user_label(const struct hold_run_record *r, bool system_scope, char *out, size_t n) {
    if (system_scope) {
        if (r->has_invocation && r->invoked_by_user[0]) snprintf(out, n, "%.*s", (int)(n - 1), r->invoked_by_user);
        else snprintf(out, n, "root");
        return;
    }
    struct hold_passwd_entry pw;
    if (hold_lookup_passwd_by_uid(r->uid, &pw) == 0 && pw.name[0]) snprintf(out, n, "%.*s", (int)(n - 1), pw.name);
    else snprintf(out, n, "%u", (unsigned)r->uid);
}

/* Parse "<id><suffix>" into a validated full-hex call id; rejects anything
 * else. Shared by the public projection reader and the orphan sweeps. */
static bool id_from_artifact_name(const char *fname, const char *suffix, char id[ID_STR_LEN]) {
    size_t len = strlen(fname), suffix_len = strlen(suffix);
    if (!hold_has_suffix(fname, suffix) || len <= suffix_len || len - suffix_len >= ID_STR_LEN) return false;
    memcpy(id, fname, len - suffix_len);
    id[len - suffix_len] = '\0';
    return hold_valid_id(id);
}

/* ---- collectors -------------------------------------------------------- */

/* Records from a private store (the caller's own, a peer's, or root's global
 * store). running_only narrows to live calls; system_scope picks the USER
 * attribution. Corrupt records warn and are skipped, never fatal. */
struct private_walk {
    const char *name_filter, *boot_id;
    bool running_only, system_scope;
    struct list_rows *rows;
};

static int collect_private_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)path;
    struct private_walk *w = vctx;
    if (!r) {
        fprintf(stderr, "hold: warning: skipping corrupt record %s.json\n", id);
        return 0;
    }
    if (w->name_filter && (!r->has_name || strcmp(r->name, w->name_filter) != 0)) return 0;
    enum run_state st = hold_eval_state(r, w->boot_id);
    if (w->running_only && st != STATE_RUNNING) return 0;
    struct list_row row;
    memset(&row, 0, sizeof(row));
    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r->id, display_id);
    snprintf(row.id, sizeof(row.id), "%s", display_id);
    resolve_user_label(r, w->system_scope, row.owner, sizeof(row.owner));
    if (r->has_name && r->name[0]) snprintf(row.name, sizeof(row.name), "%s", r->name);
    snprintf(row.state, sizeof(row.state), "%s", hold_state_str(st));
    row.start_unix_ns = r->start_unix_ns;
    row.running = st == STATE_RUNNING;
    snprintf(row.cmd, sizeof(row.cmd), "%s", r->cmdline[0] ? r->cmdline : "?");
    /* CREATED is humanized Docker-style: "About a minute ago", "2 days ago". */
    char human[48];
    format_age(r->has_created_at && r->created_unix_ns > 0 ? r->created_unix_ns : r->start_unix_ns,
               human, sizeof(human));
    snprintf(row.created, sizeof(row.created), "%s ago", human);
    format_status(st, r, row.status, sizeof(row.status));
    if (r->saved) {
        /* Surface protection where the eye already rests: a STATUS suffix. */
        size_t used = strlen(row.status);
        snprintf(row.status + used, sizeof(row.status) - used, " (saved)");
    }
    if (st == STATE_RUNNING) hold_observe_run_ports_column(r, row.ports, sizeof(row.ports));
    return append_list_row(w->rows, &row) != 0 ? -1 : 0;
}

static int collect_list_private(const struct hold_store *store, const char *name_filter,
                                bool running_only, bool system_scope, struct list_rows *rows) {
    char boot[128];
    struct private_walk w = { name_filter, hold_boot_id_or_null(boot), running_only, system_scope, rows };
    return hold_for_each_record(store->record_dir, collect_private_cb, &w);
}

/* The redacted global projection every user may see: the public index only,
 * never the root-private records. COMMAND and owner both read the literal
 * "hidden" -- exists-but-not-yours-to-see rather than "-"'s none. A row
 * carries the call id, its name if published, the honest projected status,
 * CREATED, and the ports root last observed. */
static int collect_list_public(const struct hold_store *store, const char *name_filter,
                               bool running_only, struct list_rows *rows) {
    DIR *d = opendir(store->public_dir);
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char file_id[ID_STR_LEN], path[HOLD_PATH_MAX];
        if (!id_from_artifact_name(e->d_name, ".json", file_id)) continue;
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", store->public_dir, e->d_name) != 0) continue;
        struct hold_public_index pi;
        if (hold_load_public_index(path, &pi) != 0 || strcmp(pi.id, file_id) != 0) continue;
        if (name_filter && (!pi.has_name || strcmp(pi.name, name_filter) != 0)) continue;
        bool running = pi.state_hint[0] && strcmp(pi.state_hint, "running") == 0;
        if (running_only && !running) continue;
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
        char human[48];
        int64_t ns = 0;
        if (running && pi.started_at[0] && hold_parse_rfc3339_utc_to_ns(pi.started_at, &ns)) {
            format_age(ns, human, sizeof(human));
            snprintf(row.status, sizeof(row.status), "Up %s", human);
        } else if (running) {
            snprintf(row.status, sizeof(row.status), "Up");
        } else {
            snprintf(row.status, sizeof(row.status), "%s", pi.state_hint[0] ? pi.state_hint : "Unknown");
        }
        /* The table never shows a raw ISO stamp: humanize the published
         * created_at, falling back to "-" when unparseable. */
        if (hold_parse_rfc3339_utc_to_ns(pi.created_at, &ns)) {
            format_age(ns, human, sizeof(human));
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

/* The home root the cross-user scan walks; a test override keeps the scan
 * exercisable without writing under a real /home. */
static const char *user_home_root(void) {
#ifdef HOLD_TESTING
    const char *override = getenv("HOLD_TEST_HOME_ROOT");
    if (override && *override) return override;
#endif
    return "/home";
}

/* Enumerate every user's personal store under the home root. Each row's USER
 * comes from the record's own uid, so a home named differently from its
 * account still attributes honestly. Unreadable stores are simply skipped. */
static int collect_all_user_stores(const char *name_filter, bool running_only, struct list_rows *rows) {
    DIR *d = opendir(user_home_root());
    if (!d) return 0;
    int rc = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char home[HOLD_PATH_MAX];
        if (hold_checked_snprintf(home, sizeof(home), "%s/%s", user_home_root(), e->d_name) != 0) continue;
        struct hold_store store;
        if (hold_init_user_store_from_home(home, &store) != 0) continue;
        if (collect_list_private(&store, name_filter, running_only, false, rows) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}

/* Refresh the public port projection of every live global call. Root can
 * observe the process group; non-root cannot, so this is where the PORTS a
 * user sees in `list -a` come from. Best-effort and eventually consistent: a
 * failed observe or rewrite just leaves the last projection in place. Only
 * listening TCP and bound UDP sockets are published (the hold_observe filter),
 * never outbound connections, so a call's peers cannot leak. */
struct ports_refresh_walk { const struct hold_store *store; const char *boot_id; };

static int refresh_ports_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)id;
    (void)path;
    const struct ports_refresh_walk *w = vctx;
    if (r && hold_eval_state(r, w->boot_id) == STATE_RUNNING) {
        char ports[HOLD_PATH_MAX];
        hold_observe_run_ports_column(r, ports, sizeof(ports));
        (void)hold_write_public_index_atomic(w->store, r, ports);
    }
    return 0;
}

static void refresh_system_public_ports(const struct hold_store *store) {
    if (store->kind != STORE_SYSTEM_MANAGED) return;
    char boot[128];
    struct ports_refresh_walk w = { store, hold_boot_id_or_null(boot) };
    hold_for_each_record(store->record_dir, refresh_ports_cb, &w);
}

/* The system side both list and ps share: root refreshes the projections and
 * reads the global store for real; everyone else gets the redacted index. */
static int collect_system_view(const struct hold_invocation *inv, const struct hold_store *system_store,
                               const char *name_filter, bool running_only, struct list_rows *rows) {
    if (inv->euid_root) {
        refresh_system_public_ports(system_store);
        return collect_list_private(system_store, name_filter, running_only, true, rows);
    }
    return collect_list_public(system_store, name_filter, running_only, rows);
}

/* ---- the table renderer ------------------------------------------------ */

/* The Docker-shaped call table, content-sized like `docker ps`: a first pass
 * measures every column from the actual rows (never a fixed printf width that
 * shears a long value), a second pass prints. Two-space gutters; each column
 * at least as wide as its header; the final NAMES column is never padded, so
 * no line carries trailing spaces.
 *
 * with_user adds the USER column in Docker's IMAGE slot (second). It is
 * exclusive to `list`: `ps` speaks only Docker, which has no USER concept, and
 * does not repurpose IMAGE's position -- ps simply omits it. */
#define TABLE_COLS 7

static void row_cells(const struct list_row *r, const char *cmd_cell, const char *cells[TABLE_COLS]) {
    cells[0] = r->id;
    cells[1] = r->owner[0] ? r->owner : "-";
    cells[2] = cmd_cell;
    cells[3] = r->created[0] ? r->created : "-";
    cells[4] = r->status[0] ? r->status : r->state;
    cells[5] = r->ports;
    cells[6] = r->name[0] ? r->name : "-";
}

static void print_table_row(const char *cells[TABLE_COLS], const size_t widths[TABLE_COLS], bool with_user) {
    for (int c = 0; c < TABLE_COLS; c++) {
        if (c == 1 && !with_user) continue;
        if (c < TABLE_COLS - 1) printf("%-*s  ", (int)widths[c], cells[c]);
        else printf("%s\n", cells[c]);
    }
}

static int print_collected_table(struct list_rows *rows, bool with_user) {
    static const char *headers[TABLE_COLS] = {
        "CALL ID", "USER", "COMMAND", "CREATED", "STATUS", "PORTS", "NAMES"
    };
    if (rows->count > 1) qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    char (*cmds)[PS_CMD_CELL] = NULL;
    if (rows->count > 0 && !(cmds = malloc(rows->count * sizeof(*cmds)))) return 3;
    size_t widths[TABLE_COLS];
    for (int c = 0; c < TABLE_COLS; c++) widths[c] = strlen(headers[c]);
    for (size_t i = 0; i < rows->count; i++) {
        const struct list_row *r = &rows->items[i];
        /* A projected global row has no command to show: the COMMAND cell is a
         * bare "hidden" (never quoted like a real command, never leaked). */
        if (r->redacted) snprintf(cmds[i], sizeof(cmds[i]), "%s", "hidden");
        else quote_command(cmds[i], r->cmd);
        const char *cells[TABLE_COLS];
        row_cells(r, cmds[i], cells);
        for (int c = 0; c < TABLE_COLS; c++)
            if (strlen(cells[c]) > widths[c]) widths[c] = strlen(cells[c]);
    }
    print_table_row(headers, widths, with_user);
    for (size_t i = 0; i < rows->count; i++) {
        const char *cells[TABLE_COLS];
        row_cells(&rows->items[i], cmds[i], cells);
        print_table_row(cells, widths, with_user);
    }
    free(cmds);
    return 0;
}

/* ---- list / ps --------------------------------------------------------- */

/* list is Hold's scoped ledger. The requested scope resolves against
 * privilege: a normal user's default is their own store, root's the global
 * one. SYSTEM shows the global store (real for root, redacted for a normal
 * user); USER shows personal calls only, even under sudo; BOTH (-a) unions
 * the two. Every list view carries the USER column. */
int hold_cmd_list(const struct hold_invocation *inv, const struct hold_store *user_store,
                  const struct hold_store *system_store, const char *name_filter,
                  enum hold_list_scope scope, bool live_only) {
    if (scope == HOLD_LIST_SCOPE_DEFAULT)
        scope = inv->euid_root ? HOLD_LIST_SCOPE_SYSTEM : HOLD_LIST_SCOPE_USER;
    struct list_rows rows = {0};
    int rc = 0;
    if (scope != HOLD_LIST_SCOPE_USER && /* the system side of SYSTEM and BOTH */
        collect_system_view(inv, system_store, name_filter, live_only, &rows) != 0)
        rc = 3;
    if (rc == 0 && scope != HOLD_LIST_SCOPE_SYSTEM) { /* the user side of USER and BOTH */
        int crc = (scope == HOLD_LIST_SCOPE_BOTH && inv->euid_root)
                      ? collect_all_user_stores(name_filter, live_only, &rows)
                      : collect_list_private(user_store, name_filter, live_only, false, &rows);
        if (crc != 0) rc = 3;
    }
    if (rc == 0) rc = print_collected_table(&rows, true);
    free_list_rows(&rows);
    return rc;
}

/* ps is Docker's machine-wide, running-first view. Docker has no user/system
 * split, so a faithful ps shows everything running on the machine and speaks
 * only Docker: no scope flags, no USER column. A normal user sees their own
 * calls in full and the global calls redacted; root sees the global store
 * directly. -a adds ended calls. */
int hold_cmd_ps(const struct hold_invocation *inv, const struct hold_store *user_store,
                const struct hold_store *system_store, const char *name_filter, bool all) {
    bool running_only = !all;
    struct list_rows rows = {0};
    int rc = 0;
    if ((!inv->euid_root && collect_list_private(user_store, name_filter, running_only, false, &rows) != 0) ||
        collect_system_view(inv, system_store, name_filter, running_only, &rows) != 0)
        rc = 3;
    if (rc == 0) rc = print_collected_table(&rows, false);
    free_list_rows(&rows);
    return rc;
}

/* ---- purge / prune ----------------------------------------------------- */

static void unlink_public_index(const struct hold_store *store, const char *id) {
    if (store->kind != STORE_SYSTEM_MANAGED || !id || !*id) return;
    char path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "%s/%s.json", store->public_dir, id) == 0) unlink(path);
}

static void unlink_log_index_for_log(const char *log_path) {
    char idx[HOLD_PATH_MAX];
    if (hold_log_idx_path(log_path, idx, sizeof(idx)) == 0) unlink(idx);
}

/* Purge derives its unlink targets from the store layout and the call id;
 * a path string stored inside a record is never followed for deletion. */
static void unlink_call_artifacts(const struct hold_store *store, const struct hold_run_record *r) {
    char log_path[HOLD_PATH_MAX], sock_path[HOLD_PATH_MAX];
    if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s.log", store->log_dir, r->id) == 0) {
        if (r->has_log && strcmp(r->log_path, log_path) != 0)
            fprintf(stderr, "hold: warning: record %s names a log outside the store; left untouched: %s\n", r->id, r->log_path);
        unlink_log_index_for_log(log_path);
        unlink(log_path);
    }
    if (hold_format_console_sock_path(store, r->id, sock_path, sizeof(sock_path)) == 0) {
        if (r->has_console && strcmp(r->console_sock, sock_path) != 0)
            fprintf(stderr, "hold: warning: record %s names a console socket outside the store; left untouched: %s\n", r->id, r->console_sock);
        unlink(sock_path);
    }
}

int hold_prune_one_run(const struct hold_store *store, const char *id, const char *boot, bool allow_stale, bool *removed) {
    struct hold_run_record r;
    char path[HOLD_PATH_MAX];
    if (hold_load_record_by_id(store->record_dir, id, &r, path, sizeof(path)) != 0) return 5;
    enum run_state st = hold_eval_state(&r, boot ? boot : NULL);
    if (st != STATE_EXITED && st != STATE_FAILED && !(allow_stale && st == STATE_STALE)) {
        fprintf(stderr, "hold: error: call %s is %s and cannot be purged\n", id, hold_state_str(st));
        hold_free_run_record(&r);
        return 2;
    }
    unlink(path);
    unlink_call_artifacts(store, &r);
    unlink_public_index(store, id);
    if (removed) *removed = true;
    hold_free_run_record(&r);
    return 0;
}

struct prune_sweep_stats { int removed, kept_live, kept_stale, kept_saved; };

/* A young .<id>.reserve marks a call mid-creation (log and socket exist
 * before the record JSON); the sweep must not eat it. A stale reserve is a
 * dead start's litter: clear it and let the orphan collection proceed. */
static bool orphan_creation_reserved(const struct hold_store *store, const char *id) {
    char reserve[HOLD_PATH_MAX];
    struct stat st;
    if (hold_checked_snprintf(reserve, sizeof(reserve), "%s/.%s.reserve", store->record_dir, id) != 0) return false;
    if (lstat(reserve, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (time(NULL) - st.st_mtime < 600) return true;
    unlink(reserve);
    return false;
}

/* Collect abandoned dot-prefixed temp files (unique-temp litter and legacy
 * fixed-name tombs); a 60s age gate protects any live writer's window. */
static void sweep_tmp_litter(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] != '.' || !hold_has_suffix(e->d_name, ".tmp")) continue;
        char path[HOLD_PATH_MAX];
        struct stat st;
        if (hold_checked_snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) != 0) continue;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && time(NULL) - st.st_mtime > 60) unlink(path);
    }
    closedir(d);
}

/* One orphan sweep: any artifact in dir with the given suffix whose id has
 * no record left is removed (a public entry, log, or console socket whose
 * record is gone must never resurrect a call). is_log also drops the
 * .log.idx sidecar beside each removed log. */
static void sweep_orphaned_artifacts(const struct hold_store *store, const char *dir,
                                     const char *suffix, bool is_log, struct prune_sweep_stats *stats) {
    DIR *d = opendir(dir);
    if (!d) return;
    const struct dirent *e;
    while ((e = readdir(d))) {
        char id[ID_STR_LEN], json_path[HOLD_PATH_MAX], artifact_path[HOLD_PATH_MAX];
        if (!id_from_artifact_name(e->d_name, suffix, id)) continue;
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            hold_checked_snprintf(artifact_path, sizeof(artifact_path), "%s/%s", dir, e->d_name) != 0) continue;
        if (access(json_path, F_OK) != 0 && !orphan_creation_reserved(store, id)) {
            if (is_log) unlink_log_index_for_log(artifact_path);
            unlink(artifact_path);
            stats->removed++;
        }
    }
    closedir(d);
}

struct prune_walk {
    const struct hold_store *store;
    const char *boot_id;
    bool include_stale;
    struct prune_sweep_stats *stats;
};

static int prune_record_cb(const char *id, const char *path, struct hold_run_record *r, void *vctx) {
    (void)id;
    struct prune_walk *w = vctx;
    if (!r) {
        unlink(path);
        return 0;
    }
    enum run_state st = hold_eval_state(r, w->boot_id);
    /* The sweep accounts for everything it sees: live calls are end's
     * business, stale needs -a, and saved calls need a targeted --force. */
    if (st == STATE_RUNNING) {
        w->stats->kept_live++;
    } else if (st == STATE_STALE && !w->include_stale) {
        w->stats->kept_stale++;
    } else if (r->saved && (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE)) {
        w->stats->kept_saved++;
    } else if (st == STATE_EXITED || st == STATE_FAILED || st == STATE_STALE) {
        unlink(path);
        unlink_call_artifacts(w->store, r);
        unlink_public_index(w->store, r->id);
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(r->id, display_id);
        /* Docker prune prints what it deletes; so do we. */
        printf("%s%s%s\n", display_id, r->has_name && r->name[0] ? "  " : "", r->has_name && r->name[0] ? r->name : "");
        w->stats->removed++;
    }
    return 0;
}

static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, struct prune_sweep_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    char boot[128];
    struct prune_walk w = { store, hold_boot_id_or_null(boot), include_stale, stats };
    hold_for_each_record(store->record_dir, prune_record_cb, &w);
    sweep_orphaned_artifacts(store, store->log_dir, ".log", true, stats);
    sweep_orphaned_artifacts(store, store->console_dir, ".sock", false, stats);
    sweep_orphaned_artifacts(store, store->public_dir, ".json", false, stats);
    sweep_tmp_litter(store->record_dir);
    sweep_tmp_litter(store->public_dir);
    return 0;
}

/* Account for everything the sweep saw, so "purged 6" while the visible list
 * still has entries is never a mystery. snprintf returns the would-have-
 * written length; the clamp keeps a (theoretical) truncation from wrapping
 * `n - off` into a huge size on the next append. */
static void format_kept_note(const struct prune_sweep_stats *stats, char *kept, size_t n) {
    const struct { int count; const char *what; } parts[] = {
        { stats->kept_live, "live" }, { stats->kept_stale, "stale" }, { stats->kept_saved, "saved" },
    };
    size_t off = 0;
    kept[0] = '\0';
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (parts[i].count <= 0) continue;
        int w = snprintf(kept + off, n - off, "%s%d %s", off ? ", " : "; kept ", parts[i].count, parts[i].what);
        if (w > 0) off = (size_t)w >= n - off ? n - 1 : off + (size_t)w;
    }
}

int hold_cmd_purge_action(const struct hold_invocation *inv, const struct hold_store *user_store,
                          const struct hold_store *system_store, const char *target_token,
                          bool all, bool force, bool system_scope) {
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
            char kept[128];
            format_kept_note(&stats, kept, sizeof(kept));
            const char *hint = stats.kept_stale > 0 ? " (purge -a sweeps stale)" : "";
            if (stats.removed > 0)
                hold_sig_note(inv, "hold: purged %d past call%s%s%s\n",
                              stats.removed, stats.removed == 1 ? "" : "s", kept, hint);
            else
                hold_sig_note(inv, "hold: nothing to purge%s%s\n", kept, hint);
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
            if (hold_load_record_by_id(targets[i].store.record_dir, targets[i].id, &r, rp, sizeof(rp)) != 0) continue;
            char display_id[ID_DISPLAY_HEX_LEN + 1];
            hold_run_id_display(r.id, display_id);
            if (r.saved) {
                fprintf(stderr,
                        "hold: '%s' is saved \xe2\x80\x94 purging a saved call requires --force\n"
                        "  hold purge %s --force\n",
                        r.has_name ? r.name : display_id, target_token);
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
        if (removed) removed_count++;
        if (rc > worst) worst = rc;
    }
    if (worst == 0) {
        if (removed_count > 0) {
            const char *atom = NULL;
            enum id_token_scope token_scope = hold_parse_id_token(target_token, &atom);
            if (token_scope != ID_TOKEN_INVALID && atom && hold_valid_alias(atom) && !hold_valid_id_prefix(atom))
                hold_sig_note(inv, "hold: purged %d past call%s for '%s'\n",
                              removed_count, removed_count == 1 ? "" : "s", atom);
            else
                hold_sig_note(inv, "hold: purged %d past call%s\n", removed_count, removed_count == 1 ? "" : "s");
        } else {
            hold_sig_note(inv, "hold: nothing to purge\n");
        }
    }
    free(targets);
    return worst;
}
