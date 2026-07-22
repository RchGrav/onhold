# Hold On

[![CI](https://github.com/RchGrav/HoldOn/actions/workflows/ci.yml/badge.svg)](https://github.com/RchGrav/HoldOn/actions/workflows/ci.yml)
[![Multi-arch Release](https://github.com/RchGrav/HoldOn/actions/workflows/release.yml/badge.svg)](https://github.com/RchGrav/HoldOn/actions/workflows/release.yml)

`nohup` says *no hang-up*: abandon the call gracefully. Hold says **hold on**:
the line stays open.

Hold keeps host processes alive, visible, and controllable after your terminal
or CI step moves on. The unit it manages is a **call** — one held process
group with a durable 64-hex call ID, a generated `adjective_noun` name,
captured logs, and safe lifecycle commands. No daemon, no config server, one
static binary.

More than nohup, less than systemd.

```sh
hold -d python3 -m http.server 8080   # place a call in the background
hold list                             # your calls
hold logs web                         # what was said
hold attach web                       # pick the call back up
hold end web                          # end it politely
```

## Install

```sh
curl -LsSf https://github.com/RchGrav/HoldOn/releases/latest/download/install.sh | sh
```

System-wide (installs to `/usr/local/bin` with sudo):

```sh
curl -LsSf https://github.com/RchGrav/HoldOn/releases/latest/download/install.sh | sh -s -- --system
```

Or build from source with any C11 compiler:

```sh
make && ./hold --help
```

Details, checksums, and offline installs: [docs/install.md](docs/install.md).

Hold does **not** create containers. It does not isolate filesystems, publish
ports, mount volumes, or pretend a host process has Docker networking. It
manages ordinary host processes and tells the truth about them. Where the
output *looks* like Docker — the call table, the flags, `-d`/`-it` — that is
deliberate familiarity, nothing more.

## What Hold is for

Development servers, CI helper processes, temporary workers, integration-test
dependencies, a qemu you want to walk away from, and interactive programs you
want to put on hold and come back to.

```text
nohup / &   too little state and safety
Hold        durable call handle, logs, save/redial, attach/detach
systemd     machine service supervision and boot policy
Docker      containers, images, isolation, networking
```

## Placing calls

```sh
hold <cmd> [args...]        # foreground: stream its output
hold -d <cmd> [args...]     # detached: prints the bare 64-hex call id
hold -it <cmd> [args...]    # attached on a PTY (Ctrl-P Ctrl-Q puts it on hold)
hold <id|name>              # redial: restart a retained call from its recipe
```

Flags mirror Docker where they exist: `-d`, `-i`, `-t`, `-e`, `--env-file`,
`--name`, `--rm`, `--restart`, `--detach-keys`. Fake substrate flags
(`-p`, `-P`, `-v`) are honestly rejected. Use `--` when the command could be
mistaken for a call name.

A redialed call replays its recorded recipe, including its session mode: a
call placed with `-it` reattaches, one placed with `-d` detaches again.

## The session: hold on

```sh
hold on
# Hold is now active. Ctrl-P Ctrl-Q puts the foreground program on hold;
# 'hold off' or exit ends the session.
```

Inside a `hold on` session, run anything. When something turns out to be
worth keeping — a server, a REPL, nyancat — press `Ctrl-P Ctrl-Q` and Hold
adopts it: it gets a call ID, a name, captured logs, and a console you can
reattach to later with `hold attach`. `hold off` or `exit` ends the session.

## Managing calls

```sh
hold list                   # your ledger: your calls, live and past
hold ps                     # the Docker view: running calls, machine-wide
hold attach <target>        # pick a call back up (Ctrl-P Ctrl-Q detaches)
hold end <target>           # end politely: TERM, then KILL (stop is an alias)
hold kill <target>          # KILL now, when it won't listen
hold logs <target> [-f]     # full-screen viewer; -p/--print for plain text
hold inspect <target>       # everything visible at your access level, JSON
hold ports <target>         # ports in use by the call's process group
hold stats <target>         # live CPU/memory/pids stream
hold save <target>          # protect a call from purge
hold rename <target> <name> # give it a meaningful name
hold purge [<target>] [-a] [--force]   # the one removal verb
                            # (rm, prune, and drop are aliases)
```

Targets are call IDs, ID prefixes, or names. Ambiguity is refused, never
guessed. Saved calls survive every purge except a targeted `--force`:

```text
$ hold purge web
hold: 'web' is saved — purging a saved call requires --force
  hold purge web --force
```

## Logs

Plain output stays script-friendly; the full-screen viewer opens only on a
TTY. Captured logs are raw bytes plus an `HLOGIDX` sidecar carrying offsets,
timestamps, and stream metadata — a documented, stable format anything can
read without Hold's help.

```sh
hold logs web               # full-screen viewer (filter as you type)
hold logs -f web            # follow (tail is an alias)
hold logs -p -n 100 web     # plain text, last 100 records
hold logs --replay web      # play it back with its recorded timing
```

Playback is a mode of the viewer: Space pauses and resumes, `.` and `,`
step fast-forward and rewind through a 1-16x ladder, and transport is live
while tailing — Space freezes the live edge, `,` rewinds mid-tail. To a
non-TTY, `--replay` is a plain linear pipe. A damaged or missing sidecar
index heals itself on read; reconstructed timing is labeled, never passed
off as recorded.

Output a program sends somewhere else on purpose — its own logfile, a
redirect — is respected, not captured; `hold inspect` reports where the
call's stdio actually points.

## Safety model

Hold is daemonless: every command reopens recorded state and revalidates
before acting. Managed calls get their own process group and session; stop
and kill are delivered to the group after the recorded identity is
revalidated, so a recycled PID is never signaled by mistake. stdout carries
machine data; human notes go to stderr; `--quiet` silences them.

## Build

```sh
make
./hold --help
```

Requires a C11 compiler and POSIX. Linux is the primary target. The design
contract lives in [docs/hold-on-identity.md](docs/hold-on-identity.md).
