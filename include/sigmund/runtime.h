#pragma once
#ifndef SIGMUND_RUNTIME_H
#define SIGMUND_RUNTIME_H

#include "sigmund/config.h"
#include "sigmund/types.h"

bool sigmund_command_accepts_target_tokens(const char *command);
bool sigmund_start_target_is_within_invoking_home(const struct sigmund_invocation *inv,
                                                 bool owned,
                                                 const char *command,
                                                 int argc,
                                                 char **argv);
int sigmund_perform_start(const struct sigmund_invocation *inv,
                         const struct sigmund_store *store,
                         bool tail,
                         bool console_mode,
                         int argc,
                         char **argv,
                         const char *exec_path,
                         const char *run_alias);
int sigmund_cmd_list_normal(const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *alias_filter,
                           bool iso);
int sigmund_cmd_list_system(const struct sigmund_store *system_store,
                           const char *alias_filter,
                           bool iso);
enum id_token_scope sigmund_parse_id_token(const char *token, const char **id_out);
int sigmund_resolve_public_profile_token(const struct sigmund_store *store,
                                        const char *token,
                                        char hash[PROFILE_HASH_STR_LEN]);
int sigmund_cmd_signal_action(const struct sigmund_invocation *inv,
                             const struct sigmund_store *user_store,
                             const struct sigmund_store *system_store,
                             const char *program,
                             const char *command,
                             int argc,
                             char **argv,
                             int sig,
                             bool graceful,
                             bool all,
                             bool print_cmd);
int sigmund_cmd_tail_action(const struct sigmund_invocation *inv,
                           const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *program,
                           const char *id_token);
int sigmund_cmd_dump_action(const struct sigmund_invocation *inv,
                           const struct sigmund_store *user_store,
                           const struct sigmund_store *system_store,
                           const char *program,
                           const char *id_token);
int sigmund_cmd_console_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              const struct sigmund_store *system_store,
                              const char *program,
                              const char *id_token);
int sigmund_cmd_prune_action(const struct sigmund_invocation *inv,
                            const struct sigmund_store *user_store,
                            const struct sigmund_store *system_store,
                            const char *program,
                            const char *target_token,
                            bool all);
int sigmund_elevate_start_token(const char *program,
                               bool tail,
                               bool console_mode,
                               const char *token_atom,
                               const char *hash,
                               bool multi,
                               int multi_count);
int sigmund_cmd_start_action(const struct sigmund_invocation *inv,
                            const struct sigmund_store *user_store,
                            const struct sigmund_store *system_store,
                            const char *program,
                            const struct sigmund_store *fallback_store,
                            bool tail,
                            bool console_mode,
                            bool multi,
                            int multi_count,
                            int argc,
                            char **argv);
int sigmund_ensure_start_store_for_command(const struct sigmund_invocation *inv,
                                          bool requested_system,
                                          bool owned,
                                          const char *command,
                                          int argc,
                                          char **argv,
                                          struct sigmund_store *store);
int sigmund_maybe_elevate_requested_system_targets(const char *program,
                                                  const char *command,
                                                  int argc,
                                                  char **argv,
                                                  bool all,
                                                  int *rc_out);
int sigmund_cmd_alias_action(const struct sigmund_invocation *inv,
                            const struct sigmund_store *user_store,
                            const struct sigmund_store *system_store,
                            const char *program,
                            int argc,
                            char **argv);
int sigmund_cmd_aliases_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              const struct sigmund_store *system_store,
                              bool verbose);
int sigmund_cmd_profile_action(const struct sigmund_invocation *inv,
                              const struct sigmund_store *user_store,
                              int argc,
                              char **argv);
void sigmund_usage(void);
int sigmund_cmd_elevated_capability_action(const struct sigmund_invocation *inv,
                                          const struct sigmund_store *system_store,
                                          const char *command,
                                          bool tail,
                                          bool console_mode,
                                          int sig,
                                          bool graceful,
                                          int argc,
                                          char **argv);

#endif /* SIGMUND_RUNTIME_H */
