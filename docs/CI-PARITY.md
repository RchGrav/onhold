# CI parity contract

Hold's tests are intentionally split by privilege model. Keeping these lanes
separate is part of the product contract, not just build hygiene.

## Normal CI lane

Run:

```sh
bash scripts/ci.sh
```

This lane must start as a normal non-root user. It verifies the default user
experience: user-local state, non-elevated refusal paths, normal ownership, and
that commands do not accidentally become root/system operations.

Do **not** run this lane with `sudo`. `scripts/ci.sh` fails fast when invoked as
UID 0 because a root run can mask bugs that only exist for ordinary users.

## Privilege-delegation lane

Run:

```sh
bash scripts/test_root.sh
```

This lane also starts as a non-root user, but requires passwordless sudo. The
suite elevates only the individual operations that are supposed to model
root/system behavior. This preserves `SUDO_UID`/`SUDO_USER` provenance and keeps
normal-user assertions meaningful.

Do **not** replace this with `sudo make test`; that erases the invoking-user
boundary the tests are designed to exercise.

## GitHub release gate

The release workflow must pass both lanes before publishing artifacts:

1. normal non-root `make test`;
2. privilege-delegation `scripts/test_root.sh`.

The publish job depends on both. A tag should not produce release assets unless
both the normal user contract and the sudo-per-test contract are green.

## Static analysis parity

`cppcheck` versions differ between local machines and GitHub runners. The
committed static-analysis command should stay identical between `scripts/ci.sh`
and `.github/workflows/ci.yml`; targeted suppressions should be added in both
places with the reason captured in the commit message.
