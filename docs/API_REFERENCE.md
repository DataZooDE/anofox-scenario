# anofox-scenario API Reference

This document describes the SQL API for the anofox-scenario DuckDB extension.

> **Using an AI assistant?** The [Claude Code plugin](../README.md#-claude-code-skills-ai-pair-programming)
> packages this reference and the common workflows (what-if planning, dry-run and merge,
> snapshots and audit) as skills: `/plugin marketplace add DataZooDE/anofox-scenario`.

## Scenario Catalogs (v2 surface, recommended)

A scenario is exposed as an **attached catalog**: ordinary SQL reads and writes, no special
functions. (The legacy v0.1 function surface was removed in v0.2 - see `scenario_migrate()`.)

```sql
-- given a base table forecast(id INTEGER PRIMARY KEY, region VARCHAR, qty DOUBLE)
CALL scenario_create('optimistic', 'demand +10%');   -- description optional
CALL scenario_create('baseline', mode := 'materialized');       -- full copy: immune to base changes
CALL scenario_create('variant', from_scenario := 'optimistic'); -- branch: inherits the parent's changes

SELECT * FROM scenario_list();                    -- scenario_id, name, mode, frozen, parent, created_at, description, merged_at

ATTACH 'optimistic' AS opt (TYPE scenario);

SELECT * FROM opt.forecast;                          -- merged view: base + scenario changes
INSERT INTO opt.forecast VALUES (5, 'LATAM', 40);    -- transparent copy-on-write
UPDATE opt.forecast SET qty = qty * 1.1 WHERE region = 'US';   -- base is never modified
DELETE FROM opt.forecast WHERE qty < 70;

-- streaming, typed diffs (the delta IS the changelog)
SELECT * FROM scenario_diff('optimistic', 'forecast');  -- <pk cols>, change_type, column_name, old/new_value
SELECT * FROM scenario_diff_summary('optimistic');      -- per-table rows_added/modified/removed

TRUNCATE opt.forecast;                               -- every row deleted, in the scenario only
DETACH opt;                                          -- handle gone, scenario data persists

CALL scenario_freeze('optimistic');                  -- reject all writes (reads keep working)
CALL scenario_unfreeze('optimistic');
CALL scenario_drop('variant');                       -- branches go first
CALL scenario_drop('optimistic');                    -- refuses while attached or branched
```

**Semantics**

- **Copy-on-write:** modifications are stored per scenario in delta tables under the internal
  `__anofox_scenario` schema of the host database; base tables are never written.
- **Lazy deltas (#34):** `scenario_create` in the default delta mode writes one registry row (plus
  one row per declared `key_columns` table) and creates no tables, so its cost does not grow with
  the schema. A table's delta is created by its first write (`INSERT`, `UPDATE`, `DELETE`,
  `MERGE INTO`, `INSERT ... ON CONFLICT`, `COPY FROM DATABASE`), at execution, in the caller's
  transaction: `ROLLBACK` removes it again, and the rest of the transaction sees it. A write that
  touches no rows creates nothing. Everything that reads a scenario treats a table without a
  delta as unchanged.
- **Tables added to the base later** are readable *and writable* in an overlay scenario without
  any extra step (`scenario_refresh` is no longer needed). To give a keyless table added later a
  row identity, declare it with `scenario_refresh(name, key_columns := ...)` before its first
  write. Materialized scenarios don't see tables added after their creation. For DuckLake bases
  only tables in the pinned snapshot belong to the scenario (a branch keeps its parent's
  snapshot): writing a table the lake created later fails with `the table is not in the
  scenario's base snapshot`, and so does a first write to a table whose lake columns changed
  after the snapshot. `scenario_diff` reports such a table as not found, and `key_columns` for it
  are refused. To work with it, create a new scenario over the lake's current state.
- **Row identity of a table** is fixed by its first write: the declared `key_columns` (also when
  the base gains a PRIMARY KEY afterwards), else the base PRIMARY KEY, else keyless. Once the
  delta exists, its shape decides, even if a `scenario_refresh` declaration raced the first
  write and committed alongside it. If a declared key column is renamed or dropped in the base
  before the table's first write, reads and introspection keep working, while writes and
  `scenario_diff` fail with `declared key column '...' no longer exists in the base table`;
  re-declare the key with `scenario_refresh(name, key_columns := ...)`.
