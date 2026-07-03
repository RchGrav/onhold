# Changelog

## 0.5.2

### Fixed

- The installer no longer installs the same binary to the same path twice.
  A leftover duplicate install lane from an old rename made every step run
  and print twice (`install:`, `binary:`, `HOLD_BIN=`); the second lane is
  gone and the output is single-lined.

## 0.5.1 - On Hold

The repository is renamed to `RchGrav/onhold` to match the tool. No code
changes.

### Changed

- README, install documentation, and the installer's default repository
  coordinate point at `RchGrav/onhold`. Old `RchGrav/sigmund` URLs keep
  working through GitHub's rename redirect.

## 0.5.0 - Hold On

`nohup` says no hang-up; Hold says hold on. This release re-founds the tool
around its identity: one small, reviewable static binary that places ordinary
commands as durable **calls** — held process groups with a 64-hex call ID, a
generated name, captured logs, and a line you can always pick back up. The
design contract is `docs/hold-on-identity.md`.

### Added

- Added `hold on` / `hold off`: a guarded shell session. Run anything; press
  `Ctrl-P Ctrl-Q` to put the foreground program on hold — it is adopted with a
  call ID, a generated name, captured logs, and a reattachable console.
- Added `hold attach` (alias `console`) as the canonical way to pick a call
  back up, including calls adopted from a `hold on` session.
- Added `hold end` (alias `stop`): end the call politely — TERM, then KILL.
- Added `hold save <target>`: protect a call from purge. No unsave exists;
  removal of a saved call is an explicit targeted `purge --force`.
- Added `hold purge` (aliases `rm`, `prune`, `drop`) as the one removal verb:
  sweeps ended calls, `-a` includes stale, a target removes one call,
  `--force` removes regardless of state. Purging a saved call without
  `--force` refuses with exit 2 and prints the exact command to copy.
- Added `hold rename <target> <name>` with the same validity and uniqueness
  rules as generated names.
- Added recipe-mode redial: `hold <id|name>` restarts a retained call in the
  session mode it was recorded with (`-it` recipes reattach, `-d` recipes
  detach); explicit flags override.
- Added `hold ports <target>` (observed listening sockets for the call's
  process group) and `hold stats <target>` (live CPU/memory/pids stream).
- Added neutral stdio reporting to `hold inspect`: where the call's fds
  actually point, including deliberate redirects.
- Rebuilt the full-screen `hold logs` viewer to the before-0.5 design: quiet
  persistent header/footer chrome, right-justified view status, `Ctrl-T`
  timestamp cycle (none/time/date) and `Ctrl-U` local/UTC toggle from the
  sidecar index, source and wrap controls, center-out filtering with
  Space-exclude and `Ctrl-R` reset, no counters and no flicker.

### Changed

- The bare form is the launch surface: `hold <cmd>`, `hold -d <cmd>`,
  `hold -it <cmd>`. There is no `run` verb. Docker familiarity now applies at
  the flag level (`-d`, `-i`, `-t`, `-e`, `--env-file`, `--name`, `--rm`,
  `--restart`, `--detach-keys`) and the table level, not the verb level.
- `hold -d` prints exactly one line: the full 64-hex call id.
- `hold list` (alias `ps`) renders one Docker-look table: content-sized
  columns, humanized CREATED, `Up`/`Exited (code)` STATUS phrasing with a
  ` (saved)` suffix for protected calls, and no PROFILE column.
- The vocabulary is calls, not runs or containers, across all output.
- Adopted (`hold on`) calls are served by the console broker: named, logged,
  and reattachable like every other call.

### Removed

- Removed the profile system, the alias store, and the
  `profile`/`profiles`/`export`/`import`/`commit` commands. A saved call is
  the reusable thing: save it, rename it, redial it.
- Removed the grants/sudoers/elevation subsystem (`grant`, `revoke`,
  capability tokens, sudo self-elevation). Acting on a root-managed call as a
  normal user now returns a clear requires-root error.
- Removed the captive IOS-style configuration CLI.
- Removed the `run`, `start`, `status`, `show`, `clean`, `doctor`, and `dump`
  verbs (`logs --print` covers dump; redial covers start).
- Removed the syslog log-destination mirror; captured logs are raw bytes plus
  the documented `HLOGIDX` sidecar.

## 0.4.0 - Hold process-management redesign

