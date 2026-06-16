# Review Report

## Scope

This review focused on end-to-end portability, correctness, and safety for `sigmund`, with particular attention to Linux/macOS behavior, process-group lifecycle handling, stale-record detection, record/log cleanup, and the reliability of the shell test suite.

## Summary

The implementation was hardened across process identity validation, state evaluation, record parsing/writing, launch rollback, test reliability, and documentation. The most important behavior change is that `sigmund tail <id>` now behaves differently depending on whether the tracked process is still running: live jobs are followed from the end, while finished/stale/unknown jobs print the existing log from the beginning.

That `tail` behavior was changed because the old behavior made completed jobs look empty: it would seek to EOF and wait even when the process had already exited. For a job-oriented tool, `sigmund tail <id>` is most useful when it can show the output of the identified run, not only future bytes that can no longer arrive.

## Process

### 1. Baseline unpack and test

- Unpacked the original archive.
- Built the project with the existing `Makefile`.
- Ran the original test suite.
- Confirmed the baseline could build and pass the existing tests in the Linux container.

### 2. Test harness review

The original shell test runner invoked test functions inside an `if`. In Bash, that suppresses `errexit` behavior in contexts where failed assertions should abort the test. This meant some intermediate failures could be hidden.

The harness was rewritten so each test runs in a fresh strict Bash process. This made the test suite a more trustworthy signal before making further code changes.

### 3. Process lifecycle and safety review

The process-handling paths were reviewed for:

- PID and process-group reuse;
- leader exit before child exit;
- zombie process leaders;
- session membership checks;
- signaling safety;
- stale records across reboot;
- macOS process metadata availability;
- conservative behavior when validation is incomplete.

The implementation now avoids blindly trusting `kill(-pgid, 0)` as proof that a recorded process group is safe to signal. It validates the leader where possible and scans remaining same-session process-group members when the leader has exited or become a zombie.

### 4. Launch and rollback review

Launch was reviewed around fork/exec handshaking, partial reads, interrupted reads, log creation, record creation, and cleanup after failures.

The implementation now cleans reservation files and logs in more failure paths, handles partial exec-handshake payloads, and reaps children more robustly.

### 5. Record and filesystem review

Record loading and writing were reviewed for malformed input, oversized files, atomicity, persistence, collision behavior, and ID/file mismatches.

The implementation now limits record size, validates records more strictly, verifies requested IDs match loaded record IDs, checks for `.json`, `.log`, and reservation collisions, and improves atomic write error handling.

### 6. Documentation and spec review

The README and spec were updated to reflect the actual post-review behavior, including Linux/macOS build expectations, platform boot markers, process identity checks, same-session group validation, `tail` semantics, and known edge cases.

### 7. Re-review and final checks

After each change round, the code and docs were re-reviewed for mismatch or newly introduced issues. The final package passed the available Linux checks listed below.

## Verification performed

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

## Known limitations

- The Linux paths were runtime-tested in the review container.
- The macOS-specific paths were statically reviewed but not runtime-tested on macOS in the review container.
- The shell tests are stronger than before, but they still do not replace a real macOS CI lane.
- The JSON parser remains a small purpose-built parser for `sigmund`'s own records rather than a complete JSON implementation.

## Files changed

See `CHANGELOG.md` for the exact file-by-file change list.
