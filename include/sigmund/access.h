#pragma once
#ifndef SIGMUND_ACCESS_H
#define SIGMUND_ACCESS_H

#include "sigmund/config.h"
#include "sigmund/types.h"

int detect_invocation(struct invocation *inv, bool requested_system, bool elevated);
int init_invoking_user_store(const struct invocation *inv, struct store_paths *store);
int elevate_with_sudo_canonical(const char *program, int canonical_argc, char **canonical_argv);
int elevate_with_sudo_parsed(const char *program,
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
int cmd_grant_revoke_action(const struct invocation *inv,
                                   const struct store_paths *system_store,
                                   const char *program,
                                   bool grant,
                                   int argc,
                                   char **argv);

#endif /* SIGMUND_ACCESS_H */
