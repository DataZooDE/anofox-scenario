# Anofox Scenario — How scenario tables behave

Companion to the `anofox-scenario-reference` skill. Read it for edge-case semantics, using a
scenario from Python, or sharing one.

## Storage

Everything lives in the host database (the session's default database at `scenario_create` /
`ATTACH` time) under the `__anofox_scenario` schema: the registry, one delta table per
(scenario, table) created by that table's first write, pre-images for delta scenarios, and
for materialized scenarios a copy per table. That schema is read-only for SQL: `SELECT`
works, writes and DDL fail with `Cannot modify … with SQL`. For `IMPORT DATABASE` of an
export with scenarios, or manual repair, `SET anofox_scenario_allow_internal_writes = true`
on that connection, then `RESET` it. `EXPORT DATABASE` needs no setting.

Scenarios cannot be renamed and their description cannot be edited. Workaround: branch it
under the new name (`from_scenario`), but the old one cannot be dropped while the branch
exists.

## Reads and row identity

A scenario table is the base table with the scenario's inserts, updates and deletes applied
(delta: the live base; materialized: its copy; DuckLake: the pinned snapshot). Base tables
added later are readable and writable in delta scenarios.

Row identity is fixed by a table's first write in the scenario: the declared `key_columns`,
else the base PRIMARY KEY, else none (keyless).

```sql
CREATE TABLE events (event_id INTEGER, kind VARCHAR, amount INTEGER);   -- no PRIMARY KEY
INSERT INTO events VALUES (1, 'sale', 10), (2, 'sale', 10), (3, 'refund', -5);
CALL scenario_create('keyed', key_columns := MAP {'events': ['event_id']});
```

**Keyless tables** behave like a bag of rows, as in plain DuckDB: `UPDATE` / `DELETE` /
`MERGE` act on every matching copy, `scenario_diff` uses all columns as the identity (a
changed row is `removed` + `added`, duplicates once per copy), and the merge applies net copy
counts per distinct row. Which base edits such a merge detects: see "Base edits a merge does
not detect" in the reference skill.

## Constraints

The base's `NOT NULL`, `CHECK`, `PRIMARY KEY` and `UNIQUE` constraints are checked against the
scenario's merged state. A unique value whose base row the scenario deleted or changed is free
again. `FOREIGN KEY`s are not checked inside a scenario; `scenario_merge` checks them and rolls
back as a whole on a violation.

## Repeated matches in one statement

If an `UPDATE ... FROM` or `MERGE` matches the same target row several times, repeated deletes
count once, but a second update of the same row fails with `modified the same row twice` and
the statement leaves no trace (plain DuckDB silently picks one match).

## Transactions

Host writes and scenario writes in one transaction commit atomically; the scenario sees the
host row inserted earlier in the same transaction:

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

## Read-only, frozen, concurrency

Writes in a `READ ONLY` transaction, through a `READ_ONLY` attachment or to a frozen scenario
fail when the statement executes, even if they would change zero rows. A transaction reads its
own snapshot of a scenario; two transactions changing the same keys get DuckDB's write-write
conflict; two transactions making the *first* write to the same scenario table at once: the
second gets a retryable `TransactionException`.

## Virtual columns

Scenario tables expose `__scenario_origin` (0 = base row, 1 = scenario row),
`__scenario_key_<k>` and, on keyless tables, `__scenario_rowno`. They can be selected and
filtered on, but are not stable identifiers.

```sql
ATTACH 'keyed' AS keyed (TYPE scenario);
SELECT __scenario_origin, * FROM keyed.events ORDER BY event_id;
```

## Table names as strings

In `scenario_diff`'s table argument, `key_columns` keys and `table_name` columns: `t` and
`main.t` are the same; other schemas are `schema.table`; a part containing a dot is
double-quoted: `'"a.b"'` is the main table `a.b`.

## Views

Views in the host database can be read through the alias (`SELECT * FROM opt.kpi_view`). A
view whose query names the table without a catalog (`demand` or `main.demand`) reads the
scenario; a view that names the catalog (`mydb.main.demand`) keeps reading the real table,
without an error. Check before trusting a KPI view:

```sql
SELECT view_name, sql FROM duckdb_views() WHERE NOT internal;
```

## USE

