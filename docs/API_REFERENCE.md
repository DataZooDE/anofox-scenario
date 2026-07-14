# anofox-scenario API Reference

This document describes the SQL API for the anofox-scenario DuckDB extension.

## Scenario Catalogs (v2 surface, recommended)

A scenario is exposed as an **attached catalog**: ordinary SQL reads and writes, no special
functions. (The legacy v0.1 function surface was removed in v0.2 - see `scenario_migrate()`.)

```sql
CALL scenario_create('optimistic', 'demand +10%');   -- description optional

-- Phase 2 options:
CALL scenario_create('baseline', mode := 'materialized');       -- frozen full copy: immune to base changes
CALL scenario_create('variant', from_scenario := 'optimistic'); -- branch: inherits the parent's changes

SELECT * FROM scenario_list();                    -- name, mode, frozen, parent, created_at, description

-- Phase 3: streaming, typed diffs (the delta IS the changelog)
SELECT * FROM scenario_diff('optimistic', 'forecast');  -- <pk cols>, change_type, column_name, old/new_value
SELECT * FROM scenario_diff_summary('optimistic');      -- per-table rows_added/modified/removed

ATTACH 'optimistic' AS opt (TYPE scenario);

SELECT * FROM opt.forecast;                          -- merged view: base + scenario changes
INSERT INTO opt.forecast VALUES (...);               -- transparent copy-on-write
UPDATE opt.forecast SET qty = qty * 1.1 WHERE ...;   -- base is never modified
DELETE FROM opt.forecast WHERE obsolete;
TRUNCATE opt.forecast;

DETACH opt;                                          -- handle gone, scenario data persists

CALL scenario_freeze('optimistic');                  -- reject all writes (reads keep working)
CALL scenario_unfreeze('optimistic');
CALL scenario_drop('optimistic');                    -- refuses while attached
```

**Semantics**

- **Copy-on-write:** modifications are stored per scenario in delta tables under the internal
  `__anofox_scenario` schema of the host database; base tables are never written.
- **Isolation tiers** (all shipped):
  | Tier | How | Isolation from base changes |
  | --- | --- | --- |
  | `overlay` (default) | delta over the live base | none - base churn shows through unless the scenario touched the same PK (documented semantics) |
  | `materialized` | `mode := 'materialized'`: full copy per table | complete |
  | `snapshot` | `base := '<ducklake attach>'`: reads pinned via `AT (TIMESTAMP => created_at)` | complete, O(1) creation - no data copied |
- **Constraints:** base `NOT NULL`, `CHECK`, and PRIMARY KEY constraints are enforced for
  scenario writes against the *merged* state, with distinct errors for conflicts with base rows
  vs. scenario changes.
- **Transactions:** scenario DML runs in the caller's transaction; `ROLLBACK` undoes it.
- **DDL:** not permitted inside scenarios (one canonical error; REQ-COW-007).
- **Host database:** the scenario registry lives in the session's *default* database at
  `scenario_create` / `ATTACH` time.

**Remaining limitations** (clean errors; everything else from the v1 list has shipped)

| Limitation | Notes |
| --- | --- |
| Host writes and scenario writes in the *same explicit transaction* | single-writer rule; documented restriction |
| Views with *qualified* table references keep reading their explicit target (unqualified references rebind to scenario tables) | documented semantics |
| `MERGE INTO` / `ON CONFLICT` on keyless tables (no PK, no `key_columns`) | UPDATE/DELETE work (bag semantics); declare `key_columns :=` for keyed matching |
| Secondary `UNIQUE` constraints on *keyless* tables are not enforced between scenario rows (keyed tables enforce them fully) | rare combination; documented |
| `scenario_diff` on keyless tables | requires a PK or `key_columns :=` |
| DuckLake merge-back: lake apply commits in its own transaction (autocommit required, no cross-catalog 2PC); keyless lake deletes are refused | documented protocol |
| Scenarios created before v0.4.1 keep the old keyless gate (their deltas lack `_count`) | recreate the scenario |
| Unique values *vacated* by scenario deletes/updates are conservatively still treated as taken when a scenario write reuses them | rejected with a clean error |
| Table or schema names containing `.` | rejected at `scenario_create` (naming-contract separator) |

## Function Reference

