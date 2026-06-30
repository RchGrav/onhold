# Review status

Date: 2026-06-29
Branch: `mund-0.4.0-redesign`
Scope: release-readiness notes for the 0.4.0 documentation, branch alignment, and hardening plan.

## Current status

This file replaces an older review report whose test counts and file references described a previous implementation review. Treat this document as a status tracker, not proof that the current branch is release-ready. The current branch version is `0.4.0` from the canonical `VERSION` file; release-readiness wording still requires fresh verification evidence.

0.4.0 is release-ready only after both tracks are complete:

1. the full `hold` CLI/product direction in `docs/HOLD_0_4_UX_SPEC.md` is implemented and documented; and
2. the hardening backlog in that same spec is implemented, tested, and reviewed.

## Latest release-cut planning pass

Date: 2026-06-29

The current planning pass added [0.4 release cut and 0.4.x backlog](docs/0.4-release-cut.md) and [Dynamic log viewer design before 0.5](docs/future/viewer-fixes-before-0.5.md). These documents capture the completed 0.4 implementation slice, the proposed 0.4.0 cut line, and near-future pre-0.5 viewer work. The viewer redesign is intentionally documented for later instead of expanding the current oversized 0.4.0 implementation commit.

Current focused verification from this pass:

```sh
make -C /home/rich/sigmund hold-dynamic -j2
make -C /home/rich/sigmund hold -j2
make -C /home/rich/sigmund test-040
make -C /home/rich/sigmund viewer-filter-test
make -C /home/rich/sigmund hash-vector
bash -n /home/rich/sigmund/tests/test_hold.sh
bash -n /home/rich/sigmund/tests/test_040_dockerish.sh
git -C /home/rich/sigmund diff --check
```

Result: all focused commands above passed. The static build emitted the known glibc `getpwuid` static-link warning.

Historical full legacy-suite verification from the same branch, before the stale-test/focused-fix pass:

```sh
HOLD_TEST_TIMEOUT=20 make -C /home/rich/sigmund test
# summary: 99 passed, 58 failed, 0 skipped
```

Those failures were triaged into stale legacy assertions, real product fixes, and release-script test harness issues. The later full-suite pass below supersedes this historical failure count.

Latest viewer-design documentation pass:

```sh
make -C /home/rich/sigmund hold-dynamic -j2
make -C /home/rich/sigmund viewer-filter-test
make -C /home/rich/sigmund test-040
bash -n /home/rich/sigmund/tests/test_hold.sh
git -C /home/rich/sigmund diff --check
# plus local markdown-link validation for the touched docs
```

Result: all commands above passed. This pass documented the before-0.5 viewer design and deliberately left the redesign out of the 0.4.0 implementation cut.

Latest full-suite verification pass:

```sh
HOLD_TEST_TIMEOUT=25 make -C /home/rich/sigmund test 2>&1 | tee /home/rich/sigmund/.omx/logs/full-verify-make-test-20260629-rerun4.log
git -C /home/rich/sigmund diff --check
make -B -C /home/rich/sigmund hold hold-dynamic -j2
```

Result:

- `tests/test_hold.sh`: `157 passed, 0 failed, 0 skipped`
- `viewer-filter-test`: passed
- `hash-vector`: `all 3 golden + 3 collision checks pass`
- `tests/test_version_makefile.sh`: passed inside `make test` after suppressing inherited make directory chatter
- `tests/test_release_installer.sh`: passed inside `make test`
- `git diff --check`: passed
- release rebuild restored `hold` as statically linked and `hold-dynamic` as dynamically linked; the static link emitted the known glibc `getpwuid` warning.

Recommended next implementation tasks, in order:

1. **Viewer freeze for 0.4.0:** keep the redesigned dynamic viewer as documented near-future work; do not add more viewer implementation to the current 0.4.0 cut.
2. **Release cut cleanup:** review and split the oversized commit into coherent slices before tagging.
3. **Grant/security gate:** either complete trusted global-profile grant checks or hide/label grant behavior as not supported in the 0.4.0 public surface.
4. **Public docs/changelog:** draft the 0.4.0 changelog and migration notes now that the release test gate is green.
5. **Before 0.5 viewer branch:** implement the documented sidecar-index/dynamic-viewer redesign as a separate branch/commit series.

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


## Release-readiness checklist

- [x] `make test` or an explicit 0.4.0 release suite completes successfully on the current branch. Latest full run on 2026-06-29: `157 passed, 0 failed, 0 skipped`, plus viewer/hash/version/release-installer targets passed.
- [ ] CLI grammar, help text, parser behavior, and documentation agree for the 0.4.0 `hold` surface.
- [ ] GNU static, GNU dynamic, and musl static install behavior is documented without overstating glibc static portability.
- [ ] Console attach authorization is peer-credential checked before replaying output or forwarding input.
- [ ] Signal safety refuses uncertain records instead of relying on display-oriented liveness.
- [ ] Store directory creation and atomic writers reject symlink/temp-file attacks.
- [ ] Release and installer scripts fail closed, validate checksums/layouts, and avoid destructive release deletion.
- [ ] Source archive builds report the `VERSION` file value without requiring a Git checkout.

## Current caveats

- This branch now includes a substantial 0.4 implementation slice plus release-cut planning docs.
- Full `make test` passed on 2026-06-29 after stale assertions and focused product bugs were corrected.
- The before-0.5 dynamic log viewer redesign remains intentionally unimplemented in this 0.4.0 cut; use `docs/future/viewer-fixes-before-0.5.md` as the source of truth.
- Grant/global-profile hardening remains the main non-viewer caveat before declaring broader security completeness.
- Any future reviewer who reruns `make test` should update this status with the exact command, date, environment, and result.
