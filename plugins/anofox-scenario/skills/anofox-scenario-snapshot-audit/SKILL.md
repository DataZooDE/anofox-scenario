---
name: anofox-scenario-snapshot-audit
description: >
  Immutable snapshots and audit trails with the anofox_scenario DuckDB extension: freeze an
  approved plan, keep a point-in-time copy that later base changes cannot alter, show what
  changed since approval, and keep notes on why. Use when the user wants to freeze or
  lock a plan / budget / forecast version, keep an auditable record, compare today's data
  with an approved state, pin a scenario to a DuckLake snapshot, document decisions, or
  migrate a legacy v0.1 scenario database. Covers scenario_create(mode := 'materialized'),
  scenario_create(base := <ducklake>), scenario_freeze / scenario_unfreeze, scenario_diff
  against 'main', scenario_list, the _scenario_protocols notes recipe and scenario_migrate.
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
- **Freezing is a switch, not a seal.** `scenario_freeze` rejects every write through any
  handle; reads keep working. `scenario_unfreeze` reverses it. Treat unfreezing an approved
  plan as an audited action, and record it (see "Audit notes").
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

## Audit notes

Notes are plain SQL over a plain table; there is no dedicated API. Keep the decision, the
approver and every unfreeze next to the data:

```sql
CREATE TABLE IF NOT EXISTS _scenario_protocols (
    entity_type VARCHAR NOT NULL, entity_name VARCHAR NOT NULL, section VARCHAR NOT NULL,
    content VARCHAR, updated_at TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (entity_type, entity_name, section));

INSERT OR REPLACE INTO _scenario_protocols (entity_type, entity_name, section, content) VALUES
    ('scenario', 'budget_2026_approved', 'approval', 'board meeting 2026-03-14, minutes ref B-26-03'),
    ('scenario', 'budget_2026_rev1', 'why', 'sales headcount moved forward to Q2');

SELECT entity_name, section, content FROM _scenario_protocols ORDER BY entity_name, section;
```

Export as Markdown:

```sql
COPY (SELECT '## ' || entity_name || ' / ' || section || chr(10) || content
      FROM _scenario_protocols ORDER BY entity_name, section)
TO 'protocols.md' (FORMAT csv, QUOTE '', HEADER false);
```

Because scenarios, deltas, snapshots and notes all live in the host database, one
`.duckdb` file is the complete record.

## Legacy databases (v0.1)

Databases written by v0.1 (`_scenario_registry`, `_scen_*`, `_snap_*` tables) are migrated once,
in place: archived scenarios become frozen, v0.1 snapshots become materialized + frozen
scenarios, and `_scenario_protocols` rows are kept.

```sql
SELECT * FROM scenario_migrate();
```
