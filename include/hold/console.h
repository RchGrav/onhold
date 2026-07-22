#pragma once
#ifndef HOLD_CONSOLE_H
#define HOLD_CONSOLE_H

#include "hold/config.h"
#include "hold/types.h"

/* Public console entry points used by the runtime/CLI layers. The frame, replay
 * and broker internals live in console_internal.h.
 *
 * The broker supports one interactive client at a time. Additional authorized
 * clients receive a short "already attached" error and are closed. */
int hold_format_console_sock_path(const struct hold_store *store,
                             const char *id,
                             char *out,
                             size_t n);
int hold_console_set_detach_keys(const unsigned char *keys, size_t len);
/* target_pid_fd: -1 = do not report; otherwise the broker writes its forked
 * target pid exactly once after exec-handshake success, then closes the fd. The
 * write precedes closing parent_pipe so a pid-write failure still reaches the
 * parent's handshake read as an errno; the ordering also guarantees that
 * handshake EOF at the parent implies the pid was already written (or the broker
 * died), so the parent's unbounded pid read cannot deadlock. */
void hold_run_console_broker(int parent_pipe,
                        int target_pid_fd,
                        const struct hold_store *store,
                        const char *run_id,
                        const char *log_path,
                        const char *sock_path,
                        uid_t owner_uid,
                        bool have_allowed_peer_uid,
                        uid_t allowed_peer_uid,
                        int argc,
                        char **argv,
                        const char *exec_path,
                        unsigned short init_rows,
                        unsigned short init_cols);
/* The attach client. log_path/run_id/name (any may be NULL) describe the
 * held call's broker-teed indexed log: when log_path is set and the attach
 * is interactive, a Ctrl-P double-tap time-travels — the viewer opens over
 * the same never-released session (docs/future/playback.md) and another
 * double-tap, or Esc, returns to the console at real time. */
int hold_run_native_console(const char *sock_path,
                              const char *log_path,
                              const char *run_id,
                              const char *name);

/* Provision and fork the server for an already-running PTY (hold shell
 * adoption): a console broker when the socket and log can be opened (the run
 * stays reattachable), else the log-only fallback pump. The adopted target
 * is never the server's child, is never killed or waited, and its exit is
 * detected via PTY EOF or group/session liveness. Every fd is opened before
 * forking so the child has no failure path; the parent's copies are closed
 * before return. Returns the server pid with *console_sock_out holding the
 * socket path ("" when the fallback serves), or -1 with errno set and
 * nothing forked (master left open). */
pid_t hold_spawn_adopted_console_server(const struct hold_store *store,
                                          const char *run_id,
                                          const char *log_path,
                                          int master,
                                          pid_t adopted_pgid,
                                          pid_t adopted_sid,
                                          pid_t hup_pid,
                                          char *console_sock_out,
                                          size_t console_sock_n);

#endif /* HOLD_CONSOLE_H */