This release turns the project into Hold: a Docker-shaped, daemonless manager
for ordinary host processes. It keeps the lightweight single-binary model while
adding durable 64-hex run IDs, generated run names, profile-first workflows,
raw logs with sidecar indexes, a captive operator CLI, and stronger privilege
and storage boundaries.

### Added

- Added the `hold` command surface with Docker-shaped `run`, `ps`, `logs`,
  `inspect`, `stop`, `kill`, `rm`, `prune`, `profile`, and `console` workflows.
- Added full 64-hex run tracking with 12-hex display selectors and generated
  Docker-style `adjective_noun` run names.
- Added explicit profile creation/update through `hold profile <name> ...` and
  profile execution through `hold run <profile>`.
- Added Docker-shaped run modes including foreground `hold run`, explicit
  `-d/--detach`, `-i`, `-t`, `-it`, env/env-file persistence, restart metadata,
  and profile replay of saved mode options.
- Added raw local logs with an `HLOGIDX` sidecar index for offsets, lengths,
  timestamps, and stdout/stderr metadata while keeping plain `hold logs` output
  script-friendly.
- Added dynamic log-viewer/filter foundations, including type-to-filter,
  similarity exclusion, browse-away follow behavior, and PTY-friendly viewer
  tests.
- Added captive CLI profile editing, IOS-style prompts/help, profile transcript
  import/export, and robust terminal input handling.
- Added capability metadata support for direct runs and profiles, plus stronger
  grant refusal/reporting for unsafe privileged profile paths.

### Changed

- Renamed the public product identity from Sigmund/Mund-era wording to Hold.
- Reworked `ps` output into Docker-like columns with observed host listening
  ports instead of fake publish metadata.
- Made `hold run` foreground by default and kept bare `hold <cmd...>` as the
  convenience background-first launch form.
- Prioritized profile-name matches over executable-name matches unless `--` or a
  path-like command token makes the executable intent explicit.
- Unified run/profile/grant object handling around the Docker-shaped inspect
  storage shape documented in `docs/0.4-object-format-repair.md`.
- Replaced glibc NSS passwd/group lookups in the static-build path so the static
  release build is warning-free.
- Scrubbed stale public documentation and moved old planning/review material
  under `docs/archive/`.

### Fixed

- Fixed captive CLI terminal handling so mouse reports, arrow keys, and stale
  terminal modes do not leak raw escape text into the prompt.
- Fixed public CLI contract handling so retired aliases and `run <subcommand>`
  namespace passthrough forms are rejected.
- Fixed sanitizer compatibility for PTY/viewer timing tests; sanitizer lanes now
  run without timing-test skips.
- Fixed system/user path classification so elevated runs targeting user-home
  executables or path-like argv values remain user-scoped.
- Fixed restart behavior so retained runs append to existing logs instead of
  replacing them.

### Removed

- Removed fake Docker substrate behavior for `-p/--publish`, `-P`, and
  `-v/--volume`; Hold rejects those flags because it is not containerized.
- Removed the misleading captive `ping` command.

### Release notes

0.4.0 is the first Hold release candidate for the redesigned host-process
management surface. Local release-gate CI passed with strict static/dynamic
builds, the full regression suite, ASan/UBSan, cppcheck, layer lint, and the
0.4 Docker-shaped smoke tests.

## 0.3.9 - Layered internal architecture and a hardened, reproducible build

No runtime behavior change from 0.3.8: the CLI, on-disk formats, the profile-hash
capability key, sudoers semantics, and exit codes are identical (locked by a
golden profile-hash vector and the regression suite). This release restructures
the internals and makes the build and tests strict and reproducible everywhere.

### Changed

- The single ~8k-line translation unit is now a layered multi-translation-unit
  architecture (core → platform → store → console/access → runtime → cli) with
  header-enforced layer boundaries and `hold_`-prefixed cross-module symbols.
  All objects still link into one `hold` binary; no archives, no new ABI.

### Added

- One command, same rigor everywhere: `make ci` runs strict static + dynamic
  builds, the regression suite, the profile-hash golden vector, ASan/UBSan,
  cppcheck, and a layer-dependency lint — with a real SKIP state so nothing
  passes vacuously.
