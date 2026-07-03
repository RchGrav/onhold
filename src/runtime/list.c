#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/store.h"
#include "hold/console.h"
#include "hold/access.h"

struct list_row {
    char id[ID_STR_LEN];
    char name[ALIAS_MAX_LEN + 1];
    char state[16];
    char started[64];
    char created[64];
    char status[64];
    char ports[HOLD_PATH_MAX];
    char cmd[HOLD_PATH_MAX];
    int64_t start_unix_ns;
    bool running;
};

struct list_rows {
    struct list_row *items;
    size_t count;
};

static void free_list_rows(struct list_rows *rows);
static int append_list_row(struct list_rows *rows, const struct list_row *row);
static int compare_list_rows(const void *a, const void *b);
static void print_list_header(bool iso);
static void print_list_row(const struct list_row *row, bool iso);
static void print_ps_header(void);
static void print_ps_row(const struct list_row *row);
static int collect_list_private(const struct hold_store *store,
                                const char *alias_filter,
                                bool iso,
                                bool include_all,
                                bool docker_ps,
                                struct list_rows *rows);
static int collect_list_public(const struct hold_store *store,
                               const char *alias_filter,
                               bool iso,
                               bool include_all,
                               bool docker_ps,
                               struct list_rows *rows);
static int print_collected_list(struct list_rows *rows, bool iso);
static int print_collected_ps(struct list_rows *rows);
static void unlink_public_index(const struct hold_store *store, const char *id);
static void unlink_log_index_for_log(const char *log_path);
static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, int *removed_count);

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

static void print_list_header(bool iso) {
    printf("%-12s %-8s %-*s %s\n", "CALL ID", "STATE", iso ? 24 : 8, iso ? "STARTED_AT" : "STARTED", "CMD");
}

static void print_list_row(const struct list_row *row, bool iso) {
    char cmd[80];
    const char *src = row->cmd[0] ? row->cmd : "?";
    snprintf(cmd, sizeof(cmd), "%.72s%s", src, strlen(src) > 72 ? "..." : "");
    printf("%-12s %-8s %-*s %s\n", row->id, row->state, iso ? 24 : 8, row->started, cmd);
}

static void quote_command(char out[32], const char *cmd) {
    const char *src = (cmd && *cmd) ? cmd : "?";
    char body[28];
    size_t len = strlen(src);
    snprintf(body, sizeof(body), "%.24s%s", src, len > 24 ? "..." : "");
    snprintf(out, 32, "\"%s\"", body);
}

static void print_ps_header(void) {
    printf("%-12s %-28s %-14s %-22s %-36s %s\n",
           "CALL ID", "COMMAND", "CREATED", "STATUS", "PORTS", "NAMES");
}

static void print_ps_row(const struct list_row *row) {
    char cmd[32];
    quote_command(cmd, row->cmd);
    printf("%-12s %-28s %-14s %-22s %-36s %s\n",
           row->id,
           cmd,
           row->created[0] ? row->created : "-",
           row->status[0] ? row->status : row->state,
           row->ports,
           row->name[0] ? row->name : "-");
}

static void format_status(enum run_state st, const struct hold_run_record *r, char out[64]) {
    char age[24];
    hold_format_relative_age(r->start_unix_ns, age, sizeof(age));
    switch (st) {
    case STATE_RUNNING:
        snprintf(out, 64, "Up %s", age);
        break;
    case STATE_EXITED:
        if (r->has_exit_code) {
            snprintf(out, 64, "Exited (%d) %s", r->exit_code, age);
        } else if (r->has_term_signal) {
            snprintf(out, 64, "Exited (%d) %s", 128 + r->term_signal, age);
        } else {
            snprintf(out, 64, "Exited (?) %s", age);
        }
        break;
    case STATE_FAILED:
        snprintf(out, 64, "Failed %s", age);
        break;
    case STATE_STALE:
        snprintf(out, 64, "Stale %s", age);
        break;
    default:
        snprintf(out, 64, "Unknown");
        break;
    }
}

