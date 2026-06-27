#pragma once
#ifndef HOLD_CONSOLE_INTERNAL_H
#define HOLD_CONSOLE_INTERNAL_H

/* Console-private protocol constants, attach-state structs, and the SIGWINCH
 * flag. Included only by the console translation units; not part of the public
 * console.h surface. */
#include "hold/config.h"

#define CONSOLE_ATTACH_MAGIC "holdv1\0\0"
#define CONSOLE_ATTACH_MAGIC_LEN 8
#define CONSOLE_FRAME_DATA 'D'
#define CONSOLE_FRAME_RESIZE 'W'
#define CONSOLE_FRAME_DETACH 'X'
#define CONSOLE_FRAME_HEADER_LEN 3
#define CONSOLE_ATTACH_CTRL_P 0x10
#define CONSOLE_ATTACH_CTRL_Q 0x11
#define CONSOLE_ATTACH_DETACH_TIMEOUT_USEC 500000
#define CONSOLE_REPLAY_LIMIT (64 * 1024)

struct console_client_state {
    bool framed;
    bool decided;
    unsigned char pending[16384];
    size_t pending_len;
};

struct console_replay_buffer {
    unsigned char *data;
    size_t cap;
    size_t len;
    size_t start;
};

extern volatile sig_atomic_t g_console_resized;

/* Cross-file console internals (frame/replay/broker/attach share these). */
void hold_handle_console_sigwinch(int signo);
void hold_console_replay_init(struct console_replay_buffer *replay);
void hold_console_replay_free(struct console_replay_buffer *replay);
void hold_console_replay_append(struct console_replay_buffer *replay, const void *buf, size_t n);
int hold_console_replay_write(const struct console_replay_buffer *replay, int fd);
int hold_write_console_frame(int fd, unsigned char type, const void *payload, uint16_t len);
int hold_send_console_resize(int fd, const struct winsize *ws);
int hold_maybe_get_terminal_size(struct winsize *ws);
int hold_broker_process_client_input(struct console_client_state *state, int master,
                                const unsigned char *buf, size_t n);
int hold_make_console_listener(const char *sock_path);
int hold_console_peer_uid(int fd, uid_t *uid_out);
int hold_open_console_pty(int *master_out, int *slave_out,
                          unsigned short init_rows, unsigned short init_cols);
int hold_connect_console_socket(const char *sock_path);
void hold_make_raw_termios(const struct termios *in, struct termios *out);

#endif /* HOLD_CONSOLE_INTERNAL_H */
