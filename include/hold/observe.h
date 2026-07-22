#pragma once
#ifndef HOLD_OBSERVE_H
#define HOLD_OBSERVE_H

#include "hold/config.h"
#include "hold/types.h"

/* Live observation of a call's process group, orchestrated over platform's
 * /proc-shaped primitives (never signals or ptrace); a no-op that reports
 * "nothing observed" on platforms without /proc. This is the one
 * implementation behind the PORTS column, `hold ports`, and `hold stats`. */

/* A growable list of formatted listening endpoints, e.g. "127.0.0.1:8080/tcp". */
struct hold_port_list {
    char **items;
    size_t count;
};
void hold_port_list_free(struct hold_port_list *list);

/* Collect the non-zombie pids that belong to the call's process group
 * (matching both pgid and sid). Caller frees *pids_out with free(). *denied is
 * set true when /proc exists but the pids' details are unreadable. */
int hold_observe_run_pids(const struct hold_run_record *r,
                          pid_t **pids_out,
                          size_t *count_out,
                          bool *denied);

/* Collect the listening sockets held by the call's process group: tcp/tcp6 in
 * the LISTEN state and bound udp/udp6. Caller frees the list. *denied is set
 * true when the pids' fd tables could not be read (another user's call). */
int hold_observe_run_ports(const struct hold_run_record *r,
                           struct hold_port_list *out,
                           bool *denied);

/* The PORTS-column projection of hold_observe_run_ports: the same endpoints
 * joined with ", " (empty string when none, or when denied). */
void hold_observe_run_ports_column(const struct hold_run_record *r, char *out, size_t n);

#endif /* HOLD_OBSERVE_H */