- Reproducible Linux parity from a Mac via `scripts/linux.sh` (a committed CI
  image), a privilege-delegation lane (`scripts/linux.sh root`) that runs the
  suite as a non-root user which elevates per-test, and a local release-artifact
  build (`scripts/linux.sh release` + `scripts/release_build.sh`).
- Regression tests for security invariants that previously had none: system-store
  modes/ownership, sudo provenance, sudoers generation + visudo validation,
  no-symlink reads, console-socket permissions, and the signal exit-code contract.

### Fixed

- The privilege-delegation test lane runs as a non-root user that elevates per
  test — matching how hold is actually used and what its `SUDO_*` provenance
  checks require — instead of running the whole suite as root.

### Release notes

Internal-quality release: rebuilt multi-arch binaries with no runtime behavior
change since 0.3.8.

## 0.3.8 - Console sockets bound relative to the private store

This point release rebuilds the published binaries; the only runtime change
since 0.3.7 is how console sockets are created.

### Fixed

- Console sockets are always created inside the run's private, 0700-owned store
  console directory and are bound/connected through a short name relative to
  that directory. A long store path no longer exceeds the AF_UNIX socket address
  limit, and the socket is never placed under a predictable `/tmp` path. The PTY
  broker restores its working directory before launching the target, so the
  launched process still runs in the caller's working directory.

### Release notes

No command-behavior change for normal-length paths; this hardens and unifies
where the console socket lives. The Linux and macOS suites pass, including new
coverage that the socket resides in the store console directory and that the
launched process runs in the caller's working directory.

Full changelog:
https://github.com/RchGrav/hold/compare/v0.3.7...v0.3.8

## 0.3.7 - Cross-platform test-validated binaries

This release uses the same On Hold runtime implementation as 0.3.6. It is the
preferred release because the binaries were rebuilt after the Linux and macOS
test gates were expanded and verified on real build runners.

### Added

- Added a macOS-specific CI script that validates the macOS runner
  architecture, Mach-O binary shape, macOS system-store documentation, the full
  runtime suite, and installer artifact selection for both supported macOS
  architectures.
- Added the same macOS-specific checks to the release workflow before macOS
  binaries are packaged.

### Fixed

- Fixed the Linux root-runner harness coverage for console tests so those tests
  use the same non-root actor boundary as the rest of the suite.

### Release notes

The 0.3.7 binaries are preferred over 0.3.6 because they were created after the
expanded test gates passed for Linux and for both macOS build boxes. No runtime
behavior change beyond the release version string is intended.

Full changelog:
https://github.com/RchGrav/hold/compare/v0.3.6...v0.3.7

## 0.3.6 - Record-scan fix and demo asset

This point release rebuilds the published binaries because the On Hold runtime
changed after 0.3.5.

### Fixed

- Fixed private record scans so user-local metadata such as `aliases.json` is
  not reported as a corrupt run record.
- Documented the secured installed binary requirement next to the quickstart
  `grant` example.

### Added

- Added `demo.sh` as a self-contained release asset for trying On Hold in a
  temporary sandbox.
- Added `examples/interactive-demo/` with the source version of the demo.
- Added `make check` as a profile for `make test`.

### Release notes

The binary archives were rebuilt for the `aliases.json` record-scan fix in
`src/hold.c`. The demo and documentation updates are support material
included with the release, not the reason the runtime binaries needed to
change.

Full changelog:
https://github.com/RchGrav/hold/compare/v0.3.5...v0.3.6

## 0.3.0 - Alias capability profiles and stable fingerprints

This release promotes On Hold from run-ID lifecycle management into named
aliases while preserving the existing user-local and root-managed privilege
boundary. Run IDs remain concrete process-group handles; aliases are human
labels and launch recipes; protected hashes are root-managed capability
material, not normal action targets.

### Added

- Added `hold profile save <id> as <name>` to create or update an alias from a
  recorded run. User-local aliases store a direct recipe in private
  `aliases.json`; root-managed aliases publish only `alias -> hash` while the
  protected recipe stays in root-private `profiles.json`.
- Added `hold start <alias>` so aliases can start the recorded command
  template without retyping the original argv.
- Added `--multi [N]` for alias starts. Without it, `start <alias>` refuses when
  that alias already has a running process.
- Added alias-aware target resolution for `stop`, `kill`, `tail`, `dump`, and
  `prune`; ambiguous alias selections exit 6 and print the matching run IDs.
