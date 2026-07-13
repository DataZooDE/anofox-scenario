# Implementation Plan — Phases 2–4: Consolidation, Diff Engine, DuckLake Isolation

**Status:** Roadmap draft | July 2026
**Prerequisites:** `docs/spec/diagnosis_and_redesign.md` (target architecture),
`docs/spec/implementation_plan_phase1.md` (the ATTACH catalog core — everything below assumes
Phase 1 is merged and green).

Phase 1 delivers the Critical requirements (transparent DML). Phases 2–4 deliver the rest of
the redesign: one object model instead of three subsystems, a streaming diff engine, and real
snapshot isolation. They are ordered by dependency, but Phase 3 only depends on Phase 1 and
can run in parallel with Phase 2.

Release mapping: **Phase 1 + 2 → v0.2.0** (the breaking release that retires the legacy API),
**Phase 3 → v0.2.x**, **Phase 4 → v0.3.0**.

---

## Phase 2 — Lifecycle consolidation and the unified scenario/snapshot model (~2–3 weeks)

Goal: one concept (`scenario` with `mode` and `frozen`), one registry, eight functions —
delete the three parallel subsystems and the legacy API.

### WP2.1 — Unified object model

- Registry v2 (from Phase 1 WP4) gains `mode VARCHAR CHECK (mode IN ('delta','materialized'))`
  and `base_snapshot_id BIGINT` (nullable; reserved for Phase 4).
- `scenario_create(name, [description], [from_scenario], [mode])`:
  - `mode := 'delta'` (default): O(1), as in Phase 1.
  - `mode := 'materialized'`: CTAS each base table into `__anofox_scenario.s<id>_mat_<table>`;
    the scenario catalog serves these as plain passthrough table entries (no merge scan, no
    delta) — full isolation from base changes.
  - `from_scenario := 'x'`: branching. Copies the source's delta tables
    (`CREATE TABLE ... AS SELECT * FROM s<src>_delta_<t>` — cheap, deltas are small), records
    `parent_id`. Fixes REQ-SCEN-002 by construction. Branching a *materialized* scenario
    creates a delta scenario whose base is the parent's materialized tables (overlay on a
    frozen copy — this is "scenario from snapshot" without a second concept).
- Freeze + materialized ≡ the old "snapshot". Compatibility shims (thin wrappers, one release):
  `snapshot_create(name, desc)` → `scenario_create(name, desc, mode := 'materialized')` +
  `scenario_freeze(name)`; `scenario_from_snapshot(s, n)` → `scenario_create(n, from := s)`;
  `snapshot_list()` → `scenario_list() WHERE mode='materialized' AND frozen`. Emitted with a
  deprecation notice; removed in v0.3.

### WP2.2 — Legacy teardown and migration

- `CALL scenario_migrate()`: reads the legacy `_scenario_registry` / `_scenario_tables` /
  `_scenario_snapshots` / delta schemas, rewrites into registry v2 + `__anofox_scenario`
  layout, renames delta tables, drops `_scenario_base_rowids` outright (dead weight per the
  diagnosis), archives legacy protocol rows unchanged. Idempotent; refuses on partial state.
- Delete: `scenario_write`, `delta_create`, `delta_drop`, `scenario_schema`,
  `scenario_validate`, the scalar-function lifecycle verbs, `snapshot_manager.cpp`,
  `ddl_blocker.cpp` (already dead after Phase 1), `metadata_store.cpp` load hook, the
  `scenario_schema_prefix` setting.
- `scenario_stats(name)` reimplemented per spec: per-table `rows_inserted/updated/deleted`
  (one `GROUP BY _op` over each delta; materialized scenarios report row counts).

### WP2.3 — Protocols as macros

- Keep the `_scenario_protocols` table (portability, REQ-PROT-001). Re-ship the seven
  `protocol_*` functions as SQL table macros registered at load
  (`CREATE OR REPLACE TEMP MACRO` into the system catalog); `protocol_export_markdown` is
  dropped from C++ — the API doc shows the equivalent two-line `COPY (SELECT ...) TO` recipe.
- `protocol_manager.cpp` (766 lines) is deleted.

### WP2.4 — Test-suite rewrite

- Port `scenario_lifecycle/read/write/constraints/errors/...` to the ATTACH surface — tests
  now express the *promise* (`INSERT INTO s.t ...`), not the plumbing
  (`INSERT INTO _scen_x._delta_t VALUES ('I', ...)`). Migration test: build a legacy-layout
  database fixture, run `scenario_migrate()`, assert equivalence.

