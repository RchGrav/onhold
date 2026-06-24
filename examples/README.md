# On Hold examples

These scripts are runnable examples for CI and automation patterns.

## `uv-webserver-profile.sh`

Installs or locates `uv` and `hold`, starts a Python HTTP server through On Hold, creates a durable profile from the recorded run, stops the first run, then starts the profile from a different directory to prove the profile recipe is independent of the script's current working directory.

```sh
examples/uv-webserver-profile.sh
```

Useful knobs:

```sh
HOLD_DEMO_PORT=8765 \
HOLD_DEMO_PROFILE=uv-web-demo \
HOLD_DEMO_ROOT="$PWD/.hold-demo" \
examples/uv-webserver-profile.sh
```

The script writes installer handoff state under `HOLD_DEMO_ROOT` and uses absolute `HOLD_BIN` and `UV_BIN` paths internally. It does not rely on a shell profile being reloaded.