#if defined(__linux__)
struct inode_set {
    unsigned long long *items;
    size_t count;
};

static void inode_set_free(struct inode_set *set) {
    free(set->items);
    set->items = NULL;
    set->count = 0;
}

static bool inode_set_contains(const struct inode_set *set, unsigned long long inode) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i] == inode) return true;
    }
    return false;
}

static int inode_set_add(struct inode_set *set, unsigned long long inode) {
    if (inode == 0 || inode_set_contains(set, inode)) return 0;
    unsigned long long *next = realloc(set->items, (set->count + 1) * sizeof(*next));
    if (!next) return -1;
    set->items = next;
    set->items[set->count++] = inode;
    return 0;
}

static int list_read_proc_ids(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
    char path[128], buf[4096];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t nr;
    do {
        nr = read(fd, buf, sizeof(buf) - 1);
    } while (nr < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (nr <= 0) {
        errno = nr < 0 ? saved : EIO;
        return -1;
    }
    buf[nr] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp) {
        errno = EINVAL;
        return -1;
    }
    char *fields = rp + 2;
    char *save = NULL;
    int idx = 0;
    bool got_pgid = false, got_sid = false, got_state = false;
    pid_t pgid = 0, sid = 0;
    char state = 0;
    for (char *tok = strtok_r(fields, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
        if (idx == 0) {
            state = tok[0];
            got_state = true;
        } else if (idx == 2) {
            pgid = (pid_t)strtol(tok, NULL, 10);
            got_pgid = true;
        } else if (idx == 3) {
            sid = (pid_t)strtol(tok, NULL, 10);
            got_sid = true;
            break;
        }
    }
    if ((pgid_out && !got_pgid) || (sid_out && !got_sid) || (state_out && !got_state)) {
        errno = EINVAL;
        return -1;
    }
    if (pgid_out) *pgid_out = pgid;
    if (sid_out) *sid_out = sid;
    if (state_out) *state_out = state;
    return 0;
}

static int collect_pid_socket_inodes(pid_t pid, struct inode_set *set) {
    char fd_dir[128];
    if (hold_checked_snprintf(fd_dir, sizeof(fd_dir), "/proc/%ld/fd", (long)pid) != 0) return 0;
    DIR *d = opendir(fd_dir);
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char fd_path[HOLD_PATH_MAX], target[256];
        if (hold_checked_snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir, e->d_name) != 0) continue;
        ssize_t n = readlink(fd_path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        unsigned long long inode = 0;
        if (sscanf(target, "socket:[%llu]", &inode) == 1 && inode_set_add(set, inode) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int collect_run_socket_inodes(const struct hold_run_record *r, struct inode_set *set) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char *end = NULL;
        long pid_long = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end || pid_long <= 0) continue;
        pid_t pgid = 0, sid = 0;
        char state = 0;
        if (list_read_proc_ids((pid_t)pid_long, &pgid, &sid, &state) != 0) continue;
        if (pgid != r->pgid || sid != r->sid || state == 'Z') continue;
        if (collect_pid_socket_inodes((pid_t)pid_long, set) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static bool append_port(char *out, size_t n, const char *entry) {
    if (!entry || !*entry) return true;
    if (strstr(out, entry)) return true;
    size_t used = strlen(out);
    const char *sep = used ? ", " : "";
    return hold_checked_snprintf(out + used, n - used, "%s%s", sep, entry) == 0;
}

static void ipv4_from_proc_hex(const char *hex, char out[32]) {
    unsigned long v = strtoul(hex, NULL, 16);
    snprintf(out, 32, "%lu.%lu.%lu.%lu",
             v & 0xffUL,
             (v >> 8) & 0xffUL,
             (v >> 16) & 0xffUL,
             (v >> 24) & 0xffUL);
}

static bool ipv6_proc_hex_is_any(const char *hex) {
    if (!hex) return false;
    for (size_t i = 0; i < 32 && hex[i]; i++) {
        if (hex[i] != '0') return false;
    }
    return strlen(hex) >= 32;
}

static int scan_proc_net_file(const char *path, const char *proto, const struct inode_set *set, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *fields[16] = {0};
        int nf = 0;
        char *save = NULL;
        for (char *tok = strtok_r(line, " \t\r\n", &save); tok && nf < 16; tok = strtok_r(NULL, " \t\r\n", &save)) {
            fields[nf++] = tok;
        }
        if (nf <= 9 || strcmp(fields[3], "0A") != 0) continue;
        unsigned long long inode = strtoull(fields[9], NULL, 10);
        if (!inode_set_contains(set, inode)) continue;
        char addr_hex[64] = {0};
        char port_hex[16] = {0};
        if (sscanf(fields[1], "%63[^:]:%15s", addr_hex, port_hex) != 2) continue;
        unsigned long port = strtoul(port_hex, NULL, 16);
        char host[80];
        if (strstr(path, "tcp6")) {
            snprintf(host, sizeof(host), "%s", ipv6_proc_hex_is_any(addr_hex) ? "[::]" : "[::]");
        } else {
            ipv4_from_proc_hex(addr_hex, host);
        }
        char entry[128];
        snprintf(entry, sizeof(entry), "%s:%lu/%s", host, port, proto);
        if (!append_port(out, n, entry)) break;
    }
    fclose(f);
    return 0;
}

static void observe_run_ports(const struct hold_run_record *r, char out[HOLD_PATH_MAX]) {
    out[0] = '\0';
    if (!r || r->pgid <= 1 || r->sid <= 0) return;
    struct inode_set set = {0};
    if (collect_run_socket_inodes(r, &set) == 0 && set.count > 0) {
        (void)scan_proc_net_file("/proc/net/tcp", "tcp", &set, out, HOLD_PATH_MAX);
        (void)scan_proc_net_file("/proc/net/tcp6", "tcp", &set, out, HOLD_PATH_MAX);
    }
    inode_set_free(&set);
}
#else
static void observe_run_ports(const struct hold_run_record *r, char out[HOLD_PATH_MAX]) {
    (void)r;
    out[0] = '\0';
}
#endif

static int collect_list_private(const struct hold_store *store,
                                const char *alias_filter,
                                bool iso,
                                bool include_all,
                                bool docker_ps,
                                struct list_rows *rows) {
    DIR *d = opendir(store->record_dir);
    if (!d) {
        return 0;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
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
        if (alias_filter && (!r.has_alias || strcmp(r.alias, alias_filter) != 0)) {
            hold_free_run_record(&r);
            continue;
        }
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        if (!include_all && st != STATE_RUNNING) {
            hold_free_run_record(&r);
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(r.id, display_id);
        snprintf(row.id, sizeof(row.id), "%s", display_id);
        if (r.has_name && r.name[0]) snprintf(row.name, sizeof(row.name), "%s", r.name);
        snprintf(row.state, sizeof(row.state), "%s", hold_state_str(st));
        row.start_unix_ns = r.start_unix_ns;
        row.running = st == STATE_RUNNING;
        int64_t created_ns = r.has_created_at && r.created_unix_ns > 0 ? r.created_unix_ns : r.start_unix_ns;
        if (iso) {
            if (r.has_started_at && r.started_at[0]) {
                snprintf(row.started, sizeof(row.started), "%s", r.started_at);
            } else {
                hold_format_rfc3339_utc_from_ns(r.start_unix_ns, row.started, sizeof(row.started));
            }
            if (r.has_created_at && r.created_at[0]) {
                snprintf(row.created, sizeof(row.created), "%s", r.created_at);
            } else {
                hold_format_rfc3339_utc_from_ns(created_ns, row.created, sizeof(row.created));
            }
        } else {
            hold_format_relative_age(r.start_unix_ns, row.started, sizeof(row.started));
            hold_format_relative_age(created_ns, row.created, sizeof(row.created));
        }
        snprintf(row.cmd, sizeof(row.cmd), "%s", r.cmdline[0] ? r.cmdline : "?");
        if (docker_ps) {
            format_status(st, &r, row.status);
            if (st == STATE_RUNNING) {
                observe_run_ports(&r, row.ports);
            }
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

static int collect_list_public(const struct hold_store *store,
                               const char *alias_filter,
                               bool iso,
                               bool include_all,
                               bool docker_ps,
                               struct list_rows *rows) {
    if (!include_all) {
        return 0;
    }
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
        if (alias_filter && (!pi.has_alias || strcmp(pi.alias, alias_filter) != 0)) {
            continue;
        }
        struct list_row row;
        memset(&row, 0, sizeof(row));
        char display_id[ID_DISPLAY_HEX_LEN + 1];
        hold_run_id_display(pi.id, display_id);
        snprintf(row.id, sizeof(row.id), "%s", display_id);
        if (pi.has_name && pi.name[0]) snprintf(row.name, sizeof(row.name), "%s", pi.name);
        snprintf(row.state, sizeof(row.state), "%s", pi.state_hint[0] ? pi.state_hint : "unknown");
        row.running = !strcmp(row.state, "running");
        row.start_unix_ns = 0;
        if (iso) {
            snprintf(row.started, sizeof(row.started), "%s", pi.started_at[0] ? pi.started_at : "-");
        } else {
            snprintf(row.started, sizeof(row.started), "%s", "-");
        }
        snprintf(row.created, sizeof(row.created), "%s", pi.created_at[0] ? pi.created_at : row.started);
        snprintf(row.cmd, sizeof(row.cmd), "%s", "<root-managed>");
        if (docker_ps) {
            snprintf(row.status, sizeof(row.status), "%s", pi.state_hint[0] ? pi.state_hint : "Unknown");
        }
        if (append_list_row(rows, &row) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int print_collected_list(struct list_rows *rows, bool iso) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }
    print_list_header(iso);
    for (size_t i = 0; i < rows->count; i++) {
        print_list_row(&rows->items[i], iso);
    }
    return 0;
}

static int print_collected_ps(struct list_rows *rows) {
    if (rows->count > 1) {
        qsort(rows->items, rows->count, sizeof(rows->items[0]), compare_list_rows);
    }
    print_ps_header();
    for (size_t i = 0; i < rows->count; i++) {
        print_ps_row(&rows->items[i]);
    }
    return 0;
}

int hold_cmd_list_normal(const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, alias_filter, iso, true, false, &rows) != 0 ||
        collect_list_public(system_store, alias_filter, iso, true, false, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

int hold_cmd_list_system(const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, alias_filter, iso, true, false, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_list(&rows, iso);
    }
    free_list_rows(&rows);
    return rc;
}

int hold_cmd_ps_normal(const struct hold_store *user_store,
                       const struct hold_store *system_store,
                       bool all) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(user_store, NULL, false, all, true, &rows) != 0 ||
        collect_list_public(system_store, NULL, false, all, true, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_ps(&rows);
    }
    free_list_rows(&rows);
    return rc;
}

int hold_cmd_ps_system(const struct hold_store *system_store, bool all) {
    struct list_rows rows = {0};
    int rc = 0;
    if (collect_list_private(system_store, NULL, false, all, true, &rows) != 0) {
        rc = 3;
    } else {
        rc = print_collected_ps(&rows);
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

static int cmd_prune_store_all(const struct hold_store *store, bool include_stale, int *removed_count) {
    if (removed_count) {
        *removed_count = 0;
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
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
        enum run_state st = hold_eval_state(&r, have_boot ? boot : NULL);
        if (st == STATE_EXITED || st == STATE_FAILED || (include_stale && st == STATE_STALE)) {
            unlink(path);
            if (r.has_log) {
                unlink(r.log_path);
            }
            if (r.has_console) {
                unlink(r.console_sock);
            }
            unlink_public_index(store, r.id);
            if (removed_count) {
                (*removed_count)++;
            }
        }
        hold_free_run_record(&r);
    }
    closedir(d);

sweep_logs:
    d = opendir(store->log_dir);
    if (!d) {
        goto sweep_consoles;
    }
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".log")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 4) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - 4;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], log_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0) {
            continue;
        }
        if (hold_checked_snprintf(log_path, sizeof(log_path), "%s/%s", store->log_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink_log_index_for_log(log_path);
            unlink(log_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);

sweep_consoles:
    d = opendir(store->console_dir);
    if (!d) {
        goto sweep_public;
    }
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".sock")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], sock_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            hold_checked_snprintf(sock_path, sizeof(sock_path), "%s/%s", store->console_dir, e->d_name) != 0) {
            continue;
        }
        if (access(json_path, F_OK) != 0) {
            unlink(sock_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);

sweep_public:
    d = opendir(store->public_dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d))) {
        if (!hold_has_suffix(e->d_name, ".json")) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len <= 5) {
            continue;
        }
        char id[ID_STR_LEN];
        size_t id_len = len - 5;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, e->d_name, id_len);
        id[id_len] = '\0';
        if (!hold_valid_id(id)) {
            continue;
        }
        char json_path[HOLD_PATH_MAX], public_path[HOLD_PATH_MAX];
        if (hold_checked_snprintf(json_path, sizeof(json_path), "%s/%s.json", store->record_dir, id) != 0 ||
            hold_checked_snprintf(public_path, sizeof(public_path), "%s/%s", store->public_dir, e->d_name) != 0) {
            continue;
        }
        /* A public index entry whose record is gone is an orphan; ps must not resurrect it. */
        if (access(json_path, F_OK) != 0) {
            unlink(public_path);
            if (removed_count) {
                (*removed_count)++;
            }
        }
    }
    closedir(d);
    return 0;
}