- Added `--all` to resolve alias ambiguity for `stop`, `kill`, and `prune`.
- Added `hold profiles` for visible profile lookup. User profiles show `-` in the
  hash column; system aliases show `<root-managed>` with a truncated protected
  profile hash by default and the full hash with `-v`.
- Added `stop --print` and `kill --print` as the dry-run replacement for the
  old standalone signal-command helper.
- Added root-managed `grant` and `revoke` support for hash-scoped sudoers
  entries covering `start`, `stop`, `kill`, `tail`, `dump`, `prune`, and
  `console`.
- Added `hold --console <cmd...>` and `hold console <target>` for
  socat-backed attachable PTY sessions. Console runs still tee output to the
  normal log so `tail` and `dump` continue to work.
- Added `hold help [topic]` plus action `-h` help for the core command
  surface.
- Added `-f` as the documented start-and-follow short form while keeping
  `--tail` as a compatibility spelling.
- Added macOS arm64 and x86_64 package builds to the multi-arch release
  workflow.

### Changed

- Profile fingerprints now use the stable `hold-profile` domain, resolved
  absolute binary path, argc, and indexed argv NUL framing only.
- Profile fingerprints deliberately exclude environment, cwd, uid, timestamps,
  hostnames, and other context so aliases, profiles, and grants remain stable.
- Profile starts inherit On Hold's current environment unchanged; privilege
  crossing continues to rely on sudo's standard `env_reset` behavior.
- Run IDs are now generated as 8 lowercase hex characters, with `000000000000` and
  `ffffffffffff` reserved for internal capability selectors.
- Alias-started runs record their alias label in private run records and, for
  root-managed runs, in the redacted public index.
- Root-managed alias self-elevation now carries the internal capability argv
  shape `<verb> <runid_sel> <alias> <hash>` so sudoers remains hash-pinned
  while root On Hold resolves concrete runs by alias label and command intent.
- Console socket paths are private run-state fields and are cleaned up by
  normal prune lifecycle handling.
- Start now writes only the bare 8-hex run ID to stdout and writes the human
  banner to stderr; `--quiet` suppresses that human banner/status output.
- `list` now supports alias filtering and relative start ages by default; use
  `--iso` or `-l` for absolute timestamps.
- Successful `stop`, `kill`, and `prune` operations now confirm what happened
  on stderr while keeping stdout scriptable.
- `alias`, `grant`, and `revoke` confirmations now use stderr as human status;
  alias pinning prints the pinned command instead of making scripts parse a
  hash-oriented status line.
- Release/dev artifact fallback versions now use the `0.3.0-<sha>` prefix.

### Fixed

- Fixed sudoers profile grants so generated command entries contain the
  selected run ID slot, alias label, and immutable hash, and root On Hold
  verifies the pair plus selected run records before acting.
- Fixed the profile fingerprint regression test to extract the `bin` value from
  the exact hash entry under test.
- Fixed the SHA-256 test helper to fail with a clear diagnostic when neither
  `sha256sum` nor `shasum` is available.

### Verification

The following checks passed in the update environment:

```bash
make clean && make test CC=gcc CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'
make clean && make test CC=clang CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'
make clean && make test CC=gcc CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined' STATIC_LDFLAGS='' LDFLAGS='-fsanitize=address,undefined' TEST_LDFLAGS='-fsanitize=address,undefined'
cppcheck --enable=warning,performance,portability --error-exitcode=1 --suppress=missingIncludeSystem --std=c11 src/hold.c
```

## 0.2.0 - Root-managed state stabilization pass

This pass hardens the root-managed state boundary without adding future command features.

### Added

- Added a bounded JSON value skip depth (`JSON_MAX_DEPTH = 64`) so malformed deeply nested records are rejected instead of recursing without limit.
- Added `O_NOFOLLOW` for On Hold-owned record, public/private index, and log reads where the platform supports it.
- Added regression coverage for symlinked record/log rejection, deeply nested corrupt JSON rejection, sudo fork/wait status propagation, sudo exec failure, and tail Ctrl-C detach behavior.

### Changed

