#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"
#include "hold/platform.h"
#include "hold/observe.h"

void hold_port_list_free(struct hold_port_list *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

#if defined(__linux__)

static int port_list_add(struct hold_port_list *list, const char *entry) {
    if (!entry || !*entry) {
        return 0;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], entry) == 0) {
            return 0;
        }
    }
    char **next = realloc(list->items, (list->count + 1) * sizeof(*next));
    if (!next) {
        return -1;
    }
    list->items = next;
    list->items[list->count] = strdup(entry);
    if (!list->items[list->count]) {
        return -1;
    }
    list->count++;
    return 0;
}

struct pid_accum {
    pid_t *pids;
    size_t count;
    bool oom;
};

static int accum_pid_cb(pid_t pid, void *ctx) {
    struct pid_accum *acc = ctx;
    pid_t *next = realloc(acc->pids, (acc->count + 1) * sizeof(*next));
    if (!next) {
        acc->oom = true;
        return -1;
    }
    acc->pids = next;
    acc->pids[acc->count++] = pid;
    return 0;
}

int hold_observe_run_pids(const struct hold_run_record *r,
                          pid_t **pids_out,
                          size_t *count_out,
                          bool *denied) {
    *pids_out = NULL;
    *count_out = 0;
    if (denied) *denied = false;
    if (!r || r->pgid <= 1 || r->sid <= 0) {
        return 0;
    }
    struct pid_accum acc = {0};
    if (hold_for_each_group_process(r->pgid, r->sid, accum_pid_cb, &acc, denied) != 0 && acc.oom) {
        free(acc.pids);
        return -1;
    }
    *pids_out = acc.pids;
    *count_out = acc.count;
    return 0;
}

/* A deduplicated set of the group's socket inodes. */
struct inode_accum {
    unsigned long long *items;
    size_t count;
};

static int add_inode_cb(unsigned long long inode, void *ctx) {
    struct inode_accum *set = ctx;
    if (inode == 0) {
        return 0;
    }
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i] == inode) return 0;
    }
    unsigned long long *next = realloc(set->items, (set->count + 1) * sizeof(*next));
    if (!next) {
        return -1;
    }
    set->items = next;
    set->items[set->count++] = inode;
    return 0;
}

struct socket_scan {
    struct inode_accum *set;
    bool *denied;
};

static int socket_pid_cb(pid_t pid, void *ctx) {
    struct socket_scan *scan = ctx;
    return hold_proc_socket_inodes(pid, add_inode_cb, scan->set, scan->denied);
}

static int emit_port_cb(const char *entry, void *ctx) {
    return port_list_add(ctx, entry);
}

int hold_observe_run_ports(const struct hold_run_record *r,
                           struct hold_port_list *out,
                           bool *denied) {
    out->items = NULL;
    out->count = 0;
    if (denied) *denied = false;
    if (!r || r->pgid <= 1 || r->sid <= 0) {
        return 0;
    }
    struct inode_accum set = {0};
    struct socket_scan scan = {.set = &set, .denied = denied};
    if (hold_for_each_group_process(r->pgid, r->sid, socket_pid_cb, &scan, denied) != 0) {
        free(set.items);
        return -1;
    }
    if (set.count > 0) {
        (void)hold_scan_listening_sockets(set.items, set.count, emit_port_cb, out);
    }
    free(set.items);
    return 0;
}

#else /* !__linux__ */

int hold_observe_run_pids(const struct hold_run_record *r, pid_t **pids_out, size_t *count_out, bool *denied) {
    (void)r;
    *pids_out = NULL;
    *count_out = 0;
    if (denied) *denied = false;
    return 0;
}

int hold_observe_run_ports(const struct hold_run_record *r, struct hold_port_list *out, bool *denied) {
    (void)r;
    out->items = NULL;
    out->count = 0;
    if (denied) *denied = false;
    return 0;
}

#endif

void hold_observe_run_ports_column(const struct hold_run_record *r, char *out, size_t n) {
    out[0] = '\0';
    struct hold_port_list ports = {0};
    if (hold_observe_run_ports(r, &ports, NULL) == 0) {
        size_t used = 0;
        for (size_t i = 0; i < ports.count; i++) {
            const char *sep = used ? ", " : "";
            int wrote = snprintf(out + used, n - used, "%s%s", sep, ports.items[i]);
            if (wrote < 0 || (size_t)wrote >= n - used) {
                break;
            }
            used += (size_t)wrote;
        }
    }
    hold_port_list_free(&ports);
}
