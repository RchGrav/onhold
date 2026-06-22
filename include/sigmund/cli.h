#pragma once
#ifndef SIGMUND_CLI_H
#define SIGMUND_CLI_H

#include "sigmund/config.h"
#include "sigmund/types.h"

int show_help(const char *topic);
bool is_sigmund_owned_command(const char *s);
bool parse_positive_count(const char *s, int *out);

#endif /* SIGMUND_CLI_H */
