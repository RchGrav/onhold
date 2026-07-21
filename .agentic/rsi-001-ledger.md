# rsi-001 — generation ledger (append-only; the experiment's evolution record)

Experiment: can generation N's loop instructions, amended by generation N-1,
steer a fresh agent to a better improvement than a static prompt would?
Target: the Hold On log viewer. Baseline: 146/0 suite, reconciled tree.

Row format — one per generation, appended by the generation's agent:

```
## gen <N> — <commit hash> — <ACCEPTED|REVERTED>
- item: <priority letter + one line>
- changed: <what, concretely>
- evidence: <suite summary line + any new test names>
- unverified: <honest list, or "nothing">
- lesson fed forward: <the one line added to the loop file>
- validator: <verdict + one line, filled by the validator agent>
```