- **Declaring keys concurrently:** the first `key_columns` declaration on a database creates
  `__anofox_scenario.scenario_tables`. If two transactions do that at the same time, the second
  gets a retryable `TransactionException` naming its scenario.
- **First writes under concurrency:** when two transactions make the first write to the same
  scenario table at the same time, both try to create its delta. The one that comes second gets a
  `TransactionException` (`another transaction concurrently made the first write to table ...
  ROLLBACK and retry the transaction`); after the first one commits, a retry succeeds. Later
  writes to the table only conflict on the same rows, as before.
- **Isolation tiers** (all shipped):
  | Tier | How | Isolation from base changes |
  | --- | --- | --- |
  | `overlay` (default) | delta over the live base | none - base churn shows through unless the scenario touched the same PK (documented semantics) |
  | `materialized` | `mode := 'materialized'`: full copy per table | complete |
  | `snapshot` | `base := '<ducklake attach>'`: reads pinned via `AT (TIMESTAMP => created_at)` | complete, O(1) creation - no data copied |
- **Constraints:** base `NOT NULL`, `CHECK`, PRIMARY KEY and `UNIQUE` constraints are enforced
  for scenario writes against the *merged* state, with distinct errors for conflicts with base
  rows vs. scenario changes. A unique value whose base row the scenario deleted or changed is
  free again: another row may take it, in the same statement or a later one, and
  `scenario_merge` applies the result. Keyless tables (no PK, no `key_columns`) enforce their
  `UNIQUE` constraints the same way. `FOREIGN KEY` constraints are not enforced inside a scenario: a parent row
  may exist only in the scenario. `scenario_merge` enforces them against the base and rolls back
  atomically on a violation. Materialized copies don't carry the FKs, so introspection through a
  materialized scenario doesn't list them. Overlay scenarios mirror the base's FKs.
