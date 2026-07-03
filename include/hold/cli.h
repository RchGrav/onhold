#pragma once
#ifndef HOLD_CLI_H
#define HOLD_CLI_H

#include "hold/config.h"
#include "hold/types.h"

int hold_show_help(const char *topic);
bool hold_cli_command_is_parser_owned(const char *s);
bool hold_cli_command_is_public(const char *s);
bool hold_cli_command_allows_all(const char *s);
const char *hold_cli_command_usage(const char *s);
int hold_validate_owned_command_arity(const char *command, int argc);

#endif /* HOLD_CLI_H */
