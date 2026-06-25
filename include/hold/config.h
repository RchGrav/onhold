#pragma once
#ifndef HOLD_CONFIG_H
#define HOLD_CONFIG_H

/* Feature-test macros must precede every system header in EVERY translation
 * unit, so this header must be the FIRST include in every .c file. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ID_HEX_LEN 12
#define PROFILE_HASH_HEX_LEN 64
#define PROFILE_HASH_STR_LEN (PROFILE_HASH_HEX_LEN + 1)
#define ALIAS_MAX_LEN 64
#define STOP_TIMEOUT_MS 5000
#define POLL_SLEEP_MS 25
#ifndef HOLD_VERSION
#define HOLD_VERSION "dev"
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define HOLD_PATH_MAX PATH_MAX
#define MAX_RECORD_BYTES (1024 * 1024)
#ifndef HOLD_BOOT_ID_PATH
#define HOLD_BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#define HOLD_NEED_SOCKET_CLOEXEC 1
#endif
#define JSON_MAX_DEPTH 64
#ifndef HOLD_SYSTEM_STATE_DIR
#if defined(__APPLE__)
#define HOLD_SYSTEM_STATE_DIR "/var/db/hold"
#else
#define HOLD_SYSTEM_STATE_DIR "/var/lib/hold"
#endif
#endif

#endif /* HOLD_CONFIG_H */
