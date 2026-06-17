# Changelog

## 0.3.0 - Alias capability profiles and stable fingerprints

This release promotes Sigmund from run-ID lifecycle management into named
aliases while preserving the existing user-local and root-managed privilege
boundary. Run IDs remain concrete process-group handles; aliases are human
labels and launch recipes; protected hashes are root-managed capability
material, not normal action targets.

### Added

- Added `sigmund alias <id> <name>` to create or update an alias from a
  recorded run. User-local aliases store a direct recipe in private
  `aliases.json`; root-managed aliases publish only `alias -> hash` while the
  protected recipe stays in root-private `profiles.json`.
- Added `sigmund start <alias>` so aliases can start the recorded command
  template without retyping the original argv.
- Added `--multi [N]` for alias starts. Without it, `start <alias>` refuses when
  that alias already has a running process.
- Added alias-aware target resolution for `stop`, `kill`, `tail`, `dump`, and
  `prune`; ambiguous alias selections exit 6 and print the matching run IDs.
- Added `--all` to resolve alias ambiguity for `stop`, `kill`, and `prune`.
- Added `sigmund aliases` for visible alias lookup. User aliases show `-` in the
  hash column; system aliases show their protected profile hash.
- Added root-managed `grant` and `revoke` support for hash-scoped sudoers
  entries covering `start`, `stop`, `kill`, `tail`, `dump`, and `prune`.
- Added macOS arm64 and x86_64 package builds to the multi-arch release
  workflow.

### Changed

- Profile fingerprints now use the stable `sigmund-profile` domain, resolved
  absolute binary path, argc, and indexed argv NUL framing only.
- Profile fingerprints deliberately exclude environment, cwd, uid, timestamps,
  hostnames, and other context so aliases, profiles, and grants remain stable.
- Profile starts inherit Sigmund's current environment unchanged; privilege
  crossing continues to rely on sudo's standard `env_reset` behavior.
- Run IDs are now generated as 8 lowercase hex characters, with `00000000` and
  `ffffffff` reserved for internal capability selectors.
- Alias-started runs record their alias label in private run records and, for
  root-managed runs, in the redacted public index.
- Root-managed alias self-elevation now carries an internal
  `system:<alias>@<hash>` capability token so sudoers remains hash-pinned while
  root Sigmund resolves concrete runs by alias label and command intent.
- Release/dev artifact fallback versions now use the `0.3.0-<sha>` prefix.

### Fixed