int hold_cmd_purge_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *target_token,
                            bool all,
                            bool force) {
    if (!target_token || strcmp(target_token, "all") == 0) {
        /* A no-target sweep only clears already-ended calls; --force adds nothing
         * here (mass-ending live calls needs the saved-call protection that lands
         * with a later task). */
        const struct hold_store *store = inv->euid_root ? system_store : user_store;
        int removed = 0;
        bool include_stale = all || force || (target_token && strcmp(target_token, "all") == 0);
        int rc = cmd_prune_store_all(store, include_stale, &removed);
        if (rc == 0) {
            if (removed > 0) {
                hold_sig_note(inv, "hold: purged %d past call%s\n", removed, removed == 1 ? "" : "s");
            } else {
                hold_sig_note(inv, "hold: nothing to purge\n");
            }
        }
        return rc;
    }
    /* --force on a specific target ends a live call first, then removes it.
     * Saved-call protection (refuse without --force) arrives with a later task;
     * this is the seam it plugs into. */
    if (force) {
        char *one[1] = { (char *)target_token };
        int stop_rc = hold_cmd_signal_action(inv, user_store, system_store, "stop", 1, one,
                                             SIGTERM, true, false, false);
        if (stop_rc != 0) {
            return stop_rc;
        }
    }
    struct hold_resolved_target *targets = NULL;
    int ntargets = 0;
    int rc = hold_resolve_action_token(inv, user_store, system_store, "prune", target_token, all, &targets, &ntargets);
    if (rc != 0) {
        free(targets);
        return rc;
    }
    if (ntargets == 0) {
        free(targets);
        hold_sig_note(inv, "hold: nothing to purge\n");
        return 0;
    }
    for (int i = 0; i < ntargets; i++) {
        if (targets[i].requires_root) {
            rc = hold_report_requires_root(targets[i].id);
            free(targets);
            return rc;
        }
    }
    char boot[128] = {0};
    bool have_boot = hold_current_boot_id(boot, sizeof(boot));
    int worst = 0;
    int removed_count = 0;
    for (int i = 0; i < ntargets; i++) {
        bool removed = false;
        rc = hold_prune_one_run(&targets[i].store, targets[i].id, have_boot ? boot : NULL, true, &removed);
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
