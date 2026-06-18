# Installing Sigmund

[Docs index](index.md) | [Quickstart](quickstart.md) | [Using Sigmund in CI](ci.md) | [Repository README](../README.md)

Sigmund publishes small release artifacts for supported Linux and macOS targets. The installer detects your platform, chooses the matching artifact, verifies its SHA-256 checksum, installs the `sigmund` binary, and prints the absolute path it installed.

## One-line install

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sh
```

If the system has `wget` instead of `curl`:

```sh
wget -qO- https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sh
```

Normal users install to:

```text
$HOME/.local/bin/sigmund
```

Root installs to:

```text
/usr/local/bin/sigmund
```

Run the same installer through root when you want a system install:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sudo sh
```

If the install directory is not on the current `PATH`, the installer still verifies the binary by absolute path, updates a future shell profile for normal users, and prints the export command to use immediately:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

## Script and CI handoff

A child installer process cannot change the parent shell's current `PATH`. In scripts, ask the installer to write an environment handoff file, then source it:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh |
  SIGMUND_ENV_FILE="$PWD/.sigmund-env" sh
. "$PWD/.sigmund-env"

"$SIGMUND_BIN" --version
```

The handoff file exports:

```sh
SIGMUND_BIN=/absolute/path/to/sigmund
PATH=/install/dir:$PATH
```

Use `"$SIGMUND_BIN"` inside automation when you need a deterministic path that does not depend on shell startup files.

## Custom install directory

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh |
  SIGMUND_INSTALL_DIR="$PWD/.bin" \
  SIGMUND_ENV_FILE="$PWD/.sigmund-env" \
  sh
. "$PWD/.sigmund-env"

"$SIGMUND_BIN" --help
```

## Pinned version

By default the installer uses the latest GitHub release. Pin a release when reproducibility matters:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/download/v0.3.0/install.sh | sh
```

You can also run the latest installer against a specific release by setting `SIGMUND_VERSION`; `SIGMUND_VERSION=0.3.0` and `SIGMUND_VERSION=v0.3.0` are equivalent.

## Selection logic

The installer detects:

- operating system: Linux or macOS.
- CPU architecture: `amd64`, `arm64`, `armhf`, `mipsel`, or `riscv64`.
- Linux libc: `gnu` or `musl`.
- UID: root installs system-wide; normal users install under `$HOME`.

Windows does not currently have a native Sigmund release artifact. Use the installer inside WSL, or build from source in a POSIX-like environment.

Default artifact choices are:

| Detected target | Artifact choice |
| --- | --- |
| macOS arm64 | `macos-arm64` |
| macOS x86_64 | `macos-x86_64` |
| Linux amd64 glibc | `linux-amd64-gnu-static` |
| Linux amd64 musl | `linux-amd64-musl-static` |
| Linux arm64 glibc | `linux-arm64-gnu-static` |
| Linux arm64 musl | `linux-arm64-musl-static` |
| Linux armhf glibc | `linux-armhf-gnu-static` |
| Linux armhf musl | `linux-armhf-musl-static` |
| Linux mipsel | `linux-mipsel-musl-static` |
| Linux riscv64 | `linux-riscv64-musl-static` |

GNU dynamic artifacts are available for users who specifically want them:

```sh
SIGMUND_FLAVOR=gnu-dynamic sh install.sh
```

The installer fails clearly instead of guessing when the platform is unsupported or libc cannot be detected safely.

## Manual build

If no release artifact matches your platform, build from source:

```sh
git clone https://github.com/RchGrav/sigmund.git
cd sigmund
make
./sigmund --version
```

## Continue

[Back to quickstart](quickstart.md) | [Using Sigmund in CI](ci.md) | [Back to docs index](index.md)
