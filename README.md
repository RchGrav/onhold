# sigmund

What comes to mind when you hear the name _sigmund_?

Maybe a certain Viennese doctor. We won't dwell on him, though we'll concede that a man so fixated on what goes wrong between parents and children is oddly on-theme for a tool that keeps your child processes from being orphaned. Or maybe you land on Saturday mornings and Sid and Marty Krofft's _Sigmund and the Sea Monsters_, the little monster who couldn't bring himself to scare anyone. We'll take that one. We do keep track of `sid`s, session IDs, and we try hard not to become a C-monster. Pun intended.

The real meaning is older than both. _Mund_ is Old English for guardianship: a protector's care over what's in its keeping. _Sig_ is the signals the kernel uses to start, stop, and watch over your processes. Together they're the whole job: a guardian for the things you launch.

Today you do that job by hand: a `nohup`, a stray `&`, a PID written down somewhere, a `kill` you hope hits the right process. Or you reach for `systemd` and wind up authoring unit files and leaving a service resident on your box just to watch something behave in the background.

`sigmund` is the small, daemonless middle. Point it at a command and it remembers exactly what it started, keeps the log for you, and when you tell it to stop, it confirms the thing still running is the thing it launched. If it cannot be sure, it refuses. It takes down the whole process group, so nothing is orphaned.

```bash
run_id="$(sigmund ./your-server --port 9000)"
sigmund tail "$run_id"
sigmund stop "$run_id"
sigmund prune "$run_id"
```

## Install

The one-line installer detects Linux or macOS, chooses the matching release artifact, verifies its SHA-256 checksum, and installs `sigmund`.

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sh
```

By default, the installer uses `/usr/local/bin/sigmund` when it can write there, and otherwise falls back to `$HOME/.local/bin/sigmund`.

Force a system install to `/usr/local/bin`:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sh -s -- --system
```

Install into a custom directory:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh |
  SIGMUND_INSTALL_DIR="$HOME/bin" sh
```

For scripts and CI, ask the installer to write an environment handoff file:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh |
  SIGMUND_ENV_FILE="$PWD/.sigmund-env" sh
. "$PWD/.sigmund-env"

"$SIGMUND_BIN" --version
```

More install modes are in [Installing Sigmund](docs/install.md).

## Try The Demo

The demo is self-contained so you can see how Sigmund works without installing it to your system. It is a small shell script, and it is worth reading if you want to see exactly what it will do:

- [examples/interactive-demo/run-demo.sh](examples/interactive-demo/run-demo.sh)
- [demo README](examples/interactive-demo/README.md)

Run it:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/demo.sh | bash
```

What to expect: it creates a temporary directory, uses an existing `sigmund` if one is on `PATH` or downloads a release binary into that temporary directory, starts a tiny helper process, shows `list`, `dump`, `stop`, `prune`, creates an alias, starts from that alias, then removes the temporary directory. It does not install Sigmund, edit shell profiles, or write to `/etc`.

## Build From Source

Requires a C11 compiler and POSIX process APIs. Linux and macOS are supported.

```bash
make
./sigmund --help
```

On Linux, `make` attempts to build a static standalone binary. On macOS, it builds a normal dynamically linked binary.

## Useful Patterns

Start a helper, inspect it, then clean it up:

```bash
run_id="$(sigmund python3 -m http.server 8765)"
sigmund list
sigmund dump "$run_id"
sigmund stop "$run_id"
sigmund prune "$run_id"
```

Keep a CI helper alive across steps:

```yaml
- name: Start helper
  run: |
    run_id="$(sigmund ./your-server --port 9000)"
    echo "HELPER_RUN_ID=$run_id" >> "$GITHUB_ENV"

- name: Run tests
  run: ./run-tests

- name: Show helper log on failure
  if: failure()
  run: sigmund dump "$HELPER_RUN_ID" || true

- name: Stop helper
  if: always()
  run: |
    sigmund stop "$HELPER_RUN_ID" || true
    sigmund prune "$HELPER_RUN_ID" || true
```

Run a small local development stack:

```bash
frontend_id="$(sigmund npm run dev:frontend)"
backend_id="$(sigmund npm run dev:backend)"

sigmund list
sigmund tail "$backend_id"
sigmund stop "$frontend_id" "$backend_id"
```

Turn a recorded command into a reusable name:

```bash
id="$(sigmund ./your-server --port 9000)"
sigmund alias "$id" web

sigmund start web
sigmund stop web
```

Start something interactive with an attachable console:

```bash
id="$(sigmund --console bash)"
sigmund console "$id"
```

Press `Ctrl-]` to detach from a console without stopping the run.

## Documentation

The README is only the front door. The deeper material lives under [docs](docs/index.md):

- [Quickstart](docs/quickstart.md): a guided walkthrough from first run to aliases and root-scoped delegation.
- [Using Sigmund in CI](docs/ci.md): copyable workflow patterns.
- [Installing Sigmund](docs/install.md): installer modes, checksums, and CI handoff.
- [Documentation index](docs/index.md): the full map for command behavior, internals, security boundaries, console mode, and the implementation spec.
- [Examples](examples/README.md): runnable scripts, including the isolated interactive demo.

## What Sigmund Is Not

Sigmund is not a service supervisor. It does not restart processes after reboot, keep a resident daemon, or continuously refresh state in the background. It records enough information at launch to make later inspection and cleanup safer.

## License

Apache-2.0. See [LICENSE](LICENSE).