- **Keyless tables** (no PK, no `key_columns`) have bag semantics for every DML form,
  matching DuckDB on a plain table:
  - `UPDATE` / `DELETE` / `MERGE INTO` act on *every* matching visible copy of a row. A
    `MERGE` matched update writes `D(old) + I(new)` per copy, a matched delete `D(old)`, and
    `WHEN NOT MATCHED [BY SOURCE]` works as usual.
  - `INSERT ... ON CONFLICT (cols) DO NOTHING / DO UPDATE` and `INSERT OR REPLACE` need a
    `UNIQUE` constraint on the conflict target. Conflicts are checked against the merged state:
    base rows the scenario deleted or changed don't conflict, scenario rows do, and NULLs never
    do. Without a `UNIQUE` constraint to target you get DuckDB's own binder error (`There are
    no UNIQUE/PRIMARY KEY constraints ...`).
  - One difference from a plain table: if several source rows match the *same* target copy
    and would *update* it, the statement fails with `modified the same row twice` (plain DuckDB
    silently applies one of the matches). This is the same rule keyed scenario tables follow.
- **One target row matched several times** (`UPDATE ... FROM`, `DELETE ... USING`, `MERGE`
  with a non-unique source), keyed and keyless tables alike: repeated *deletes* are applied
  once, as in core - the row is deleted, counted and returned by `RETURNING` once, and two
  `MERGE ... THEN DELETE` actions may both match it. Anything that would *update* a row
  twice, or update and delete it in one `MERGE`, fails with `modified the same row twice`
  (core silently picks one match); the statement leaves no trace.
- **Transactions:** scenario DML runs in the caller's transaction; `ROLLBACK` undoes it. Host
  writes and scenario writes may share one explicit transaction, in any order and across any
  number of scenarios on the same host: every scenario row physically lands in the host
  database, so `BEGIN; INSERT INTO main.orders ...; UPDATE s.orders ...; COMMIT;` commits (or
  rolls back) both sides atomically. Writing to a genuinely different attached database in the
  same transaction is still refused by the single-writer rule, with an error naming the
  scenario (`Cannot write to scenario '<name>': this transaction already modified database
  ...`). Writes in a `READ ONLY` transaction, through a scenario attached `READ_ONLY`, or to a
  frozen scenario fail at the start of execution, even when they would touch zero rows. Like
  core, these checks run at `EXECUTE`, not `PREPARE`: a write prepared in a read-only
  transaction (or while the scenario is frozen) runs once the executing transaction allows it.
- **Reads under concurrency:** a transaction keeps reading its own snapshot of a scenario even
  after another connection commits changes to the same keys. A write from that older snapshot to
  a key someone else changed since fails with DuckDB's normal write-write conflict.
- **Read performance:** a scan hides base rows replaced by the scenario through an in-memory hash
  of the updated or deleted keys, so it stays parallel however large the base is. A filter that
  pins the key (`WHERE id = 42`, `WHERE id IN (...)`, or every column of a composite key) reads
  only those keys from the delta. Setting:
  | Setting | Default | Meaning |
  | --- | --- | --- |
  | `anofox_scenario_suppression_hash_limit` | `4194304` | The most updated/deleted keys per table that are hashed in memory (about 16 bytes each). Above it, each base row is looked up in the delta's key index instead: less memory, but scans of that table no longer run in parallel. `0` always uses the index. |
- **Internal identity columns:** scenario tables expose virtual columns that the write path
  uses to identify rows - `__scenario_origin` (0 = base row, 1 = scenario row),
  `__scenario_key_<k>`, and, on keyless tables, `__scenario_rowno`. They can be selected and
  filtered on, but they are not part of the table and not stable identifiers: `__scenario_rowno`
  is numbered afresh by every scan (scenario rows first), so a value read by one statement may
  name a different row in the next. Don't store or page by it.
- **DDL:** not permitted inside scenarios (`DDL operations are not permitted in scenarios`).
  Change the base schema instead: new base tables show up in delta scenarios automatically.
  Materialized scenarios (and their branches) keep the copy made at creation and DuckLake-based
  ones their creation snapshot, so other schema changes reach them only through a new scenario;
  the error's hint says which case applies.
- **Base schema changes of tables a scenario changed** (#62):
  - `ALTER TABLE t ADD COLUMN` on the base works whether the scenarios are attached or not, and
    reaches every scenario over the host: delta and materialized ones, branches, keyed and
    keyless tables. The same statement adds the column to the scenarios' stored changes
    (deltas, pre-images, materialized copies). Rows a scenario changed before show the value
    the base rows got: the column's `DEFAULT`, NULL without one, and also NULL for a volatile
    default (`nextval(...)`, `random()`, `uuid()`), whose per-row values cannot be repeated.
    A materialized copy gets the column filled the same way; later base *data* changes still do
    not reach it. Scenarios can then read and write the column (`INSERT`, `UPDATE`, `MERGE INTO`,
    `ON CONFLICT`), and `scenario_diff`, `scenario_merge_preview` and `scenario_merge` include
    it (drift detection compares it like any other column).
  - `DROP COLUMN`, `RENAME COLUMN`, a column type change (also of a struct field),
    `RENAME TO` and `DROP TABLE` on the base fail while any scenario over the host has unmerged
    changes to the table: `Cannot drop a column of table 't': scenario 's' has unmerged changes
    to it, ...`. Merge (`scenario_merge`) or drop the scenario first. Scenarios without changes
    to the table do not block; materialized copies keep their old columns. Other `ALTER`s
    (defaults, `NOT NULL`, comments) are unaffected.
  - A change made without the extension loaded (plain DuckDB, another process) is caught when a
    scenario next uses the table: `Scenario 's' cannot use table 't' ...`. A gained column is
    added at the next `ATTACH` of the scenario (autocommit, writable, no other transaction
    active on the host) or by `scenario_merge`; for a dropped, renamed or retyped column,
    restore it or drop the scenario.
- **Host database:** the scenario registry lives in the session's *default* database at
  `scenario_create` / `ATTACH` time. So after `USE alias` (a scenario) the `scenario_*` functions look in the
  scenario catalog and find nothing: `scenario_list()` returns no rows and the other functions
  fail with `Scenario '…' not found`. `USE` the host database again (`memory`, or the file's
  stem) before calling them; prefer `alias.t` over `USE alias`.
- **Telemetry:** the extension sends anonymous usage telemetry (an `extension_loaded` event
  with extension version and platform). Setting:
  | Setting | Default | Meaning |
  | --- | --- | --- |
  | `anofox_scenario_telemetry_enabled` | `true` | `SET anofox_scenario_telemetry_enabled = false` turns it off for the process. Set it before `SET lock_configuration = true`, which freezes every option. The load event itself fires during `LOAD`; to suppress it as well, set the environment variable `DATAZOO_DISABLE_TELEMETRY=1` before starting. Telemetry is off automatically in common CI environments (`CI`, `GITHUB_ACTIONS`, ...). |

**Remaining limitations** (clean errors; everything else from the v1 list has shipped)

| Limitation | Notes |
| --- | --- |
| Delta scenarios detect base edits to rows they touched only for keyed tables written since pre-images were introduced (#53). Deltas written by earlier versions and the rows a branch inherited from a materialized parent keep the overlay checks: a base edit to such a row is overwritten by the merge. Keyless tables of delta scenarios have no snapshot: a deleted row whose base copies are gone is a conflict, but a base that still holds enough copies is not (a base edit that adds copies, or removes fewer than the scenario deletes, goes undetected) | use `mode := 'materialized'` or a DuckLake base, or merge right after the dry run |
| A scenario that deletes a referenced parent row together with its children cannot be merged: DuckDB refuses deleting a row referenced by FOREIGN KEY in the transaction that deletes the referencing rows (plain DuckDB behaves the same). The same holds for an update that changes a referenced parent's key or another indexed column | merge the child deletions first (a scenario of their own), then the parent's |
| Views with *qualified* table references keep reading their explicit target (unqualified references rebind to scenario tables) | documented semantics |
| DuckLake merge-back: lake apply commits in its own transaction (autocommit required, no cross-catalog 2PC) | the lake commit is tagged; if the host bookkeeping fails afterwards, re-running `scenario_merge` finishes it without re-applying. Refused if the scenario was written to in between |
| Base schema changes other than `ADD COLUMN` (drop/rename/retype a column, rename/drop the table) of a table a scenario has unmerged changes to | refused at the `ALTER` / `DROP`: merge or drop the scenario first. `CREATE OR REPLACE TABLE` and `DROP SCHEMA ... CASCADE` are not intercepted: a replaced table with other columns fails the scenario's next use of it with `cannot use table`, a dropped one fails `scenario_merge` with `base table '…' no longer exists` |
| Keyless scenarios written before v0.4.1: keyless `UPDATE`/`DELETE` needs the delta upgraded first, which happens on a plain `ATTACH` (autocommit or a transaction already writing the host, not `READ_ONLY`, no other transaction active on the host). Otherwise ATTACH succeeds, reads are correct, and the DML error asks to re-`ATTACH` | reads and `INSERT` are unaffected |

## Function Reference

| Function | Kind | Description |
| --- | --- | --- |
| `CALL scenario_create(name, [description], [mode := 'delta'\|'materialized'], [from_scenario := parent], [base := catalog], [key_columns := MAP {'table': ['col', ...]}])` | verb | Register a scenario. `materialized` copies every base table (full isolation); `from_scenario` branches, inheriting the parent's changes; `base` uses another attached catalog's tables as the base (DuckLake bases are pinned to creation time - true snapshot isolation); `key_columns` declares row identity for tables without a PRIMARY KEY (validated up front - unknown or repeated columns are refused, errors name the table as `schema.table` - and recorded in `__anofox_scenario.scenario_tables`). Delta mode is O(1): no delta tables are created until a table's first write. A branch copies only the parent deltas that exist, plus the parent's declared keys, and may not change an inherited identity; it inherits the parent's base catalog and DuckLake snapshot pin. `from_scenario` and `base` each exclude `mode := 'materialized'`, and each other. Returns `success`. |
| `CALL scenario_drop(name)` | verb | Remove the scenario, its delta/materialized tables and its declared keys. Refuses while attached or while branches exist. |
| `CALL scenario_freeze(name)` / `scenario_unfreeze(name)` | verb | Reject/allow writes through any attached handle (reads keep working). A frozen materialized scenario is a snapshot. Plain SQL cannot flip the flag or edit the frozen data behind the extension's back either: the internal schema is read-only for SQL (see [SQL access to the internal schema](#sql-access-to-the-internal-schema)). |
| `SELECT * FROM scenario_list()` | table | `scenario_id, name, mode, frozen, parent, created_at, description, merged_at` (`parent` is the parent scenario's name; `merged_at` is `NULL` until `scenario_merge` absorbs the scenario). |
| `SELECT * FROM scenario_diff(scenario, table)` | table | Compare to origin (`table` is matched like a scenario table: `main.t` and `t` are the same): `<pk cols>` (native types), `change_type` (`added`/`removed`/`modified`), `column_name`, `old_value`, `new_value`. Streams through the engine. Keyless tables (no PK, no `key_columns`) get a bag diff: every column is the identity, a changed row is `removed` + `added`, and duplicates appear once per copy. |
| `SELECT * FROM scenario_diff(a, b, table)` | table | Generic diff between two merged relations (`'main'` or any scenario). `old` = side a, `new` = side b. `'main'` is the origin table of the scenario on the other side, in its base catalog: pinned to the scenario's creation snapshot for DuckLake bases, live otherwise. DuckLake-based scenarios are always diffed at their snapshot, so later lake changes never show as scenario changes. |
| `SELECT * FROM scenario_diff_summary(scenario)` | table | Per-table `rows_added / rows_modified / rows_removed` from the delta changelog, one row per table the scenario has written (a table never written has no delta and no row). For keyless tables these are the net counts per distinct row (`rows_modified` is 0). |
| `SELECT * FROM scenario_merge_preview(scenario)` | table | Planned merge-back actions: `table_name, key, action, conflict`. Streaming; no side effects. Overlay conflicts: an `insert` whose key now exists in base, an `update` whose key vanished. Every scenario additionally flags 3-way *drift* conflicts: the base row of a key the scenario updated or deleted changed since its snapshot - the creation copy (materialized), the pinned lake snapshot (DuckLake), or for delta scenarios the row's pre-image, recorded when the scenario first touched it (keyed tables, #53). Keyless tables list the per-value net plan the merge applies: one row per copy deleted or inserted, `key` = the whole row as text, e.g. `(A, 1)`. For keyless tables of snapshot scenarios (materialized, DuckLake) a `delete` conflicts when the base's number of copies of that row differs from the snapshot's; for keyless tables of delta scenarios (no snapshot) a `delete` conflicts when the base now holds fewer copies of that row than the merge deletes - e.g. the base edited or deleted the row after the scenario updated it. |
| `SELECT * FROM scenario_merge(scenario, [on_conflict := 'abort'\|'ours'\|'theirs'])` | verb | Apply the scenario's delta to the base. `abort` (default) throws on any conflict; `ours` = scenario wins; `theirs` = base wins. Host bases apply atomically in the caller's transaction; DuckLake bases apply atomically on the lake side in their own transaction (autocommit required). Keyless tables merge as bags: per distinct row (NULL-safe whole-row match) the net delete count `n` removes exactly `n` copies and a net insert count adds that many; on a drifted row of a snapshot scenario (materialized or DuckLake) `theirs` keeps the base's copies and `ours` sets the base to the scenario's count. In a delta scenario a keyless delete whose copies are no longer all in the base is a conflict: `abort` refuses the merge, `theirs` keeps the base's remaining copies of that row, `ours` deletes the copies still there (the missing ones are skipped; nothing is re-added). The scenario's keyless inserts have no identity to conflict on and apply under every policy - so `(x, 1) -> (x, 2)` in the scenario, merged with `ours` or `theirs` after the base changed `(x, 1)` to `(y, 1)`, leaves both `(y, 1)` and `(x, 2)`. Each conflicting row value counts once in `conflicts_resolved`. On a DuckLake base the lake commit is tagged (`commit_extra_info`), so a merge whose host bookkeeping failed can simply be re-run. On success the scenario ends `frozen` with `merged_at` set and an empty delta, and the function returns one row `tables_changed, rows_applied, conflicts_resolved`. A scenario with nothing to merge - its merge plan is empty, i.e. `scenario_merge_preview` lists no rows: no delta rows, or keyless changes that net to zero per row - is a no-op: it returns `0, 0, 0`, writes nothing (no lake commit on a DuckLake base), and stays open (not frozen, `merged_at` NULL). A plan whose actions all turn out to change nothing (a delete of a key the base already deleted, conflicts all resolved `theirs`) is still a merge. Refuses while branches exist (a merged branch counts until it is dropped), while the scenario is frozen, and once it has been merged. Merging a branch applies its parent's changes too. Drift (a base edit to a row the scenario touched) is a conflict like the overlay ones, also for delta scenarios (#53; see `scenario_merge_preview`). Tables are applied referenced-first, so new parent rows exist before the children that reference them; an update that keeps the key and every indexed column is applied in place, so rows referencing it stay valid (#54). With a host base, a `ROLLBACK` of the surrounding transaction undoes the merge and leaves the scenario mergeable. |
| `SELECT * FROM scenario_refresh(name, [key_columns := MAP {...}])` | verb | **Deprecated (#34).** Tables added to the base after `scenario_create` are writable without it, and it no longer creates delta tables. What remains: `key_columns` declares the row identity of keyless tables the scenario does not track yet (no declared key, never written); rejected once a table is tracked, except to replace a declared key whose column was renamed or dropped in the base before the table's first write. For DuckLake bases the table must be in the scenario's snapshot. Returns `refreshed_tables`, the number of keys declared (`0` without `key_columns`). Refuses frozen and materialized scenarios. |
| `SELECT * FROM scenario_migrate()` | verb | Returns `migrated_scenarios, migrated_snapshots` (both `0` on a database without legacy tables). One-way migration of a legacy v0.1 database (`_scenario_registry`, `_scen_*`, `_snap_*`) into the v2 layout. Archived -> frozen; snapshots -> materialized+frozen; multi-op delta rows folded to net effects; `_scenario_base_rowids` dropped; `_scenario_protocols` preserved. |
| `ATTACH 'name' AS alias (TYPE scenario [, READ_ONLY])` / `DETACH alias` | SQL | The entire read/write UX. `USE alias` makes unqualified table names resolve to the scenario, but also hides the registry from the `scenario_*` functions until you `USE` the host database again. |

## Internal Layout

All state lives in the host database (single-file portability):

```
__anofox_scenario.registry              -- scenario_id, name, mode, frozen, parent_id,
                                        --   base_snapshot_id (v0.3), created_at, merged_at (v0.5), description
