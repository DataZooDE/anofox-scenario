---
name: anofox-scenario-reference
description: >
  Supplementary lookup for the anofox_scenario DuckDB extension, loaded alongside a workflow
  skill or when a scenario statement fails. Error message -> cause -> fix table; every
  signature and output column (scenario_create, scenario_list, scenario_diff,
  scenario_diff_summary, scenario_merge_preview, scenario_merge, scenario_freeze,
  scenario_unfreeze, scenario_drop, scenario_refresh, scenario_migrate); settings (telemetry
  opt-out, suppression hash limit, internal writes); which base edits a merge does not
  detect; transactions; ATTACH ... (TYPE scenario) and USE; keyless tables, views, row
  identity; which tables a scenario changed; using a scenario from Python; sharing one with
  a colleague. Not a workflow on its own: plan variants -> anofox-scenario-what-if, dry run
  and merge -> anofox-scenario-safe-change, approved versions -> anofox-scenario-snapshot-audit.
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — Reference & Troubleshooting

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0

Lookup table behind the workflow skills `anofox-scenario-what-if`,
`anofox-scenario-safe-change` and `anofox-scenario-snapshot-audit`. Longer semantics, Python
and sharing: [behaviour.md](behaviour.md). Full specification: `docs/API_REFERENCE.md`.

**Ask first.** Fixes marked *(ask first)* merge into the real tables, drop a scenario,
unfreeze an approved one or pick a conflict winner. Name the target and the consequence and
get the user's yes, unless they already asked for exactly that. Never choose `'ours'` /
`'theirs'` just to make an error go away.

## Errors: cause and fix

Rarer errors (`key_columns`, DuckLake pins, creation options, policy typos): [behaviour.md](behaviour.md), "More errors".

