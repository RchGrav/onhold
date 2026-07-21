#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"
#include "hold/observe.h"

#if defined(__linux__)
#include <arpa/inet.h>
#endif

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

int hold_proc_read_ids(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
    char path[128], buf[4096];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
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

int hold_proc_read_cpu_rss(pid_t pid, uint64_t *cpu_ticks_out, uint64_t *rss_bytes_out) {
    if (cpu_ticks_out) {
        char path[128], buf[4096];
        if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) != 0) {
            return -1;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return -1;
        }
        ssize_t nr;
        do {
            nr = read(fd, buf, sizeof(buf) - 1);
        } while (nr < 0 && errno == EINTR);
        close(fd);
        if (nr <= 0) {
            return -1;
        }
        buf[nr] = '\0';
        char *rp = strrchr(buf, ')');
        if (!rp) {
            return -1;
        }
        /* Fields after "(comm)" start at stat field 3 (state); utime is field 14
         * and stime field 15, i.e. tokens 11 and 12 counting state as token 0. */
        char *save = NULL;
        int idx = 0;
        unsigned long long utime = 0, stime = 0;
        bool got_u = false, got_s = false;
        for (char *tok = strtok_r(rp + 2, " ", &save); tok; tok = strtok_r(NULL, " ", &save), idx++) {
            if (idx == 11) { utime = strtoull(tok, NULL, 10); got_u = true; }
            else if (idx == 12) { stime = strtoull(tok, NULL, 10); got_s = true; break; }
        }
        if (!got_u || !got_s) {
            return -1;
        }
        *cpu_ticks_out = (uint64_t)(utime + stime);
    }
    if (rss_bytes_out) {
        char path[128], buf[256];
        if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/statm", (long)pid) != 0) {
            return -1;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return -1;
        }
        ssize_t nr;
        do {
            nr = read(fd, buf, sizeof(buf) - 1);
        } while (nr < 0 && errno == EINTR);
        close(fd);
        if (nr <= 0) {
            return -1;
        }
        buf[nr] = '\0';
        /* statm: "size resident shared ..." in pages; resident is field 2. */
        unsigned long long size_pages = 0, resident_pages = 0;
        if (sscanf(buf, "%llu %llu", &size_pages, &resident_pages) != 2) {
            return -1;
        }
        long page = sysconf(_SC_PAGESIZE);
        *rss_bytes_out = (uint64_t)resident_pages * (uint64_t)(page > 0 ? page : 4096);
    }
    return 0;
}

int hold_proc_fd_target(pid_t pid, int fd, char *out, size_t n) {
    char path[128], target[HOLD_PATH_MAX];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/fd/%d", (long)pid, fd) != 0) {
        return -1;
    }
    ssize_t got = readlink(path, target, sizeof(target) - 1);
    if (got < 0) {
        return -1;
    }
    target[got] = '\0';
    /* A held call's captured stdout/stderr point at Hold's own capture pipe;
     * report the plumbing by its role, not its ephemeral inode. */
    if (strncmp(target, "pipe:", 5) == 0) {
        snprintf(out, n, "pipe:hold");
    } else {
        snprintf(out, n, "%s", target);
    }
    return 0;
}

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

/* Enumerate the live pids in the group, invoking cb(pid, ctx) for each. Returns
 * 0 on success, -1 on allocation failure. Sets *denied when a matching pid's
 * details are unreadable (EACCES). */