__anofox_scenario.registry_seq          -- id sequence
__anofox_scenario.scenario_tables       -- (scenario_id, table_name, key_columns VARCHAR[]):
                                        --   declared keys (key_columns :=), created by the
                                        --   first declaration
__anofox_scenario.s<id>_delta_<table>   -- created by the table's first write;
                                        --   (_op 'I'|'U'|'D', _ts, <base columns>) PK = base PK
                                        --   or the declared key_columns; keyless tables append
                                        --   _count BIGINT NOT NULL (bag changelog, no PK)
                                        --   instead - named __count (more underscores as
                                        --   needed) when the table has its own _count;
                                        --   pre-v0.4.1 keyless deltas (no _count) are
                                        --   upgraded in place on a plain ATTACH (not
                                        --   READ_ONLY, not inside a transaction that does
                                        --   not write the host, not while another
                                        --   transaction is active); until then they read
                                        --   as _count = 1
__anofox_scenario.s<id>_mat_<table>     -- materialized base copies (mode = 'materialized');
                                        --   FOREIGN KEY constraints are not copied
__anofox_scenario.s<id>_pre_<table>     -- pre-images (#53): <base columns>, PK = the delta's
                                        --   key; each base row as it was when the scenario
                                        --   first updated or deleted it. Keyed tables of
                                        --   scenarios over the host that are not materialized;
                                        --   created with the delta, emptied by the merge
```

Non-main schemas use `<schema>.<table>` as the `<table>` part of the naming contract
(`s3_delta_analytics.events`). A name part that contains a `.` is written in SQL double quotes:
`'"a.b"'` is the `main` table `a.b`, `'a.b'` is table `b` in schema `a`, and
`'"my.schema"."t.1"'` quotes both parts. The same spelling is used everywhere a table name is a
string: the `scenario_diff` table argument, `key_columns :=` keys, and the `table_name` column of
`scenario_diff_summary` / `scenario_merge_preview`. Names without a dot are written as is.

The delta table is a stable changelog contract: `scenario_diff` reads it, branching copies it,
and `scenario_merge` replays it.

### SQL access to the internal schema

`__anofox_scenario` is read-only for SQL (#55). `SELECT` works; every statement that would
write to it or change its schema fails with `Cannot modify … with SQL` (a permission error),
in every attached database and on every connection of a database that has the extension
loaded:

- `INSERT` (also `COPY ... FROM`, `INSERT ... ON CONFLICT`), `UPDATE`, `DELETE`, `TRUNCATE`
  and `MERGE INTO` on its tables;
- `CREATE` of anything in it (`TABLE`, `TABLE AS`, `VIEW`, `SEQUENCE`, `MACRO`, `TYPE`,
  `INDEX`, `SCHEMA __anofox_scenario`), `ALTER` and `COMMENT ON` of its entries, `DROP` of its
  entries or of the schema;
- `COPY FROM DATABASE` from a database that holds scenarios (it would re-create the schema in
  the target), and `IMPORT DATABASE` of an export that contains it.

<!-- doc-snippets: new session -->
```sql
CREATE TABLE plan (id INTEGER PRIMARY KEY, qty INTEGER);
CALL scenario_create('approved', mode := 'materialized');
CALL scenario_freeze('approved');
SELECT name, frozen FROM __anofox_scenario.registry;
```

```sql
UPDATE __anofox_scenario.registry SET frozen = false WHERE name = 'approved';
```

Scenarios change only through the scenario functions and attached scenarios, which do not
go through SQL. The check runs when a statement is planned, independent of the optimizer and
of the statement form (a prepared statement that was allowed with the setting on is checked
again at every execution). A rejected
statement writes nothing. Inside an explicit transaction, roll back afterwards: DuckDB 1.5 keeps
the transaction usable, DuckDB 2.0 aborts it, so a later `COMMIT` there discards its earlier
writes.

The escape hatch is a per-connection setting:

| Setting | Default | Meaning |
| --- | --- | --- |
| `anofox_scenario_allow_internal_writes` | `false` | `true` lets SQL on this connection write to and change the schema of `__anofox_scenario`. For backup/restore (`IMPORT DATABASE`, `COPY FROM DATABASE`), manual repair, or wiping every scenario (`DROP SCHEMA __anofox_scenario CASCADE`). Hand-made changes are not validated: a wrong row or column breaks the affected scenarios. |

`EXPORT DATABASE` needs no setting (it only reads). To restore an export that contains
scenarios, import with the setting on and turn it off again:

```
SET anofox_scenario_allow_internal_writes = true;
IMPORT DATABASE 'backup_dir';
RESET anofox_scenario_allow_internal_writes;
```

Without it, `IMPORT DATABASE` stops at `CREATE SCHEMA __anofox_scenario` and imports nothing.

Limits: the guard lives in the extension, so it protects the internal schema only in sessions
that have `anofox_scenario` loaded. A process that opens the database file without the
extension (or an older version of it) can still change those tables. Any session can also
turn the setting on unless the configuration is locked: for tamper resistance within DuckDB,
load the extension and then run `SET lock_configuration = true`. Functions with side effects
in a `SELECT` are not statements the guard sees (`nextval('__anofox_scenario.registry_seq')`
only skips scenario ids).

## Protocols / Audit Notes

Protocol notes are plain SQL over plain tables - no dedicated API. Recommended recipe: an
append-only event log (every approval, unfreeze or revision is a new row; nothing is replaced):

```sql
CREATE SEQUENCE IF NOT EXISTS audit_events_seq;
CREATE TABLE IF NOT EXISTS audit_events (
    event_id BIGINT DEFAULT nextval('audit_events_seq') PRIMARY KEY,
    scenario VARCHAR NOT NULL, event VARCHAR NOT NULL, actor VARCHAR,
    logged_at TIMESTAMP DEFAULT current_timestamp, note VARCHAR);

INSERT INTO audit_events (scenario, event, actor, note) VALUES
    ('optimistic', 'created', 'planning', 'testing demand +10% for the Q3 review');

-- export to markdown
COPY (SELECT '## ' || scenario || ' / ' || event || chr(10) || coalesce(note, '')
      FROM audit_events ORDER BY event_id)
TO 'audit_events.md' (FORMAT csv, QUOTE '', HEADER false);
```

Freezing rejects writes through scenario catalogs, and the internal `__anofox_scenario` tables
reject SQL writes in every session that has the extension loaded (see
[SQL access to the internal schema](#sql-access-to-the-internal-schema)). That is not tamper
evidence: a process that opens the file without the extension, or a session that sets
`anofox_scenario_allow_internal_writes`, can still change them. For tamper evidence keep an
external copy (`EXPORT DATABASE` plus a checksum stored elsewhere) or use DuckLake snapshots
with retention.

(Databases migrated from v0.1 keep their existing `_scenario_protocols` rows unchanged.)
