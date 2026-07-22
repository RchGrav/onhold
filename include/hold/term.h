#pragma once
#ifndef HOLD_TERM_H
#define HOLD_TERM_H

/* term: the one PTY spawn engine and the one PTY-master pump. This layer
 * knows PTYs, processes-on-PTYs, and pumps — never records, sockets, or
 * commands (layer DAG: store -> term -> console). */

#include "hold/config.h"

struct hold_term_spawn {
    char *const *argv;     /* required: argv[0] non-NULL, NULL-terminated */
    const char *exec_path; /* pre-resolved binary => execv; NULL/"" => execvp(argv[0]) */
    const char *cwd;       /* chdir in the child before exec; NULL/"" => inherit */
    unsigned short rows;   /* initial PTY window size; 0x0 => 80x24 preset */
    unsigned short cols;
};

/* Spawns argv on a fresh PTY: opens the pair (nonzero winsize before exec),
 * forks, and in the child does setsid + TIOCSCTTY + dup2 x3 + exec, with any
 * pre-exec failure reported over the errno handshake (EOF = exec succeeded).
 * On success returns 0 with the PTY master and child pid; on any failure
 * returns -1 with errno set and nothing left open or unreaped. */
int hold_term_pty_spawn(const struct hold_term_spawn *spec,
                        int *master_out, pid_t *pid_out);

/* THE CLOEXEC pipe: pipe2(O_CLOEXEC) where available, else pipe() + FD_CLOEXEC
 * (acceptable: hold is single-threaded at every spawn site). Returns 0 with
 * fds[0]=read, fds[1]=write, or -1 with errno set and nothing left open. */
int hold_cloexec_pipe(int fds[2]);

/* ANSI TUI detection (docs/future/playback.md): sticky best-effort
 * recognition of screen-taking escapes in a PTY byte stream — alt-screen
 * switches (CSI ? 47/1047/1049 h) and explicit cursor addressing
 * (CSI row ; col H|f). Plain logs — even colored (SGR-only) ones, even
 * carriage-return progress bars — contain neither, so they never
 * misclassify; a TUI that uses only bare-\033[H homing is the accepted
 * best-effort miss. Feed is resumable across read chunks. */
struct hold_term_tui_detect {
    unsigned char st;
    unsigned param;
    bool saw_digit;
    bool tui; /* sticky: the stream has shown itself to be a terminal UI */
};

void hold_term_tui_feed(struct hold_term_tui_detect *d, const void *buf, size_t n);

/* One drain step of a PTY master: reads once into buf[n], appends the bytes
 * to the indexed log, and returns them for caller fan-out (client, replay
 * ring, tty). tui (optional) accumulates ANSI TUI detection across chunks;
 * once it trips, the sidecar entries are tagged with the pty stream so the
 * log is self-describing as a terminal recording. Returns >0 bytes pumped;
 * 0 when the target side is gone (EOF, or EIO from a failed read after the
 * last slave closed); -1 on a retryable read error (errno preserved). */
ssize_t hold_term_pump_master(int master, int logfd, int logidxfd,
                              struct hold_term_tui_detect *tui,
                              char *buf, size_t n);

/* THE detach-key FSM (Ctrl-P Ctrl-Q by default at both call sites): input
 * bytes are fed one at a time; bytes that stop matching a chord are handed
 * back through the sink in original order, a completed chord reports
 * through *completed without forwarding, and a pending prefix left hanging
 * is flushed after HOLD_TERM_DETACH_FLUSH_USEC (drive it from poll via
 * hold_term_detach_timeout_ms: -1 = nothing pending, 0 = flush now).
 * An optional alternate chord (the attach client's Ctrl-P Ctrl-P time-travel
 * double-tap, docs/future/playback.md) may share a prefix with the primary;
 * *completed reports 1 for the primary chord, 2 for the alternate. */
#define HOLD_TERM_DETACH_MAX_KEYS 8
#define HOLD_TERM_DETACH_FLUSH_USEC 500000

struct hold_term_detach {
    unsigned char keys[HOLD_TERM_DETACH_MAX_KEYS];
    size_t nkeys;
    unsigned char alt_keys[HOLD_TERM_DETACH_MAX_KEYS];
    size_t alt_nkeys;
    unsigned char pending[HOLD_TERM_DETACH_MAX_KEYS];
    size_t pending_len;
    int64_t deadline_usec;
};

/* Sink for bytes the FSM releases; nonzero aborts the feed/flush with -1. */
typedef int (*hold_term_detach_sink)(void *ctx, const unsigned char *bytes, size_t n);

void hold_term_detach_init(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys);
void hold_term_detach_set_alt(struct hold_term_detach *d, const unsigned char *keys, size_t nkeys);
int hold_term_detach_feed(struct hold_term_detach *d, unsigned char c,
                          hold_term_detach_sink sink, void *ctx, int *completed);
int hold_term_detach_flush(struct hold_term_detach *d, hold_term_detach_sink sink, void *ctx);
int hold_term_detach_timeout_ms(const struct hold_term_detach *d);

#endif /* HOLD_TERM_H */