**Exit criteria:** API surface is exactly the eight functions + ATTACH from the redesign §3.7;
`main` contains no extension tables unless scenarios exist; a v0.1 database migrates cleanly.

---

## Phase 3 — Streaming diff engine (~1.5–2 weeks, parallel to Phase 2 after Phase 1)

Goal: REQ-COMP-001..004 with engine-streamed, typed results — replacing the current
`vector<vector<Value>>` materialization and stringly-typed PKs.

- **Mechanism:** `scenario_diff` / `scenario_diff_summary` are *explicit table functions*, so
  `bind_replace` **is** available (verified: `bind_table_function.cpp:223` in the pinned
  submodule — unlike catalog scans). Bind generates the diff SQL as a `TableRef` and hands it
  back to the binder: results stream through the engine, filters/aggregations push down, PK
  columns keep their real types (fixes DATE/DECIMAL-as-VARCHAR).
- **Compare-to-origin (2-arg form):** no anti-joins — read the delta directly:
  `added` = `_op='I'`, `removed` = `_op='D'`, `changed` = `_op='U'` joined to base for old
  values, `unnest` over changed columns for the per-column rows. For materialized scenarios,
  fall through to the generic path against the recorded parent.
- **Generic two-relation diff (3-arg form):** one generated
  `FULL OUTER JOIN ... ON pk WHERE a IS DISTINCT FROM b` + `unnest` query; works for
  scenario↔scenario, scenario↔main, and frozen-materialized (snapshot) comparisons — REQ-SNAP-003
  needs no dedicated function.
- `scenario_diff_summary(a, [b])`: per-table counts via aggregation over the same generated
  relations, without materializing row-level diffs (REQ-COMP-002's efficiency clause).
- Delete `comparison_engine.cpp`'s row-materializing implementation; keep its output-schema
  tests, re-pointed at the new functions.

**Exit criteria:** diff of a 10M-row table with 1% changes streams (constant memory), output
schema matches REQ-COMP-001 with native PK types, all four COMP requirements covered by tests.

---

## Phase 4 — Snapshot-isolation tiers and DuckLake bases (~2–3 weeks + 1 week spike)

Goal: honest isolation semantics per the redesign §3.3 — documented overlay by default,
materialized on request, and **O(1) true snapshot isolation when the base lives in DuckLake**.

- **Spike first (timeboxed 1 week):** the novel part is a scenario catalog whose *base* is
  another attached (DuckLake) catalog rather than the host DuckDB catalog. Validate:
  (a) resolving base table entries cross-catalog at bind time, (b) planning the merge scan on
  top of a DuckLake time-travel scan (`AT (VERSION => n)`), (c) where delta tables live when
  the base is remote — decision: deltas stay in the *host* DuckDB file (keeps REQ-NFR-006
  single-file portability for scenario state; the DuckLake base carries its own durability).
- **Implementation:**
  - `scenario_create` detects the base catalog type; for DuckLake bases records
    `base_snapshot_id` (O(1)) in registry v2's reserved column.
  - The Phase 1 merge scan's base side binds `base_table AT (VERSION => base_snapshot_id)`
    instead of the live table — creation-time reads forever, immune to base churn and VACUUM.
  - `scenario_list()` gains an `isolation` column: `overlay` | `materialized` | `snapshot(vN)`.
  - Docs: a three-tier isolation matrix replaces the current (false) snapshot-isolation claims.
- Non-goals, explicitly: implementing branching *inside* DuckLake's metadata, multi-database
  scenarios, merge-back into base.

**Exit criteria:** on a DuckLake base, scenario creation is O(1), base mutations after creation
are invisible to the scenario, and dropping the scenario leaves the DuckLake catalog untouched.

---

## Backlog (post-v0.3, unscheduled)

Deliberately cut from Phases 1–4; each has a clean NotImplemented error until then:

- `ON CONFLICT` / `RETURNING` / `MERGE INTO` in scenario DML (the Phase 1 sink architecture
  leaves room: the insert sink already probes PK indexes).
- UPDATE/DELETE on no-PK tables (whole-row identity semantics).
- PK-column updates in scenarios.
- Two-step merge-back / promotion to base (the requirements' deferred flagship; builds
  directly on Phase 3's diff relations + `MERGE INTO` against base).
- Scan parallelism + filter pushdown tuning beyond Phase 1's WP2.1 stretch goal.
