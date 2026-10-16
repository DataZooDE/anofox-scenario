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
Its primary key is the base table's PK, or the `key_columns` declared at `scenario_create`.

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
  within one. That re-bind also re-runs plan-time checks such as the frozen flag.
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

For keyed deltas, the claimed set is never materialized (#36). Step 1 emits the `I`/`U` rows
chunk-wise. In step 2, each base chunk probes the delta's `PRIMARY KEY` index
(`ScenarioDeltaIndexProbe`, `scenario_delta.cpp`) and drops the rows whose key has a `U` or `D` row.
A point query therefore pays for the rows it reads, not for the whole delta: a 100k-row delta no
longer costs every single-row `UPDATE`/`DELETE` a full key-set build. Deltas without a bound PK
index, and keyless bag deltas, keep the materialized claim set.

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

The update and delete sinks reject a second touch of the same row number with the keyed path's
`modified the same row twice` error. `PlanMergeInto` hands all of a `MERGE`'s action sinks one
shared touched-row set, so a copy matched once by a `DELETE` action and again by an `UPDATE`
action is caught too.

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

`ScenarioCatalog::MarkHostWrite` is the chokepoint enforcing the single-writer rule and the
frozen flag.

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
re-bind on catalog changes), and every sink calls `MarkHostWrite` before its first physical
write, which calls `ModifyDatabase(host)`. Host and scenario writes thus name the same writer; a
write to an unrelated database still trips DuckDB's check. The flag has no storage effect on
scenario entries - `temporary` only matters for entries stored in a `CatalogSet` and logged to
the WAL, and scenario entries live in neither.

Because the binder-time checks no longer see the scenario catalog, the plan hooks (`PlanInsert`,
`PlanUpdate`, `PlanDelete`, `PlanMergeInto`) restore them in `ThrowIfNotWritable`, next to the
frozen check and with DuckDB's own wording: read-only transaction, scenario attached `READ_ONLY`,
host attached read-only. They run before execution, so zero-row writes fail too, and prepared
statements re-plan per transaction (see *Entries are per transaction*). `MarkHostWrite` repeats
them as a backstop.

Introspection reads the flag as well (`duckdb_tables().temporary`, and from it
`information_schema.tables.table_type` / `commit_action` and `SHOW [ALL] TABLES`). So each
transaction caches *two* entries per table in `ScenarioTransaction`: the binder's, from
`LookupEntry`, flagged temporary; and the enumeration one, from `ScenarioSchemaEntry::Scan`,
which is a plain `BASE TABLE` (cache key prefixed with `\x01`). This split holds because DuckDB
resolves DML targets only through `Catalog::GetEntry` -> `SchemaCatalogEntry::LookupEntry`
(`bind_insert.cpp:538`, `bind_basetableref.cpp:163/229` for UPDATE/DELETE/MERGE). `Scan` feeds
only enumeration: `duckdb_tables.cpp:83`, `duckdb_columns.cpp:91`, `duckdb_constraints`, and
`bind_export.cpp:171`, which reads. If a DML statement ever bound an enumerated entry, it would
only fall back to registering the scenario database as the writer; `MarkHostWrite` handles that
case. `GetInfo` also clears the flag, so `ToSQL` stays a plain `CREATE TABLE`. Commit, WAL,
checkpoint and write-write conflict detection are the host `DuckTransaction`'s, unchanged.

---

## Storage layout

All state lives in the host database. That is what keeps a `.duckdb` file self-contained — copy
the file and the branches travel with it.

```
__anofox_scenario.registry              scenario_id, name, mode, frozen, parent_id,
                                        base_snapshot_id, created_at, merged_at, description
__anofox_scenario.registry_seq          id sequence
__anofox_scenario.s<id>_delta_<table>   the changelog described above
__anofox_scenario.s<id>_mat_<table>     base copies, for mode := 'materialized' (columns, NOT NULL/
                                        CHECK/PK/UNIQUE + data; FOREIGN KEYs stripped: they would
                                        resolve inside this schema, and merge-back enforces them)
```

`<table>` is the logical name: bare for `main`, otherwise `<schema>.<table>`. `.` is the separator,
so a part that contains a dot (or starts with `"`) is double-quoted SQL-style, with inner quotes
doubled (`ScenarioDelta::EncodeLogicalName` / `SplitLogicalName`). `"a.b"` is the `main` table
`a.b`, while `a.b` is table `b` in schema `a`. Every other name is written exactly as before, so
existing databases keep their internal table names. The registry stores no table list: the
internal names *are* the record of which tables a scenario tracks. User-facing table names
(`scenario_diff`, `key_columns`, `table_name` outputs) use the same spelling.

The registry is manipulated in the **caller's** transaction, so a rolled-back `scenario_create`
leaves nothing behind.

---

## Isolation tiers

| Tier | Created by | Isolation from later base changes | Cost |
|------|-----------|-----------------------------------|------|
| `overlay` (default) | `scenario_create(name)` | None — base churn shows through on keys the scenario never touched | O(#tables) metadata |
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

`on_conflict` selects `abort` (default), `ours`, or `theirs`. Host bases apply atomically in the
caller's transaction. DuckLake bases apply atomically on the lake side in their own transaction,
because there is no cross-catalog 2PC — hence the autocommit requirement.

Keyless (bag) tables merge by net multiplicity per distinct row: the delta's `I`/`D` rows are
summed (`_count`) per whole-row value. A net of `-n` deletes exactly `n` base copies, a net of `+n`
appends `n` copies. Host bases pick the copies by scanning rowids. DuckLake bases do the same in
SQL on the lake connection: DuckLake exposes stable `rowid`s, so the apply is
`DELETE ... WHERE rowid IN (...)` over `row_number() OVER (PARTITION BY <all columns> ORDER BY rowid)`
joined NULL-safely (`IS NOT DISTINCT FROM`) to the per-value plan, followed by an `INSERT` of the
net-positive copies. Drift for a keyless DuckLake row: the scenario deletes copies of a value whose
current lake count differs from its count at the snapshot (copies were added or removed
underneath — a removed copy may be the one the scenario removed, so the outcome is ambiguous).
`abort` refuses, `theirs` leaves that value's lake copies alone, and `ours` sets the lake's count to the
scenario's (`snapshot count + net`), deleting or re-adding copies as needed. Net inserts never
conflict. All counts are captured in temp tables before the lake is touched.

On success the scenario ends frozen, with `merged_at` set and an empty delta.

---

## Source map

| Path | Responsibility |
|------|----------------|
| `anofox_scenario_extension.cpp` | Entry point, function registration |
| `catalog/scenario_storage_extension.cpp` | `ATTACH (TYPE scenario)`, base source resolution |
| `catalog/scenario_catalog.cpp` | Catalog impl, `MarkHostWrite`, freeze chokepoint |
| `catalog/scenario_schema_entry.cpp` | Synthetic `main` schema, DDL rejection |
| `catalog/scenario_table_entry.cpp` | Base mirror, virtual identity columns |
| `catalog/scenario_scan.cpp` | Merge-on-read scan |
| `catalog/scenario_insert.cpp` | Insert sink, PK collision policy |
| `catalog/scenario_update_delete.cpp` | Update/delete sinks, op-transition matrix |
| `catalog/scenario_merge_into.cpp` | `MERGE INTO` support |
| `catalog/scenario_delta.cpp` | Delta and materialized table management |
| `catalog/scenario_registry.cpp` | Registry, caller-transaction catalog ops |
| `catalog/scenario_transaction.cpp` | Transaction shim |
| `lifecycle/scenario_lifecycle.cpp` | create / drop / freeze / unfreeze / list / refresh |
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
