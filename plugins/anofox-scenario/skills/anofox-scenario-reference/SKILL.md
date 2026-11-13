---
name: anofox-scenario-reference
description: >
  Reference for the anofox_scenario DuckDB extension: every function signature and result
  column, how scenario tables behave (row identity, keyless bag semantics, merged-state
  constraints, DDL rejection, transactions, concurrency, read-only and frozen checks,
  __scenario_* virtual columns, table-name quoting, settings), and an error message ->
  cause -> fix table. Use when a scenario statement fails with an error, when the exact
  signature or output columns of scenario_create / scenario_drop / scenario_freeze /
  scenario_unfreeze / scenario_list / scenario_diff / scenario_diff_summary /
  scenario_merge_preview / scenario_merge / scenario_refresh / scenario_migrate are
  needed, or when a question is about edge-case semantics rather than a workflow.
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — Reference & Troubleshooting

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0

Workflows live in `anofox-scenario-what-if`, `anofox-scenario-safe-change` and
`anofox-scenario-snapshot-audit`. This page is the lookup table behind them. The complete
specification is `docs/API_REFERENCE.md` in the repository.

## Functions

Every verb is a table function: `CALL f(...)` and `SELECT * FROM f(...)` both work.

| Call | Returns | Notes |
|---|---|---|
| `scenario_create(name, [description], mode := 'delta'\|'materialized', from_scenario := parent, base := catalog, key_columns := MAP {'table': ['col', ...]})` | `success` | Name: letters, digits and `_` only, starting with a letter or `_`, at most 63 characters. `delta` (default) is O(1). `materialized` copies every base table. `from_scenario` branches: a delta scenario over the parent's current state that inherits its changes, declared keys, base catalog and DuckLake snapshot. `base` uses another attached catalog as the base (DuckLake: pinned to creation time). `from_scenario` and `base` each exclude `mode := 'materialized'`, and each other. `key_columns` gives keyless tables a row identity. |
| `scenario_drop(name)` | `success` | Removes deltas, copies and declared keys. Refuses while attached or while branches exist. |
| `scenario_freeze(name)` / `scenario_unfreeze(name)` | `success` | Rejects / allows writes through every scenario handle; reads keep working. A merged scenario cannot be unfrozen. Freeze does not stop direct host SQL on the internal `__anofox_scenario` tables (issue #55). |
| `scenario_list()` | `scenario_id, name, mode, frozen, parent, created_at, description` | `parent` is the parent's name. |
| `ATTACH 'name' AS alias (TYPE scenario [, READ_ONLY])` / `DETACH alias` | | The whole read/write surface. `USE alias` makes unqualified table names resolve to the scenario. |
| `scenario_diff(scenario, table)` | `<key columns>, change_type, column_name, old_value, new_value` | Scenario vs. its origin. Key columns keep their types; values are VARCHAR. `change_type`: `added` / `removed` / `modified` (one row per changed column). |
| `scenario_diff(a, b, table)` | same | Any two sides; `'main'` = the base table. `old` = `a`, `new` = `b`. When either side is a scenario over a DuckLake base, `'main'` is the lake table *at that scenario's snapshot*, not the live lake. |
| `scenario_diff_summary(scenario)` | `table_name, rows_added, rows_modified, rows_removed` | One row per table the scenario has written. |
| `scenario_merge_preview(scenario)` | `table_name, key, action, conflict` | No side effects. `action`: `insert` / `update` / `delete`; `key` as text. |
| `scenario_merge(scenario, on_conflict := 'abort'\|'ours'\|'theirs')` | `tables_changed, rows_applied, conflicts_resolved` | Applies the delta to the base; the scenario ends frozen, `merged_at` set, delta empty. Works while attached. A branch's merge includes its parent's changes. A base edit to a row the scenario updated or deleted is a conflict (drift), in every mode; delta scenarios compare with the row as it was at their first touch (keyed tables). Referenced tables are applied first. |
| `scenario_refresh(name, key_columns := MAP {...})` | `refreshed_tables` | **Deprecated.** Only still needed to declare `key_columns` for a keyless table the scenario has not written yet. |
| `scenario_migrate()` | `migrated_scenarios, migrated_snapshots` | One-way upgrade of a v0.1 database. |

Setting: `anofox_scenario_suppression_hash_limit` (default `4194304`): the most updated or
deleted keys per table kept in an in-memory hash for scans. Above it scans look keys up in the
delta's index instead (less memory, no parallel scan). `0` always uses the index.

## How scenario tables behave

**Storage.** Everything lives in the host database (the session's default database at
`scenario_create` / `ATTACH` time) under the `__anofox_scenario` schema: the registry, one delta
table per (scenario, table) created by that table's first write, and for materialized
scenarios a copy per table. Copying the `.duckdb` file copies its scenarios.

**Reads.** A scenario table is the base table with the scenario's inserts, updates and
deletes applied (delta mode reads the live base; materialized reads its copy; DuckLake reads
the pinned snapshot). Tables added to the base later are readable and writable in delta
scenarios without any extra step.

**Row identity** is fixed by a table's first write in the scenario: the declared
`key_columns`, else the base PRIMARY KEY, else none (keyless).

```sql
CREATE TABLE events (event_id INTEGER, kind VARCHAR, amount INTEGER);   -- no PRIMARY KEY
INSERT INTO events VALUES (1, 'sale', 10), (2, 'sale', 10), (3, 'refund', -5);
CALL scenario_create('keyed', key_columns := MAP {'events': ['event_id']});
```

**Keyless tables** (no PK, no `key_columns`) behave like a bag of rows, as in plain DuckDB:
`UPDATE` / `DELETE` / `MERGE` act on every matching copy, `scenario_diff` uses all columns as
the identity (a changed row is `removed` + `added`, duplicates appear once per copy), and the
merge applies net copy counts per distinct row.

**Constraints.** The base's `NOT NULL`, `CHECK`, `PRIMARY KEY` and `UNIQUE` constraints are
checked against the scenario's merged state, with different messages for collisions with base
rows and with the scenario's own rows. A unique value whose base row the scenario deleted or
changed is free again. `FOREIGN KEY`s are not checked inside a scenario; `scenario_merge`
checks them against the base and rolls back as a whole on a violation.

**DDL** (`CREATE` / `ALTER` / `DROP` on a scenario catalog) is always rejected. Change the base.

**Repeated matches in one statement.** If an `UPDATE ... FROM`, `MERGE` or similar matches the
same target row several times, repeated *deletes* count once (as in DuckDB), but a second
*update* of the same row fails with `modified the same row twice` and the statement leaves
no trace. Plain DuckDB silently picks one match. Deduplicate the source first.

**Transactions.** Scenario writes run in the caller's transaction; `ROLLBACK` undoes them,
including a table's first write. Host writes and writes to any number of scenarios on the same
host may share one transaction and commit atomically:

```sql
CREATE TABLE orders (id INTEGER PRIMARY KEY, status VARCHAR);
INSERT INTO orders VALUES (1, 'open');
CALL scenario_create('ops');
ATTACH 'ops' AS ops (TYPE scenario);

BEGIN;
INSERT INTO orders VALUES (2, 'open');           -- host
UPDATE ops.orders SET status = 'closed';         -- scenario, sees row 2
COMMIT;
SELECT * FROM ops.orders ORDER BY id;
```

Writing to a *different* attached database in the same transaction is refused (DuckDB's
one-writer-per-transaction rule). Writes in a `READ ONLY` transaction, through a `READ_ONLY`
attachment or to a frozen scenario fail when the statement executes, even if it would change
zero rows; a statement prepared in such a state runs once the executing transaction allows it.

**Concurrency.** A transaction reads its own snapshot of a scenario. Two transactions changing
the same keys get DuckDB's normal write-write conflict. Two transactions making the *first*
write to the same scenario table at once: the second gets a retryable `TransactionException`;
retry after the first commits.

**Virtual columns.** Scenario tables expose `__scenario_origin` (0 = base row, 1 = scenario
row), `__scenario_key_<k>` and, on keyless tables, `__scenario_rowno`. They can be selected and
filtered on but are not stable identifiers (`__scenario_rowno` is renumbered by every scan).

```sql
SELECT __scenario_origin, * FROM ops.orders ORDER BY id;
```

**Table names as strings** (`scenario_diff`'s table argument, `key_columns` keys,
`table_name` output columns): `t` and `main.t` are the same; other schemas are `schema.table`;
a name part containing a dot is double-quoted: `'"a.b"'` is the main table `a.b`.

## Errors: cause and fix

| Error contains | Cause | Fix |
|---|---|---|
| `DDL operations are not permitted in scenarios` | `CREATE` / `ALTER` / `DROP` against a scenario catalog | change the base schema; new base tables appear in delta scenarios automatically |
| `Scenario '…' not found in database '…'` (ATTACH) | no scenario with that name in the session's default database | `CALL scenario_create('…')` first, or `USE` the database that holds it |
| `Scenario '…' already exists` | name taken | pick another name or `scenario_drop` the old one |
| `Invalid scenario name` | only letters, digits and `_`, starting with a letter or `_` (`price-import` fails) | rename, e.g. `price_import` |
| `exceeds the maximum length of 63 characters` | scenario name too long | shorten it |
| `is attached as '…'. DETACH it before dropping` | `scenario_drop` on an attached scenario | `DETACH alias` first |
| `other scenarios branch from it` | drop or merge of a scenario with branches | drop the branches first; a merged branch still counts until it is dropped |
| `is frozen. Unfreeze it with CALL scenario_unfreeze` | write to a frozen scenario | `scenario_unfreeze` (record why), or branch with `from_scenario` |
| `is frozen. Unfreeze it before merging` | `scenario_merge` on a frozen scenario | unfreeze, then merge |
| `was already merged back into the base` | second `scenario_merge`, or `scenario_unfreeze` of a merged scenario | nothing to do; create a new scenario for further changes |
| `the base changed underneath it` | merge conflict with `on_conflict := 'abort'` | inspect `scenario_merge_preview`, then rerun with `'ours'` or `'theirs'` |
| `Violates foreign key constraint` (at merge) | a merged row references a parent missing in the base (and not added by the scenario) | add the parent row to the scenario or the base, then merge again; nothing was applied |
| `is still referenced by a foreign key` (at merge) | the scenario deletes a referenced parent row together with its children (DuckDB refuses that in one transaction), or changes a referenced row's key or another indexed column | merge the child changes first in a scenario of their own, then the parent's |
| `the key exists in the base table` | INSERT of a key the base already has | `UPDATE`, `INSERT OR REPLACE` / `ON CONFLICT`, or delete it in the scenario first |
| `the key was already inserted or modified in this scenario` | INSERT of a key the scenario already wrote | same as above |
| `UNIQUE constraint on scenario table` | duplicate unique value vs. base or scenario rows | as for keys |
| `NOT NULL constraint failed` / `CHECK constraint failed` | merged row violates the base's constraint | fix the value |
| `modified the same row twice` | one statement updates the same target row through several source rows | deduplicate the source (`DISTINCT ON`, aggregate) |
| `There are no UNIQUE/PRIMARY KEY constraints that refer to this table` | `ON CONFLICT` / `INSERT OR REPLACE` on a table without a unique target | add a `UNIQUE` / PK to the base table, or use `MERGE INTO` |
| `attached in read-only mode` | write through a `READ_ONLY` attachment | re-`ATTACH` without `READ_ONLY` |
| `transaction is launched in read-only mode` | write inside `BEGIN TRANSACTION READ ONLY` | use a read-write transaction |
| `this transaction already modified database` | the transaction already wrote another attached database | commit that first; one transaction writes one database (host + its scenarios count as one) |
| `concurrently made the first write to table` | two transactions created the same delta at once | `ROLLBACK` and retry |
| `key_columns declared for table '…', but it already has a PRIMARY KEY` | the base PK is the identity already | drop that entry from `key_columns` |
| `key_columns declared for unknown table` / `column '…' does not exist` / `is listed twice` | typo in `key_columns` | fix the map (table names as above) |
| `declared key column '…' no longer exists in the base table` | key column renamed or dropped in the base before the table's first write | re-declare with `scenario_refresh(name, key_columns := ...)` |
| `the table is not in the scenario's base snapshot` | DuckLake table created after the scenario's snapshot | create a new scenario over the lake's current state |
| `changed after the scenario's base snapshot` | DuckLake table's columns changed after the snapshot | same |
| `Base catalog '…' is not attached` | `base :=` names a catalog that is not attached | `ATTACH` it first |
| `Table '…' not found in scenario '…'` | `scenario_diff` table argument not in the base | check the name / schema quoting |
| `Invalid scenario mode` | `mode :=` other than `'delta'` / `'materialized'` | use one of those |
| `Branching (from_scenario) and mode := 'materialized' cannot be combined` | a branch is always a delta over its parent | branch without `mode`, or materialize from the base |
| `cannot be combined with materialized mode or branching` | `base :=` with `mode := 'materialized'` or `from_scenario` | for a DuckLake base the snapshot pin already isolates; drop `mode` |
| `Cannot branch from scenario '…': not found` | `from_scenario` names a missing scenario | check `scenario_list()` |