static int for_each_group_pid(const struct hold_run_record *r,
                              int (*cb)(pid_t, void *),
                              void *ctx,
                              bool *denied) {
    DIR *d = opendir("/proc");
    if (!d) {
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char *end = NULL;
        long pid_long = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end || pid_long <= 0) continue;
        pid_t pgid = 0, sid = 0;
        char state = 0;
        if (hold_proc_read_ids((pid_t)pid_long, &pgid, &sid, &state) != 0) {
            if (errno == EACCES && denied) *denied = true;
            continue;
        }
        if (pgid != r->pgid || sid != r->sid || state == 'Z') continue;
        if (cb((pid_t)pid_long, ctx) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
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
    if (for_each_group_pid(r, accum_pid_cb, &acc, denied) != 0 && acc.oom) {
        free(acc.pids);
        return -1;
    }
    *pids_out = acc.pids;
    *count_out = acc.count;
    return 0;
}

struct socket_accum {
    struct inode_set *set;
    bool *denied;
};

static int socket_inode_cb(pid_t pid, void *ctx) {
    struct socket_accum *acc = ctx;
    char fd_dir[128];
    if (hold_checked_snprintf(fd_dir, sizeof(fd_dir), "/proc/%ld/fd", (long)pid) != 0) {
        return 0;
    }
    DIR *d = opendir(fd_dir);
    if (!d) {
        if (errno == EACCES && acc->denied) *acc->denied = true;
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char fd_path[HOLD_PATH_MAX], target[256];
        if (hold_checked_snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir, e->d_name) != 0) continue;
        ssize_t nr = readlink(fd_path, target, sizeof(target) - 1);
        if (nr <= 0) continue;
        target[nr] = '\0';
        unsigned long long inode = 0;
        if (sscanf(target, "socket:[%llu]", &inode) == 1 && inode_set_add(acc->set, inode) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static void ipv4_from_proc_hex(const char *hex, char out[32]) {
    unsigned long v = strtoul(hex, NULL, 16);
    snprintf(out, 32, "%lu.%lu.%lu.%lu",
             v & 0xffUL,
             (v >> 8) & 0xffUL,
             (v >> 16) & 0xffUL,
             (v >> 24) & 0xffUL);
}

/* /proc renders an IPv6 address as four 32-bit words, each printed in host byte
 * order. Reassemble the 16 raw bytes and let inet_ntop compress them. */
static void ipv6_from_proc_hex(const char *hex, char out[48]) {
    unsigned char bytes[16] = {0};
    if (strlen(hex) < 32) {
        snprintf(out, 48, "[::]");
        return;
    }
    for (int w = 0; w < 4; w++) {
        char chunk[9];
        memcpy(chunk, hex + w * 8, 8);
        chunk[8] = '\0';
        unsigned long word = strtoul(chunk, NULL, 16);
        bytes[w * 4 + 0] = (unsigned char)(word & 0xff);
        bytes[w * 4 + 1] = (unsigned char)((word >> 8) & 0xff);
        bytes[w * 4 + 2] = (unsigned char)((word >> 16) & 0xff);
        bytes[w * 4 + 3] = (unsigned char)((word >> 24) & 0xff);
    }
    char addr[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, bytes, addr, sizeof(addr))) {
        snprintf(out, 48, "[::]");
        return;
    }
    snprintf(out, 48, "[%s]", addr);
}

static int scan_proc_net_file(const char *path,
                              const char *proto,
                              bool is_tcp,
                              bool is_v6,
                              const struct inode_set *set,
                              struct hold_port_list *out) {
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
        if (nf <= 9) continue;
        /* TCP: only LISTEN sockets (state 0A). UDP has no listen state, so an
         * unconnected bound socket is the honest analogue — a socket with a
         * remote address set is an outbound client (e.g. an ephemeral DNS
         * port) and never belongs in PORTS. */
        if (is_tcp && strcmp(fields[3], "0A") != 0) continue;
        if (!is_tcp && fields[2]) {
            const char *rem_port = strchr(fields[2], ':');
            if (rem_port && strtoul(rem_port + 1, NULL, 16) != 0) continue;
        }
        unsigned long long inode = strtoull(fields[9], NULL, 10);
        if (!inode_set_contains(set, inode)) continue;
        char addr_hex[64] = {0};
        char port_hex[16] = {0};
        if (sscanf(fields[1], "%63[^:]:%15s", addr_hex, port_hex) != 2) continue;
        unsigned long port = strtoul(port_hex, NULL, 16);
        if (port == 0) continue;
        char host[48];
        if (is_v6) {
            ipv6_from_proc_hex(addr_hex, host);
        } else {
            ipv4_from_proc_hex(addr_hex, host);
        }
        char entry[80];
        snprintf(entry, sizeof(entry), "%s:%lu/%s", host, port, proto);
        if (port_list_add(out, entry) != 0) break;
    }
    fclose(f);
    return 0;
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
    struct inode_set set = {0};
    struct socket_accum acc = { .set = &set, .denied = denied };
    if (for_each_group_pid(r, socket_inode_cb, &acc, denied) != 0) {
        inode_set_free(&set);
        return -1;
    }
    if (set.count > 0) {
        (void)scan_proc_net_file("/proc/net/tcp", "tcp", true, false, &set, out);
        (void)scan_proc_net_file("/proc/net/tcp6", "tcp", true, true, &set, out);
        (void)scan_proc_net_file("/proc/net/udp", "udp", false, false, &set, out);
        (void)scan_proc_net_file("/proc/net/udp6", "udp", false, true, &set, out);
    }
    inode_set_free(&set);
    return 0;
}
#else /* !__linux__ */
int hold_proc_read_ids(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
    (void)pid; (void)pgid_out; (void)sid_out; (void)state_out;
    errno = ENOSYS;
    return -1;
}
int hold_proc_read_cpu_rss(pid_t pid, uint64_t *cpu_ticks_out, uint64_t *rss_bytes_out) {
    (void)pid; (void)cpu_ticks_out; (void)rss_bytes_out;
    return -1;
}
int hold_proc_fd_target(pid_t pid, int fd, char *out, size_t n) {
    (void)pid; (void)fd; (void)out; (void)n;
    return -1;
}
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
