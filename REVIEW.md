# Review status

Date: 2026-06-23
Branch: `mund-0.4.0-redesign`
Scope: release-readiness notes for the 0.4.0 documentation, branch alignment, and hardening plan.

## Current status

This file replaces an older review report whose test counts and file references described a previous implementation review. Treat this document as a status tracker, not proof that the current branch is release-ready. The current branch version file remains `0.3.9`; 0.4.0 wording is release-plan status unless backed by fresh verification.

0.4.0 is release-ready only after both tracks are complete:

1. the full `mund` CLI/product direction in `docs/MUND_0_4_UX_SPEC.md` is implemented and documented; and
2. the hardening backlog in that same spec is implemented, tested, and reviewed.

## Latest alignment pass

Date: 2026-06-23

The Autopilot spec-alignment pass added [0.4.0 branch alignment](docs/0.4.0-alignment.md) as the tracked matrix for implemented behavior, accepted v1 deviations, and follow-up gaps. It also labels legacy/current docs so they do not conflict with the 0.4.0 `mund` direction. This pass was docs-only and does not certify runtime release readiness.

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
files = [Path(p) for p in ['README.md','docs/install.md','docs/MUND_0_4_UX_SPEC.md','REVIEW.md','docs/index.md']]
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
  README.md docs/install.md docs/MUND_0_4_UX_SPEC.md REVIEW.md

python3 - <<'PY'
from pathlib import Path
import re, sys
patterns = [
    re.compile("26" + "/26"),
    re.compile("Result:" + r" .*" + "tests " + "passed"),
    re.compile("make clean && " + "make test"),
    re.compile("UBSan build " + "passed"),
]
files = [Path(p) for p in ['REVIEW.md','docs/MUND_0_4_UX_SPEC.md','README.md','docs/install.md']]
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
  docs/MUND_0_4_UX_SPEC.md docs/index.md
```

Result: local links in touched files resolved, static-artifact caveats were present in the expected docs, stale historical pass claims were absent, and 0.4.0 scope/alignment language was present.

Latest implementation pass: `make test` was run on this branch after adding name-first profile command editing/CRUD and passed (103 shell tests, viewer filter engine, and profile-hash vectors). Earlier docs-only review notes above remain link/grep evidence only and should not be reused as runtime proof.

## Release-readiness checklist

- [x] `make test` completes successfully on this branch for the 2026-06-23 Autopilot alignment pass.
- [ ] CLI grammar, help text, parser behavior, and documentation agree for the 0.4.0 `mund` surface.
- [ ] GNU static, GNU dynamic, and musl static install behavior is documented without overstating glibc static portability.
- [ ] Console attach authorization is peer-credential checked before replaying output or forwarding input.
- [ ] Signal safety refuses uncertain records instead of relying on display-oriented liveness.
- [ ] Store directory creation and atomic writers reject symlink/temp-file attacks.
- [ ] Release and installer scripts fail closed, validate checksums/layouts, and avoid destructive release deletion.
- [ ] Source archive builds report the `VERSION` file value without requiring a Git checkout.

## Current caveats

- This branch now includes both documentation alignment and initial profile-editing implementation slices.
- The implementation test evidence in this file is limited to the 2026-06-23 run after the name-first profile command edit/CRUD work: `make test` passed with 103 shell tests plus viewer filter and profile-hash vectors. It does not certify the remaining 0.4.0 release-readiness checklist items.
- Any future reviewer who reruns `make test` should update this status with the exact command, date, environment, and result.
