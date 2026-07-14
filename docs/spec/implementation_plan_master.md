# Implementation Plan — Master Roadmap (All Phases)

**Status:** Consolidated roadmap | July 2026
**Companion docs:**
- `docs/spec/diagnosis_and_redesign.md` — why the catalog layer, target architecture (§3)
- `docs/spec/implementation_plan_phase1.md` — Phase 1 in full detail (the ATTACH catalog core)
- `docs/spec/implementation_plan_phase2_4.md` — Phases 2–4 in detail

This document is the cross-phase view: the single critical path, the invariants Phase 1 must
lock in so later phases don't force rework, the decision gates, and — new here — the former
"post-v0.3 backlog" promoted into planned **Phases 5–6** with dependencies and scope.

All integration points cited by the phase docs were re-verified against the pinned submodule
(`08e34c447b`): `StorageExtension{attach, create_transaction_manager}`
(`storage/storage_extension.hpp`) and `Catalog::PlanInsert/PlanUpdate/PlanDelete/
PlanCreateTableAs/PlanMergeInto` (`catalog/catalog.hpp:308–318`) exist as described.

---

## 1. Phase map and critical path

Everything hangs off one spine. Phases 3 and 4 branch off Phase 1 **directly** (they need the
merge scan, not the lifecycle consolidation); Phase 5 extends Phase 1's sinks; Phase 6 sits on
top of Phases 3 + 4.

```
P1·WP0 attach+txn ─▶ WP1 catalog ─▶ WP2 merge scan ─▶ WP3 write path ─▶ WP5 harden
                          ▲                │                 │
      P1·WP4 registry v2 ─┘ (parallel)     │                 │
                                           │                 │
        ┌──────────────────────────────────┼─────────────────┼──────────────┐
        ▼                                  ▼                 ▼              │
   P2 lifecycle consolidation         P3 diff engine    P5 DML completeness │
   (needs all of P1)                  (needs P1·WP2)    (extends P1·WP3)    │
        │                                  │                                ▼
        │                                  │                     P4 DuckLake bases
        │                                  │                     (needs P1·WP2)
        │                                  └──────────┬─────────────────────┘
        │                                             ▼
        │                                  P6 merge-back / promotion
        │                                  (needs P3 diff + P4 snapshot_id + P5 MERGE)
        ▼
   v0.2.0 (breaking) ── P3 → v0.2.x ── P4 → v0.3.0 ── P5 → v0.4.0 ── P6 → v0.5.0
```

Milestone labels (`v0.2.0` …) follow the phase docs; actual release *tags* follow the
project's CalVer scheme (`vYYYY.MM.DD`) — the milestone labels name feature sets, not tags.

| Phase | Content | Depends on | Estimate |
| --- | --- | --- | --- |
| 1 | ATTACH catalog: transparent DML, merge scan, freeze, DDL errors | — | 4–5 wks |
| 2 | Unified scenario/snapshot model, legacy teardown, migration, protocol macros | P1 | 2–3 wks |
| 3 | Streaming diff engine (bind-replace table functions) | P1·WP2 | 1.5–2 wks |
| 4 | Isolation tiers + DuckLake `AT (VERSION)` bases | P1·WP2 | 2–3 wks + 1 wk spike |
| 5 | DML completeness: RETURNING, ON CONFLICT, PK-column updates, MERGE INTO, no-PK DML | P1·WP3 | 2–3 wks |
| 6 | Merge-back / promotion to base | P3 + P4 + P5·WP5.5 | 2–3 wks + 1 wk spike |

**Single track:** ≈ 14–19 weeks. **Two engineers:** P3 ∥ P2, P5 ∥ P4, → ≈ 10–12 weeks.
WP2 (merge-on-read scan) is the highest-risk item and gates P2, P3, P4 — staff it strongest.

## 2. Cross-phase invariants (lock in during Phase 1)

These four choices, made naively in Phase 1, each force a later phase to reopen finished code:

1. **The scan's base source is an abstraction, not a live table.** P1 binds a live host
   `DuckTableEntry`; P2 `materialized` serves CTAS copies as passthrough; P4 binds
   `base AT (VERSION => snapshot_id)`. `ScenarioTableEntry`'s bind data holds a `BaseSource`
   variant (live | materialized | versioned) with only *live* implemented in P1.
2. **Registry v2 carries the later columns from day one.** P1·WP4 creates
   `__anofox_scenario.registry` already including nullable `mode` (P2) and `base_snapshot_id`
   (P4), plus a `merged_at`/`status` slot reserved for P6 — no migration-of-a-migration.
3. **The delta table is a public changelog contract.** `(_op, _ts, payload, PK)` and the
   op-transition matrix are consumed by three later phases: P3 reads it as the diff
   (`added`=`I`, `removed`=`D`, `changed`=`U`), P6 replays it as the merge source, and P2's
   branching copies it. Sinks must never write states outside the matrix; freeze the contract
   in P1 and test every matrix cell.
