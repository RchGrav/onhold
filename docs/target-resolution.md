# Target resolution

[Docs index](index.md) | [Quickstart](quickstart.md) | [Previous: Identity](identity.md) | [Next: Profiles and aliases](profiles-and-aliases.md) | Related: [Security](security.md), [CLI contract](cli-contract.md)

Outer loop bridge: this is the deep dive for [Step 4: Make Targeting Deterministic](quickstart.md#step-4-make-targeting-deterministic).

Target resolution answers one user-facing question: when you type `sigmund stop web`, `sigmund tail 7f3`, or `sigmund dump system:api`, which concrete run did you mean? It does not decide whether the process is safe to signal; that is the identity validator's job.

The resolver is split because Sigmund has several addressing forms and two authority contexts. `resolve_target` is used for alias creation, while `resolve_action_token` is used for action commands that may expand one alias into multiple concrete targets.

## Accepted target forms

Targets for actions may be:

- A full 8-character run ID.
- A leading run ID prefix.
- An alias name.
- `user:<target>` to force user-local lookup.
- `system:<target>` to force system-managed lookup.

`valid_id`, `valid_id_prefix`, and `valid_alias` enforce the lexical rules. Aliases are 1 to 64 characters, use alphanumeric characters plus `_` and `-`, and cannot be full profile hashes.

## Non-root plain resolution

```mermaid
flowchart TD
    Token["Plain target"] --> UserId["User-local ID or prefix?"]
    UserId -->|yes| UserTarget["Use user-local run"]
    UserId -->|no| UserAlias["User-local alias match?"]
    UserAlias -->|yes| UserAliasTarget["Use matching user run or runs"]
    UserAlias -->|no| PublicId["Root public ID or prefix?"]
    PublicId -->|yes| ElevateId["Use system run via sudo"]
    PublicId -->|no| PublicAlias["Root public alias match?"]
    PublicAlias -->|yes| ElevateAlias["Use alias capability via sudo"]
    PublicAlias -->|no| NotFound["Not found"]

    classDef user fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef root fill:#ede9fe,stroke:#6d28d9,color:#3b0764
    classDef safe fill:#dcfce7,stroke:#15803d,color:#14532d
    classDef miss fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    class Token,UserId,UserAlias user
    class PublicId,PublicAlias,ElevateId,ElevateAlias root
    class UserTarget,UserAliasTarget safe
    class NotFound miss
```

For normal users, user-local matches win over root-public matches. This is deliberate: a local token should not unexpectedly cross a privilege boundary merely because the same prefix or alias is visible in the system public index.

`user:<target>` disables system lookup. `system:<target>` disables user lookup and may require sudo self-elevation.

## Root and sudo resolution

```mermaid
flowchart TD
    Token["Plain target as root"] --> RootMatch["System private match?"]
    RootMatch -->|yes| RootTarget["Use system run"]
    RootMatch -->|no| SudoCtx["Sudo provenance available?"]
    SudoCtx -->|no| NotFound["Not found"]
    SudoCtx -->|yes| UserMatch["Invoking-user match?"]
    UserMatch -->|yes| UserTarget["Use invoking-user run"]
    UserMatch -->|no| NotFound

    classDef root fill:#ede9fe,stroke:#6d28d9,color:#3b0764
    classDef user fill:#e0f2fe,stroke:#0369a1,color:#0c4a6e
    classDef miss fill:#fee2e2,stroke:#b91c1c,color:#7f1d1d
    class Token,RootMatch,RootTarget root
    class SudoCtx,UserMatch,UserTarget user
    class NotFound miss
```

Root reads private system records directly. When root was reached through sudo and the token does not match a root-managed run, Sigmund can resolve against the invoking user's local store. Direct root without sudo provenance has no invoking user context and cannot resolve `user:<target>`.

## Alias intent

Aliases are filtered by command because the same alias label can be attached to several past or current runs.

| Command | Alias candidates |
| --- | --- |
| `start` | Running alias-labeled runs, used to enforce the no-duplicate default. |
| `stop` | Running alias-labeled runs. |
| `kill` | Running alias-labeled runs. |
| `tail` | Running alias-labeled runs. |
| `console` | Running alias-labeled runs that have `console_sock`. |
| `dump` | Alias-labeled runs that have logs. |
| `prune` | Alias-labeled past runs that are exited, failed, or stale. |

`record_matches_alias_intent` is the source of this table. If a known alias has no applicable candidate for an action, Sigmund treats that as a successful no-op. If an alias has multiple candidates, it exits 6 and prints candidates unless the command supports `--all` and `--all` was supplied. `--all` applies only to `stop`, `kill`, and `prune`.

## Public alias capabilities

A normal user cannot read root-private records, so root-managed alias actions begin with public data. `append_public_alias_elevation_target` uses public alias metadata and public index files to build either:

- a concrete run selector plus alias/hash capability, or
- the `ffffffff` selector for approved multi-target `--all` actions.

Root Sigmund later verifies that the alias still maps to the supplied hash and that concrete run selectors are recorded under that alias. The public side selects intent; the root side rechecks authority.

## Why this design works

The resolver is conservative about authority. It avoids surprising privilege escalation, returns concrete store/run pairs before acting, and lets root re-validate capability data after crossing sudo. That supports the validate-before-signal model because signal code receives a resolved private record path, not an ambiguous user token.

The daemonless constraint also shapes ambiguity behavior. Without a daemon to arbitrate a "current" alias run, Sigmund must either identify one candidate, apply an explicit `--all`, or refuse with candidates.

## Implementation map

For maintainers, the primary functions and structs are `parse_id_token`, `valid_target_atom`, `resolve_target`, `resolve_action_token`, `append_private_alias_targets`, `append_public_alias_elevation_target`, `collect_private_alias_matches`, `collect_public_alias_matches`, `record_matches_alias_intent`, `report_alias_ambiguity`, and `struct resolved_target`.

## Continue

[Back to Step 4](quickstart.md#step-4-make-targeting-deterministic) | [Back to docs index](index.md) | [Top](#target-resolution) | [Next: Profiles and aliases](profiles-and-aliases.md) | Branch to: [Security](security.md), [CLI contract](cli-contract.md)
