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
- **Overlay reads:** a scenario sees the *live* base plus its own changes. Base rows
  inserted/updated after scenario creation show through unless the scenario modified the same
  primary key. (Point-in-time isolation: `mode := 'materialized'` in v0.2, DuckLake bases in v0.3.)
- **Constraints:** base `NOT NULL`, `CHECK`, and PRIMARY KEY constraints are enforced for
  scenario writes against the *merged* state, with distinct errors for conflicts with base rows
  vs. scenario changes.
- **Transactions:** scenario DML runs in the caller's transaction; `ROLLBACK` undoes it.
- **DDL:** not permitted inside scenarios (one canonical error; REQ-COW-007).
- **Host database:** the scenario registry lives in the session's *default* database at
  `scenario_create` / `ATTACH` time.

**v1 limitations** (clean errors; roadmap in `docs/spec/implementation_plan_master.md`)

| Limitation | Planned |
| --- | --- |
| `ON CONFLICT` / `INSERT OR REPLACE`, `RETURNING`, `MERGE INTO`, PK-column updates | v0.4 |
| `UPDATE`/`DELETE` on tables without a PRIMARY KEY (insert/read work) | v0.4+ |
| Secondary `UNIQUE` constraints: enforced against base rows, but not between scenario-written rows (PK is fully enforced) | v0.4 |
| Tables created in the base *after* `scenario_create` are read-only in the scenario | v0.2 |
| Host writes and scenario writes in the *same explicit transaction* | documented restriction |
| Views are not exposed inside scenarios | v0.2 |

## Function Reference

| Function | Kind | Description |
| --- | --- | --- |
| `CALL scenario_create(name, [description], [mode := 'delta'\|'materialized'], [from_scenario := parent])` | verb | Register a scenario. `materialized` copies every base table (full isolation); `from_scenario` branches, inheriting the parent's changes. O(#tables) metadata for delta mode. |
| `CALL scenario_drop(name)` | verb | Remove the scenario and its delta/materialized tables. Refuses while attached or while branches exist. |
| `CALL scenario_freeze(name)` / `scenario_unfreeze(name)` | verb | Reject/allow writes through any attached handle (reads keep working). A frozen materialized scenario is a snapshot. |
| `SELECT * FROM scenario_list()` | table | `scenario_id, name, mode, frozen, parent, created_at, description`. |
| `SELECT * FROM scenario_diff(scenario, table)` | table | Compare to origin: `<pk cols>` (native types), `change_type` (`added`/`removed`/`modified`), `column_name`, `old_value`, `new_value`. Streams through the engine. |
| `SELECT * FROM scenario_diff(a, b, table)` | table | Generic diff between two merged relations (`'main'` or any scenario). `old` = side a, `new` = side b. |
| `SELECT * FROM scenario_diff_summary(scenario)` | table | Per-table `rows_added / rows_modified / rows_removed` from the delta changelog. |
| `SELECT * FROM scenario_merge_preview(scenario)` | table | Planned merge-back actions: `table_name, key, action, conflict`. Streaming; no side effects. Overlay-tier conflicts: an `insert` whose key now exists in base, an `update` whose key vanished. |
| `SELECT * FROM scenario_merge(scenario, [on_conflict := 'abort'\|'ours'\|'theirs'])` | verb | Apply the scenario's delta to the base in the caller's transaction (atomic across tables). `abort` (default) throws on any conflict; `ours` = scenario wins; `theirs` = base wins. On success the scenario ends `frozen` with `merged_at` set and an empty delta. Delta scenarios only; refuses while branches exist. |
| `SELECT * FROM scenario_migrate()` | verb | One-way migration of a legacy v0.1 database (`_scenario_registry`, `_scen_*`, `_snap_*`) into the v2 layout. Archived -> frozen; snapshots -> materialized+frozen; multi-op delta rows folded to net effects; `_scenario_base_rowids` dropped; `_scenario_protocols` preserved. |
| `ATTACH 'name' AS alias (TYPE scenario)` / `DETACH alias` | SQL | The entire read/write UX. |

## Internal Layout

All state lives in the host database (single-file portability):

```
__anofox_scenario.registry              -- scenario_id, name, mode, frozen, parent_id,
                                        --   base_snapshot_id (v0.3), created_at, merged_at (v0.5), description
__anofox_scenario.registry_seq          -- id sequence
__anofox_scenario.s<id>_delta_<table>   -- (_op 'I'|'U'|'D', _ts, <base columns>) PK = base PK
__anofox_scenario.s<id>_mat_<table>     -- materialized base copies (mode = 'materialized')
```

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
