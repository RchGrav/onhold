#include "hold/config.h"
#include "hold/types.h"
#include "hold/runtime.h"
#include "hold/runtime_internal.h"
#include "hold/core.h"
#include "hold/store.h"
#include "hold/platform.h"
#include "hold/observe.h"

/* hold stats <target>: a docker-stats-like live view of the call's process
 * group - CPU %, resident memory, and process count - refreshing in place once
 * a second until Ctrl-C. Falls back to a single plain frame when stdout is not
 * a TTY or --no-stream is given, so it stays script-safe. Aggregation is over
 * the process group via /proc; no cgroups. */

static volatile sig_atomic_t stats_stop = 0;

static void stats_on_sigint(int sig) {
    (void)sig;
    stats_stop = 1;
}

struct group_sample {
    uint64_t cpu_ticks; /* summed utime+stime across the group */
    uint64_t rss_bytes; /* summed resident set size */
    size_t pids;        /* live process count */
};

static void sample_group(const struct hold_run_record *r, struct group_sample *s) {
    memset(s, 0, sizeof(*s));
    pid_t *pids = NULL;
    size_t count = 0;
    if (hold_observe_run_pids(r, &pids, &count, NULL) != 0) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        uint64_t cpu = 0, rss = 0;
        if (hold_proc_read_cpu_rss(pids[i], &cpu, &rss) == 0) {
            s->cpu_ticks += cpu;
            s->rss_bytes += rss;
            s->pids++;
        }
    }
    free(pids);
}

static void format_bytes(uint64_t bytes, char *out, size_t n) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t u = 0;
    while (value >= 1024.0 && u < (sizeof(units) / sizeof(units[0])) - 1) {
        value /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(out, n, "%.0f%s", value, units[u]);
    } else {
        snprintf(out, n, "%.1f%s", value, units[u]);
    }
}

/* CPU% as a fraction of one core * 100, summed across the group (so a busy
 * multi-threaded call can exceed 100%), measured over an interval in seconds. */
static void format_cpu(uint64_t delta_ticks, double interval_s, char *out, size_t n) {
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0 || interval_s <= 0.0) {
        snprintf(out, n, "--");
        return;
    }
    double pct = 100.0 * (double)delta_ticks / ((double)hz * interval_s);
    snprintf(out, n, "%.2f%%", pct);
}

static void print_stats_frame(const char *id, const char *name,
                              const char *cpu, const char *mem, size_t pids,
                              bool clear) {
    if (clear) {
        /* Home the cursor and clear to end of screen: redraw in place without
         * the flicker of a full erase-scrollback. */
        fputs("\033[H\033[J", stdout);
    }
    printf("%-14s  %-20s  %-8s  %-12s  %s\n", "CALL ID", "NAME", "CPU %", "MEM (RSS)", "PIDS");
    printf("%-14s  %-20s  %-8s  %-12s  %zu\n", id, name, cpu, mem, pids);
    fflush(stdout);
}

int hold_cmd_stats_action(const struct hold_invocation *inv,
                          const struct hold_store *user_store,
                          const struct hold_store *system_store,
                          const char *id_token,
                          bool no_stream) {
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

    char display_id[ID_DISPLAY_HEX_LEN + 1];
    hold_run_id_display(r.id, display_id);
    char name[ALIAS_MAX_LEN + 1];
    snprintf(name, sizeof(name), "%s", r.has_name && r.name[0] ? r.name : "-");

    if (st != STATE_RUNNING) {
        hold_sig_note(inv, "hold: %s is not running\n", display_id);
        print_stats_frame(display_id, name, "--", "0B", 0, false);
        hold_free_run_record(&r);
        free(targets);
        return 0;
    }

    bool stream = isatty(STDOUT_FILENO) && !no_stream;

    if (!stream) {
        /* One frame: sample twice across a short window so CPU% is a real
         * measurement, not a cumulative counter. */
        struct group_sample a, b;
        sample_group(&r, &a);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 250 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        sample_group(&r, &b);
        char cpu[16], mem[24];
        format_cpu(b.cpu_ticks >= a.cpu_ticks ? b.cpu_ticks - a.cpu_ticks : 0, 0.25, cpu, sizeof(cpu));
        format_bytes(b.rss_bytes, mem, sizeof(mem));
        print_stats_frame(display_id, name, cpu, mem, b.pids, false);
        hold_free_run_record(&r);
        free(targets);
        return 0;
    }

    struct sigaction sa, old_int, old_term;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stats_on_sigint;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    struct group_sample prev;
    sample_group(&r, &prev);
    while (!stats_stop) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        if (nanosleep(&ts, NULL) != 0 && errno == EINTR) {
            break;
        }
        struct group_sample cur;
        sample_group(&r, &cur);
        char cpu[16], mem[24];
        format_cpu(cur.cpu_ticks >= prev.cpu_ticks ? cur.cpu_ticks - prev.cpu_ticks : 0, 1.0, cpu, sizeof(cpu));
        format_bytes(cur.rss_bytes, mem, sizeof(mem));
        print_stats_frame(display_id, name, cpu, mem, cur.pids, true);
        prev = cur;
        if (cur.pids == 0) {
            /* The call ended while we watched; stop cleanly rather than spin on
             * an empty group. */
            break;
        }
    }

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    hold_free_run_record(&r);
    free(targets);
    return 0;
}
