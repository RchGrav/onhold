# Parameterized profiles (design note)

[Future work](README.md) | [Docs index](../index.md) | [Profiles and storage aliases](../profiles-and-aliases.md) | [Security](../security.md)

Status: future-work proposal, not current behavior. This supersedes the earlier
sketch and captures the design as discussed.

## Goal

A general, easy, flexible way to delegate a hash-pinned command behind a profile,
exposing a small set of **optional, validated inputs**, without revealing what is
behind it. "Easy" and "flexible" are the goals; the sudo grant model is the
*envelope* those goals live inside, not the limit of what the system can enforce.

## Core principle (three layers)

- **sudoers = the safe-crossing gate, not the constraint language.** Its only job
  is to make using sudo safe: pin *this* hold binary, `--system --elevated`,
  and *this* profile hash, plus a cheap structural shape of the inputs — so the
  only thing that can cross into root is the exact delegated profile, and a
  root-side bug can only ever be reached with structurally-sane input.
- **profile = the real, expressive enforcement** (first layer, hash-immutable).
  Ranges, `min <= max`, "if `--tls` then `--cert`", path confinement, ordering,
  uniqueness — anything hold can compute. The flexibility ceiling is the
  profile's validator (effectively unbounded), **not** the sudoers regex.
- **the hash = immutability.** It fingerprints the profile, including its base
  argv *and* its input rules, so neither the command nor the allowed-input rules
  can drift without changing the identity the grant is keyed to.

The run record stores only the profile label (currently in the internal `alias` field) that was called — never the hash, target binary, argv, or input values.

## The additive modifier model

The base argv (the NUL-separated list after the program) is **immutable and is the
default**. Parameterizing never edits it. Parameters are **optional, regex-bounded
modifiers tacked on** — a fixed flag plus a constrained value (or a bare optional
flag). Omit everything and you get the exact command the profile was created from,
which by construction is a known-good invocation.

This is fail-safe and additive-only: parameters can only *add* validated trailing
tokens; they can never rewrite the base. It maps cleanly onto optional regex
groups `( ... )?` (omit -> group absent -> base default). Effective "override" of a
base default works when the target honors last-wins for repeated flags; that is a
property of the target CLI, documented, not something hold forces.

Implication: parameters must be expressible as optional trailing flags. Values
hard-coded as positionals in the base cannot be overridden by appending; you
parameterize by creating the base *without* them and letting a modifier add them.

## Two authoring dictionaries

Both are root-owned, used only at authoring time, and **never** part of
enforcement — a mistake in either costs an extra confirmation click, never safety.

### Pattern dictionary — named value shapes

Reusable POSIX-ERE value classes, authored and audited once, referenced by name:

```text
port        = [0-9]{1,5}
date        = [0-9]{4}-[0-9]{2}-[0-9]{2}
time        = ([01][0-9]|2[0-3]):[0-5][0-9]
ident       = [A-Za-z0-9_-]{1,32}
size        = [0-9]+[KMG]?
mode        = (summary|full)
skip        = [sS][kK][iI][pP]
```

Lint rules at load (these are what keep the *gate* safe when a pattern is composed
into a sudoers rule):

- must be valid POSIX ERE (no PCRE-isms; no `\d`, no lazy quantifiers, no
  backreferences);
- **must not match a space** — the space is the slot separator in the single
  joined-argument regex, so a value class that can match it blurs slot boundaries;
- no free-form `.*` / `.+`;
- guard against catastrophic backtracking (cheap ReDoS check);
- `#` must be escaped (`\#`); other sudoers metacharacters need no escaping in
  regex mode.

### Switchionary — named switches and their conventions

A registry of known switches keyed by long and short form, each declaring its
*conventional* behavior and a suggested value pattern (by name, from the pattern
dictionary). It is a **prior for inference**, extensible, shipped with sane
defaults and user-augmentable.

```text
# switch        takes_value  value_pattern   short
--port          yes          port            -p
--host          yes          host            -h
--config        yes          path            -c
--output        yes          path            -o
--file          yes          path            -f
--timeout       yes          duration        -t
--count         yes          uint            -n
--verbose       no           -               -v
--force         no           -               -
--dry-run       no           -               -
```

Distinction: the **pattern dictionary** names value *shapes*; the **switchionary**
names *switches* and points each at the value shape it usually carries. A
switchionary entry composes with a pattern-dictionary entry. Neither is
authoritative — both feed the suggester's default guess.

## CLI-shape suggester (priors + human confirm)

When building a profile from a real invocation, hold tokenizes the command and
proposes a classification. It is a **suggester, not an authority** — it minimizes
clicks; the human is the oracle that resolves anything ambiguous, in the editor.

Confident (assert): `--name=value` and `-x=value` are certainly valued; `--`
begins passthrough; token 0 is the command; a verb may follow.

