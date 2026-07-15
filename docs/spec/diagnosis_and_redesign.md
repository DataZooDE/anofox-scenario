# anofox-scenario: Core Diagnosis and DuckDB-Idiomatic Redesign

**Status:** Proposal (v2 architecture) | July 2026
**Scope:** Full diagnosis of the scenario-management core against the intent in
`docs/features/requirements.md` / `docs/features/design.md`, and a redesign that achieves the
intent with a fraction of the surface area, using the extension points DuckDB actually
provides today (v1.4 LTS / v1.5).

---

## 1. What this extension is actually for

Strip the six components and 36 requirements down and the product is one sentence:

> **Branch the database, edit the branch with ordinary SQL, see the merged state, diff it,
> throw it away.**

Everything else — snapshots, protocols, archive, stats — is garnish around that loop. The two
personas (AI agent, human analyst) share one non-negotiable: *ordinary SQL* on the branch
(`INSERT INTO scen.forecast ...`, `UPDATE scen.forecast SET ...`). REQ-COW-001..004 are marked
**Critical**; they are the product. The Delta-Main storage decision (immutable columnar base +
light row-oriented delta, merge-on-read) is correct for DuckDB physics and should be kept.

## 2. Diagnosis

### 2.1 The core promise is not implemented

The current implementation delivers the garnish and not the meal:

| Promise (Critical reqs) | Reality |
| --- | --- |
| Transparent `INSERT/UPDATE/DELETE` on scenario tables (REQ-COW-002/003/004) | **Not implemented.** There is no write interception anywhere (`grep LOGICAL_INSERT src/` → nothing). Users must call `scenario_write('scen','t','U', {id: 42, ...})` — one row at a time — or hand-insert `('I'|'U'|'D', ...)` rows into `_delta_*` tables. The tests do the latter, which means the test corpus pins the workaround, not the product. |
| Transparent read of merged state (REQ-COW-001) | Partially. Merge views exist, but only after the user manually calls `delta_create(scen, t)` **per table**. A freshly created scenario is an empty schema: `SELECT * FROM _scen_x.t` errors. |
| Snapshot isolation — scenario sees base as of creation (REQ-NFR-004, REQ-SCEN-001) | **Not implemented, but paid for.** `scenario_create` captures every rowid of every table into `_scenario_base_rowids` (O(N) time and storage, on by default) — and then the merge views **never reference the captured rowids**. Scenario reads see the *live* base. The capture is dead weight consumed only by `scenario_validate`'s advisory report. This is the worst point in the trade-off space: O(N) creation cost *and* no isolation. |
| Branching inherits source modifications (REQ-SCEN-002) | **Broken.** `scenario_branch` copies registry rows, `_scenario_tables` rows and rowids — but not the delta tables and not the views. The branch is an empty, unqueryable schema; the lifecycle tests only assert metadata, so this passes CI. |
| Archived scenarios reject writes (REQ-SCEN-005) | **Not enforced.** Archive flips a status column; nothing checks it (there is no write path to check it in). |
| Snapshot of `main` before a meeting (REQ-SNAP-001, UC-03) | **Impossible.** `snapshot_create(scenario, name)` only snapshots an existing *scenario*; the spec's primary snapshot use case — freeze `main` — has no code path. |
| PK uniqueness across base + delta (REQ-COW-002/006) | **Violated silently.** The delta table's PK only dedupes within the delta. Insert a delta row with `_op='I'` and a PK that exists in base: the merge view (`base WHERE NOT EXISTS (delta with op U/D) UNION ALL delta WHERE op IN (I,U)`) returns **both rows** — a duplicate-PK result set with no error. |

### 2.2 The designed write path cannot work — even in principle

