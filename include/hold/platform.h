#pragma once
#ifndef HOLD_PLATFORM_H
#define HOLD_PLATFORM_H

#include "hold/config.h"
#include "hold/types.h"

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
int hold_resolve_binary_path(const char *argv0, char *out, size_t n);
int hold_resolve_existing_path_from_cwd(const char *token, const char *cwd, char *out, size_t n);
int hold_normalize_existing_argv_paths_from_cwd(char **argv, int argc, int first_arg, const char *cwd);
bool hold_path_is_within_dir(const char *path, const char *dir);
int hold_resolve_self_executable_path(const char *argv0, char *out, size_t n);
int hold_lookup_passwd_by_uid(uid_t uid, struct hold_passwd_entry *out);

#endif /* HOLD_PLATFORM_H */