4. **One freeze chokepoint.** The `frozen` check at the top of the three Plan hooks is the
   *only* write gate. P2's snapshot (= materialized + frozen), archive (= freeze), and P6's
   freeze-during-merge all reuse it; a second enforcement path anywhere means every later
   phase inherits two.

Dual-registry note: during P1 the legacy API and the new ATTACH path coexist. Boundary:
**the ATTACH path is registry-v2-only; the legacy API is frozen (bugfixes only) against the
old registry** until P2's `scenario_migrate()` unifies them. Legacy-created scenarios are not
ATTACH-able before migration — documented, not papered over.

## 3. Phases 1–4 (summary; detail in the companion docs)

- **Phase 1 — the catalog** (`implementation_plan_phase1.md`): WP0 attach + trivial
  transaction shim (host `DuckTransactionManager` provides ACID; `MetaTransaction::
  ModifyDatabase` is the one novel integration — test first). WP1 catalog/schema entries, all
  DDL → one `NotImplementedException`. WP2 merge-on-read scan (passthrough when no delta;
  suppression set + delta stream otherwise; virtual identity columns for DML). WP3 sinks +
  lazy delta creation + op-transition matrix + PK-vs-base probe. WP4 registry v2 + `CALL`
  lifecycle verbs in the caller's transaction. WP5 hardening. Delivers REQ-COW-001..007.
- **Phase 2 — consolidation** (`implementation_plan_phase2_4.md` §2): `mode := 'delta' |
  'materialized'`, branching via `from_scenario` (copies delta tables — honest REQ-SCEN-002),
  snapshot/archive collapse into mode+frozen, `scenario_migrate()`, legacy API deleted,
  protocols → SQL macros, test corpus rewritten against the promise. Exit: exactly the eight
  functions + ATTACH of redesign §3.7.
- **Phase 3 — diff engine** (§3): `scenario_diff`/`scenario_diff_summary` as explicit table
  functions using `bind_replace` (available there, unlike catalog scans) — streaming, typed,
  no `vector<vector<Value>>`. 2-arg form reads the delta directly; 3-arg form generates one
  `FULL OUTER JOIN … unnest` query.
- **Phase 4 — DuckLake bases** (§4): spike cross-catalog base resolution first; then record
  `base_snapshot_id` at create (O(1)), merge scan binds `AT (VERSION => n)` via the
  `BaseSource` seam from invariant #1. Three documented isolation tiers:
  `overlay | materialized | snapshot(vN)`.

## 4. Phase 5 — DML completeness (~2–3 weeks, parallel to Phase 4) → v0.4.0

Everything Phase 1 gates with `NotImplementedException` on the write path, retired in order
of (value ÷ risk). All work extends the P1·WP3 sinks — no new architecture. Until each lands,
its clean NotImplemented message names the workaround.

### WP5.1 — RETURNING (~2 days)
`LogicalInsert::return_chunk` (and the update/delete equivalents): the sinks already have the
complete post-image rows in hand (the matrix requires full rows); emit them as the operator's
result chunk. Cheapest item, disproportionate agent-persona value (`INSERT … RETURNING id`).

### WP5.2 — ON CONFLICT in scenario INSERT (~4–5 days)
Lift the `OnConflictAction::THROW`-only gate in `PlanInsert`. Semantics are defined against
the **merged relation**, reusing the two probes the insert sink already performs:
- conflict vs **base** row: `DO NOTHING` → drop the row; `DO UPDATE` → write `('U', merged
  values)` per the matrix — an upsert over base.
- conflict vs **delta** row (`I`/`U`): `DO NOTHING` → drop; `DO UPDATE` → update the delta
  row in place, op unchanged per the matrix.
Tests mirror `attach_constraints.test`'s distinct base-vs-delta error cases, now as actions.

### WP5.3 — PK-column updates (~3–4 days)
Mirror core's `update_is_del_and_insert`: an `UPDATE` that touches a PK column becomes a
`('D', old_pk)` tombstone plus a new-PK row — `('I', …)` when the old row originated in the
delta as `I` or the new PK doesn't exist in base, else the insert-path collision check fires
with the same error text as WP3's probe. No new delta states: strictly a composition of two
matrix transitions in one sink.

### WP5.4 — MERGE INTO scenarios (~3–4 days)
Implement `Catalog::PlanMergeInto` by lowering each action to the existing sinks
(WHEN MATCHED UPDATE/DELETE → matrix transitions; WHEN NOT MATCHED INSERT → insert path with
the WP5.2 probes). Mostly plumbing once 5.1–5.3 exist. **Prerequisite for Phase 6**, which
generates MERGE against the *base* — implementing it here first exercises the lowering.

### WP5.5 — UPDATE/DELETE on no-PK tables (~4–6 days, last; may slip a release)
The whole-row-identity swamp, deliberately sequenced last. Scope per REQ-COW-008
(implementation-defined): identity is `IS NOT DISTINCT FROM` over all columns; duplicate rows
are treated **as a group** (a matching DELETE tombstones all identical copies; UPDATE rewrites
all copies). Requires a `_count BIGINT` multiplicity column on no-PK delta tables — a delta
*shape* extension, which is why the decision belongs in this plan: P3's diff and P6's
merge-back must interpret `_count` when present. If WP5.5 slips, nothing downstream blocks —
P6 can refuse no-PK tables with a clear error.

