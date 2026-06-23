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

By default, the installer uses `/usr/local/bin` when the current process can write there. That covers root installs and machines where `/usr/local/bin` is intentionally group-writable.

When `/usr/local/bin` is not writable, it falls back to:

```text
$HOME/.local/bin/sigmund
```

Run the same installer through root when you want a system install:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sudo sh
```

You can also force a system install from a normal shell. This targets `/usr/local/bin` and asks for a `sudo` password only if the current process cannot write there:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh | sh -s -- --system
```

For CI or scripted use, the equivalent environment switch is:

```sh
curl -LsSf https://github.com/RchGrav/sigmund/releases/latest/download/install.sh |
  SIGMUND_INSTALL_SYSTEM=1 sh
```

The installed binary is written with mode `0755`. For the default root-owned system install, ownership is set to `root:root` where the platform allows it.

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
curl -LsSf https://github.com/RchGrav/sigmund/releases/download/vX.Y.Z/install.sh | sh
```

You can also run the latest installer against a specific release by setting `SIGMUND_VERSION`; values with and without the leading `v` are equivalent.

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

### Linux artifact families

Linux artifact names deliberately distinguish the libc ABI from the link mode:

- `linux-*-gnu-static` is built for glibc systems and linked with `-static`. It reduces shared-library dependencies, but it is not a universal standalone binary: glibc can still load host NSS modules at runtime for user, group, hostname, DNS, and other name-service lookups depending on `/etc/nsswitch.conf`.
- `linux-*-gnu-dynamic` is built for glibc systems and dynamically links against the host glibc. Choose it when you want the target distribution's libc, NSS, and security update behavior.
- `linux-*-musl-static` is statically linked with musl. Choose this for the closest thing to a true standalone Linux artifact, especially for scratch/minimal containers, rescue systems, or copying one binary between compatible Linux systems.

On glibc hosts, the installer defaults to `gnu-static` for compatibility with the detected host ABI. If true standalone behavior matters more than matching the host glibc ABI, request the musl artifact explicitly:

```sh
SIGMUND_FLAVOR=musl-static sh install.sh
```

GNU dynamic artifacts are available for users who specifically want normal dynamic glibc behavior:

```sh
SIGMUND_FLAVOR=gnu-dynamic sh install.sh
```

GNU static artifacts can also be requested explicitly:

```sh
SIGMUND_FLAVOR=gnu-static sh install.sh
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

On Linux, the default `Makefile` link mode is `STATIC_LDFLAGS=-static`. With a glibc compiler, that creates the same kind of GNU static binary described above: useful, but still subject to glibc NSS runtime caveats. For a dynamic glibc build, run `make STATIC_LDFLAGS=` or build the `sigmund-dynamic` target. For a true standalone-style Linux build, use a musl-targeting compiler and keep static linking enabled.

## Continue

[Back to quickstart](quickstart.md) | [Using Sigmund in CI](ci.md) | [Back to docs index](index.md)
