#pragma once
#ifndef HOLD_PLATFORM_H
#define HOLD_PLATFORM_H

#include "hold/config.h"
#include "hold/types.h"

/* The ONLY home for OS-specific knowledge about processes, boot identity,
 * paths, and accounts. Everything /proc-shaped lives behind this API; no
 * other layer opens /proc. On platforms without /proc the observation
 * primitives fail or report nothing — never fabricate. */

enum group_liveness { GROUP_SCAN_ERROR = -1, GROUP_EMPTY = 0, GROUP_ZOMBIE_ONLY = 1, GROUP_LIVE = 2 };

struct hold_passwd_entry {
    char name[128];
    char home[HOLD_PATH_MAX];
    char shell[HOLD_PATH_MAX];
};

bool hold_current_boot_id(char *buf, size_t n);
const char *hold_boot_id_or_null(char buf[128]);
enum group_liveness hold_group_session_liveness(pid_t pgid, pid_t sid);
int hold_count_session_escapees(pid_t sid, pid_t expected_pgid);
int hold_read_proc_stat_tokens(pid_t pid, char *state_out, uint64_t *starttime_out);
int hold_read_proc_exe(pid_t pid, uint64_t *dev, uint64_t *ino);
bool hold_leader_present(pid_t pid);
int hold_group_exists(pid_t pgid);
int hold_proc_read_ids(pid_t pid, pid_t *pgid_out, pid_t *sid_out, char *state_out);
int hold_proc_read_cpu_rss(pid_t pid, uint64_t *cpu_ticks_out, uint64_t *rss_bytes_out);
int hold_proc_fd_target(pid_t pid, int fd, char *out, size_t n);

/* cb(pid, ctx) for every live (non-zombie) process in group (pgid, sid).
 * Nonzero cb aborts with rc -1; an unreadable table reads as empty (rc 0).
 * *denied is set when a candidate's ids were unreadable. */
int hold_for_each_group_process(pid_t pgid, pid_t sid, int (*cb)(pid_t pid, void *ctx), void *ctx,
                                bool *denied);

/* cb(inode, ctx) for each socket inode pid holds open; nonzero cb aborts with
 * rc -1. *denied is set when the fd table is unreadable (another user's). */
int hold_proc_socket_inodes(pid_t pid, int (*cb)(unsigned long long inode, void *ctx), void *ctx,
                            bool *denied);

/* Scan the kernel's socket tables (tcp/tcp6 LISTEN, bound unconnected
 * udp/udp6) for inodes listed in inodes[], reporting each match to cb as
 * "host:port/proto"; nonzero cb stops the scan. */
int hold_scan_listening_sockets(const unsigned long long *inodes, size_t count,
                                int (*cb)(const char *entry, void *ctx), void *ctx);

int hold_resolve_binary_path(const char *argv0, char *out, size_t n);
int hold_resolve_existing_path_from_cwd(const char *token, const char *cwd, char *out, size_t n);
int hold_normalize_existing_argv_paths_from_cwd(char **argv, int argc, int first_arg, const char *cwd);
bool hold_path_is_within_dir(const char *path, const char *dir);
int hold_resolve_self_executable_path(const char *argv0, char *out, size_t n);
int hold_lookup_passwd_by_uid(uid_t uid, struct hold_passwd_entry *out);

#endif /* HOLD_PLATFORM_H */