| Error contains | Cause | Fix |
|---|---|---|
| `Scenario '…' not found` (any scenario function) | a typo, or `USE <scenario alias>` is active and the functions search the scenario catalog (also: empty `scenario_list()`, DDL error from `scenario_create`) | check `scenario_list()`; after `USE alias`, `USE <host db>` (`memory` or the file's stem); prefer `alias.t` |
| `Scenario '…' not found in database '…'` (ATTACH) | not in the session's default database | create it, or `USE` the database holding it |
| `DDL operations are not permitted in scenarios` | `CREATE` / `ALTER` / `DROP` on a scenario catalog | change the base schema ("Schema changes" below) |
| `Cannot … table '…': scenario '…' has unmerged changes to it` | base `DROP COLUMN` / `RENAME COLUMN` / column type change / `RENAME TO` / `DROP TABLE` on a table a scenario changed | merge the scenario (or drop it, *ask first*), then change the base; `ADD COLUMN` works anyway |
| `Scenario '…' cannot use table '…'` | the base table changed shape without the extension loaded (another process, plain DuckDB) | for a gained column: `DETACH`, then `ATTACH` the scenario in autocommit; for a dropped/renamed/retyped column: restore it, or drop the scenario *(ask first)* |
| `Cannot modify … with SQL` | SQL write/DDL on `__anofox_scenario` (also `IMPORT DATABASE` / `COPY FROM DATABASE` with scenarios) | use the functions; for restore/repair set `anofox_scenario_allow_internal_writes`, then `RESET` |
| `Scenario '…' already exists` | name taken | pick another name, or `scenario_drop` the old one *(ask first)* |
| `Invalid scenario name` / `exceeds the maximum length of 63 characters` | letters, digits, `_` only, starting with a letter or `_`, ≤ 63 chars | e.g. `price_import` instead of `price-import` |
| `is attached as '…'. DETACH it before dropping` | `scenario_drop` of an attached scenario | `DETACH alias`, then drop *(ask first)* |
| `other scenarios branch from it` | drop or merge of a scenario with branches | drop the branches first *(ask first)*; a merged branch counts until dropped |
| `is frozen. Unfreeze it with CALL scenario_unfreeze` | write to a frozen scenario | branch it with `from_scenario`; or unfreeze *(ask first, log why)* |
| `is frozen. Unfreeze it before merging` | `scenario_merge` of a frozen scenario | unfreeze *(ask first)*, then merge *(ask first)* |
| `was already merged back into the base` | second merge, or unfreeze of a merged scenario (`merged_at` is set in `scenario_list()`) | nothing to do; a new scenario for further changes |
| `the base changed underneath it` | merge conflict under `on_conflict := 'abort'` (overlay or drift, keyed or keyless) | show `scenario_merge_preview(s)` `WHERE conflict`; the user picks `'ours'` / `'theirs'` *(ask first)* or redoes the change over the current base |
| `Violates foreign key constraint` (merge) | a merged row references a parent missing in the base | add the parent to the scenario or base; nothing was applied |
| `is still referenced by a foreign key` (merge) | the scenario deletes a parent row with its children, or changes a referenced key | merge the child changes first in a scenario of their own |
| `the key exists in the base table` / `the key was already inserted or modified in this scenario` | `INSERT` of an existing key | `UPDATE`, `INSERT OR REPLACE` / `ON CONFLICT`, or delete it first |
| `UNIQUE constraint on scenario table` / `NOT NULL constraint failed` / `CHECK constraint failed` | the merged row violates a base constraint | fix the values |
| `modified the same row twice` | one statement updates a row through several source rows | deduplicate the source (`DISTINCT ON`, aggregate) |
| `There are no UNIQUE/PRIMARY KEY constraints that refer to this table` | `ON CONFLICT` / `INSERT OR REPLACE` on a keyless table | `MERGE INTO`, or a PK on the base table |
| `attached in read-only mode` / `transaction is launched in read-only mode` | write through `READ_ONLY` / in `BEGIN TRANSACTION READ ONLY` | re-`ATTACH` without `READ_ONLY` / read-write transaction |
| `this transaction already modified database` | the transaction already wrote another attached database | commit first; host + its scenarios count as one database |
| `Cannot change configuration option` | `SET` after `SET lock_configuration = true` | set options (telemetry opt-out) before locking |

## Functions

Every verb is a table function: `CALL f(...)` and `SELECT * FROM f(...)` both work.

| Call | Returns | Notes |
|---|---|---|
| `scenario_create(name, [description], mode := 'delta'\|'materialized', from_scenario := parent, base := catalog, key_columns := MAP {'t': ['col', ...]})` | `success` | Unbranched delta (default): one registry row. `materialized`: copies every base table. `from_scenario`: a delta over the parent's current state that copies the parent's existing deltas and pre-images, declared keys, base catalog and DuckLake pin. `base`: another attached catalog as base (DuckLake: pinned to creation time). `materialized` excludes `from_scenario` and `base`, which also exclude each other. |
| `scenario_drop(name)` | `success` | Removes deltas, copies, declared keys. Refuses while attached or while branches exist. |
| `scenario_freeze(name)` / `scenario_unfreeze(name)` | `success` | Rejects / allows writes through every handle; reads keep working. A merged scenario stays frozen. |
| `scenario_list()` | `scenario_id, name, mode, frozen, parent, created_at, description, merged_at` | `parent` is the parent's name; `merged_at` is NULL until merged. Run it from the host database, not after `USE alias`. |
| `ATTACH 'name' AS alias (TYPE scenario [, READ_ONLY])` / `DETACH alias` | | The read/write surface: `alias.t`. Per connection; re-attach in every new session. |
| `scenario_diff(scenario, table)` | `<key columns>, change_type, column_name, old_value, new_value` | Scenario vs. its origin. One row per changed column (`modified`) or per `added` / `removed` key; values as text, no values for `removed`. |
| `scenario_diff(a, b, table)` | same | `old` = `a`, `new` = `b`; `'main'` = the base table (for a DuckLake scenario: at its pinned snapshot). |
| `scenario_diff_summary(scenario)` | `table_name, rows_added, rows_modified, rows_removed` | One row per table the scenario wrote: "which tables did it touch". |
| `scenario_merge_preview(scenario)` | `table_name, key, action, conflict` | No side effects. `key` as text; keyless tables list one row per copy, `key` = the whole row. |
| `scenario_merge(scenario, on_conflict := 'abort'\|'ours'\|'theirs')` | `tables_changed, rows_applied, conflicts_resolved` | Applies the delta. On success: frozen, `merged_at` set, delta empty. An empty plan (preview lists no rows) is a no-op: `0, 0, 0`, the scenario stays open and unfrozen. A branch's merge includes its parent's changes. Referenced tables first. Drift exceptions below. |
| `scenario_refresh(name, key_columns := MAP {...})` | `refreshed_tables` | Deprecated; only to declare `key_columns` for a keyless table not written yet. |
| `scenario_migrate()` | `migrated_scenarios, migrated_snapshots` | One-way upgrade of a v0.1 database. |

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `anofox_scenario_telemetry_enabled` | `true` | Anonymous usage telemetry (`extension_loaded`: version, platform). Opt out with `SET anofox_scenario_telemetry_enabled = false` after `LOAD`, **before** `SET lock_configuration = true`. The load event itself fires during `LOAD`: to stop it too, set env `DATAZOO_DISABLE_TELEMETRY=1`. Off in CI. |
| `anofox_scenario_suppression_hash_limit` | `4194304` | Updated/deleted keys per table hashed in memory for scans (~16 bytes each). Above it, scans probe the delta's index: less memory, no parallel scan. `0` = always the index. |
| `anofox_scenario_allow_internal_writes` | `false` | Lets SQL write `__anofox_scenario` (restore, repair). Set it only for that, then `RESET` it. |

## Base edits a merge does not detect

A merge treats a base edit to a row the scenario updated or deleted as a conflict (*drift*),
compared with the materialized copy, the pinned lake snapshot, or (delta scenarios) the row as
it was when the scenario first touched it. Exceptions:

- **Keyless tables of delta scenarios** (no PK, no `key_columns`): a delete whose copies are
  no longer all in the base *is* a conflict (`abort` refuses; `theirs` deletes nothing for that
  value; `ours` deletes the copies that remain). Not flagged: a delete that still finds enough
  copies. Keyless inserts always apply, so with `ours` / `theirs` a changed row can end up in
  both versions.
- **Deltas written by v2026.10.21 or earlier** (before pre-images): a base edit is overwritten.
- **Rows a branch inherited from a materialized parent**: a base edit is overwritten.

Remedy: declare `key_columns`, merge right after the dry run, or redo the change in a fresh
scenario over the current base.

## Transactions (the one place for BEGIN / COMMIT advice)

- Scenario writes, `scenario_create` / `_freeze` / `_unfreeze` / `_drop` and host-base
  `scenario_merge` run in the caller's transaction: `ROLLBACK` undoes them. Host writes and
  any number of scenarios on the same host commit atomically together.
- Wrap a host-base merge in `BEGIN ... COMMIT` to check the base before anyone sees it;
  `ROLLBACK` leaves the scenario mergeable.
- A DuckLake merge needs autocommit (no `BEGIN`): the lake commits on its own and cannot be
  rolled back.
- Writing a *different* attached database in the same transaction is refused.

Example: [behaviour.md](behaviour.md), "Transactions".

## Schema changes

DDL inside a scenario is rejected; change the base instead. New base tables appear in delta
scenarios automatically (materialized ones keep their table set).

- **`ALTER TABLE t ADD COLUMN`** on the base reaches every scenario, attached or not, also
  when it has changed `t`, and in materialized scenarios and branches. Rows the scenario
  changed before show the value the base rows got: the column's `DEFAULT`, NULL without one
  (also NULL for a volatile default such as `nextval(...)`). Scenarios can then write the
  column, and `scenario_diff` / `scenario_merge` include it.
- **`DROP COLUMN`, `RENAME COLUMN`, a column type change, `RENAME TO`, `DROP TABLE`** on a base
  table are refused while any scenario has unmerged changes to it (`… has unmerged changes to
  it`): merge the scenario first. A scenario without changes to the table does not block it.
  Materialized copies keep their old columns.

Example: [behaviour.md](behaviour.md), "Base schema changes".
