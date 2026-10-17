# Architecture

How anofox-scenario works internally. If you only want to *use* the extension, read
[API_REFERENCE.md](../API_REFERENCE.md) instead — this document assumes you're changing the code.

---

## The core idea

A scenario is a **branch over a base table**. Rather than copying the table, the extension stores
only the rows a scenario changed, and reconstructs the scenario's view of the world at read time.

```
                    SELECT * FROM opt.forecast
                                │
                                ▼
                    ┌───────────────────────┐
                    │   merge-on-read scan  │
                    └───────────────────────┘
                       │                 │
              delta first          then base,
             (authoritative)     minus claimed keys
                       │                 │
                       ▼                 ▼
        ┌──────────────────────┐  ┌───────────────────┐
        │ __anofox_scenario    │  │  main.forecast    │
        │  .s1_delta_forecast  │  │  (never written)  │
        │  _op | _ts | cols…   │  │                   │
        └──────────────────────┘  └───────────────────┘
```

The delta table is a **changelog**: each row carries `_op` (`I`/`U`/`D`) plus the base columns.
Its primary key is the base table's PK, or the `key_columns` declared at `scenario_create`. It is
created by the table's first write, not by `scenario_create` (see *Delta lifecycle*).

That single structure serves four consumers, which is why it is treated as a stable contract:

| Consumer | Uses the delta to |
|----------|-------------------|
| Scan | Reconstruct the merged view |
| `scenario_diff` | Emit typed row-level changes |
| Branching | Seed a child scenario by copying it |
| `scenario_merge` | Replay changes onto the base |

---

## Why a catalog extension

The extension registers a **storage extension**, so `ATTACH 'name' (TYPE scenario)` produces a
real catalog. Most of the design follows from that one choice:

- **Ordinary SQL works.** `SELECT`, `INSERT`, `UPDATE`, `DELETE`, joins, CTEs, and prepared
  statements route through DuckDB's normal planner. There is no SQL rewriting and no optimizer
  pass intercepting writes.
- **Entries are per transaction.** Scenario table entries are projections of the base built for
  the current transaction (owned by `ScenarioTransaction`), and physical plans hold references
  to them. `ScenarioCatalog::GetCatalogVersion` therefore folds a per-transaction serial into the
  host catalog's version: a prepared statement is re-bound in each new transaction and reused
  within one. Write checks (read-only, frozen) run at execution instead, see *Write attribution*.
- **Constraints, types, and binder errors are DuckDB's**, not reimplementations.
- **Transactions ride the host's.** Scenario DML runs in the caller's transaction, so `ROLLBACK`
  undoes scenario writes for free and ACID is inherited rather than rebuilt. Writes are
  attributed to the host database, not to the scenario catalog (see *Write attribution*
  below), so host DML and scenario DML can share one transaction.

The cost is that the catalog must present a *synthetic* schema mirroring the base, and must
reject the operations that make no sense on a branch — which is why all DDL is refused.

---

## Read path

`catalog/scenario_scan.cpp` is the heart of the extension.

1. Scan the delta. Rows marked `I` or `U` are emitted directly, and their keys recorded as
   **claimed**.
2. Scan the base. Emit a row only if its key was not claimed and not marked `D`.

Delta-first ordering is what makes a scenario row win over the base row with the same key — so an
`UPDATE` reads back as a modification rather than a duplicate.

For keyed deltas, the claimed set is never materialized as strings (#36, #39). Step 1 emits the
`I`/`U` rows chunk-wise and, for every `U`/`D` row, inserts the vectorized hash of its key into an
open-addressing hash set (`KeyHashSet`, `scenario_scan.cpp`). In step 2, each base chunk hashes its
keys the same way. A row whose hash misses is kept without further work. Only the hits are
confirmed exactly, through the delta's `PRIMARY KEY` index (`ScenarioDeltaIndexProbe`,
`scenario_delta.cpp`), because a 64-bit hash can collide. The set is read-only after init, so
parallel base scans never serialize on the index lock; the index is touched about once per
suppressed row. Past `anofox_scenario_suppression_hash_limit` `U`/`D` keys (default 4,194,304,
about 16 bytes each) the set is dropped and every base row probes the index instead: bounded
memory, but serialized per index.