- Action self-elevation for `stop`, `kill`, `prune`, `tail`, and `dump` now forks a `sudo` child, waits in the non-root parent, and returns the child/root-On Hold status while preserving inherited stdio.
- Renamed internal sudo elevation helpers and regression labels so the code and tests match the fork/wait behavior.
- Documentation now describes the current user-local/root-managed state model, redacted public index, conservative public `state_hint`, list/no-prompt behavior, root-only log/private details, and plain-ID conflict resolution.

### Fixed

- Fixed root-owned file read hardening so symlinked records/logs/index entries are rejected instead of followed.
- Fixed the minor saved-`errno` analyzer pattern by assigning `EIO` explicitly when an error path has no errno.

### Verification

The following checks passed in the update environment:

```bash
make clean && make test
make clean && make CC=clang STATIC_LDFLAGS= CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2' EXTRA_CPPFLAGS=-DHOLD_TESTING
clang --analyze -std=c11 -Wall -Wextra -Wpedantic -DHOLD_TESTING -DHOLD_BOOT_ID_PATH='"/tmp/hold_test_boot_id"' src/hold.c
make clean && UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 make test CC=clang STATIC_LDFLAGS= CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=undefined' LDFLAGS='-fsanitize=undefined'
```

Result: 45/45 tests passed in the root-started harness, including explicit normal-user, direct-root, and sudo-provenance actor paths.

A full clang ASan/UBSan harness run was attempted in this Linux container, but the sanitizer runtime crashed before On Hold logic in the `sudo -u` actor path. macOS runtime validation is expected to run in CI on a macOS host.

## Root-managed state and argv-preserving fork/wait elevation update

This update completes the root-managed state split, public redacted discovery behavior, sudo-aware resolution rules, argv-preserving fork/wait self-elevation boundary, and root-independent test harness fixes.

### Added

- Added user-local vs root-managed storage selection:
  - normal non-root runs use `~/.local/state/hold`;
  - root, sudo, and `--system` runs use `/var/lib/hold` on Linux or `/var/db/hold` on macOS.
- Added root-managed layout with private `runs/`, private `logs/`, and public `public/` index directories.
- Added private sudo provenance metadata for root-managed records: `invoked_by_uid`, `invoked_by_gid`, `invoked_by_user`, and `invoked_via_sudo`.
- Added public root index writing with redacted discovery fields only.
- Added explicit deterministic target tokens: `user:<id>` and `system:<id>`.
- Added argv-preserving sudo self-elevation for action self-elevation:

  ```text
  sudo -- /absolute/path/to/hold --system --elevated <canonical-command...>
  ```

- Added executable-path resolution before sudo self-elevation using `/proc/self/exe` on Linux, `_NSGetExecutablePath()` plus `realpath()` on macOS, and `realpath(argv[0])` fallback when safe.
- Added an internal `--elevated` guard that fails cleanly if present without root authority.
- Added `HOLD_TESTING`-gated test overrides for system state and invoking-user home resolution. Production builds no longer honor an arbitrary environment variable for the root-managed system store.
- Added explicit test actors for user, root, and sudo-provenance contexts so `make test` works whether the test runner starts as root or non-root.
- Added GitHub CI coverage for a root-started Linux test harness and a macOS runtime test lane.
- Added regression coverage for long command truncation in `list`, root public state display, root/system store writes, and sudo-context management of unique invoking-user local runs.

### Changed

- Normal `hold list` now shows user-local rows plus redacted root-public rows without prompting for sudo.
- Root-public rows are displayed with `STATE` as `unknown` because public index state is non-authoritative and daemonless On Hold cannot continuously refresh it after natural process exit.
- Public index files now write `"state_hint": "unknown"` for new root-managed entries.
- Action commands (`stop`, `kill`, `prune`, `tail`, and `dump`) route target selection through the shared resolver.
- Normal non-root plain-ID resolution prefers user-local state and only self-elevates for root-public matches when no user-local target exists.
- Root/sudo plain-ID resolution prefers root-managed state, then the invoking user's local state when sudo provenance exists.
- `--system` parsing preserves raw child arguments in raw start form while canonicalizing On Hold-owned commands.
- The test build now compiles with `-DHOLD_TESTING`; the Makefile also honors `CPPFLAGS` and `EXTRA_CPPFLAGS`.
- README and `docs/SPEC.md` now describe root-managed state, public redaction, sudo-aware resolution, argv-preserving fork/wait elevation, and the explicit test actor model.

### Fixed

