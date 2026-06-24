#pragma once
#ifndef HOLD_ACCESS_H
#define HOLD_ACCESS_H

#include "hold/config.h"
#include "hold/types.h"

int hold_detect_invocation(struct hold_invocation *inv, bool requested_system, bool elevated);
int hold_init_invoking_user_store(const struct hold_invocation *inv, struct hold_store *store);
int hold_elevate_with_sudo_canonical(const char *program, int canonical_argc, char **canonical_argv);
int hold_elevate_with_sudo_direct(const char *program, int canonical_argc, char **canonical_argv);
int hold_elevate_with_sudo_parsed(const char *program,
                                    bool owned,
                                    const char *command,
                                    bool tail,
                                    bool console_mode,
                                    bool all,
                                    bool print_cmd,
                                    bool multi,
                                    int multi_count,
                                    bool force_raw,
                                    int argc,
                                    char **argv);
int hold_subject_grant_hash_for(const struct hold_store *system_store,
                                const char *subject,
                                const char *profile,
                                char hash[PROFILE_HASH_STR_LEN]);
int hold_load_subject_grant_profile(const struct hold_store *system_store,
                                      const char *subject,
                                      const char *profile,
                                      const char *expected_hash,
                                      const char *required_action,
                                      struct hold_profile *profile_out);
int hold_cmd_grant_revoke_action(const struct hold_invocation *inv,
                                   const struct hold_store *system_store,
                                   const char *program,
                                   bool grant,
                                   int argc,
                                   char **argv);

#endif /* HOLD_ACCESS_H */
