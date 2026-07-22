#include "hold/config.h"
#include "hold/types.h"
#include "hold/platform.h"
#include "hold/core.h"
#include <arpa/inet.h>

#if defined(__APPLE__)

static int mac_kinfo_pid(pid_t pid, struct kinfo_proc *kp) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t len = sizeof(*kp);
    memset(kp, 0, sizeof(*kp));
    return (sysctl(mib, 4, kp, &len, NULL, 0) != 0 || len == 0) ? -1 : 0;
}

/* No /proc on macOS: the observation primitives fail or report nothing —
 * never fabricate. */
int hold_proc_read_ids(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
    (void)pid; (void)pgid_out; (void)sid_out; (void)state_out;
    errno = ENOSYS;
    return -1;
}
int hold_proc_read_cpu_rss(pid_t pid, uint64_t *cpu, uint64_t *rss) {
    (void)pid; (void)cpu; (void)rss;
    return -1;
}
int hold_proc_fd_target(pid_t pid, int fd, char *out, size_t n) {
    (void)pid; (void)fd; (void)out; (void)n;
    return -1;
}
int hold_proc_socket_inodes(pid_t pid, int (*cb)(unsigned long long, void *), void *ctx, bool *denied) {
    (void)pid; (void)cb; (void)ctx; (void)denied;
    return 0;
}
int hold_scan_listening_sockets(const unsigned long long *inodes, size_t count,
                                int (*cb)(const char *, void *), void *ctx) {
    (void)inodes; (void)count; (void)cb; (void)ctx;
    return 0;
}
int hold_proc_read_comm(pid_t pid, char *out, size_t n) {
    (void)pid; (void)out; (void)n;
    errno = ENOSYS;
    return -1;
}
int hold_proc_read_cmdline(pid_t pid, char ***argv_out, int *argc_out) {
    (void)pid; (void)argv_out; (void)argc_out;
    errno = ENOSYS;
    return -1;
}
int hold_proc_entry_readlink(pid_t pid, const char *entry, char *out, size_t n) {
    (void)pid; (void)entry; (void)out; (void)n;
    errno = ENOSYS;
    return -1;
}

#else /* /proc-shaped platforms */

/* Read a small pseudo-file whole; empty reads as EIO (/proc never yields an
 * empty stat for a live process). */
static ssize_t read_small_file(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r;
    do r = read(fd, buf, n - 1); while (r < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (r <= 0) { errno = r < 0 ? saved : EIO; return -1; }
    buf[r] = '\0';
    return r;
}

/* THE /proc/<pid>/stat extractor: load the file and return the fields after
 * "(comm) ", where token 0 is the run state, 2 pgid, 3 sid, 11 utime,
 * 12 stime, 19 starttime. */
static int proc_stat_fields(pid_t pid, char *buf, size_t n, char **fields) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (read_small_file(path, buf, n) < 0) return -1;
    char *rp = strrchr(buf, ')');
    if (!rp || rp[1] != ' ' || !rp[2]) { errno = EINVAL; return -1; }
    *fields = rp + 2;
    return 0;
}

static const char *nth_field(const char *fields, int idx) {
    for (; idx > 0 && fields; idx--) {
        fields = strchr(fields, ' ');
        if (fields) fields++;
    }
    return fields && *fields ? fields : NULL;
}

int hold_proc_read_ids(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out) {
    char buf[4096], *f;
    if (proc_stat_fields(pid, buf, sizeof(buf), &f) != 0) return -1;
    const char *pg = nth_field(f, 2), *sd = nth_field(f, 3);
    if (!pg || !sd) { errno = EINVAL; return -1; }
    if (pgid_out) *pgid_out = (pid_t)strtol(pg, NULL, 10);
    if (sid_out) *sid_out = (pid_t)strtol(sd, NULL, 10);
    if (state_out) *state_out = f[0];
    return 0;
}

int hold_proc_read_cpu_rss(pid_t pid, uint64_t *cpu_ticks_out, uint64_t *rss_bytes_out) {
    char buf[4096];
    if (cpu_ticks_out) {
        char *f;
        if (proc_stat_fields(pid, buf, sizeof(buf), &f) != 0) return -1;
        const char *ut = nth_field(f, 11), *st = nth_field(f, 12);
        if (!ut || !st) return -1;
        *cpu_ticks_out = strtoull(ut, NULL, 10) + strtoull(st, NULL, 10);
    }
    if (rss_bytes_out) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/statm", (long)pid);
        if (read_small_file(path, buf, sizeof(buf)) < 0) return -1;
        /* statm: "size resident shared ..." in pages; resident is field 2. */
        unsigned long long size_pages = 0, resident_pages = 0;
        if (sscanf(buf, "%llu %llu", &size_pages, &resident_pages) != 2) return -1;
        long page = sysconf(_SC_PAGESIZE);
        *rss_bytes_out = (uint64_t)resident_pages * (uint64_t)(page > 0 ? page : 4096);
    }
    return 0;
}