**Exit criteria:** `attach_unsupported.test` shrinks to (at most) the no-PK cases; every
retired gate is replaced by semantics tests; RETURNING/ON CONFLICT/MERGE documented in
API_REFERENCE with their merged-relation semantics.

## 5. Phase 6 — Merge-back / promotion to base (~2–3 weeks + 1 week spike) → v0.5.0

The requirements' deferred flagship: apply a scenario's changes to its base. Everything it
needs is built by then — P3's diff relations are the preview, the delta is the merge source,
P5·WP5.4 proved the MERGE lowering, and P4's `base_snapshot_id` makes conflict detection
sound.

### Spike (timeboxed 1 week): the conflict model
- **DuckLake base (snapshot tier):** true three-way merge — base@fork (`AT (VERSION =>
  base_snapshot_id)`), base@now, scenario delta. A conflict is a PK modified on both sides
  since the fork. Exact, git-like.
- **Live-overlay base:** there is no recorded fork state, so drift is not fully detectable.
  Honest fallback: a conflict is a delta `U`/`D` row whose *current* base row no longer
  matches the delta's captured pre-image columns — detectable only where the delta stores
  full rows (it does, for `U`). Document the weaker guarantee; never pretend otherwise
  (same honesty principle as redesign §3.3).
- Decide child handling. Recommendation: **merge leaf scenarios only**; refuse when
  `parent_id` children exist (mirrors `scenario_drop`'s existing rule).

### Implementation
- `SELECT * FROM scenario_merge_preview(name)` — table function over P3's compare-to-origin
  relations plus a `conflict` column and planned action per row. Streaming, typed
  (bind-replace, same mechanism as `scenario_diff`).
- `CALL scenario_merge(name, [on_conflict := 'abort' | 'ours' | 'theirs'])` — in the
  **caller's transaction**, one generated `MERGE INTO base USING delta` per modified table
  (`I`→INSERT, `U`→UPDATE, `D`→DELETE), `'abort'` default: any conflict rolls the whole
  merge back with a per-table conflict count in the error. The scenario is frozen for the
  duration via the single freeze chokepoint (invariant #4); on success the registry row is
  marked `merged_at` (reserved column from invariant #2) and the scenario becomes frozen —
  auditable history, droppable at leisure.
- Non-goals (unchanged from the redesign): merging *into* DuckLake metadata beyond ordinary
  DML, multi-database merges, cross-scenario (sibling→sibling) merges.

**Exit criteria:** merge of a 1%-delta scenario over a 10M-row base streams through the
engine; `abort` provably rolls back all tables atomically; conflict semantics tested per
tier (snapshot: three-way; overlay: pre-image); a merged scenario is frozen with `merged_at`
set and its preview returns empty.

## 6. Continuous track (no phase, rides along)

- **Scan performance:** WP2.1 (filter pushdown + parallel base scan) is the P1 stretch goal;
  further tuning slots into any phase behind `attach_perf.test`'s thresholds.
- **Docs cadence per CLAUDE.md:** every user-facing change updates `docs/API_REFERENCE.md`
  in the same task; architecture deltas land in `docs/spec/`.

## 7. Decision gates

| Gate | Blocks | Recommendation (pending maintainer sign-off) |
| --- | --- | --- |
| DuckDB version targeting | P1·WP0 (changes CI) | New core targets **v1.5.x only**; v1.4.5 job builds legacy from a maintenance branch until P2, then dropped. |
| ATTACH syntax (`'scenario:name'` vs `'name' (TYPE scenario)`) | P1 acceptance tests | **`(TYPE scenario)`** — idiomatic `StorageExtension` dispatch; fix redesign §3.1 to match. |
| Breaking-release policy for v0.2.0 | P2 shim scope | Compat shims for `snapshot_*` one release, removed in v0.3; confirm no external v0.1 users need longer. |
| No-PK UPDATE/DELETE timing | P5·WP5.5 vs slip | Ship v1 limitation through v0.3; WP5.5 in v0.4 only if the `_count` design survives review, else v0.5. |
| Merge-back conflict default | P6 API | `on_conflict := 'abort'` default; `'ours'`/`'theirs'` opt-in. |

## 8. Definition of done (whole roadmap)

1. The redesign §3.7 surface (8 functions + ATTACH) plus exactly three additions:
   `scenario_merge_preview`, `scenario_merge`, and the `isolation` column in `scenario_list()`.
2. Every Critical/High requirement satisfied or explicitly re-scoped in writing
   (REQ-COW-008 no-PK semantics, REQ-SCEN-001 overlay tier).
3. No `NotImplementedException` left on the write path except deliberate non-goals, each
   naming its workaround.
4. Legacy v0.1 databases migrate via `scenario_migrate()`; single-file portability
   (REQ-NFR-006) holds in every mode except the DuckLake base's own storage.
5. `GEN=ninja make && make test` green on the targeted DuckDB line at every phase boundary.
