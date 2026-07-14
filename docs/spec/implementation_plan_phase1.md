# Implementation Plan — Phase 1: Transparent Scenario DML via a Scenario Catalog

**Status:** Approved-for-planning draft | July 2026
**Implementation deviations (July 2026, discovered during WP3):**
1. **Delta tables are created eagerly by `scenario_create`** (one empty table per base table),
   not lazily on first write. DuckDB's single-writer-per-transaction rule
   (`MetaTransaction::ModifyDatabase` + the `DuckSchemaEntry::AddEntryInternal` assertion)
   forbids creating catalog entries in the *host* database while the statement's registered
   write target is the *scenario* catalog — and the bind-time registration cannot be rewritten
   after `Planner::CreatePlan` copies the statement properties. Eager creation keeps all delta
   DDL in `scenario_create`'s transaction (where the host **is** the modified database), keeps
   EXPLAIN side-effect free, and is O(#tables) metadata only. Consequences: tables created in
   the base after `scenario_create` are read-only in the scenario (clear error, lifted in
   Phase 2); row data cost is unchanged (~0 for empty deltas).
2. **Host writes and scenario writes cannot mix in one explicit transaction** (clear
   TransactionException; DuckDB's single-writer rule sees two attached databases even though
   the rows physically land in one). Scenario DML marks the host transaction read-write
   directly (`ScenarioCatalog::MarkHostWrite`) without claiming a second modified-database slot.
3. **Row identity for UPDATE/DELETE is `__scenario_origin` + `__scenario_key_<k>` (PK values)**
   rather than `__scenario_delta_rowid`: `Binder::BindRowIdColumns` requires virtual columns,
   and PK identity lets the sinks drive the whole op-transition matrix from one delta key map.
**Prerequisite reading:** `docs/spec/diagnosis_and_redesign.md` (§2.2 for why the catalog layer
is the only viable substrate; §3 for the target architecture).
**Scope:** The *Critical* missing parts only — REQ-COW-001..007 (transparent read/write,
constraint enforcement, DDL rejection), enforced freeze/archive, and PK-collision correctness.
Diff-engine streaming, lifecycle consolidation, and the DuckLake isolation tier are Phases 2–4
and are out of scope here.

All API references below were verified against the pinned `duckdb` submodule
(`08e34c447`, v1.5.4 line) — file:line citations are to that tree.

---

## 1. Target behavior (acceptance criteria, written first)

```sql
LOAD anofox_scenario;
CALL scenario_create('optimistic', 'demand +10%');

ATTACH 'optimistic' AS opt (TYPE scenario);

SELECT count(*) FROM opt.forecast;                 -- == main.forecast (nothing modified yet)
INSERT INTO opt.forecast VALUES ('NEW-01', DATE '2026-01-01', 500, 0.9);
UPDATE opt.forecast SET qty = qty * 1.10 WHERE material_group = 'PLST';
DELETE FROM opt.forecast WHERE obsolete;

SELECT count(*) FROM main.forecast;                -- unchanged: base is never touched
CREATE TABLE opt.scratch(i INT);                   -- error: DDL not supported in scenarios

CALL scenario_freeze('optimistic');
UPDATE opt.forecast SET qty = 0;                   -- error: scenario is frozen

BEGIN; INSERT INTO opt.forecast VALUES (...); ROLLBACK;
SELECT count(*) FROM opt.forecast;                 -- rollback took effect (no side connection)

DETACH opt;                                        -- handle gone, scenario data persists
```

Every line above becomes a SQLLogicTest before the code that makes it pass (RED first).

## 2. Verified integration points

| Hook | Where (pinned submodule) | Use |
| --- | --- | --- |
| `StorageExtension { attach, create_transaction_manager }` | `storage/storage_extension.hpp:31` | `ATTACH '<name>' (TYPE scenario)` returns our catalog. Registered via `StorageExtension::Register(config, "scenario", ...)`; dispatch by `db_type` confirmed at `main/database_manager.cpp:385`. |
| `Catalog` pure virtuals | `catalog/catalog.hpp:116–452` | Must implement: `Initialize`, `GetCatalogType`, `CreateSchema`, `LookupSchema`, `ScanSchemas`, `DropSchema`, `PlanCreateTableAs`, `PlanInsert`, `PlanDelete`, `PlanUpdate`, `GetDatabaseSize`, `InMemory`, `GetDBPath`. `PlanMergeInto` (`:318`) is virtual → throw NotImplemented in v1. |
| `SchemaCatalogEntry` pure virtuals | `catalog_entry/schema_catalog_entry.hpp:55–108` | All `Create*` / `Alter` / `DropEntry` → the single clear DDL error (deletes `ddl_blocker.cpp`). `Scan`/`LookupEntry` enumerate base tables. |
| `TableCatalogEntry` virtuals | `catalog_entry/table_catalog_entry.hpp:82–131` | `GetScanFunction` (pure), `GetStatistics` (pure), `GetStorageInfo` (pure), `BindUpdateConstraints` (`:120`), `GetVirtualColumns`/`GetRowIdColumns` (`:129–131`). |
| Scan binding path | `binder/tableref/bind_basetableref.cpp:231–268` | **Verified: `bind_replace` is NOT consulted for catalog table scans** (only for explicit table functions, `bind_table_function.cpp:223`). The scan must be a genuine `TableFunction` producing chunks. |
| DML binding path | `binder/statement/bind_insert.cpp:538` | `Catalog::GetEntry<TableCatalogEntry>` — confirms entries must be TABLE_ENTRY (views are rejected at bind; the old OptimizerExtension plan is dead). |
| `LogicalInsert` | `planner/operator/logical_insert.hpp:24,64` | `action_type` (ON CONFLICT) and `return_chunk` (RETURNING) — both gated with NotImplemented in v1. |
| `TransactionManager` pure virtuals | `transaction/transaction_manager.hpp:35–41` | Thin shim (§4.1): real ACID rides the host catalog's DuckTransactionManager. |

## 3. What gets built (work packages)

New sources under `src/catalog/` (added to `CMakeLists.txt`); legacy files untouched until
Phase 2. Suggested worktree per CLAUDE.md: `git worktree add ../worktrees/attach-catalog
feature/attach-catalog`.

```
src/catalog/
  scenario_storage_extension.cpp   -- WP0: attach + txn manager registration
  scenario_transaction.cpp         -- WP0
  scenario_catalog.cpp             -- WP1
  scenario_schema_entry.cpp        -- WP1
  scenario_table_entry.cpp         -- WP2 (entry + statistics + virtual columns)
  scenario_scan.cpp                -- WP2 (merge-on-read table function)
  scenario_insert.cpp              -- WP3 (physical sink)
  scenario_update.cpp              -- WP3
  scenario_delete.cpp              -- WP3
  scenario_delta.cpp               -- WP3 (delta DDL + op-transition helpers)
src/lifecycle/
  scenario_lifecycle.cpp           -- WP4 (create/drop/freeze/unfreeze as CALL table functions)
```

### WP0 — Attach plumbing and transaction shim (~2 days)

- `ScenarioStorageExtension`: `attach` parses the path as the scenario name, resolves it in the
  registry (which lives in the **host** DuckDB catalog), returns
  `make_uniq<ScenarioCatalog>(db, scenario_name, host_catalog_name, scenario_id, frozen)`.
  Registered in `LoadInternal` with `StorageExtension::Register(config, "scenario", ...)`.
- `ScenarioTransactionManager`: **deliberately trivial.** All physical state (registry, delta
  tables) lives in the host catalog, so atomicity, MVCC, and rollback are provided by the host
  `DuckTransactionManager` inside the same `MetaTransaction`. `StartTransaction` returns a
  plain `Transaction` subclass; `Commit`/`Rollback` are no-ops; `Checkpoint` no-op. Precedent:
  `sqlite_scanner`'s manager.
- **Known sharp edge (test early):** when a statement's *target* is the scenario catalog but
  writes physically land in the host catalog, the host must be registered as modified in the
  meta-transaction. Call `MetaTransaction::Get(context).ModifyDatabase(host_db)` inside the
  three `Plan*` hooks. The rollback acceptance test in §1 exists to pin exactly this.

### WP1 — Catalog and schema entry (~2 days)

- `ScenarioCatalog`: `GetCatalogType()` → `"scenario"`; one synthetic schema (`main`) returned
  by `LookupSchema`/`ScanSchemas`; `CreateSchema`/`DropSchema` → NotImplemented; `InMemory()`
  false; `GetDBPath()` → `scenario:<name>`; `GetDatabaseSize` → sum of delta table sizes from
  the host catalog (real numbers, replacing the fabricated `COUNT(*)*100`).
- `ScenarioSchemaEntry`: every `Create*`, `Alter`, `DropEntry` throws
  `NotImplementedException("DDL operations are not permitted in scenarios. Modify the base
  schema, then create a new scenario. (REQ-COW-007)")`. `Scan`/`LookupEntry` enumerate the
  host catalog's user tables (excluding `__anofox_scenario` internals) and wrap each in a
  `ScenarioTableEntry` built on demand — columns/constraints copied from the base
  `TableCatalogEntry`, so scenario tables always mirror the live base schema. No entry cache
  in v1; `GetCatalogVersion` forwards the host catalog's version so plan caching stays sound.
- Tables created in the base *after* scenario creation: visible read-only overlay per §3.3 of
  the redesign (documented divergence from REQ-SCEN-001's snapshot wording).

### WP2 — Merge-on-read scan (~5–7 days, the largest risk item)

Since `bind_replace` is unavailable for catalog scans (verified above), implement the
DuckLake "delete-file" model directly — cheap because the delta side is small by invariant:

- `ScenarioTableEntry::GetScanFunction` returns `scenario_scan` with bind data holding: the
  base `DuckTableEntry`, the delta table's entry if it exists (a scenario table with no delta
  binds to a **plain passthrough of the base scan** — the zero-modification case stays at
  native speed), PK column indices, and projected columns.
- **Global init:** scan the delta table via the host `DataTable::Scan` API in the current
  transaction. Materialize `_op IN ('I','U')` rows into a `ColumnDataCollection`; build a hash
  set of PK keys for `_op IN ('U','D')` (the suppression set).
- **Execute:** stream the delta collection first, then stream base chunks and mask out rows
  whose PK is in the suppression set via a selection vector.
- **Pushdown policy v1:** projection pushdown forwarded to the base scan (always fetch PK
  columns internally); filter pushdown OFF (`filter_pushdown=false`) — the optimizer applies
  filters above the scan; correct, and the delta side is negligible. `max_threads=1`.
  **v1.1 (same WP, stretch):** forward filters to the base scan and apply them to the delta
  collection; expose base-scan parallelism with the (read-only) suppression set shared.
- **Row identity for DML:** `GetVirtualColumns`/`GetRowIdColumns` expose two virtual columns:
  `__scenario_origin` (TINYINT: 0=base, 1=delta) and `__scenario_delta_rowid` (BIGINT, valid
  when origin=1). Combined with the PK (a regular column), this is everything the update/
  delete sinks need.
- `GetStatistics` → base statistics (conservatively widened; deltas are small);
  `GetStorageInfo` → synthesized from base PK info (drives `ON CONFLICT` metadata paths).

**Scoped decision (document in API_REFERENCE):** tables without a PK are readable and
insertable in v1; `UPDATE`/`DELETE` on them throws
`NotImplementedException("UPDATE/DELETE in scenarios requires a PRIMARY KEY on the base table
(v1 limitation)")`. This removes the whole-row-identity swamp from the critical path;
REQ-COW-008 already declares no-PK targeting implementation-defined.

### WP3 — Write path (~6–8 days)

Delta table contract (per base table, created **lazily on first write**, in the host catalog,
in the caller's transaction): schema `__anofox_scenario`, name `s<scenario_id>_delta_<table>`,
columns `_op VARCHAR CHECK (_op IN ('I','U','D'))`, `_ts TIMESTAMP DEFAULT now()`, all base
columns, `PRIMARY KEY (<base pk>)`. No `_version`, no redundant secondary index (the PK's ART
index is the anti-join/upsert index).

The **op-transition matrix** is the correctness core; every cell gets a dedicated test:

| Existing delta state for PK | UPDATE arrives | DELETE arrives |
| --- | --- | --- |
| none (base row) | write `('U', new full row)` | write `('D', pk)` tombstone |
| `I` | stay `I`, new values (still a scenario-insert) | **remove** the delta row |
| `U` | stay `U`, new values | convert to `D` |
| `D` | unreachable (row not visible) | unreachable |

- **`ScenarioCatalog::PlanInsert`** — gate: frozen → `InvalidInputException("Scenario '%s' is
  frozen...")`; `op.action_type != OnConflictAction::THROW` → NotImplemented ("ON CONFLICT in
  scenarios: v2"); `op.return_chunk` → NotImplemented ("RETURNING: v2"). Ensure delta exists
  (catalog API, same transaction). Emit a `ScenarioInsert` physical sink:
  - Appends `('I', now(), row...)` to the delta `DataTable` via `LocalAppend`.
  - **Constraints:** evaluate the base entry's bound NOT NULL + CHECK constraints per chunk
    (`ExpressionExecutor`); duplicate-in-delta is caught by the delta PK index naturally;
    **duplicate-vs-base** is caught by probing the base PK ART index per chunk before append —
    error message distinguishes "conflicts with base row" vs "conflicts with scenario change"
    (REQ-NFR-005). This closes the silent-duplicate bug from the diagnosis.
- **`ScenarioCatalog::PlanUpdate`** + **`ScenarioTableEntry::BindUpdateConstraints`** — the
  override projects *all* table columns (the same mechanism core uses for
  `update_is_del_and_insert`), so the sink receives complete post-update rows plus the virtual
  identity columns. Sink applies the matrix above: PK-probe the delta index → delete-then-
  append for upsert semantics; re-run NOT NULL/CHECK on the new values. `UPDATE` of a PK
  column: v1 NotImplemented (same restriction core places on `update_is_del_and_insert`
  targets in foreign catalogs; revisit in v2).
- **`ScenarioCatalog::PlanDelete`** — `GetRowIdColumns` supplies identity; sink applies the
  matrix (tombstone / remove-`I` / convert-`U`). `TRUNCATE` arrives as an unfiltered DELETE
  and needs no special code (REQ-COW-007's TRUNCATE clause falls out for free).
- **`PlanCreateTableAs`** → NotImplemented (DDL). **`PlanMergeInto`** → NotImplemented v1 with
  a message pointing at INSERT/UPDATE/DELETE.
- Freeze check sits at the top of all three hooks — `scenario_freeze` becomes real, and
  frozen+materialized later gives snapshots for free (Phase 2).

### WP4 — Minimal lifecycle glue (~3 days)

Only what the catalog core needs; full consolidation stays Phase 2:

- Registry v2 table `__anofox_scenario.registry(scenario_id BIGINT DEFAULT nextval(seq),
  name UNIQUE, frozen BOOL, parent_id, created_at, description)` — created lazily by the first
  `scenario_create`, in the **caller's transaction** via the catalog/relation API (no side
  `Connection`, no load-time writes; `metadata_store.cpp`'s load hook is removed).
- `scenario_create / scenario_drop / scenario_freeze / scenario_unfreeze` as `CALL`-able table
  functions (1-row result), replacing the scalar-function versions for the new path. Create is
  now O(1): one registry row (no schema per scenario needed — delta tables are namespaced by
  `s<id>_` inside the shared internal schema; no rowid capture; no per-table registration).
- `scenario_drop`: delete registry row + `DROP TABLE` each `s<id>_delta_*`; refuse while the
  scenario is attached (check `DatabaseManager` for an attached catalog with that name) and
  refuse if children exist (`parent_id`).
- Legacy functions keep working against the old registry during Phase 1; a
  `scenario_migrate()` helper and the removal of the old API land in Phase 2.

### WP5 — Hardening (~3 days)

- Catalog-version forwarding, DETACH semantics (`OnDetach` no-op — data persists), attach of a
  frozen scenario (read-only), two scenarios attached simultaneously, re-ATTACH after drop →
  clean error, `duckdb_tables()`/`SHOW TABLES` output sanity.
- Concurrency: document single-writer-per-scenario (REQ-NFR-003); host MVCC gives
  read-concurrency for free. Write-write conflicts on the same delta table surface as normal
  DuckDB transaction conflicts — add a test proving they do.
- Performance smoke: merge scan of an unmodified 10M-row table within noise of a raw base scan
  (passthrough path); 1% delta within the design doc §8.3 thresholds.

## 4. Version-targeting decision (needs maintainer sign-off)

CI currently builds v1.4.5 (LTS) and v1.5.4. The catalog surface differs between the lines
(`EntryLookupInfo`, `PlanMergeInto`, virtual-column API). Recommendation: **the new catalog
core targets v1.5.x only**; the v1.4.5 job keeps building the legacy code from a maintenance
branch until Phase 2 ships, then is dropped or upgraded. Contorting the new core with
`__has_include` guards across both lines would roughly double WP2/WP3 cost for a line that the
redesign obsoletes.

## 5. Test plan (TDD order)

New files, written RED-first in this order; the existing suite stays green untouched:

1. `test/sql/attach_basic.test` — ATTACH/DETACH, SHOW TABLES, passthrough scan equality.
2. `test/sql/attach_write.test` — transparent INSERT/UPDATE/DELETE; the six op-transition
   cells; update-of-inserted-row stays `I`; delete-of-inserted-row leaves no trace.
3. `test/sql/attach_isolation.test` — base untouched by scenario DML; two scenarios don't see
   each other; same table modified in both.
4. `test/sql/attach_constraints.test` — NOT NULL, CHECK, PK-vs-base, PK-vs-delta (distinct
   error texts), no-PK table: insert OK, update/delete → v1 error.
5. `test/sql/attach_ddl.test` — CREATE TABLE/VIEW/INDEX, ALTER, DROP, CTAS → one canonical
   error message.
6. `test/sql/attach_txn.test` — BEGIN/ROLLBACK spans scenario DML (the §1 rollback test);
   BEGIN/COMMIT; mixed host+scenario writes in one transaction.
7. `test/sql/attach_freeze.test` — freeze blocks all three DML paths, SELECT still works,
   unfreeze restores.
8. `test/sql/attach_unsupported.test` — ON CONFLICT, RETURNING, MERGE INTO, PK-column UPDATE →
   NotImplemented with the exact guidance strings.
9. `test/sql/attach_perf.test` — passthrough + 1%-delta smoke (loose bounds, CI-safe).

Phase 2 rewrites the legacy `scenario_write`/`delta_create` tests against the new surface and
deletes the old API; not part of this phase.

## 6. Sequencing, estimates, tracking

Dependencies: WP0 → WP1 → WP2 → WP3; WP4 can start parallel to WP2 (needed by WP0's registry
lookup — stub it with a hardcoded registry read until WP4 lands); WP5 last.
Single engineer, focused: **~4–5 weeks**. Two engineers (one on WP2, one on WP3 stubs +
WP4): ~3 weeks.

Beads breakdown to file (`bd` is not installed in this environment; run on a dev machine):

```
bd create --title="Epic: ATTACH-based scenario catalog (Phase 1)" --type=epic --priority=0
bd create --title="WP0: StorageExtension attach + transaction shim + ModifyDatabase test" --type=task --priority=0
bd create --title="WP1: ScenarioCatalog/SchemaEntry, DDL errors, catalog version fwd" --type=task --priority=0
bd create --title="WP2: merge-on-read scan (passthrough, suppression set, virtual identity cols)" --type=task --priority=0
bd create --title="WP2.1: filter pushdown + parallel base scan" --type=task --priority=2
bd create --title="WP3: PlanInsert sink + lazy delta + PK-vs-base probe + constraints" --type=task --priority=0
bd create --title="WP3.1: PlanUpdate/PlanDelete sinks + op-transition matrix" --type=task --priority=0
bd create --title="WP4: registry v2 + CALL lifecycle fns in caller txn" --type=task --priority=1
bd create --title="WP5: hardening, concurrency + perf smoke tests" --type=task --priority=1
bd create --title="Docs: API_REFERENCE ATTACH surface + v1 limitations" --type=task --priority=2
# then: bd dep add <WP1> <WP0>; bd dep add <WP2> <WP1>; bd dep add <WP3> <WP2>; ...
```

## 7. Risks and mitigations

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| Meta-transaction accounting for cross-catalog writes (scenario target, host storage) | Medium — the one genuinely novel integration | `ModifyDatabase` in Plan hooks; `attach_txn.test` written first; fallback is routing writes through a host-side `CatalogTransaction` explicitly. |
| `BindUpdateConstraints` not projecting everything needed on some update shapes (subquery SET, FROM clause) | Medium | Enumerate update shapes in `attach_write.test` up front; the core `update_is_del_and_insert` path is the reference implementation to mirror. |
| Delta materialization in scan init misbehaves inside multi-statement transactions (reading own uncommitted delta writes) | Low — same-transaction `DataTable::Scan` sees local appends | Explicit test: INSERT then SELECT inside one transaction. |
| v1.4/v1.5 API drift doubles work | High if dual-targeted | §4 decision: v1.5-only for the new core. |
| Scan performance regression vs. the old view-based reads | Low (passthrough path + small deltas) | `attach_perf.test`; v1.1 pushdown/parallelism reserved headroom. |
| ON CONFLICT / RETURNING / MERGE demanded earlier than v2 | Medium | Clean NotImplemented messages naming the workaround; the sink architecture leaves room (insert sink already probes PK indexes). |

## 8. Definition of done (Phase 1)

1. Every statement in §1 behaves as shown, verified by the §5 suite in CI on v1.5.4.
2. Base tables are provably never written by scenario DML (isolation test).
3. `GEN=ninja make && make test` green, including the untouched legacy suite.
4. `docs/API_REFERENCE.md` documents `ATTACH (TYPE scenario)`, the v1 limitations
   (no-PK DML, ON CONFLICT, RETURNING, MERGE, PK-column updates), and overlay-read semantics.
5. No metadata writes at extension load; the rollback test passes (no autonomous
   transactions anywhere in the new path).
