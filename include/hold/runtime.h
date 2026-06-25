#pragma once
#ifndef HOLD_RUNTIME_H
#define HOLD_RUNTIME_H

#include "hold/config.h"
#include "hold/types.h"

bool hold_command_accepts_target_tokens(const char *command);
bool hold_start_target_is_within_invoking_home(const struct hold_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv);
int hold_perform_start(const struct hold_invocation *inv,
                         const struct hold_store *store,
                         bool tail,
                         bool console_mode,
                         int argc,
                         char **argv,
                         const char *exec_path,
                         const char *run_alias);
int hold_perform_start_with_env(const struct hold_invocation *inv,
                                  const struct hold_store *store,
                                  bool tail,
                                  bool console_mode,
                                  int argc,
                                  char **argv,
                                  const char *exec_path,
                                  const char *run_alias,
                                  int envc,
                                  char **env);
int hold_perform_start_with_env_options(const struct hold_invocation *inv,
                                          const struct hold_store *store,
                                          bool tail,
                                          bool console_mode,
                                          bool auto_remove,
                                          int argc,
                                          char **argv,
                                          const char *exec_path,
                                          const char *run_alias,
                                          int envc,
                                          char **env);
int hold_cmd_list_normal(const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso);
int hold_cmd_list_system(const struct hold_store *system_store,
                           const char *alias_filter,
                           bool iso);
enum id_token_scope hold_parse_id_token(const char *token, const char **id_out);
int hold_resolve_public_profile_token(const struct hold_store *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]);
int hold_cmd_signal_action(const struct hold_invocation *inv,
                             const struct hold_store *user_store,
                             const struct hold_store *system_store,
                             const char *program,
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
                           const char *program,
                           const char *id_token);
int hold_cmd_dump_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *program,
                           const char *id_token);
int hold_cmd_inspect_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *program,
                              const char *id_token);
int hold_cmd_view_action(const struct hold_invocation *inv,
                           const struct hold_store *user_store,
                           const struct hold_store *system_store,
                           const char *program,
                           int argc,
                           char **argv);
int hold_cmd_console_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *program,
                              const char *id_token);
int hold_cmd_prune_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *program,
                            const char *target_token,
                            bool all);
int hold_cmd_shell_action(const struct hold_invocation *inv,
                            const struct hold_store *store);
int hold_cmd_captive_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              const char *program);
int hold_elevate_start_token(const char *program,
                               bool tail,
                               bool console_mode,
                               const char *token_atom,
                               const char *hash,
                               bool multi,
                               int multi_count);
int hold_cmd_start_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *program,
                            const struct hold_store *fallback_store,
                            bool tail,
                            bool console_mode,
                            bool multi,
                            int multi_count,
                            int argc,
                            char **argv);
int hold_cmd_start_action_options(const struct hold_invocation *inv,
                                    const struct hold_store *user_store,
                                    const struct hold_store *system_store,
                                    const char *program,
                                    const struct hold_store *fallback_store,
                                    bool tail,
                                    bool console_mode,
                                    bool auto_remove,
                                    bool multi,
                                    int multi_count,
                                    int argc,
                                    char **argv);
int hold_ensure_start_store_for_command(const struct hold_invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct hold_store *store);
int hold_maybe_elevate_requested_system_targets(const char *program,
                                                  const char *command,
                                                  int argc,
                                                  char **argv,
                                                  bool all,
                                                  int *rc_out);
int hold_cmd_alias_action(const struct hold_invocation *inv,
                            const struct hold_store *user_store,
                            const struct hold_store *system_store,
                            const char *program,
                            int argc,
                            char **argv);
int hold_cmd_aliases_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              const struct hold_store *system_store,
                              bool verbose);
int hold_cmd_profile_action(const struct hold_invocation *inv,
                              const struct hold_store *user_store,
                              int argc,
                              char **argv);
int hold_cmd_profile_set_command(const struct hold_invocation *inv,
                                    const struct hold_store *user_store,
                                    const char *name,
                                    int argc,
                                    char **argv);
int hold_cmd_profile_create_command(const struct hold_invocation *inv,
                                       const struct hold_store *user_store,
                                       const char *name,
                                       int argc,
                                       char **argv);
int hold_cmd_profile_delete(const struct hold_invocation *inv,
                               const struct hold_store *user_store,
                               const char *name);
int hold_cmd_profile_rename(const struct hold_invocation *inv,
                               const struct hold_store *user_store,
                               const char *old_name,
                               const char *new_name);
void hold_usage(void);
int hold_cmd_cap_request_action(const struct hold_invocation *inv,
                                const struct hold_store *system_store,
                                bool tail,
                                bool console_mode,
                                int argc,
                                char **argv);
int hold_cmd_elevated_capability_action(const struct hold_invocation *inv,
                                          const struct hold_store *system_store,
                                          const char *command,
                                          bool tail,
                                          bool console_mode,
                                          int sig,
                                          bool graceful,
                                          int argc,
                                          char **argv);

#endif /* HOLD_RUNTIME_H */
