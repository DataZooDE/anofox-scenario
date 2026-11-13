---
name: anofox-scenario-snapshot-audit
description: >
  Frozen snapshots and audit trails with the anofox_scenario DuckDB extension: freeze an
  approved plan, keep a point-in-time copy that later base changes cannot alter, show what
  changed since approval, and keep a log of why. Use when the user wants to freeze or lock
  a plan / budget / forecast version, keep an auditable record, compare today's data with
  an approved state, pin a scenario to a DuckLake snapshot, document decisions, or migrate
  a legacy v0.1 scenario database. Main tools: scenario_create(mode := 'materialized') or
  (base := <ducklake>), scenario_freeze / scenario_unfreeze, and scenario_migrate.
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — Snapshot & Audit Playbook

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0

There is no separate snapshot API: a snapshot is a scenario that does not move with the base
(materialized, or pinned to a DuckLake snapshot) and is frozen so nobody writes to it. For
error messages see `anofox-scenario-reference`.

## Critical gotchas

- **A default (delta) scenario is not a snapshot.** It reads the live base for every row it
  did not change, so a frozen delta scenario still moves when the base changes. Use
  `mode := 'materialized'` (copies every base table at creation) or a DuckLake `base :=`
  (reads pinned to the creation time, no copy).
- **Freezing is a switch, not a seal.** `scenario_freeze` rejects every write through a
  scenario catalog; reads keep working. `scenario_unfreeze` reverses it (except for a merged
  scenario, which stays frozen). Treat unfreezing an approved plan as an audited action and
  record it (see "Audit notes").
- **Frozen is guarded, not sealed.** SQL cannot edit the internal `__anofox_scenario` tables
  (registry, deltas, materialized copies) in a session that has the extension loaded: such
  writes fail with `Cannot modify … with SQL`, so `UPDATE __anofox_scenario.registry SET
  frozen = false` does not work. But a process that opens the file without the extension, or
  a session that sets `anofox_scenario_allow_internal_writes = true`, can change them, and
  nothing would show it. Add `SET lock_configuration = true` after loading the extension to
  keep the setting off. For real tamper evidence keep an external copy: `EXPORT DATABASE`
  plus a checksum stored elsewhere, or DuckLake snapshots with a retention policy.
- **Materialized creation copies all base tables** of the host database, and tables added to
  the base later are not part of the snapshot. `mode := 'materialized'` cannot be combined
  with `base :=` (a DuckLake base is pinned already) or `from_scenario` (a branch is always a
  delta over its parent).
- **Materialized copies don't carry FOREIGN KEY constraints** (data and other constraints are
  copied); a merge back into the base still enforces them.

## The playbook

### 1. Approve a plan: materialize and freeze

```sql
CREATE TABLE budget (cost_center VARCHAR PRIMARY KEY, amount DECIMAL(12,2), owner VARCHAR);
INSERT INTO budget VALUES ('CC-100', 50000, 'ops'), ('CC-200', 72000, 'sales'), ('CC-300', 31000, 'it');

CALL scenario_create('budget_2026_approved', 'approved by the board 2026-03-14', mode := 'materialized');
CALL scenario_freeze('budget_2026_approved');
```

### 2. Read the approved state at any time

```sql
ATTACH 'budget_2026_approved' AS approved (TYPE scenario);
SELECT * FROM approved.budget ORDER BY cost_center;
```

Writes are rejected while it is frozen:

```sql
UPDATE approved.budget SET amount = 0;
```

So is plain SQL on the internal tables behind it, such as flipping the flag:

```sql
UPDATE __anofox_scenario.registry SET frozen = false WHERE name = 'budget_2026_approved';
```

### 3. The base moves on

```sql
UPDATE budget SET amount = 80000 WHERE cost_center = 'CC-200';
INSERT INTO budget VALUES ('CC-400', 12000, 'hr');
DELETE FROM budget WHERE cost_center = 'CC-300';
```

### 4. What changed since approval

Compare the snapshot with the live table (`old` = approved, `new` = today):

```sql
SELECT * FROM scenario_diff('budget_2026_approved', 'main', 'budget') ORDER BY cost_center;
```

For a DuckLake-based approval, `'main'` here is the lake table *at the scenario's snapshot*,
so this diff stays empty however the lake changes; compare with the live lake by a plain
join instead (see "DuckLake").

The 2-argument form `scenario_diff('budget_2026_approved', 'budget')` answers a different
question: what the scenario itself changed relative to its creation snapshot (nothing, for a
frozen approval).

KPI view, approved vs actual:

```sql
SELECT coalesce(a.cost_center, b.cost_center) AS cost_center,
       a.amount AS approved, b.amount AS current, b.amount - a.amount AS delta
FROM approved.budget a FULL JOIN budget b USING (cost_center)
ORDER BY cost_center;
```

### 5. Inventory