**Point lookups (#39).** When the pushed filters pin every key column to a finite set of values
(`key = c`, `key IN (...)`, or such a filter inside an `AND` or an optional filter; at most 2048
keys after the cross product), init skips the delta scan. It looks those keys up through the index,
fetches the visible `I`/`U` rows by row id (`ScenarioDeltaIndexProbe::FetchRow`), and seeds the hash
set with the `U`/`D` keys. A single-row `SELECT`/`UPDATE`/`DELETE` therefore costs O(log delta)
instead of O(delta).

**Snapshot visibility of the probe (#39).** Every delta change deletes a row and appends its
replacement. When a transaction commits such a change while older transactions are still running,
DuckDB moves the old row id from the main PK ART into `IndexEntry::deleted_rows_in_use`. Rows
appended during a checkpoint go to `added_data_during_checkpoint`. So the probe does what core's
index scan (`TableScanFunction`) does: under `IndexEntry::lock` it runs read-only `ARTOperator`
lookups in all three ARTs, collects every row id stored under the key (nested leaves included),
and keeps the one that `DataTable::CanFetch` says the transaction can see. It then repeats this
for the transaction's own `append_indexes`, whose hits supersede the committed ones. An older
snapshot thus keeps finding its own version of a key that another connection has since changed.
Without this, the base row would resurface next to the delta row, and the write sinks would
misclassify the key.

Deltas without a bound PK index keep the materialized string claim set. Keyless bag deltas use a
per-value suppression budget instead (below).

### Keyless tables

Without a PK or declared `key_columns` there is nothing to match on, so the delta becomes a
**bag changelog**: no PK, plus a trailing `_count BIGINT NOT NULL` multiplicity, with readers
aggregating inserts and deletes per distinct value.

Readers find the multiplicity column structurally, never by name: `ScenarioDelta::CountColumnIndex`
accepts a delta with no PK whose last column is a `BIGINT` carrying a `NOT NULL` constraint. A
delta never mirrors the base's `NOT NULL`s, so no payload column - and no column of a pre-v0.4.1
delta - matches. Every reader (scan, insert/update/delete sinks, UNIQUE probe, `scenario_diff`,
`scenario_diff_summary`, merge-back) goes through that helper, and SQL that names the column uses
`CountColumnName`. The column is called `_count` unless a base column already is: then
`NewCountColumnName` prefixes underscores until the name is free (`__count`), so a keyless table
with its own `_count` column works.

This supports `INSERT`/`UPDATE`/`DELETE`/`MERGE INTO` with bag semantics. Keyless row identity
is the whole row: `GetRowIdColumns()` is `__scenario_origin`, one `__scenario_key_<k>` per
column, and `__scenario_rowno`. An update sink writes `D(old row) + I(new row)` per affected
copy, and a delete sink writes `D(old row)`.

`MERGE INTO` (and `INSERT ... ON CONFLICT`, which the binder lowers to `MERGE` for extension
catalogs) runs through the same sinks. Every matching visible copy is affected, as in core.
The catch is that a join can emit one copy several times, once per matching source row, and
the whole-row identity can't tell that apart from several identical copies. So keyless scans
synthesize **`__scenario_rowno`**, a per-scan unique `BIGINT`:

- Visible delta rows take `0..n-1` when the scan initializes.
- Base chunks reserve ranges from an atomic counter as they stream.
- Filters on it stay above the scan (`supports_pushdown_type`).

The suppression budget swallows the *first* base copies of a value that reach it. A filter
pushed into the base scan on the physical `rowid` would therefore change which copy is
swallowed: with three copies at rowids 0-2 and one deleted, `WHERE rowid = 1` would consume the
budget on rowid 1 rather than 0. Whenever the budget is non-empty, keyless `rowid` filters
(including dynamic join filters) are therefore held back from storage and evaluated on the
emitted chunk after suppression. They use the same post-filter that `__scenario_rowno` filters
use (#39). Without a budget they are pushed down as usual.

The row number is only meaningful within one scan; the API reference documents it (with
`__scenario_origin` / `__scenario_key_<k>`) as an internal, unstable column. A user `WHERE`
clause may reference any of these virtual columns: the binder then reuses the already projected
column for the DML row-id list (`Binder::BindRowIdColumns`), so a `DELETE`'s row-id columns need
not be contiguous. `PlanDelete` therefore records each row-id column's own position from
`LogicalDelete::expressions`; `UPDATE` and `MERGE` project them contiguously.

**Double touch.** `ScenarioTouchedRows` records the rows a statement modified - the serialized
key on keyed tables, the row number (two growable bitmaps: touched, deleted) on keyless ones.
A repeated touch by a delete of a row a delete already removed is skipped (no second tombstone,
count or `RETURNING` row: the outcome is the same whichever match wins, as in core). Any other
repeat throws `modified the same row twice`. `PlanMergeInto` hands all of a `MERGE`'s action
sinks one shared set, keyed and keyless alike, so a row matched by an `UPDATE` action and a
`DELETE` action is caught there instead of surfacing as a raw delta PK violation. Tracking costs
memory per touched row, so `UPDATE`/`DELETE` enable it only when the child plan can repeat a
row (`ScenarioChildCanRepeatRows`: anything but scan/filter/projection, i.e. joins) or when a
keyed `UPDATE` sets a key column (a key move must see the keys the statement touched).

`ON CONFLICT` targets need a `UNIQUE` constraint, just like core. The binder derives the join
from the table's unique indexes and raises core's own errors when there is none. The scenario
scan supplies the merged state, so a value whose base row the scenario deleted doesn't match,
and the insert and update sinks count the result against the merged relation (`ProbeKeyless`,
below).

`scenario_diff` on a keyless table is a **bag diff**:

- Every column is the identity, and there is no `modified`: a changed row shows as `removed`
  (old) plus `added` (new).
- The 2-arg form nets the delta's `I`/`D` counts per distinct row (`GROUP BY` is NULL-safe,
  like `MakeKey`). It emits one row per copy, so an insert later deleted again shows nothing.
- The 3-arg form builds each side's merged bag as origin `UNION ALL` inserted copies
  `EXCEPT ALL` deleted copies, and diffs the two with `EXCEPT ALL` in both directions.
- `scenario_diff_summary` reports the netted counts.

**Pre-v0.4.1 deltas.** Before v0.4.1, keyless tables were insert-only: their delta was
`(_op, _ts, <base columns>)` with no PK, no `_count`, the base's secondary `UNIQUE`s mirrored,
and only `I` rows (UPDATE/DELETE were gated). Each old row is therefore exactly one inserted
copy, i.e. `_count = 1`. `ScenarioDelta::UpgradeLegacyKeylessDeltas` rebuilds such a delta
(no PK and no trailing multiplicity column) into the current layout: rows are buffered, the old
table (and its mirrored `UNIQUE` indexes) is dropped, the table is recreated with `_count BIGINT
NOT NULL` (or the collision-free name above) as the trailing column, and the rows are re-appended
with `_count = 1`, timestamps kept. Dropping the table is not a logical write, but it does
conflict with other transactions using the delta, so it only runs at `ATTACH ... (TYPE
scenario)`, before any statement binds against the scenario, and only when
`TryUpgradeLegacyKeylessDeltas` finds it safe. It is skipped, leaving the legacy layout, when:

- the host database is read-only, the transaction is `READ ONLY`, or the attach itself is
  `READ_ONLY` (it promises not to write the host);
- the ATTACH runs inside an explicit transaction that does not already write the host. The
  upgrade would otherwise make the host the transaction's single writer behind the user's back,
  and a later write to another database would fail. A transaction that already writes the host
  upgrades along the way;
- an older transaction is active on the host (`DuckTransactionManager::LowestActiveStart()`
  is before ours): it may hold uncommitted writes to the legacy delta, whose commit would fail
  once the table is dropped;
- the drop hits a catalog conflict (`TransactionException`): caught, so ATTACH never fails on
  account of the upgrade.

Freezing does not block it - the logical content is unchanged. `scenario_create(...,
from_scenario := p)` never rewrites the parent: `CopyTableData` copies a legacy parent delta into
the child's bag delta with `_count = 1` per row.

Scan-time and DML-time upgrades are deliberately avoided: bound plans hold the delta table
entry, and a SELECT must not turn into a host write. Readers instead accept the legacy layout
as an implicit `_count = 1` (scan, `scenario_diff`, `scenario_diff_summary`, merge-back), so a
legacy scenario on a read-only host reads correctly. Keyless UPDATE/DELETE on a delta that could
not be upgraded is rejected with guidance to re-`ATTACH` from a writable connection.

A bag delta cannot carry a mirrored `UNIQUE` index, because `D` rows hold real payloads.
Secondary `UNIQUE` constraints of a keyless table are therefore *counted* on the merged
relation instead (`ScenarioBaseUniqueProbe::ProbeKeyless`):

- A non-NULL tuple *v* is held `base_has(v) + Σ_I _count − Σ_D _count` times.
- `base_has(v)` is 0 or 1, read from the base's unique index.
- The sums run over the delta rows holding *v*, plus this statement's pending deletes. An
  `UPDATE` is `D(old) + I(new)`, so an unchanged value nets out.
- A new row is rejected if its tuple is already held once.

---

## Write path

Writes are sinks over the delta, never over the base. The subtlety is that a row may be written
repeatedly during a scenario's life, so each write resolves an **op transition**:

| Existing delta row | New operation | Result |
|--------------------|---------------|--------|
| *(none)* | INSERT | `I` |
| *(none)* | UPDATE | `U` |
| *(none)* | DELETE | `D` |
| `I` | UPDATE | `I` — still scenario-local, new values |
| `I` | DELETE | row dropped from the delta entirely |
| `U` | UPDATE | `U` — values replaced |
| `U` | DELETE | `D` |
| `D` | INSERT | `U` — resurrected as a modification of the base row |

Collapsing to net effect keeps the delta proportional to *rows touched* rather than *statements
run*, and keeps `scenario_diff` honest: a row updated five times reports one modification.

To find the existing delta row, a keyed sink resolves only the keys its chunk references
(`ScenarioDeltaKeyMap`, `scenario_insert.cpp`). It probes the delta's `PRIMARY KEY` ART, first the
committed index and then the transaction's own appends, the same two-phase lookup core uses for
`INSERT ... ON CONFLICT`. Hits on rows not visible to the transaction are dropped. Each key is
probed once per statement, before the statement changes it, so the result equals a
start-of-statement snapshot. A statement therefore costs O(rows it touches), not O(delta size).
That matters for loops of single-row prepared statements over a large delta (#36). A delta
without a bound PK index falls back to a full snapshot scan.

`NOT NULL`, `CHECK`, PK, and (on keyed tables) `UNIQUE` are validated against the **merged**
state, so a scenario insert colliding with an untouched base row is caught. Conflicts with base
rows and conflicts with scenario rows raise distinct messages.

The merged relation is *delta `I`/`U` rows* plus *base rows without a delta row for their key*,
so uniqueness splits into three checks:

- **Among scenario rows:** the delta's mirrored `UNIQUE` indexes. Tombstones carry NULL
  payloads and never collide.
- **Among untouched base rows:** the base's own indexes.
- **Scenario row vs. base row:** `ScenarioBaseUniqueProbe` (`scenario_unique.cpp`) probes the
  base ART indexes.
  - A collision counts only if the owning base row is still live.
  - Base rows the scenario *replaced* (`D` or `U` for their key, including this statement's
    unflushed changes) are masked through a delete index holding their (values, rowid). This is
    the mechanism core uses for a transaction's own deletes.
  - So a value freed by a scenario delete or update can be reused, and swaps work within one
    statement.
  - The mask is built only after the fast, unmasked probe reports a collision.
  - Merge-back registers every deleted base row in the transaction's delete indexes before it
    appends, so the base accepts the reused values too.

`ScenarioCatalog::BeginWrite` is the chokepoint enforcing the single-writer rule, the read-only
checks and the frozen flag. The sinks then create the delta on their first chunk
(`GetOrCreateWriteDelta`, see *Delta lifecycle*).

### Write attribution

DuckDB allows one modified attached database per transaction
(`MetaTransaction::ModifyDatabase`, duckdb `src/transaction/meta_transaction.cpp`). The DML
binders register the target table's catalog as modified (`StatementProperties::RegisterDBModify`
in `bind_insert/update/delete/merge_into.cpp`) unless the table entry is `temporary`, and
`ClientContext::CheckIfPreparedStatementIsExecutable` turns that registration into a
`ModifyDatabase` call before execution. Left alone, a scenario statement would make the
*scenario* database the transaction's writer, and a host write (or a write to a second scenario)
in the same transaction would fail, although only the host is ever physically modified.

`ScenarioTableEntry` therefore sets `temporary = true` on the entry the binder resolves. The
binder then skips the registration (INSERT registers a read instead, so prepared statements still
re-bind on catalog changes), and every sink calls `BeginWrite` from `GetGlobalSinkState`, which
ends in `MarkHostWrite` -> `ModifyDatabase(host)`. Host and scenario writes thus name the same
writer. A transaction that already modified an unrelated database is refused by `MarkHostWrite`
itself, naming the scenario (DuckDB's own check would only name the host). Sink global states
are created by `PipelineInitializeTask`s, which can run on worker threads in parallel (e.g. the
UNION of INSERTs `COPY FROM DATABASE` plans), and `ModifyDatabase` / `SetModifications` are not
synchronized, so `BeginWrite` serializes behind a per-connection mutex
(`ScenarioClientState::write_lock`); it runs once per sink, not per chunk. Both are state of one
transaction, which belongs to one connection, so writers on other connections or databases never
wait on it. The flag has no storage effect on
scenario entries - `temporary` only matters for entries stored in a `CatalogSet` and logged to
the WAL, and scenario entries live in neither.

Because the binder-time checks no longer see the scenario catalog, `BeginWrite` restores them in
`ThrowIfNotWritable`, next to the frozen check and with DuckDB's own wording: read-only
transaction, scenario attached `READ_ONLY`, host attached read-only. They run when the sink is
initialized - before any row flows, so zero-row writes fail too, but at `EXECUTE` rather than
at physical planning, which `PREPARE` performs: a write prepared in a read-only transaction
stays executable later, as in core. `MarkHostWrite` repeats the read-only checks as a backstop.

Introspection reads the flag as well (`duckdb_tables().temporary`, and from it
`information_schema.tables.table_type` / `commit_action` and `SHOW [ALL] TABLES`). So each
transaction caches *two* entries per table in `ScenarioTransaction`: the binder's, from
`LookupEntry`, flagged temporary; and the enumeration one, from `ScenarioSchemaEntry::Scan`,
which is a plain `BASE TABLE` (kept in a separate map, `enumeration_entries`, so no table name
can collide with it). This split holds because DuckDB
resolves DML targets only through `Catalog::GetEntry` -> `SchemaCatalogEntry::LookupEntry`
(`bind_insert.cpp:538`, `bind_basetableref.cpp:163/229` for UPDATE/DELETE/MERGE). `Scan` feeds
only enumeration: `duckdb_tables.cpp:83`, `duckdb_columns.cpp:91`, `duckdb_constraints`, and
`bind_export.cpp:171`, which reads. If a DML statement ever bound an enumerated entry, it would
only fall back to registering the scenario database as the writer; `MarkHostWrite` handles that
case. `GetInfo` also clears the flag, so `ToSQL` stays a plain `CREATE TABLE`. Commit, WAL,
checkpoint and write-write conflict detection are the host `DuckTransaction`'s, unchanged.

---

## Delta lifecycle

Delta tables are created **lazily**, by the first write to a scenario table (#34). `scenario_create`
in delta mode only inserts the registry row (and one `scenario_tables` row per declared key), so it
costs the same for 3 tables as for 3,000. Materialized mode still copies every base table at
creation - that copy is its point - but its deltas are lazy too.

**Creation.** Every write sink (insert, update, delete; `MERGE INTO` and `INSERT ... ON CONFLICT`
run through them) calls `ScenarioCatalog::GetOrCreateWriteDelta` with its first non-empty chunk.
That is at execution, after `BeginWrite` has run its checks and made the host the transaction's
writer, so a frozen scenario, a read-only transaction, or a transaction that already wrote another
database (the single-writer error) fails before anything is created. A statement that writes no
rows never reaches it and creates nothing. The delta is created in the caller's transaction:
`ROLLBACK` removes it, and the same transaction sees it at once. Creating a catalog entry bumps the
host catalog version, so prepared statements rebind in that transaction.
`GetOrCreateWriteDelta` runs under the same per-connection mutex as `BeginWrite`, because sibling
sinks of one plan (a `MERGE`'s actions, the INSERTs of `COPY FROM DATABASE`) may reach it together.

`COPY FROM DATABASE ... TO <scenario>` is the one statement that registers the *scenario* catalog
as the transaction's writer (its binder registers the target catalog directly). `MarkHostWrite`
then only flags the host transaction read-write, and DuckDB's schema refuses to add an entry to a
database that is not the registered writer when the `CatalogTransaction` carries a client context
(`DuckSchemaEntry::AddEntryInternal`). For that case the delta is created through a
`CatalogTransaction` of the host transaction without the context. The entry still lands in the
host transaction's undo buffer, so rollback, commit and the WAL behave as usual.

**Identity.** `ScenarioDelta::GetKeyColumns` resolves a table's row identity. The first write
chooses it in this order and creates the delta with exactly that shape:

1. a row in `__anofox_scenario.scenario_tables` - a key declared with `key_columns :=` at
   `scenario_create` or `scenario_refresh`. It wins over a base `PRIMARY KEY` added after the
   declaration;
2. the base table's `PRIMARY KEY`;
3. none: keyless, and the first write creates a bag delta.

Once a delta exists, **its shape is authoritative**: its `PRIMARY KEY` by position, or none for a
bag delta. Scenarios created before #34 recorded declared keys only there, so they need no
migration. The declared key and the delta only disagree after a race or a base schema change, and
then readers follow the rows that are actually stored. Example: con1 binds a keyless table, con2
commits a `scenario_refresh` key declaration, con1's first write then creates a bag delta and
commits. The `scenario_tables` insert and the `CREATE TABLE` do not write-conflict, so both commit,
and the table stays keyless. Its stale `scenario_tables` row is never consulted.

The table entry resolves its identity once per transaction. If a key was declared after the
transaction bound the table (`scenario_refresh` in the same transaction), the shapes would
disagree, so the write fails with a retryable `TransactionException`. A declared key column that
was later renamed or dropped in the base is tolerated while binding (the table reads as a plain
base passthrough, and introspection keeps working). The first write and `scenario_diff` raise
the error, and `scenario_refresh` may re-declare such a key as long as no delta was built from it.

The declared keys of a scenario are read once per transaction into `ScenarioClientState`
(`scenario_registry.cpp`), because a table entry is built per table and transaction and a scan of
`scenario_tables` per entry would cost O(tables x declarations) per enumeration. The transaction's
snapshot changes only through its own writes, which invalidate the cache.

`scenario_tables` itself is created by the first key declaration on a host, in the caller's
transaction. Two transactions declaring the first keys concurrently collide on that `CREATE
TABLE`: the loser gets a retryable `TransactionException` naming its scenario.

**Concurrency.** Two transactions making the first write to the same table both try to create
`s<id>_delta_<table>`. DuckDB's catalog lets only one of them create it: the other gets a
write-write conflict, whether the winner has committed yet or committed after the loser's
snapshot. `GetOrCreateWriteDelta` turns that into a `TransactionException` that names the
scenario and the table and asks for a retry. The loser's transaction is aborted and nothing is
half-created.

**Bases.** The delta always lives in the host. For a DuckLake base, the scenario's base is the
lake *at its creation time* (a branch keeps its parent's). The scenario schema mirrors the live
lake catalog, so a table created in the lake later is visible by name. Its first write therefore
looks the table up `AT (TIMESTAMP => created_at)` (`ScenarioDelta::GetPinnedBaseEntry`) and
refuses one that isn't in that snapshot, or whose columns differ there from the live table (the
delta is laid out like the live entry while the scan reads the snapshot). `scenario_diff` resolves
the table the same way, so a later table is "not found in scenario". `scenario_refresh` and a
branch's `key_columns` must name a table of the snapshot. Overlay scenarios pick up tables
added to the host base automatically. Materialized scenarios only see their copies.

**Readers.** A missing delta is an empty changelog everywhere:

- The scan binds no delta and passes the base through. Point lookups and the suppression probe
  only run when a delta exists.
- `scenario_diff` canonicalizes the table name first (`main.t` and `MAIN.T` are `t`), then
  substitutes an empty relation in the delta layout. `scenario_diff_summary`,
  `scenario_merge_preview` and `scenario_merge` enumerate the deltas that exist.
- Branching copies only the parent deltas that exist (each into a child delta of the same
  shape), plus the parent's `scenario_tables` rows. A child's declaration that differs from the
  parent's declared key or delta shape is refused.
- `scenario_drop` drops the deltas that exist and the scenario's `scenario_tables` rows. Freeze
  doesn't touch deltas.
- `scenario_migrate` creates a v2 delta only for a legacy delta it folds in. Migrated snapshots get
  their materialized copies and no deltas.
- The keyless legacy upgrade at `ATTACH` rewrites only the deltas that exist.

`scenario_refresh` is deprecated. It no longer creates deltas. It only records `key_columns` for
keyless tables that are not tracked yet (no `scenario_tables` row and no delta), or replaces a
declared key whose column vanished from the base before any delta was built from it.

---

## Storage layout

All state lives in the host database. That is what keeps a `.duckdb` file self-contained — copy
the file and the branches travel with it.

```
__anofox_scenario.registry              scenario_id, name, mode, frozen, parent_id,
                                        base_snapshot_id, created_at, merged_at, description
__anofox_scenario.registry_seq          id sequence
__anofox_scenario.scenario_tables       scenario_id, table_name, key_columns VARCHAR[]
                                        (PK scenario_id, table_name): declared keys, created
                                        lazily by the first declaration
__anofox_scenario.s<id>_delta_<table>   the changelog described above, created by the
                                        table's first write
__anofox_scenario.s<id>_mat_<table>     base copies, for mode := 'materialized' (columns, NOT NULL/
                                        CHECK/PK/UNIQUE + data; FOREIGN KEYs stripped: they would
                                        resolve inside this schema, and merge-back enforces them)
```

`<table>` is the logical name: bare for `main`, otherwise `<schema>.<table>`. `.` is the separator,
so a part that contains a dot (or starts with `"`) is double-quoted SQL-style, with inner quotes
doubled (`ScenarioDelta::EncodeLogicalName` / `SplitLogicalName`). `"a.b"` is the `main` table
`a.b`, while `a.b` is table `b` in schema `a`. Every other name is written exactly as before, so
existing databases keep their internal table names. The registry stores no table list: a
scenario covers every table of its base, the internal delta names record which tables it has
written, and `scenario_tables` records declared keys, keyed by the same logical name. User-facing
table names (`scenario_diff`, `key_columns`, `table_name` outputs) use the same spelling.

The registry and `scenario_tables` are manipulated in the **caller's** transaction, so a
rolled-back `scenario_create` leaves nothing behind.

---

## Isolation tiers

| Tier | Created by | Isolation from later base changes | Cost |
|------|-----------|-----------------------------------|------|
| `overlay` (default) | `scenario_create(name)` | None — base churn shows through on keys the scenario never touched | O(1): one registry row, deltas on first write |
| `materialized` | `mode := 'materialized'` | Complete | Full copy of every base table |
| `snapshot` | `base := '<ducklake attach>'` | Complete — reads pinned via `AT (TIMESTAMP => created_at)` | O(1), no data copied |

Overlay is the default because it is cheapest and fits the common case: a short-lived what-if
against a base nobody is concurrently editing. The leak-through is documented rather than
prevented, because preventing it means paying for one of the other two tiers.

Every reader of a `snapshot` scenario applies the same pin: the scan (a `BoundAtClause` on the
foreign scan), merge-back and its preview (the 3-way drift reference), and `scenario_diff`. The diff
reads the base `AT (TIMESTAMP => created_at)` for `old_value` and for the merged relation, and its
`'main'` side is the scenario's origin in its base catalog, pinned for DuckLake and live for
unversioned bases.

---

## Merge-back

`scenario_merge` replays the delta onto the base — `I` becomes an insert, `U` an update, `D` a
delete. `scenario_merge_preview` runs the same planning with no side effects.

Conflict detection depends on whether the scenario has a creation snapshot:

- **Overlay** — detects an `insert` whose key now exists in the base, or an `update` whose key
  has vanished from it.
- **Materialized / DuckLake** — additionally detects three-way *drift*: a base row that changed
  since the snapshot, on a key the scenario also touched.
  Keyless tables of both tiers get the copy-count form of drift (below).

`on_conflict` selects `abort` (default), `ours`, or `theirs`. Host bases apply atomically in the
caller's transaction. DuckLake bases apply atomically on the lake side in their own transaction,
because there is no cross-catalog 2PC — hence the autocommit requirement.

Keyless (bag) tables merge by net multiplicity per distinct row: the delta's `I`/`D` rows are
summed (`_count`, or an implicit `1` per row for a legacy pre-v0.4.1 delta that was never
upgraded) per whole-row value. A net of `-n` deletes exactly `n` base copies, a net of `+n`
appends `n` copies. Host bases pick the copies by scanning rowids. DuckLake bases do the same in
SQL on the lake connection: DuckLake exposes stable `rowid`s, so the apply is
`DELETE ... WHERE rowid IN (...)` over `row_number() OVER (PARTITION BY <all columns> ORDER BY rowid)`
joined NULL-safely (`IS NOT DISTINCT FROM`) to the per-value plan, followed by an `INSERT` of the
net-positive copies. A lake table with a user column named `rowid` (any case) shadows the
pseudo-column, so for such tables the apply deletes *every* copy of each affected value and
re-inserts the `now - del` survivors - the same multiset, since copies are indistinguishable.
Either way the deleted count is checked against the plan (`sum(__anofox_del)`); a mismatch rolls
the lake transaction back.

Drift for a keyless row of a snapshot scenario (DuckLake, or materialized on the host, where the
`s<id>_mat_<table>` copy supplies the snapshot count): the scenario deletes copies of a value
whose current base count differs from its count at the snapshot (copies were added or removed
underneath — a removed copy may be the one the scenario removed, so the outcome is ambiguous).
`abort` refuses, `theirs` leaves that value's base copies alone, and `ours` sets the base's count
to the scenario's (`snapshot count + net`), deleting or re-adding copies as needed. Net inserts
never conflict. On DuckLake all counts are captured in temp tables before the lake is touched;
on the host they come from one scan of the base and one of the mat copy, before any write.

`scenario_merge_preview` lists the same per-value net plan for keyless tables: one row per copy
the merge deletes or inserts, `key` = the row's values rendered as text (`CAST(row(...) AS
VARCHAR)`), `conflict` per value from the same count comparison. Values whose inserts and deletes
cancel out are not listed.

### Merge-back into DuckLake: the two-commit window

The lake apply commits on its own connection before the caller's transaction does the host
bookkeeping (empty the delta, `merged_at`, `frozen`). If that bookkeeping fails — a crash, or a
host write-write conflict on the registry row — the lake holds the changes while the scenario
still looks unmerged. A plain re-merge is not safe: keyed rows and keyless deletes surface as
conflicts, but keyless net inserts never conflict and would be appended a second time.

So the lake commit is tagged: just before committing, the merge calls
`<lake>.set_commit_message('anofox_scenario', ..., extra_info => tag)` with
`tag = 'anofox_scenario.merge/v1 scenario=<id> created=<created_at µs> delta=<md5> tables_changed=..
rows_applied=.. conflicts_resolved=..'`. `delta` fingerprints every delta table
(`count(*)` and `sum(hash(row))`, per table, sorted by name). Every DuckLake merge first looks for
its `scenario=/created=` prefix in `ducklake_snapshots(<lake>)`:

- **No tag** — normal apply.
- **Tag with a matching fingerprint** — the apply already happened. The lake is left alone, the
  host bookkeeping runs, and the counts from the tag are reported.
- **Tag with a different fingerprint** — the scenario was written to after the failed bookkeeping.
  The merge refuses rather than discard those writes; they must be merged by hand.

The tag lives in the lake's own snapshot metadata, so it commits atomically with the apply and needs
no extra table. Non-DuckLake cross-catalog bases have no commit metadata and are not tagged.

On success the scenario ends frozen, with `merged_at` set and an empty delta.

---

## Source map

| Path | Responsibility |
|------|----------------|
| `anofox_scenario_extension.cpp` | Entry point, function registration |
| `catalog/scenario_storage_extension.cpp` | `ATTACH (TYPE scenario)`, base source resolution |
| `catalog/scenario_catalog.cpp` | Catalog impl, `BeginWrite`/`MarkHostWrite` write chokepoint |
| `catalog/scenario_schema_entry.cpp` | Synthetic `main` schema, DDL rejection |
| `catalog/scenario_table_entry.cpp` | Base mirror, virtual identity columns |
| `catalog/scenario_scan.cpp` | Merge-on-read scan |
| `catalog/scenario_insert.cpp` | Insert sink, PK collision policy |
| `catalog/scenario_update_delete.cpp` | Update/delete sinks, op-transition matrix |
| `catalog/scenario_merge_into.cpp` | `MERGE INTO` support |
| `catalog/scenario_delta.cpp` | Delta and materialized table management |
| `catalog/scenario_registry.cpp` | Registry and `scenario_tables`, caller-transaction catalog ops |
| `catalog/scenario_transaction.cpp` | Transaction shim |
| `lifecycle/scenario_lifecycle.cpp` | create / drop / freeze / unfreeze / list / refresh (deprecated) |
| `lifecycle/scenario_diff.cpp` | Streaming diff engine |
| `lifecycle/scenario_merge.cpp` | Merge-back planning and apply |
| `lifecycle/scenario_migrate.cpp` | Legacy v0.1 → v2 migration |

Tests live in `test/sql/`, one file per concern: `attach_*.test` for catalog behaviour,
`scenario_*.test` for lifecycle, diff, and migration.

---

## Glossary

| Term | Definition |
|------|------------|
| **Scenario** | A named branch over the base data |
| **Base** | The original tables, never written by the extension |
| **Delta** | Per-(scenario, table) changelog of `I`/`U`/`D` rows |
| **Merge-on-read** | Reconstructing a scenario's view by scanning delta first, then base |
| **Overlay** | The default mode: a delta over the live base |
| **Materialized** | A scenario holding a full copy of every base table |
| **Frozen** | A scenario that rejects writes; reads still work |
| **Bag changelog** | The keyless-table delta variant, using `_count` multiplicity |