- Fixed the long-command `list` regression where a valid record could be skipped because the display buffer truncated through `checked_snprintf()` before the intended ellipsis logic ran. Long command displays are now safely truncated with ellipsis.
- Fixed misleading normal-user root-public list output by treating public state as non-authoritative and displaying `unknown` instead of stale `running`.
- Fixed root-run test harness failures by preventing the harness EUID from defining On Hold's tested context.
- Fixed the public-index rollback analyzer warning by saving `errno` explicitly and assigning `EIO` when the test failure path injects a public-index write failure.
- Fixed the test system-store override footgun by limiting environment-driven system-store overrides to test builds.

### Verification

The following checks passed in the update environment:

```bash
make clean && make test
```

Result: 37/37 tests passed when the test runner started as root. The same test harness creates an explicit non-root actor for normal On Hold behavior.

Additional verification performed for the packaged archive includes strict compile, analyzer, UBSan, static build, dynamic build, and fresh patch application checks. See the response accompanying the archive for the exact commands run in that environment.

### Known limitations

- macOS-specific code paths remain statically reviewed in this Linux container, but still need runtime verification on macOS CI before claiming full macOS runtime coverage.
- Public root index state is intentionally non-authoritative. Normal-user list output shows root-public state as `unknown`; authoritative root-managed state requires root On Hold to read private records.

## Portability and hardening review update

This changelog records the changes made during the Linux/macOS portability and process-safety review.

### Added

- Added `CHANGELOG.md` so the review change history is preserved inside the project archive.
- Added `REVIEW.md` with the review process, rationale, verification commands, and known limitations.
- Added README references to the durable review artifacts.

### Changed

#### `.gitignore`

- Added `/hold-dynamic` so the optional dynamic build artifact is ignored like `/hold`.

#### `README.md`

- Updated build guidance from Linux-first wording to Linux and macOS support.
- Clarified that Linux `make` attempts a static binary, while macOS builds a normal dynamically linked Mach-O binary.
- Added references to `CHANGELOG.md` and `REVIEW.md`.
- Updated process-safety documentation:
  - Linux validates with `/proc/<pid>/stat` and `/proc/<pid>/exe` where available.
  - macOS validates with kernel process metadata and best-effort executable identity.
  - Remaining process-group members are validated against the recorded session before signaling.
- Updated `hold tail <id>` documentation:
  - running records are followed from the end;
  - finished, stale, or unknown records are printed from the beginning.
- Updated documented edge cases:
  - leader exit with live child group is handled safely;
  - fast-exit commands show as `exited`, not `dead`;
  - exec-launch failures leave no orphan records or logs;
  - escape diagnostics are no longer described as Linux-only.
- Removed outdated roadmap items that were implemented by this review.

#### `docs/SPEC.md`

- Updated `tail <id>` semantics for exited, stale, and unknown records.
- Updated stop/kill wording to reflect dynamically evaluated state rather than record mutation.
- Replaced Linux-only boot correlation wording with platform boot markers:
  - Linux: `/proc/sys/kernel/random/boot_id`;
  - macOS: `kern.boottime`.
- Clarified that an unavailable current boot marker must not make a record stale by itself.
- Updated record field documentation:
  - `boot_id` is now a platform boot marker when available;
  - identity fields are platform identity fields, not Linux-only fields.
- Updated ID generation requirements to check `.json`, `.log`, and reservation collisions.
- Updated process identity capture for Linux and macOS.
- Updated stop/kill safety:
  - validates leader identity;
  - treats zombie leader plus live same-session group as running;
  - treats empty or zombie-only same-session group as exited;
  - refuses to signal when validation is unknown.
- Updated polling behavior to use same-session group validation rather than plain `kill(-pgid, 0)`.
- Updated list semantics to match the implementation.
- Updated escape diagnostics to be process-table based rather than Linux-only.
- Updated build compatibility notes for macOS.

#### `src/hold.c`

- Added record-size guard `MAX_RECORD_BYTES` to limit JSON record loading to 1 MiB.
- Added portability fallbacks for `O_CLOEXEC` and `O_DIRECTORY`.
- Hardened `read_file_trim()`:
  - rejects zero-length buffers;
  - opens with close-on-exec;
  - retries interrupted reads;
  - preserves `errno`.