- Fixed sudoers profile grants so generated command entries contain both the
  alias label and immutable hash (`system:<alias>@<hash>`), and root Sigmund
  verifies the pair before acting.
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
```

## 0.2.0 - Root-managed state stabilization pass

This pass hardens the root-managed state boundary without adding future command features.

### Added

- Added a bounded JSON value skip depth (`JSON_MAX_DEPTH = 64`) so malformed deeply nested records are rejected instead of recursing without limit.
- Added `O_NOFOLLOW` for Sigmund-owned record, public/private index, and log reads where the platform supports it.
- Added regression coverage for symlinked record/log rejection, deeply nested corrupt JSON rejection, sudo fork/wait status propagation, sudo exec failure, and tail Ctrl-C detach behavior.

### Changed

- Action self-elevation for `stop`, `kill`, `prune`, `tail`, and `dump` now forks a `sudo` child, waits in the non-root parent, and returns the child/root-Sigmund status while preserving inherited stdio.
- Renamed internal sudo elevation helpers and regression labels so the code and tests match the fork/wait behavior.
- Documentation now describes the current user-local/root-managed state model, redacted public index, conservative public `state_hint`, list/no-prompt behavior, root-only log/private details, and plain-ID conflict resolution.

### Fixed

- Fixed root-owned file read hardening so symlinked records/logs/index entries are rejected instead of followed.
- Fixed the minor saved-`errno` analyzer pattern by assigning `EIO` explicitly when an error path has no errno.

### Verification

The following checks passed in the update environment:

```bash
make clean && make test
make clean && make CC=clang STATIC_LDFLAGS= CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2' EXTRA_CPPFLAGS=-DSIGMUND_TESTING
clang --analyze -std=c11 -Wall -Wextra -Wpedantic -DSIGMUND_TESTING -DSIGMUND_BOOT_ID_PATH='"/tmp/sigmund_test_boot_id"' src/sigmund.c
make clean && UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 make test CC=clang STATIC_LDFLAGS= CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=undefined' LDFLAGS='-fsanitize=undefined'
```

Result: 45/45 tests passed in the root-started harness, including explicit normal-user, direct-root, and sudo-provenance actor paths.

A full clang ASan/UBSan harness run was attempted in this Linux container, but the sanitizer runtime crashed before Sigmund logic in the `sudo -u` actor path. macOS runtime validation is expected to run in CI on a macOS host.

## Root-managed state and argv-preserving fork/wait elevation update

This update completes the root-managed state split, public redacted discovery behavior, sudo-aware resolution rules, argv-preserving fork/wait self-elevation boundary, and root-independent test harness fixes.

### Added

- Added user-local vs root-managed storage selection:
  - normal non-root runs use `~/.local/state/sigmund`;
  - root, sudo, and `--system` runs use `/var/lib/sigmund` on Linux or `/var/db/sigmund` on macOS.
- Added root-managed layout with private `runs/`, private `logs/`, and public `public/` index directories.
- Added private sudo provenance metadata for root-managed records: `invoked_by_uid`, `invoked_by_gid`, `invoked_by_user`, and `invoked_via_sudo`.
- Added public root index writing with redacted discovery fields only.
- Added explicit deterministic target tokens: `user:<id>` and `system:<id>`.
- Added argv-preserving sudo self-elevation for action self-elevation:

  ```text
  sudo -- /absolute/path/to/sigmund --system --elevated <canonical-command...>
  ```

- Added executable-path resolution before sudo self-elevation using `/proc/self/exe` on Linux, `_NSGetExecutablePath()` plus `realpath()` on macOS, and `realpath(argv[0])` fallback when safe.
- Added an internal `--elevated` guard that fails cleanly if present without root authority.
- Added `SIGMUND_TESTING`-gated test overrides for system state and invoking-user home resolution. Production builds no longer honor an arbitrary environment variable for the root-managed system store.
- Added explicit test actors for user, root, and sudo-provenance contexts so `make test` works whether the test runner starts as root or non-root.
- Added GitHub CI coverage for a root-started Linux test harness and a macOS runtime test lane.
- Added regression coverage for long command truncation in `list`, root public state display, root/system store writes, and sudo-context management of unique invoking-user local runs.

### Changed

- Normal `sigmund list` now shows user-local rows plus redacted root-public rows without prompting for sudo.
- Root-public rows are displayed with `STATE` as `unknown` because public index state is non-authoritative and daemonless Sigmund cannot continuously refresh it after natural process exit.
- Public index files now write `"state_hint": "unknown"` for new root-managed entries.
- Action commands (`stop`, `kill`, `prune`, `tail`, and `dump`) route target selection through the shared resolver.
- Normal non-root plain-ID resolution prefers user-local state and only self-elevates for root-public matches when no user-local target exists.
- Root/sudo plain-ID resolution prefers root-managed state, then the invoking user's local state when sudo provenance exists.
- `--system` parsing preserves raw child arguments in raw start form while canonicalizing Sigmund-owned commands.
- The test build now compiles with `-DSIGMUND_TESTING`; the Makefile also honors `CPPFLAGS` and `EXTRA_CPPFLAGS`.
- README and `docs/SPEC.md` now describe root-managed state, public redaction, sudo-aware resolution, argv-preserving fork/wait elevation, and the explicit test actor model.

### Fixed

- Fixed the long-command `list` regression where a valid record could be skipped because the display buffer truncated through `checked_snprintf()` before the intended ellipsis logic ran. Long command displays are now safely truncated with ellipsis.
- Fixed misleading normal-user root-public list output by treating public state as non-authoritative and displaying `unknown` instead of stale `running`.
- Fixed root-run test harness failures by preventing the harness EUID from defining Sigmund's tested context.
- Fixed the public-index rollback analyzer warning by saving `errno` explicitly and assigning `EIO` when the test failure path injects a public-index write failure.
- Fixed the test system-store override footgun by limiting environment-driven system-store overrides to test builds.

### Verification

The following checks passed in the update environment:

```bash
make clean && make test
```

Result: 37/37 tests passed when the test runner started as root. The same test harness creates an explicit non-root actor for normal Sigmund behavior.

Additional verification performed for the packaged archive includes strict compile, analyzer, UBSan, static build, dynamic build, and fresh patch application checks. See the response accompanying the archive for the exact commands run in that environment.

### Known limitations

- macOS-specific code paths remain statically reviewed in this Linux container, but still need runtime verification on macOS CI before claiming full macOS runtime coverage.
- Public root index state is intentionally non-authoritative. Normal-user list output shows root-public state as `unknown`; authoritative root-managed state requires root Sigmund to read private records.

## Portability and hardening review update

This changelog records the changes made during the Linux/macOS portability and process-safety review.

### Added

- Added `CHANGELOG.md` so the review change history is preserved inside the project archive.
- Added `REVIEW.md` with the review process, rationale, verification commands, and known limitations.
- Added README references to the durable review artifacts.

### Changed

#### `.gitignore`

- Added `/sigmund-dynamic` so the optional dynamic build artifact is ignored like `/sigmund`.

#### `README.md`

- Updated build guidance from Linux-first wording to Linux and macOS support.
- Clarified that Linux `make` attempts a static binary, while macOS builds a normal dynamically linked Mach-O binary.
- Added references to `CHANGELOG.md` and `REVIEW.md`.
- Updated process-safety documentation:
  - Linux validates with `/proc/<pid>/stat` and `/proc/<pid>/exe` where available.
  - macOS validates with kernel process metadata and best-effort executable identity.
  - Remaining process-group members are validated against the recorded session before signaling.
- Updated `sigmund tail <id>` documentation:
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

#### `src/sigmund.c`

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
- Improved `sigmund tail <id>`:
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
- Improved `prune` and `killcmd`:
  - use safer boot handling;
  - refuse unknown or unvalidated signaling targets.

#### `tests/test_sigmund.sh`

- Enabled stricter shell behavior with `set -Eeuo pipefail`.
- Rewrote `run_test()` so each test runs in a fresh strict Bash process.
- Hardened process polling helpers against races with `ps`.
- Fixed an over-escaped log-path regex.
- Relaxed the version test to accept `dev` builds as well as semver-like release builds.
- Strengthened the exec-failure test to verify no orphan files at all, not just no JSON records.
- Added regression test: missing boot source does not force a running record to become stale.
- Added regression test: leader zombie with live same-session child group remains `running`.
- Added regression test: `sigmund tail <id>` prints output from an already-finished run.

### Verification

The following checks passed in the review environment:

```bash
make clean && make test
```

Result: 26/26 tests passed.

```bash
make clean && make && make sigmund-dynamic
```

Result: static Linux build and dynamic build both succeeded.

```bash
clang -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -Wformat=2 -Wno-format-nonliteral -Wstrict-prototypes \
  -Wmissing-prototypes -Werror \
  -DSIGMUND_VERSION='"dev"' \
  -c src/sigmund.c -o /tmp/sigmund_strict.o
```

Result: strict compile passed with warnings treated as errors.

```bash
make clean && make sigmund CC=clang \
  CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=undefined' \
  STATIC_LDFLAGS= \
  LDFLAGS='-fsanitize=undefined' \
  EXTRA_CPPFLAGS='-DSIGMUND_BOOT_ID_PATH="/tmp/sigmund_test_boot_id"'

SIGMUND_BIN=./sigmund bash tests/test_sigmund.sh
```

Result: UBSan build passed the full test suite.

### Known limitations

- macOS-specific code paths were reviewed statically in the Linux review container, but were not runtime-tested on macOS in that environment.
- The project still uses simple JSON parsing tailored to its own record format rather than a general JSON library.