Priors for the ambiguous `--name value` / `-x value` / `-abc` forms, best signal
first:

1. switchionary says valued/boolean -> use it;
2. `--name` followed by another `-flag`, or at end of args -> boolean;
3. next token is numeric / a path / quoted / contains `=` -> leans value;
4. `--no-*` / `--disable-*` / `--enable-*` -> boolean;
5. otherwise default `--name value` -> valued (the common case).

A wrong guess is a one-click correction, not a correctness or safety problem.
Note: the heavy lifting shrinks dramatically under the additive model — you are
defining *new* optional modifiers, not re-classifying existing tokens, so full
CLI-shape inference is only needed if you ever choose substitution over append.

## The editor (parted/diskpart style)

Authoring a parameterized profile is complex and easy to get wrong, so it is a
focused, stateful session — like editing a partition table:

- **stateful for authoring**: `select <profile>` then `info` / `list` /
  `param <index|--flag> <pattern[,skip]>` / `unparam` operate on the selection;
- **stateless for runtime**: invoking a parameterized profile
  (`hold start <profile> --foo X`) stays a plain one-shot command — it is the hot
  path and scripts call it;
- **explicit commit**: edits stage, then `commit` (a) re-hashes the profile,
  (b) regenerates the sudoers entry and validates it with `visudo -cf` before
  installing, (c) reports the new hash. A half-finished edit never produces a live
  grant;
- **locking**: a profile being edited is locked so two sessions cannot race it.

## Enforcement, end to end

```text
1. user: hold start report --date 2026-06-19 --mode summary
2. hold canonicalizes modifier ORDER and enforces UNIQUENESS (regex cannot),
   and runs semantic/relational checks (ranges, min<=max, path confinement)
   against the hash-pinned profile -- the first, expressive layer.
3. hold composes the argv that crosses sudo:
     sudo -- <abs_hold> --system --elevated report <hash> --date 2026-06-19 --mode summary
4. sudoers matches it against the managed, anchored regex for this profile
   (coarse structural gate; the binary + --elevated + hash + input shape).
5. if admitted, sudo starts root hold.
6. root hold loads the profile by hash, re-validates each value against the
   profile's own rules, appends the validated modifiers to the immutable base
   argv, and execs the target shell-free.
```

So order/uniqueness/semantics are handled by hold *before* the boundary, the
profile is the authority on values, and the sudoers regex stays a tight, simple
backstop — never the place the input language is defined.

## The sudo envelope (sudo 1.9.10+; verified on 1.9.17)

What the *gate* can express, from `man sudoers`:

- regex is opt-in and explicit: an argument spec is a regex only if it starts with
  `^` and ends with `$`; otherwise shell wildcards (which we do not use);
- **POSIX Extended Regular Expressions** (egrep-style), not PCRE;
- the command **path** and the **arguments** are matched **separately**; one regex
  cannot span both;
- the arguments are matched as **one regex over the joined argument string** — so
  the inter-token space is part of the pattern (hence "value classes must not
  match a space");
- regexes are limited to **1024 characters** — budget the composed rule; granting
  many modifiers at once may force splitting across profiles;
- in regex mode only `#` needs escaping;
- whole-pattern case-insensitivity via a leading `(?i)` (not per-token);
- an unspecified argument list means **any** arguments are allowed — the grant must
  always carry the anchored regex.

Optional complementary hardening: sudo's own `Digest_Spec` (`sha256:<digest>
/path`) can pin the hold binary natively, orthogonal to the profile hash.

## What this can and cannot safely express

The sudoers gate is a regular language. On its own it does: literals/subcommands,
enumerations (alternation), bounded length/format (`{n,m}`), optional flags with
defaults, mutual exclusion (alternation), bounded repetition. It does **not** do:
numeric magnitude/ranges, unordered-but-unique option sets, cross-field relations,
or any semantics.

Everything in that second list is exactly what the **profile** enforces, and what
**hold normalizes** (order, uniqueness) before the boundary. So the user-facing
flexibility is set by the profile, not by ERE; the gate stays a cheap structural
backstop.

## Security properties and non-goals

- Two walls: a structural sudoers gate that keeps crossing-into-root safe even if
  root hold has a validation bug, plus the profile's expressive validation.
- Opacity is separate from security: the hash and the public surface hide the
  target command, internal flags, and where inputs go — but the security comes
  from the gate + validation, not from hiding.
- Shell-free throughout: validated values are inserted as discrete argv elements,
  never reconstructed into a command string.
- If a profile resolves to a script, that script joins the trusted computing base:
  it must be root-owned, non-writable by others, hash-verified at run time, and
  written to handle its (validated) inputs without re-introducing a shell hole.
- Non-goals: remote/network delegation, arbitrary positional/order-sensitive CLIs,
  free-form string/path inputs without tight value classes, anything requiring the
  gate to encode semantics.