int hold_proc_fd_target(pid_t pid, int fd, char *out, size_t n) {
    char path[64], target[HOLD_PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%ld/fd/%d", (long)pid, fd);
    ssize_t got = readlink(path, target, sizeof(target) - 1);
    if (got < 0) return -1;
    target[got] = '\0';
    /* A held call's captured stdout/stderr point at Hold's own capture pipe;
     * report the plumbing by its role, not its ephemeral inode. */
    snprintf(out, n, "%s", strncmp(target, "pipe:", 5) == 0 ? "pipe:hold" : target);
    return 0;
}

int hold_proc_socket_inodes(pid_t pid, int (*cb)(unsigned long long, void *), void *ctx, bool *denied) {
    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%ld/fd", (long)pid);
    DIR *d = opendir(fd_dir);
    if (!d) {
        if (errno == EACCES && denied) *denied = true;
        return 0;
    }
    const struct dirent *e;
    while ((e = readdir(d))) {
        char fd_path[128], target[256];
        unsigned long long inode = 0;
        if (!isdigit((unsigned char)e->d_name[0]) ||
            hold_checked_snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir, e->d_name) != 0) continue;
        ssize_t nr = readlink(fd_path, target, sizeof(target) - 1);
        if (nr <= 0) continue;
        target[nr] = '\0';
        if (sscanf(target, "socket:[%llu]", &inode) == 1 && cb(inode, ctx) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return 0;
}

int hold_proc_read_comm(pid_t pid, char *out, size_t n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    if (read_small_file(path, out, n) < 0) return -1;
    size_t len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return 0;
}

int hold_proc_entry_readlink(pid_t pid, const char *entry, char *out, size_t n) {
    char path[128];
    if (hold_checked_snprintf(path, sizeof(path), "/proc/%ld/%s", (long)pid, entry) != 0) return -1;
    ssize_t nr = readlink(path, out, n - 1);
    if (nr < 0 || (size_t)nr >= n) return -1;
    out[nr] = '\0';
    return 0;
}

/* /proc/<pid>/cmdline: NUL-separated argv; a missing trailing NUL still
 * yields the last argument. An empty read is EIO (kernel threads). */
int hold_proc_read_cmdline(pid_t pid, char ***argv_out, int *argc_out) {
    char path[64];
    static char buf[65536];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    ssize_t nr = read_small_file(path, buf, sizeof(buf));
    if (nr <= 0) return -1; /* read_small_file maps an empty read to -1; nr>=1 here */
    int argc = 0;
    for (ssize_t i = 0; i < nr; i++)
        if (buf[i] == '\0') argc++;
    if (buf[nr - 1] != '\0') argc++;
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) return -1;
    ssize_t pos = 0;
    for (int i = 0; i < argc; i++) {
        ssize_t start = pos;
        while (pos < nr && buf[pos] != '\0') pos++;
        argv[i] = strndup(buf + start, (size_t)(pos - start));
        if (!argv[i]) {
            hold_free_argv_alloc(argv, i);
            return -1;
        }
        pos++;
    }
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static void ipv4_from_proc_hex(const char *hex, char out[48]) {
    unsigned long v = strtoul(hex, NULL, 16);
    snprintf(out, 48, "%lu.%lu.%lu.%lu", v & 0xffUL, (v >> 8) & 0xffUL, (v >> 16) & 0xffUL,
             (v >> 24) & 0xffUL);
}

/* /proc renders an IPv6 address as four 32-bit words, each printed in host
 * byte order. Reassemble the 16 raw bytes and let inet_ntop compress them. */
static void ipv6_from_proc_hex(const char *hex, char out[48]) {
    unsigned char bytes[16] = {0};
    char chunk[9], addr[INET6_ADDRSTRLEN];
    if (strlen(hex) < 32) { snprintf(out, 48, "[::]"); return; }
    for (int w = 0; w < 4; w++) {
        memcpy(chunk, hex + w * 8, 8);
        chunk[8] = '\0';
        unsigned long word = strtoul(chunk, NULL, 16);
        for (int b = 0; b < 4; b++) bytes[w * 4 + b] = (unsigned char)((word >> (8 * b)) & 0xff);
    }
    snprintf(out, 48, "[%s]", inet_ntop(AF_INET6, bytes, addr, sizeof(addr)) ? addr : "::");
}

static bool inode_listed(const unsigned long long *inodes, size_t count, unsigned long long inode) {
    for (size_t i = 0; i < count; i++)
        if (inodes[i] == inode) return true;
    return false;
}

static int scan_proc_net_file(const char *path, const char *proto, bool is_tcp, bool is_v6,
                              const unsigned long long *inodes, size_t count,
                              int (*cb)(const char *, void *), void *ctx) {
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; } /* header line */
    while (fgets(line, sizeof(line), f)) {
        char *fields[16] = {0}, *save = NULL;
        int nf = 0;
        for (char *tok = strtok_r(line, " \t\r\n", &save); tok && nf < 16;
             tok = strtok_r(NULL, " \t\r\n", &save))
            fields[nf++] = tok;
        if (nf <= 9) continue;
        /* TCP: only LISTEN sockets (state 0A). UDP has no listen state, so an
         * unconnected bound socket is the honest analogue — a socket with a
         * remote address set is an outbound client (e.g. an ephemeral DNS
         * port) and never belongs in PORTS. */
        if (is_tcp && strcmp(fields[3], "0A") != 0) continue;
        if (!is_tcp) {
            const char *rem_port = strchr(fields[2], ':');
            if (rem_port && strtoul(rem_port + 1, NULL, 16) != 0) continue;
        }
        if (!inode_listed(inodes, count, strtoull(fields[9], NULL, 10))) continue;
        char addr_hex[64] = {0}, port_hex[16] = {0}, host[48], entry[80];
        if (sscanf(fields[1], "%63[^:]:%15s", addr_hex, port_hex) != 2) continue;
        unsigned long port = strtoul(port_hex, NULL, 16);
        if (port == 0) continue;
        (is_v6 ? ipv6_from_proc_hex : ipv4_from_proc_hex)(addr_hex, host);
        snprintf(entry, sizeof(entry), "%s:%lu/%s", host, port, proto);
        if (cb(entry, ctx) != 0) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

int hold_scan_listening_sockets(const unsigned long long *inodes, size_t count,
                                int (*cb)(const char *, void *), void *ctx) {
    static const struct { const char *path, *proto; bool tcp, v6; } tables[4] = {
        {"/proc/net/tcp", "tcp", true, false},   {"/proc/net/tcp6", "tcp", true, true},
        {"/proc/net/udp", "udp", false, false},  {"/proc/net/udp6", "udp", false, true},
    };
    for (size_t i = 0; i < 4; i++)
        if (scan_proc_net_file(tables[i].path, tables[i].proto, tables[i].tcp, tables[i].v6,
                               inodes, count, cb, ctx) != 0) break;
    return 0;
}

#endif

int hold_read_proc_stat_tokens(pid_t pid, char *state_out, uint64_t *starttime_out) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) != 0) return -1;
    if (starttime_out) {
        struct timeval tv = kp.kp_proc.p_starttime;
        if (tv.tv_sec == 0 && tv.tv_usec == 0) return -1;
        *starttime_out = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
    }
    if (state_out) *state_out = kp.kp_proc.p_stat == SZOMB ? 'Z' : '?';
#else
    char buf[4096], *f;
    if (proc_stat_fields(pid, buf, sizeof(buf), &f) != 0) return -1;
    if (starttime_out) {
        const char *tok = nth_field(f, 19);
        char *end = NULL;
        errno = 0;
        unsigned long long parsed = tok ? strtoull(tok, &end, 10) : 0;
        if (!tok || end == tok || errno != 0) return -1;
        *starttime_out = parsed;
    }
    if (state_out) *state_out = f[0];
#endif
    return 0;
}