| Function | Kind | Description |
| --- | --- | --- |
| `CALL scenario_create(name, [description], [mode := 'delta'\|'materialized'], [from_scenario := parent], [base := catalog], [key_columns := MAP {'table': ['col', ...]}])` | verb | Register a scenario. `materialized` copies every base table (full isolation); `from_scenario` branches, inheriting the parent's changes; `base` uses another attached catalog's tables as the base (DuckLake bases are pinned to creation time - true snapshot isolation); `key_columns` declares row identity for tables without a PRIMARY KEY, unlocking UPDATE/DELETE/MERGE on them. O(#tables) metadata for delta mode. |
| `CALL scenario_drop(name)` | verb | Remove the scenario and its delta/materialized tables. Refuses while attached or while branches exist. |
| `CALL scenario_freeze(name)` / `scenario_unfreeze(name)` | verb | Reject/allow writes through any attached handle (reads keep working). A frozen materialized scenario is a snapshot. |
| `SELECT * FROM scenario_list()` | table | `scenario_id, name, mode, frozen, parent, created_at, description`. |
| `SELECT * FROM scenario_diff(scenario, table)` | table | Compare to origin: `<pk cols>` (native types), `change_type` (`added`/`removed`/`modified`), `column_name`, `old_value`, `new_value`. Streams through the engine. |
| `SELECT * FROM scenario_diff(a, b, table)` | table | Generic diff between two merged relations (`'main'` or any scenario). `old` = side a, `new` = side b. |
| `SELECT * FROM scenario_diff_summary(scenario)` | table | Per-table `rows_added / rows_modified / rows_removed` from the delta changelog. |
| `SELECT * FROM scenario_merge_preview(scenario)` | table | Planned merge-back actions: `table_name, key, action, conflict`. Streaming; no side effects. Overlay conflicts: an `insert` whose key now exists in base, an `update` whose key vanished. Scenarios with a creation snapshot (materialized, DuckLake) additionally flag 3-way *drift* conflicts: the base row changed since the snapshot on a touched key. |
| `SELECT * FROM scenario_merge(scenario, [on_conflict := 'abort'\|'ours'\|'theirs'])` | verb | Apply the scenario's delta to the base. `abort` (default) throws on any conflict; `ours` = scenario wins; `theirs` = base wins. Host bases apply atomically in the caller's transaction; DuckLake bases apply atomically on the lake side in their own transaction (autocommit required). On success the scenario ends `frozen` with `merged_at` set and an empty delta. Refuses while branches exist. |
| `SELECT * FROM scenario_refresh(name, [key_columns := MAP {...}])` | verb | Create delta tables for base tables added after `scenario_create`, making them writable. `key_columns` declares identity for new keyless tables (rejected once a table is already tracked). Returns `refreshed_tables`. Idempotent; refuses materialized scenarios. |
| `SELECT * FROM scenario_migrate()` | verb | One-way migration of a legacy v0.1 database (`_scenario_registry`, `_scen_*`, `_snap_*`) into the v2 layout. Archived -> frozen; snapshots -> materialized+frozen; multi-op delta rows folded to net effects; `_scenario_base_rowids` dropped; `_scenario_protocols` preserved. |
| `ATTACH 'name' AS alias (TYPE scenario)` / `DETACH alias` | SQL | The entire read/write UX. |

## Internal Layout

All state lives in the host database (single-file portability):

```
__anofox_scenario.registry              -- scenario_id, name, mode, frozen, parent_id,
                                        --   base_snapshot_id (v0.3), created_at, merged_at (v0.5), description
__anofox_scenario.registry_seq          -- id sequence
__anofox_scenario.s<id>_delta_<table>   -- (_op 'I'|'U'|'D', _ts, <base columns>) PK = base PK
                                        --   or the declared key_columns; keyless tables append
                                        --   _count BIGINT (bag changelog, no PK) instead
__anofox_scenario.s<id>_mat_<table>     -- materialized base copies (mode = 'materialized')
```

Non-main schemas use `<schema>.<table>` as the `<table>` part of the naming contract
(`s3_delta_analytics.events`); `key_columns :=` accepts the same qualified names.

The delta table is a stable changelog contract: `scenario_diff` reads it, branching copies it,
and merge-back (v0.5) will replay it.

## Protocols / Audit Notes

Protocol notes are plain SQL over a plain table - no dedicated API. Recommended recipe:

```sql
CREATE TABLE IF NOT EXISTS _scenario_protocols (
    entity_type VARCHAR NOT NULL, entity_name VARCHAR NOT NULL, section VARCHAR NOT NULL,
    content VARCHAR, updated_at TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (entity_type, entity_name, section));

INSERT OR REPLACE INTO _scenario_protocols VALUES
    ('scenario', 'optimistic', 'why', 'testing demand +10% for the Q3 review', current_timestamp);

-- export to markdown
COPY (SELECT '## ' || entity_name || chr(10) || content FROM _scenario_protocols ORDER BY entity_name)
TO 'protocols.md' (FORMAT csv, QUOTE '', HEADER false);
```

(Databases migrated from v0.1 keep their existing `_scenario_protocols` rows unchanged.)
