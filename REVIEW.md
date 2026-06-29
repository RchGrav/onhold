# Review status

Date: 2026-06-23
Branch: `hold-0.4.0-redesign`
Scope: release-readiness notes for the 0.4.0 documentation, branch alignment, and hardening plan.

## Current status

This file replaces an older review report whose test counts and file references described a previous implementation review. Treat this document as a status tracker, not proof that the current branch is release-ready. The current branch version file remains `0.3.9`; 0.4.0 wording is release-plan status unless backed by fresh verification.

0.4.0 is release-ready only after both tracks are complete:

1. the full `hold` CLI/product direction in `docs/HOLD_0_4_UX_SPEC.md` is implemented and documented; and
2. the hardening backlog in that same spec is implemented, tested, and reviewed.

## Latest recovery/readiness pass

Date: 2026-06-29

The crash-recovery pass restored a test-green working tree and preserved the
2026-06-28 direction/security findings as branch review artifacts:

- [0.4.0 direction and decisions](docs/0.4.0-direction-2026-06-28.md)
- [0.4.0 security review](docs/security-review-2026-06-28.md)

Current verification from this pass:

```sh
make test
# summary: 157 passed, 0 failed, 0 skipped
# viewer filter engine: PASS
# profile-hash vector: PASS

make lint
# layer dependency direction: clean
```

This proves the current recovered branch is ready for the next implementation
slice. It does **not** prove 0.4.0 release readiness.

Recommended next implementation tasks, in order:

1. **Privilege hardening gate:** implement the trusted-execution-context layer
   before shipping delegation: pinned cwd, root-owned binary/path-arg/ancestor
   validation, sanitized/hash-covered env, and `O_NOFOLLOW`/fd-based log
   handling. This is the highest-risk release blocker.
2. **Profile schema expansion:** persist and hash cwd/env so the capability
   digest matches the actual execution context.
3. **Run identity/name and Docker-shaped listing:** replace the legacy
   random 12-hex generator with creation-hash run/profile tracking IDs, store
   full hashes, display the first 12 hex characters, persist generated
   `adjective_noun` names from the same hash, and make `ps -a` match Docker's
   table shape with `RUN ID`, `PROFILE`, `COMMAND`, `CREATED`, `STATUS`,
   observed `PORTS`, and `NAMES`.
4. **Public CLI contract completion:** finish the table-driven parser/help
   agreement for the 0.4 shell surface, including the strict distinction
   between Docker-shaped `hold run` and background-first bare `hold`, the
   profile-over-executable selection rule, `--` conflict escape, and the
   non-running `hold profile` creation twin; then remove or hide remaining
   legacy primary verbs that conflict with the target command language.
5. **Release evidence refresh:** after each implementation slice, update this
   file with exact command/date/environment evidence instead of relying on
   historical green runs.

## Latest alignment pass

Date: 2026-06-23

The Autopilot spec-alignment pass added [0.4.0 branch alignment](docs/0.4.0-alignment.md) as the tracked matrix for implemented behavior, accepted v1 deviations, and follow-up gaps. It also labels legacy/current docs so they do not conflict with the 0.4.0 `hold` direction. This pass was docs-only and does not certify runtime release readiness.

## Verification policy

Do not claim the test suite passes unless this exact command completes successfully on the current branch:

```sh
make test
```

Docs-only changes may be reviewed with read/grep/link checks. Those checks are useful for documentation consistency, but they do not prove implementation behavior or release readiness.

## Latest docs-only verification

Date: 2026-06-23

Commands run for this docs cleanup:

```sh
python3 - <<'PY'
from pathlib import Path
import re, sys
files = [Path(p) for p in ['README.md','docs/install.md','docs/HOLD_0_4_UX_SPEC.md','REVIEW.md','docs/index.md']]
missing=[]
for f in files:
    text=f.read_text()
    for m in re.finditer(r'\[[^\]]+\]\(([^)]+)\)', text):
        target=m.group(1).split('#',1)[0]
        if not target or re.match(r'^[a-z]+:', target) or target.startswith('mailto:'):
            continue
        p=(f.parent/target).resolve()
        if not p.exists():
            missing.append((str(f), target))
if missing:
    print('Missing markdown link targets:')
    for f,t in missing:
        print(f'{f}: {t}')
    sys.exit(1)
print('All local markdown link targets exist for touched files.')
PY

grep -RIn "NSS\|musl-static\|GNU static\|GNU dynamic\|musl static\|STATIC_LDFLAGS" \
  README.md docs/install.md docs/HOLD_0_4_UX_SPEC.md REVIEW.md

python3 - <<'PY'
from pathlib import Path
import re, sys
patterns = [
    re.compile("26" + "/26"),
    re.compile("Result:" + r" .*" + "tests " + "passed"),
    re.compile("make clean && " + "make test"),
    re.compile("UBSan build " + "passed"),
]
files = [Path(p) for p in ['REVIEW.md','docs/HOLD_0_4_UX_SPEC.md','README.md','docs/install.md']]
hits = []
for f in files:
    for i, line in enumerate(f.read_text().splitlines(), 1):
        if any(p.search(line) for p in patterns):
            hits.append((f, i, line))
if hits:
    for f, i, line in hits:
        print(f'{f}:{i}:{line}')
    sys.exit(1)
PY

grep -n "full .*product direction\|release criteria\|later minor\|implementation plan" \
  docs/HOLD_0_4_UX_SPEC.md docs/index.md
```

Result: local links in touched files resolved, static-artifact caveats were present in the expected docs, stale historical pass claims were absent, and 0.4.0 scope/alignment language was present.

Latest implementation pass: `make test` was run on this branch after adding name-first profile command editing/CRUD and a shell profile submode and passed (104 shell tests, viewer filter engine, and profile-hash vectors). Earlier docs-only review notes above remain link/grep evidence only and should not be reused as runtime proof.

## Release-readiness checklist

- [x] `make test` completes successfully on this branch for the 2026-06-23 Autopilot alignment pass.
- [ ] CLI grammar, help text, parser behavior, and documentation agree for the 0.4.0 `hold` surface.
- [ ] GNU static, GNU dynamic, and musl static install behavior is documented without overstating glibc static portability.
- [ ] Console attach authorization is peer-credential checked before replaying output or forwarding input.
- [ ] Signal safety refuses uncertain records instead of relying on display-oriented liveness.
- [ ] Store directory creation and atomic writers reject symlink/temp-file attacks.
- [ ] Release and installer scripts fail closed, validate checksums/layouts, and avoid destructive release deletion.
- [ ] Source archive builds report the `VERSION` file value without requiring a Git checkout.

## Current caveats

- This branch now includes both documentation alignment and initial profile-editing implementation slices.
- The implementation test evidence in this file is limited to the 2026-06-23 run after the name-first profile command edit/CRUD and shell-submode work: `make test` passed with 104 shell tests plus viewer filter and profile-hash vectors. It does not certify the remaining 0.4.0 release-readiness checklist items.
- Any future reviewer who reruns `make test` should update this status with the exact command, date, environment, and result.