int hold_read_proc_exe(pid_t pid, uint64_t *dev, uint64_t *ino) {
    struct stat st;
#if defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return -1;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
#endif
    if (stat(path, &st) != 0) return -1;
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return 0;
}

/* THE process-table walk. cb sees every visible process (zombie flagged) and
 * a nonzero return stops the walk (and is returned). rc -1: the table itself
 * was unreadable. *denied is set when a process's ids were unreadable. */
static int for_each_process(int (*cb)(pid_t pid, pid_t pgid, pid_t sid, bool zombie, void *ctx),
                            void *ctx, bool *denied) {
    int rc = 0;
#if defined(__APPLE__)
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    (void)denied;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) return -1;
    struct kinfo_proc *procs = malloc(len ? len : sizeof(*procs));
    if (!procs) return -1;
    if (sysctl(mib, 3, procs, &len, NULL, 0) != 0) { free(procs); return -1; }
    size_t nprocs = len / sizeof(procs[0]);
    for (size_t i = 0; rc == 0 && i < nprocs; i++) {
        pid_t pid = procs[i].kp_proc.p_pid;
        if (pid <= 0) continue;
        rc = cb(pid, procs[i].kp_eproc.e_pgid, getsid(pid), procs[i].kp_proc.p_stat == SZOMB, ctx);
    }
    free(procs);