- Added `path_exists()`.
- Added `current_boot_id()` so the program can distinguish an unavailable boot marker from an available marker mismatch.
- Improved ID generation:
  - checks existing `<id>.json`, `<id>.log`, and reservation files before accepting an ID;
  - rechecks after reserving to close collision races.
- Hardened `write_all()` so zero-byte writes are treated as `EIO`.
- Hardened atomic record writes:
  - checks `ferror()`;
  - checks `fflush()`;
  - opens the storage directory with close-on-exec before `fsync()`.
- Added shared process identity and state parsing:
  - Linux `/proc/<pid>/stat` parsing;
  - macOS `sysctl`/kernel process metadata path.
- Added same-session process-group liveness scanning:
  - distinguishes scan error, empty group, zombie-only group, and live group.
- Reworked session escape counting to reuse the safer process parser.
- Hardened `/proc` reads:
  - close-on-exec;
  - interrupted-read retry;
  - better error preservation.
- Improved state evaluation:
  - invalid `pgid` or `sid` becomes `unknown`;
  - boot mismatch applies only when the current boot marker is known;
  - zombie leader plus live same-session child group is `running`;
  - empty or zombie-only same-session group is `exited`;
  - process-table validation failure becomes `unknown`;
  - fallback behavior is conservative rather than blindly signaling possibly reused process groups.
- Improved `hold tail <id>`:
  - starts from the end only for running records;
  - prints finished, stale, or unknown logs from the beginning;
  - non-following tails exit immediately after EOF.
- Hardened rollback paths:
  - `waitpid()` retries on `EINTR`;
  - reservation files are removed on pipe, `fcntl`, and fork failures;
  - orphan log files are removed on exec failure;
  - spawned groups are killed and reaped when handshake or record-write rollback is needed.
- Added robust exec-handshake reading:
  - handles partial reads;
  - handles interrupted reads;
  - detects malformed partial payloads.
- Strengthened record validation:
  - requires valid hex ID;
  - requires positive `pid`;
  - requires safe `pgid > 1`;
  - requires positive `sid`.
- Strengthened `load_record_by_id()`:
  - validates loaded records;
  - requires the internal record ID to match the requested filename ID.
- Improved stop/kill behavior:
  - refuses `unknown` targets instead of signaling without validation;
  - waits for validated same-session target group disappearance;
  - treats zombie-only groups as gone for signaling purposes;
  - rechecks after forced kill.
- Improved `list`:
  - warns on unreadable or corrupt `.json` records;
  - uses boot markers only when available.
- Improved `prune` and dry-run signal printing:
  - use safer boot handling;
  - refuse unknown or unvalidated signaling targets.

#### `tests/test_hold.sh`

- Enabled stricter shell behavior with `set -Eeuo pipefail`.
- Rewrote `run_test()` so each test runs in a fresh strict Bash process.
- Hardened process polling helpers against races with `ps`.
- Fixed an over-escaped log-path regex.
- Relaxed the version test to accept `dev` builds as well as semver-like release builds.
- Strengthened the exec-failure test to verify no orphan files at all, not just no JSON records.
- Added regression test: missing boot source does not force a running record to become stale.
- Added regression test: leader zombie with live same-session child group remains `running`.
- Added regression test: `hold tail <id>` prints output from an already-finished run.

### Verification

The following checks passed in the review environment:

```bash
make clean && make test
```

Result: 26/26 tests passed.

```bash
make clean && make && make hold-dynamic
```

Result: static Linux build and dynamic build both succeeded.

```bash
clang -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -Wformat=2 -Wno-format-nonliteral -Wstrict-prototypes \
  -Wmissing-prototypes -Werror \
  -DHOLD_VERSION='"dev"' \
  -c src/hold.c -o /tmp/hold_strict.o
```

Result: strict compile passed with warnings treated as errors.

```bash
make clean && make hold CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=undefined' \
  STATIC_LDFLAGS= \
  LDFLAGS='-fsanitize=undefined' \
  EXTRA_CPPFLAGS='-DHOLD_BOOT_ID_PATH="/tmp/hold_test_boot_id"'

HOLD_BIN=./hold bash tests/test_hold.sh
```

Result: UBSan build passed the full test suite.

### Known limitations

- macOS-specific code paths were reviewed statically in the Linux review container, but were not runtime-tested on macOS in that environment.
- The project still uses simple JSON parsing tailored to its own record format rather than a general JSON library.