```sql
SELECT name, mode, frozen, parent, created_at, description FROM scenario_list() ORDER BY created_at;
```

### 6. A revision on top of the approved plan

Branch it instead of unfreezing it, so the approved version stays as it was:

```sql
CALL scenario_create('budget_2026_rev1', 'Q2 revision', from_scenario := 'budget_2026_approved');
ATTACH 'budget_2026_rev1' AS rev1 (TYPE scenario);
UPDATE rev1.budget SET amount = amount * 1.05 WHERE owner = 'sales';
SELECT * FROM scenario_diff('budget_2026_approved', 'budget_2026_rev1', 'budget');
```

## DuckLake: snapshots without copying

Over a DuckLake catalog a scenario is pinned to the lake snapshot at creation
(`AT (TIMESTAMP => created_at)`), so creation is O(1) and later lake commits are invisible to it.
Branches keep their parent's snapshot.

```sql
INSTALL ducklake;
ATTACH 'ducklake:lake.ducklake' AS lake (DATA_PATH 'lake_files/');
CREATE TABLE lake.budget (cost_center VARCHAR, amount DECIMAL(12,2));
INSERT INTO lake.budget VALUES ('CC-100', 50000), ('CC-200', 72000);

CALL scenario_create('lake_budget_approved', base := 'lake',
                     key_columns := MAP {'budget': ['cost_center']});
CALL scenario_freeze('lake_budget_approved');

INSERT INTO lake.budget VALUES ('CC-300', 9000);    -- later lake commit
ATTACH 'lake_budget_approved' AS lba (TYPE scenario);
SELECT count(*) FROM lba.budget;                    -- 2: pinned to the snapshot
```

Tables the lake creates after the snapshot are not part of the scenario.

What changed in the live lake since approval (a `scenario_diff` against `'main'` would compare
with the pinned snapshot and show nothing):

```sql
-- requires: ducklake
SELECT coalesce(a.cost_center, l.cost_center) AS cost_center,
       a.amount AS approved, l.amount AS current
FROM lba.budget a FULL JOIN lake.budget l USING (cost_center)
WHERE a.amount IS DISTINCT FROM l.amount
ORDER BY cost_center;
```

A DuckLake-based snapshot is only readable while the lake keeps it: the DuckLake catalog
file, its `DATA_PATH` files and the snapshot itself (not expired or compacted away) must be
retained alongside the `.duckdb` file.

## Audit notes

Notes are plain SQL over plain tables; there is no dedicated API. Keep an append-only log
of decisions next to the data: every approval, unfreeze and revision is a new row, nothing is
updated or replaced.

```sql
CREATE SEQUENCE IF NOT EXISTS audit_events_seq;
CREATE TABLE IF NOT EXISTS audit_events (
    event_id BIGINT DEFAULT nextval('audit_events_seq') PRIMARY KEY,
    scenario VARCHAR NOT NULL, event VARCHAR NOT NULL, actor VARCHAR,
    logged_at TIMESTAMP DEFAULT current_timestamp, note VARCHAR);

INSERT INTO audit_events (scenario, event, actor, note) VALUES
    ('budget_2026_approved', 'approved', 'board', 'board meeting 2026-03-14, minutes ref B-26-03'),
    ('budget_2026_rev1', 'created', 'cfo', 'sales headcount moved forward to Q2');

SELECT scenario, event, actor, note FROM audit_events ORDER BY event_id;
```

Log an unfreeze in the same transaction as the unfreeze itself, so one never happens
without the other:

```sql
BEGIN;
CALL scenario_unfreeze('budget_2026_approved');
INSERT INTO audit_events (scenario, event, actor, note)
    VALUES ('budget_2026_approved', 'unfrozen', 'controller', 'correct CC-300 owner');
COMMIT;
CALL scenario_freeze('budget_2026_approved');
INSERT INTO audit_events (scenario, event, actor) VALUES ('budget_2026_approved', 'frozen', 'controller');
```

Export as Markdown:

```sql
COPY (SELECT '## ' || scenario || ' / ' || event || ' (' || logged_at || ', ' || coalesce(actor, '?') || ')'
             || chr(10) || coalesce(note, '')
      FROM audit_events ORDER BY event_id)
TO 'audit_events.md' (FORMAT csv, QUOTE '', HEADER false);
```

For a host-database scenario, the scenarios, their deltas and copies, and these notes all
live in the one `.duckdb` file. DuckLake-based scenarios additionally need the lake (see
above). Databases migrated from v0.1 keep their notes in `_scenario_protocols`.

## Legacy databases (v0.1)

Databases written by v0.1 (`_scenario_registry`, `_scen_*`, `_snap_*` tables) are migrated once,
in place: archived scenarios become frozen, v0.1 snapshots become materialized + frozen
scenarios, and `_scenario_protocols` rows are kept.

```sql
SELECT * FROM scenario_migrate();
```