#else
    DIR *d = opendir("/proc");
    if (!d) return -1;
    const struct dirent *e;
    while (rc == 0 && (e = readdir(d))) {
        char *end = NULL;
        long pid = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0' || pid <= 0) continue;
        pid_t pgid = 0, sid = 0;
        char state = 0;
        if (hold_proc_read_ids((pid_t)pid, &pgid, &sid, &state) != 0) {
            if (errno == EACCES && denied) *denied = true;
            continue;
        }
        rc = cb((pid_t)pid, pgid, sid, state == 'Z', ctx);
    }
    closedir(d);
#endif
    return rc;
}

struct group_scan { pid_t pgid, sid; bool any, live; int count; };

static int liveness_cb(pid_t pid, pid_t pgid, pid_t sid, bool zombie, void *ctx) {
    struct group_scan *g = ctx;
    (void)pid;
    if (pgid != g->pgid || sid != g->sid) return 0;
    g->any = true;
    if (!zombie) g->live = true;
    return g->live ? 1 : 0;
}

enum group_liveness hold_group_session_liveness(pid_t pgid, pid_t sid) {
    if (pgid <= 1 || sid <= 0) return GROUP_SCAN_ERROR;
    struct group_scan g = {pgid, sid, false, false, 0};
    if (for_each_process(liveness_cb, &g, NULL) < 0) return GROUP_SCAN_ERROR;
    if (g.live) return GROUP_LIVE;
    return g.any ? GROUP_ZOMBIE_ONLY : GROUP_EMPTY;
}

static int escapee_cb(pid_t pid, pid_t pgid, pid_t sid, bool zombie, void *ctx) {
    struct group_scan *g = ctx;
    (void)pid; (void)zombie;
    if (sid == g->sid && pgid != g->pgid) g->count++;
    return 0;
}

int hold_count_session_escapees(pid_t sid, pid_t expected_pgid) {
    struct group_scan g = {expected_pgid, sid, false, false, 0};
    if (for_each_process(escapee_cb, &g, NULL) < 0) return -1;
    return g.count;
}

struct pgid_find { pid_t pgid, best, best_sid; };

static int pgid_find_cb(pid_t pid, pid_t pgid, pid_t sid, bool zombie, void *ctx) {
    struct pgid_find *f = ctx;
    if (pgid != f->pgid || zombie) return 0;
    if (pid == f->pgid) {
        f->best = pid;
        f->best_sid = sid;
        return 1; /* the live leader wins outright */
    }
    if (f->best == 0 || pid < f->best) {
        f->best = pid;
        f->best_sid = sid;
    }
    return 0;
}

int hold_find_process_in_pgid(pid_t pgid, pid_t *pid_out, pid_t *sid_out) {
    struct pgid_find f = {pgid, 0, 0};
    if (for_each_process(pgid_find_cb, &f, NULL) < 0) return -1;
    if (f.best <= 0) {
        errno = ESRCH;
        return -1;
    }
    *pid_out = f.best;
    *sid_out = f.best_sid;
    return 0;
}

struct group_walk { pid_t pgid, sid; int (*cb)(pid_t, void *); void *ctx; };

static int group_walk_cb(pid_t pid, pid_t pgid, pid_t sid, bool zombie, void *ctx) {
    struct group_walk *g = ctx;
    if (pgid != g->pgid || sid != g->sid || zombie) return 0;
    return g->cb(pid, g->ctx) != 0;
}

int hold_for_each_group_process(pid_t pgid, pid_t sid, int (*cb)(pid_t, void *), void *ctx, bool *denied) {
    struct group_walk g = {pgid, sid, cb, ctx};
    return for_each_process(group_walk_cb, &g, denied) > 0 ? -1 : 0;
}

bool hold_leader_present(pid_t pid) {
#if defined(__APPLE__)
    struct kinfo_proc kp;
    if (mac_kinfo_pid(pid, &kp) == 0) return kp.kp_proc.p_stat != SZOMB;
#else
    char state = 0;
    if (hold_read_proc_stat_tokens(pid, &state, NULL) == 0) return state != 'Z';
#endif
    return kill(pid, 0) == 0 || errno == EPERM;
}

int hold_group_exists(pid_t pgid) {
    if (kill(-pgid, 0) == 0 || errno == EPERM) return 1;
    return errno == ESRCH ? 0 : -1;
}