`docs/features/design.md` §5.2 commits to `OptimizerExtension.pre_optimize_function` rewriting
`LogicalInsert/Update/Delete` against scenario views into delta operations. This has a fatal
flaw: **DuckDB's binder rejects DML against views before the optimizer ever runs.**
`INSERT INTO _scen_x.forecast ...` fails at bind time ("not a table" / "can only update base
tables"); there is no plan for an optimizer extension to rewrite. DuckDB has no `INSTEAD OF`
triggers and no updatable views. So the architecture as designed dead-ends: reads are views,
and views can never accept the writes that the same design routes through the optimizer.

The one API DuckDB *does* provide for exactly this job is the **catalog layer**: a
`StorageExtension` registered for an `ATTACH` prefix returns a custom `Catalog` whose
`TableCatalogEntry` subclass controls both sides —

- `GetScanFunction()` → serve merge-on-read for scans,
- `Catalog::PlanInsert / PlanUpdate / PlanDelete` → route DML into delta storage,
- unimplemented DDL → clean `NotImplementedException` for free.

This is not exotic; it is the established pattern of `sqlite_scanner`, `postgres_scanner`,
`duckdb-iceberg`, and above all **DuckLake**, whose entire value proposition (transparent DML
over snapshot-versioned storage with a SQL metadata catalog) is a superset of this extension's
mechanics. The design doc itself lists this as "v0.2, Phase 7" — it is not an enhancement, it
is the only viable substrate for the Critical requirements, and it should be Phase 1.

### 2.3 Architectural misfits with DuckDB

These are structural, not bugs-to-patch:

1. **Procedures as scalar functions.** `scenario_create/drop/branch/archive/...` are
   `ScalarFunction`s with DDL side effects. Scalar functions may be evaluated per chunk, in
   parallel, inside `WHERE` clauses, and their bind/execute split creates TOCTOU races (the
   existence check runs at bind, the mutation at execute). DuckDB's idiom for verbs is a
   table function invoked via `CALL` (cf. `ducklake_*`, `pragma_*`).

2. **Autonomous side-connections.** Every operation opens `Connection con(db)` — a *separate
   transaction*. Consequences: `scenario_create` inside a user transaction commits even if the
   user rolls back; the internal `BEGIN/COMMIT` is invisible to the session; `MAX(id)+1`
   ID allocation races between concurrent sessions (use `CREATE SEQUENCE`). Extension code
   running inside a query should mutate through the caller's `ClientContext`/catalog, in the
   caller's transaction.

3. **Metadata written at `LOAD` time.** `MetadataStore::Initialize` creates five tables in the
   user's `main` schema the moment the extension loads. Loading an extension must not write to
   the database: it breaks `-readonly`, pollutes every attached database's default catalog, and
   the `con.Query` results are discarded, so failures are silent and surface later as confusing
   "table not found" errors. Metadata must be created lazily on first use, in the catalog being
   branched.

4. **SQL-by-string-concatenation as the engine.** ~90 % of the 5 000 C++ lines format SQL text
   and ship it over side connections. Identifier quoting is inconsistent
   (`snapshot_create` builds `CREATE SCHEMA _snap_...` and `CREATE TABLE %s.%s` unquoted), and
   escaping is inconsistent — the snapshot description is interpolated **unescaped**
   (`"'" + bind_data.description + "'"`, `snapshot_manager.cpp:193`): a plain SQL-injection
   hole. `scenario_write` stringifies typed values with `Value::ToString()`, so a `DATE`
   becomes an unquoted `2026-01-01` → arithmetic, not a date. Logic that must compose SQL
   belongs in bound catalog operations or, where text is unavoidable, in `KeywordHelper`-quoted
   builders — but most of it shouldn't exist at all (see §3).

5. **The configurable schema prefix is a landmine.** The registry stores `schema_name`, but
   every operation *recomputes* it from the current session setting
   (`GetSchemaName = current prefix + name`). `SET scenario_schema_prefix='_x_'` mid-session and
   `scenario_drop` deletes the registry row while `DROP SCHEMA IF EXISTS` targets a schema that
   doesn't exist — orphaning the real one, silently. A stored physical name must always be read
   back from the registry; better, the prefix should not be configurable at all (see §3.4).

6. **Redundant/derivable metadata.** `_scenario_tables` caches PK columns and row counts that
   the catalog already knows (and that go stale by design); the explicit
   `CREATE INDEX idx_delta_*_pk` duplicates the ART index the delta table's PK already created
   (pure write amplification); `size_bytes` in snapshots is `COUNT(*) * 100` — a fabricated
   number returned to users.

7. **DDL blocking by plan-walking is porous.** The optimizer-extension blocker checks
   `CREATE_TABLE / ALTER / DROP` only — `CREATE VIEW`, `CREATE INDEX`, `COMMENT ON`, etc. pass
   through; it also depends on the hardcoded `_scen_` prefix (misfire when the prefix setting
   changes). In a catalog-based design this file is deleted: DDL you don't implement fails
   naturally, with a message you control.

### 2.4 Smaller correctness notes (for completeness)

- Update-then-update of the same base row relies on `INSERT OR REPLACE` keyed on the delta PK —
  fine with a PK, undefined without one (contradictory `U`/`D` rows accumulate; the no-PK merge
  view then double-excludes or double-includes).
- `scenario_stats` returns one aggregate row (`table_count`, `delta_row_count`) while its own
  registered description says "per-table insert, update, and delete counts" (REQ-SCEN-004).
- Comparison engine materializes the entire diff into `vector<vector<Value>>` row-by-row via
  `Value` APIs instead of streaming the (perfectly good) generated SQL through the engine.
- PK output typing in `scenario_compare` maps "INTEGER-ish → BIGINT, everything else →
  VARCHAR", so `DATE`/`DECIMAL` PKs come back as strings.
- FK enforcement against the logical view (REQ-COW-006) is absent — inevitable, since there is
  no write path to hook it into.

### 2.5 Verdict

The Delta-Main *storage* idea is right, and the merge-on-read SQL shape (hash anti-join) is
right. Everything wrapped around it fights DuckDB instead of using it: the wrong function
class for verbs, the wrong transaction boundary, the wrong integration point for writes, an
isolation mechanism that costs O(N) and delivers nothing, and three subsystems (snapshots,
protocols, validation) that duplicate what one concept could provide. The codebase is ~5 000
lines of C++ plus ~4 700 lines of tests pinning the workaround API. This is a case for
fundamental simplification, not incremental repair.

---

## 3. Redesign: a scenario is a catalog, not a schema

### 3.1 The one structural decision

Expose each scenario as an **attached catalog** implemented by a `StorageExtension`:

```sql
ATTACH 'scenario:optimistic' AS opt;

SELECT * FROM opt.forecast;                   -- merge-on-read scan (base ⊎ delta)
INSERT INTO opt.forecast VALUES (...);        -- planned into delta ('I')
UPDATE opt.forecast SET qty = qty*1.1 WHERE ...;   -- delta upsert ('U')
DELETE FROM opt.forecast WHERE obsolete;      -- tombstone ('D')
CREATE TABLE opt.scratch (...);               -- error: "DDL is not supported in scenarios..."

DETACH opt;                                   -- forget the handle; data stays
```

`ScenarioCatalog` lists exactly the base tables (name resolution comes free — no `SET SCHEMA`
tricks, no search-path hacks, no prefix); its table entries implement:

- **`GetScanFunction`** — bind-generated merge-on-read (reuse today's anti-join shape; with a
  PK, semi-join on PK; without, `IS NOT DISTINCT FROM` on all columns).
- **`PlanInsert`** — insert into the delta table with `_op='I'`, after an anti-check against
  the merged relation for PK collisions (fixes the silent-duplicate bug; DuckDB ≥1.4's
  `MERGE INTO` is the natural executor for the upsert paths).
- **`PlanUpdate` / `PlanDelete`** — rewrite to delta upserts / tombstones keyed on PK (rowid
  for no-PK tables is *not* used; no-PK tables update/delete by whole-row equality, as
  REQ-COW-008 already accepts).
- **DDL entry points** — single clear `NotImplementedException` (delete `ddl_blocker.cpp`).
- **Archive check** — one `if (frozen) throw` at the top of the three plan hooks. Archive
  becomes real.

Physical layout underneath is unchanged in spirit: one hidden schema per scenario holding only
delta tables. But it becomes an implementation detail users never see or touch — the schema
can be renamed, the views disappear entirely (scans are planned, not view-expanded), and
`delta_create`/`delta_drop`/`scenario_write`/`scenario_schema` are deleted from the API.
Delta tables are created **lazily on first write** to a table (an empty scenario is truly
O(1) and <1 KB, finally meeting REQ-NFR-001 *and* REQ-SCEN-001).

This is the same integration DuckLake uses; it is exercised by four first-party extensions, is
stable across DuckDB minor versions (unlike logical-plan rewriting), and it is the only route
to the Critical write requirements at all (§2.2).

### 3.2 Unify scenario and snapshot: one object, two storage modes, one flag

A snapshot is nothing but a scenario that is (a) materialized and (b) frozen. Collapse the
`SnapshotManager` and the archive machinery into the scenario model:

| | `mode = 'delta'` (default) | `mode = 'materialized'` |
| --- | --- | --- |
| Creation cost | O(1), lazy | O(N) CTAS per table |
| Storage | ∝ modifications | full copy |
| Isolation from base changes | **overlay** — sees live base (documented; see §3.3) | complete |
| Use case | what-if editing | baseline freeze, audit, no-PK-heavy data |

Then:

- `snapshot_create(name)` ≡ `scenario_create(name, mode := 'materialized')` +
  `scenario_freeze(name)` — and it can finally capture `main`, fixing UC-03.
- `scenario_archive/unarchive` → `scenario_freeze/unfreeze`, enforced in the plan hooks.
- `scenario_from_snapshot` → `scenario_create(name, from := 'frozen_baseline')`.
- `snapshot_list/compare/drop` → the ordinary scenario functions.

One registry, one lifecycle, one dependency tree, five fewer API entry points, and the entire
`snapshot_manager.cpp` (955 lines) folds into ~50 lines of `scenario_create` options.

Branching becomes honest: `scenario_create(new, from := source)` copies the source's delta
tables (`CREATE TABLE ... AS SELECT * FROM src_delta`) — cheap because deltas are small by
definition — and records `parent_id`. The branch inherits modifications (REQ-SCEN-002) by
construction rather than by broken metadata copying.

### 3.3 Snapshot isolation: stop pretending, then get it for free

Delete the rowid capture wholesale (`_scenario_base_rowids`, `capture_rowids`,
`scenario_validate`'s rowid logic, the planned VACUUM `ParserExtension`). It delivers nothing
today and can't be made sound — rowids are physical locators, and the design doc's own
analysis says correctness should not depend on them.

Replace with three honest tiers:

1. **Default (`delta` mode): documented overlay semantics.** The scenario sees live base +
   its deltas. For the actual workflows (a session of what-if edits over a stable warehouse
   snapshot) this is what happens anyway, and DuckDB's MVCC already gives per-transaction
   consistency during any single query/transaction.
2. **`materialized` mode** when point-in-time is a requirement (audit, board deck) — full
   isolation, no validation machinery, no caveats.
3. **DuckLake bases (v0.3 direction): real O(1) snapshot isolation.** When the base catalog is
   a DuckLake attach, `scenario_create` records the base's current `snapshot_id`, and the merge
   scan reads `base AT (VERSION => snapshot_id)`. Creation is O(1), isolation is exact, VACUUM
   is a non-issue — the git-like semantics the design document wanted, provided by
   infrastructure DuckDB already ships instead of by a hand-rolled rowid ledger. This should be
   the headline of the roadmap: *anofox-scenario becomes the branching UX on top of DuckLake's
   versioned storage*, exactly the layer DuckLake does not yet provide itself.

### 3.4 Metadata: one lazy table, no knobs

- One registry table, created **lazily** inside the first `scenario_create`, in the catalog
  being branched, in the caller's transaction:
  `__anofox_scenario.registry(scenario_id from a SEQUENCE, name UNIQUE, mode, frozen, parent_id, base_snapshot_id, created_at, description)`.
  A dedicated internal schema (`__anofox_scenario`) keeps `main` clean.
- Delete `_scenario_tables` (derive tables/PKs from the catalog at plan time — never stale),
  `_scenario_base_rowids` (§3.3), `_scenario_snapshots` (§3.2).
- Delete the `scenario_schema_prefix` setting. The physical schema name is
  `__anofox_scenario.s_<id>` — collision-free without configuration, and immune to the
  recompute-vs-registry bug because the id, not the session, names it.
- Keep the delta-table shape (`_op`,`_ts`, payload columns, PK) — drop `_version` (unused) and
  the redundant secondary index.

### 3.5 Protocols: it's a table

The seven `protocol_*` C++ functions write rows into one table. That is not an engine concern.
Keep `_scenario_protocols` (portability requirement) but ship the accessors as **SQL macros
registered by the extension** (`CREATE OR REPLACE MACRO protocol_set_why(e, c) AS TABLE
INSERT ... RETURNING true`-style, or a single generic `scenario_note(entity, section,
content)` table function) — or fold the common fields (`description` already exists) into the
registry and let agents keep richer notes in their own tables. Either way,
`protocol_manager.cpp` (766 lines) reduces to ~a dozen macro definitions. Export-to-markdown
is a `COPY (SELECT ...) TO` one-liner users can compose themselves; it does not need to be API.

### 3.6 Comparison: the delta *is* the diff

- **Compare-to-origin** (the default question, REQ-COMP-003) needs no anti-joins at all in
  delta mode: `added` = `_op='I'`, `removed` = `_op='D'`, `changed` = `_op='U'` joined to base
  for old values. It reads the changelog that already exists.
- **Arbitrary two-relation diff** (scenario↔scenario, scenario↔main) is one generated
  `FULL OUTER JOIN ... unnest` query. Implement `scenario_diff(a, b, table)` as a table
  function that *binds the generated SQL as a subquery* (bind-replace, the idiomatic pattern)
  so results stream through the engine with real types instead of being materialized into
  `Value` vectors and stringly-typed columns. `scenario_diff_summary(a, b)` aggregates counts
  the same way.

### 3.7 The resulting surface

25 functions + 1 setting today → **8 functions + ATTACH**, all verbs as `CALL`-able table
functions running in the caller's transaction:

```sql
CALL scenario_create(name, [description], [from := scenario], [mode := 'delta'|'materialized']);
CALL scenario_drop(name);
CALL scenario_freeze(name);       -- absorbs archive + snapshot immutability
CALL scenario_unfreeze(name);
SELECT * FROM scenario_list();    -- + mode, frozen, parent, base_snapshot_id
SELECT * FROM scenario_stats(name);         -- real per-table I/U/D counts (GROUP BY on deltas)
SELECT * FROM scenario_diff(a, b, table);   -- 2-arg form = compare to origin
SELECT * FROM scenario_diff_summary(a, [b]);

ATTACH 'scenario:name' AS s;      -- the entire read/write UX
```

Deleted outright: `delta_create`, `delta_drop`, `scenario_write`, `scenario_schema`,
`scenario_validate` (nothing left to validate), `scenario_branch` (a `from :=` option),
`scenario_archive/unarchive` (renamed freeze), all five `snapshot_*`, all seven `protocol_*`
(→ macros), `scenario_schema_prefix`, `ddl_blocker.cpp`, `metadata_store.cpp`'s load-time
writes, `_scenario_base_rowids`, `_scenario_tables`.

Estimated core: a `ScenarioStorageExtension` + `ScenarioCatalog/SchemaEntry/TableEntry` +
transaction manager shim (~1.5–2 k lines, closely following the sqlite_scanner/DuckLake
skeleton), lifecycle table functions (~400), diff bind-replace functions (~300), SQL macros
(~100). Net: roughly **half the current C++**, with the Critical requirements actually met and
~10 correctness classes structurally impossible instead of unpatched.

### 3.8 Phasing

1. **Phase 1 — the catalog.** `ATTACH 'scenario:…'`, merge scan, `PlanInsert/Update/Delete`,
   lazy delta creation, freeze enforcement, DDL errors. Rewrite the write/read/constraint test
   files against transparent SQL (the current tests encode the workaround and must change —
   that is the point). This alone delivers REQ-COW-001..007.
2. **Phase 2 — lifecycle consolidation.** `CALL`-style verbs in caller's transaction, unified
   scenario/snapshot model, branch-with-delta-copy, registry v2 + migration function for
   existing databases, delete dead subsystems.
3. **Phase 3 — diff engine** as bind-replaced streaming SQL; per-table stats.
4. **Phase 4 — DuckLake base support**: record `snapshot_id`, scan `AT (VERSION …)`; document
   the three isolation tiers.

### 3.9 What is deliberately kept

- Delta-Main with merge-on-read (validated choice; the anti-join shape compiles to a hash
  anti-join as intended).
- Single-file portability (all state in the database; REQ-NFR-006).
- No DDL in scenarios (now enforced structurally).
- The protocol *table* (portable audit trail), minus its C++ API.
- The SQLLogicTest corpus as a scaffold — rewritten to test the promise (`INSERT INTO
  scen.t ...`) rather than the plumbing (`INSERT INTO _scen_x._delta_t ('I', ...)`).
