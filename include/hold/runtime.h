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
    int portc;
    char **ports;
    int volumec;
    char **volumes;
    int cap_addc;
    char **cap_add;
    int cap_dropc;
    char **cap_drop;
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
int hold_cmd_list_normal(const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso);
int hold_cmd_list_system(const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso);
int hold_cmd_ps_normal(const struct hold_store *user_store,
                         const struct hold_store *system_store,
                         bool all);
int hold_cmd_ps_system(const struct hold_store *system_store, bool all);
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
int hold_cmd_purge_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *target_token,
                            bool all,
                            bool force);
int hold_cmd_shell_action(const struct hold_invocation *inv,
                            const struct hold_store *store);
int hold_cmd_off_action(void);
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
                      const char *restart_policy,
                      int restart_delay_seconds,
                      const char *token,
                      bool *redialed);
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
