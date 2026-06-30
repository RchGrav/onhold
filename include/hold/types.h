#pragma once
#ifndef HOLD_TYPES_H
#define HOLD_TYPES_H

/* Shared domain types crossing module boundaries. Definitions only, no code.
 * config.h supplies the fixed-width integer / POSIX types and the size macros
 * (HOLD_PATH_MAX, PROFILE_HASH_STR_LEN, ALIAS_MAX_LEN, ...) used below. */
#include "hold/config.h"

struct hold_run_record {
    int version;
    char id[ID_STR_LEN];
    char run_id[ID_STR_LEN];
    char alias[ALIAS_MAX_LEN + 1];
    char name[ALIAS_MAX_LEN + 1];
    char console_sock[HOLD_PATH_MAX];
    pid_t pid;
    pid_t pgid;
    pid_t sid;
    int64_t start_unix_ns;
    int64_t created_unix_ns;
    uid_t uid;
    gid_t gid;
    char log_path[HOLD_PATH_MAX];
    char boot_id[128];
    uint64_t proc_starttime_ticks;
    uint64_t exe_dev;
    uint64_t exe_ino;
    char cmdline[HOLD_PATH_MAX];
    char started_at[64];
    char created_at[64];
    char ended_at[64];
    char state[16];
    int exit_code;
    int term_signal;
    char launch_error[64];
    uid_t invoked_by_uid;
    gid_t invoked_by_gid;
    char invoked_by_user[128];
    bool invoked_via_sudo;
    bool has_log;
    bool has_boot;
    bool has_started_at;
    bool has_ended_at;
    bool has_state;
    bool has_exit_code;
    bool has_term_signal;
    bool has_launch_error;
    bool has_invocation;
    bool has_alias;
    bool has_name;
    bool has_created_at;
    bool has_console;
    bool has_stdio_config;
    bool attach_stdin;
    bool attach_stdout;
    bool attach_stderr;
    bool tty;
    bool open_stdin;
    bool stdin_once;
    int argc;
    char **argv;
    int envc;
    char **env;
    int portc;
    char **ports;
    int volumec;
    char **volumes;
    int cap_addc;
    char **cap_add;
    int cap_dropc;
    char **cap_drop;
    bool mode_interactive;
    bool mode_tty;
    bool mode_detach;
    bool allow_multi;
    bool has_observed;
    char observed_exe[HOLD_PATH_MAX];
    char observed_cwd[HOLD_PATH_MAX];
    int observed_argc;
    char **observed_argv;
    char restart_policy[64];
    int restart_delay_seconds;
    bool has_restart_policy;
    bool has_restart_delay;
    char log_destination[32];
    bool has_log_destination;
};

enum run_state { STATE_RUNNING, STATE_EXITED, STATE_STALE, STATE_FAILED, STATE_UNKNOWN };

enum store_kind { STORE_USER_LOCAL, STORE_SYSTEM_MANAGED };

struct hold_store {
    enum store_kind kind;
    char base[HOLD_PATH_MAX];
    char record_dir[HOLD_PATH_MAX];
    char log_dir[HOLD_PATH_MAX];
    char public_dir[HOLD_PATH_MAX];
    char console_dir[HOLD_PATH_MAX];
    char profile_path[HOLD_PATH_MAX];
    char alias_path[HOLD_PATH_MAX];
};

struct hold_invocation {
    bool euid_root;
    bool requested_system;
    bool elevated;
    bool quiet;
    bool have_sudo_user;
    uid_t invoking_uid;
    gid_t invoking_gid;
    char invoking_user[128];
    char invoking_home[HOLD_PATH_MAX];
};

enum resolve_scope { RESOLVE_USER_LOCAL, RESOLVE_SYSTEM_MANAGED, RESOLVE_NOT_FOUND, RESOLVE_ERROR };

struct hold_resolved_target {
    enum resolve_scope scope;
    char id[ALIAS_MAX_LEN + 1 + PROFILE_HASH_STR_LEN];
    char cap_alias[ALIAS_MAX_LEN + 1];
    char cap_hash[PROFILE_HASH_STR_LEN];
    struct hold_store store;
    bool needs_elevation;
    bool has_capability;
};

struct hold_public_index {
    char id[ID_STR_LEN];
    char alias[ALIAS_MAX_LEN + 1];
    char name[ALIAS_MAX_LEN + 1];
    bool root_managed;
    bool requires_elevation;
    bool has_alias;
    bool has_name;
    char state_hint[16];
    char started_at[64];
    char created_at[64];
    char finished_at[64];
    pid_t pid;
    pid_t pgid;
    pid_t sid;
    int exit_code;
    char error[64];
    bool running;
    bool paused;
    bool restarting;
    bool dead;
    bool has_state;
    bool has_exit_code;
};

struct hold_profile {
    char hash[PROFILE_HASH_STR_LEN];
    char binary_path[HOLD_PATH_MAX];
    int argc;
    char **argv;
    int envc;
    char **env;
    int portc;
    char **ports;
    int volumec;
    char **volumes;
    int cap_addc;
    char **cap_add;
    int cap_dropc;
    char **cap_drop;
    bool mode_interactive;
    bool mode_tty;
    bool mode_detach;
    bool allow_multi;
    char restart_policy[64];
    int restart_delay_seconds;
    bool has_restart_policy;
    bool has_restart_delay;
    char log_destination[32];
    bool has_log_destination;
};

struct hold_alias {
    char name[ALIAS_MAX_LEN + 1];
    char hash[PROFILE_HASH_STR_LEN];
    char binary_path[HOLD_PATH_MAX];
    int argc;
    char **argv;
    int envc;
    char **env;
    int portc;
    char **ports;
    int volumec;
    char **volumes;
    int cap_addc;
    char **cap_add;
    int cap_dropc;
    char **cap_drop;
    bool mode_interactive;
    bool mode_tty;
    bool mode_detach;
    bool allow_multi;
    char restart_policy[64];
    int restart_delay_seconds;
    bool has_restart_policy;
    bool has_restart_delay;
    char log_destination[32];
    bool has_log_destination;
    bool has_hash;
    bool has_recipe;
};

enum id_token_scope { ID_TOKEN_PLAIN, ID_TOKEN_USER, ID_TOKEN_SYSTEM, ID_TOKEN_INVALID };

#endif /* HOLD_TYPES_H */
