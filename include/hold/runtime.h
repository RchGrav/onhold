#pragma once
#ifndef HOLD_RUNTIME_H
#define HOLD_RUNTIME_H

#include "hold/config.h"
#include "hold/types.h"

bool hold_start_target_is_within_invoking_home(const struct hold_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv);
struct hold_start_options {
    bool tail;
    bool console_mode;
    bool auto_remove;
    bool interactive_stdin;
    int argc;
    char **argv;
    const char *exec_path;
    const char *run_name;
    int envc;
    char **env;
    const char *restart_policy;
    int restart_delay_seconds;
    const char *existing_id;
    const char *existing_log_path;
    const char *existing_run_name;
    int64_t existing_created_unix_ns;
    const char *existing_created_at;
};
int hold_perform_start_options(const struct hold_invocation *inv,
                                 const struct hold_store *store,
                                 const struct hold_start_options *opts);
/* list is Hold's scoped ledger: which stores it draws from depends on the
 * requested scope, resolved against the caller's privilege. */
enum hold_list_scope {
    HOLD_LIST_SCOPE_DEFAULT, /* non-root: the user's own store; root: the global store */
    HOLD_LIST_SCOPE_USER,    /* -u/--user: personal calls only, even under sudo */
    HOLD_LIST_SCOPE_SYSTEM,  /* -s/--system: the global/system store only */
    HOLD_LIST_SCOPE_BOTH     /* -a/--all: user and system scopes together */
};

int hold_cmd_list(const struct hold_invocation *inv,
                    const struct hold_store *user_store,
                    const struct hold_store *system_store,
                    const char *name_filter,
                    enum hold_list_scope scope,
                    bool live_only);
/* ps is Docker's machine-wide view: running calls (plus ended with -a) across
 * both the caller's own store and the global store, no USER column. */
int hold_cmd_ps(const struct hold_invocation *inv,
                  const struct hold_store *user_store,
                  const struct hold_store *system_store,
                  const char *name_filter,
                  bool all);
enum id_token_scope hold_parse_id_token(const char *token, const char **id_out);
int hold_cmd_signal_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd);
int hold_cmd_tail_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *id_token);
int hold_cmd_inspect_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token);
int hold_cmd_view_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           int argc,
                           char **argv);
int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *id_token);
int hold_cmd_ports_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *id_token);
int hold_cmd_stats_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *id_token,
                            bool no_stream);
int hold_cmd_purge_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *target_token,
                            bool all,
                            bool force,
                            bool system_scope);
int hold_cmd_shell_action(const struct hold_invocation *inv,
                            const struct hold_store *store);
int hold_cmd_off_action(void);
/* Call-record operations (src/runtime/call.c): redial a retained recipe, save or
 * rename a call, and assign a generated adjective_noun name. */
int hold_cmd_rename_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *target_token,
                             const char *new_name);
int hold_cmd_redial(const struct hold_invocation *inv,
                      const struct hold_store *user_store,
                      const struct hold_store *system_store,
                      bool tail,
                      bool console_mode,
                      bool auto_remove,
                      bool interactive_stdin,
                      bool explicit_session_mode,
                      const char *restart_policy,
                      int restart_delay_seconds,
                      const char *token,
                      bool *redialed);
int hold_cmd_save_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *target_token);
int hold_generate_run_name_for_id(const struct hold_store *store,
                                    const char *id,
                                    const char *requested,
                                    char out[ALIAS_MAX_LEN + 1]);
int hold_ensure_start_store_for_command(const struct hold_invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct hold_store *store);
void hold_usage(void);

#endif /* HOLD_RUNTIME_H */