`USE alias` makes the scenario the default database. The `scenario_*` functions then look for
their registry there and find nothing: `scenario_list()` returns no rows and other calls fail
with `Scenario '…' not found`. Prefer `alias.t`; after a `USE alias`, `USE memory` (or the
file's stem) before calling a scenario function.

## Which scenarios changed what

```sql
SELECT name, mode, frozen, parent, merged_at FROM scenario_list() ORDER BY created_at;
```

Then `scenario_diff_summary('<name>')` per scenario: one row per table it wrote. A merged
scenario has an empty delta, so its diff is empty: save the diff before merging (see the
safe-change skill's [recipes.md](../anofox-scenario-safe-change/recipes.md)).

## From Python

The same SQL on a `duckdb` connection. `ATTACH` is per connection: re-attach after every
`connect`.

```python
import duckdb

con = duckdb.connect("plan.duckdb")
con.install_extension("anofox_scenario", repository="community")
con.load_extension("anofox_scenario")
con.execute("CALL scenario_create('optimistic')")   # once; persists in plan.duckdb
con.execute("ATTACH 'optimistic' AS opt (TYPE scenario)")
con.execute("UPDATE opt.demand SET qty = qty * 1.1 WHERE region = 'US'")
df = con.sql("SELECT * FROM scenario_diff('optimistic', 'demand')").df()
```

## Sharing a scenario with a colleague

A scenario lives inside the `.duckdb` file, so share the file: copy it (with no process
writing it), or `EXPORT DATABASE` and let them `IMPORT DATABASE` with
`anofox_scenario_allow_internal_writes` set for the import. DuckDB allows one writing process
per file; others open it `READ_ONLY`. A scenario over a DuckLake base also needs the lake
(catalog file, `DATA_PATH`, unexpired snapshot). For someone without the extension, export the
scenario's view of a table: `COPY (SELECT * FROM opt.demand) TO 'optimistic_demand.parquet'`.

## Base schema changes

A column added to a base table reaches a scenario that already changed the table. Rows the
scenario changed before show the column's default, like every base row:

```sql
-- doc-snippets: new session
CREATE TABLE prices (id INTEGER PRIMARY KEY, price INTEGER);
INSERT INTO prices VALUES (1, 10), (2, 20);
CALL scenario_create('repricing');
ATTACH 'repricing' AS rp (TYPE scenario);
UPDATE rp.prices SET price = 11 WHERE id = 1;

ALTER TABLE prices ADD COLUMN currency VARCHAR DEFAULT 'EUR';   -- base
UPDATE rp.prices SET currency = 'USD' WHERE id = 2;             -- scenario writes it
SELECT * FROM rp.prices ORDER BY id;
```

Dropping, renaming or retyping a column, renaming or dropping the table waits until no
scenario has unmerged changes to it:

```sql
-- fails: has unmerged changes to it
ALTER TABLE prices DROP COLUMN currency;
```

## More errors

| Error contains | Cause | Fix |
|---|---|---|
| `Invalid on_conflict policy` | typo | `'abort'`, `'ours'` or `'theirs'` |
| `it was already applied to base catalog` | DuckLake merge: lake commit done, host bookkeeping failed, then the scenario was written to | inspect lake and scenario, merge the rest by hand |
| `concurrently made the first write to table` | two transactions created the same delta at once | `ROLLBACK` and retry |
| `key_columns declared for table '…', but it already has a PRIMARY KEY` | the PK already is the identity | drop that `key_columns` entry |
| `key_columns declared for unknown table` / `column '…' does not exist` / `is listed twice` | typo in `key_columns` | fix the map (names as in "Table names as strings") |
| `declared key column '…' no longer exists in the base table` | key column renamed/dropped before the first write | re-declare: `scenario_refresh(name, key_columns := ...)` |
| `the table is not in the scenario's base snapshot` / `changed after the scenario's base snapshot` | DuckLake table created or altered after the pin | new scenario over the lake's current state |
| `Base catalog '…' is not attached` | `base :=` names an unattached catalog | `ATTACH` it first |
| `Table '…' not found in scenario '…'` | `scenario_diff` table argument not in the base | check name and schema quoting |
| `Invalid scenario mode` | `mode :=` other than `'delta'` / `'materialized'` | use one of those |
| `Branching (from_scenario) and mode := 'materialized' cannot be combined` / `cannot be combined with materialized mode or branching` | `mode := 'materialized'` with `from_scenario` or `base :=` | drop `mode`; a branch is a delta over its parent, a DuckLake base is pinned already |
| `Cannot branch from scenario '…': not found` | `from_scenario` names a missing scenario | check `scenario_list()` |
