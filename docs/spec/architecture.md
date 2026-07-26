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
- **Constraints, types, and binder errors are DuckDB's**, not reimplementations.
- **Transactions ride the host's.** Scenario DML runs in the caller's transaction, so `ROLLBACK`
  undoes scenario writes for free and ACID is inherited rather than rebuilt.

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

### Keyless tables

Without a PK or declared `key_columns` there is nothing to match on, so the delta becomes a
**bag changelog**: no PK, plus a trailing `_count BIGINT` multiplicity, with readers aggregating
inserts and deletes per distinct value.

This supports `INSERT`/`UPDATE`/`DELETE` with bag semantics, but cannot support `scenario_diff`,
`MERGE INTO`, or `ON CONFLICT` — all three need row identity. Those are rejected with an error
pointing at `key_columns :=`.

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

`NOT NULL`, `CHECK`, PK, and (on keyed tables) `UNIQUE` are validated against the **merged**
state, so a scenario insert colliding with an untouched base row is caught. Conflicts with base
rows and conflicts with scenario rows raise distinct messages.

`ScenarioCatalog::MarkHostWrite` is the chokepoint enforcing the single-writer rule and the
frozen flag.

---

## Storage layout

All state lives in the host database. That is what keeps a `.duckdb` file self-contained — copy
the file and the branches travel with it.

```
__anofox_scenario.registry              scenario_id, name, mode, frozen, parent_id,
                                        base_snapshot_id, created_at, merged_at, description
__anofox_scenario.registry_seq          id sequence
__anofox_scenario.s<id>_delta_<table>   the changelog described above
__anofox_scenario.s<id>_mat_<table>     full base copies, for mode := 'materialized'
```

`<table>` is the logical name: bare for `main`, otherwise `<schema>.<table>`. Since `.` is the
naming-contract separator, table and schema names containing a dot are rejected up front at
`scenario_create`.

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
