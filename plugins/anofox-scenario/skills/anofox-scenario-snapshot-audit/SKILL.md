---
name: anofox-scenario-snapshot-audit
description: >
  Approved, signed-off versions and audit trails with the anofox_scenario DuckDB extension:
  freeze an approved plan, budget or forecast so later base changes cannot alter it, capture
  an approved what-if variant, show what changed since sign-off, log who corrected or revised
  it and why, pin a version to a DuckLake snapshot, migrate a legacy v0.1 scenario database.
  Use when the user says approved, signed off, sign-off, baseline, frozen, locked, "as of" a
  date, "the version from March", audit trail or compliance record. Not for exploring new
  variants (anofox-scenario-what-if) or applying changes to the real tables
  (anofox-scenario-safe-change).
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — Snapshot & Audit Playbook

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0

A snapshot is a scenario that does not move with the base (materialized, or pinned to a
DuckLake snapshot) and is frozen so nobody writes to it. Errors and settings:
`anofox-scenario-reference`. DuckLake, Markdown export, legacy migration:
[recipes.md](recipes.md).

## Ask first

Unfreezing an approved scenario, dropping it, or merging a revision into the real tables
changes the record. Name the scenario and the consequence, get the user's yes, and log it
(see "Audit notes"). Prefer branching an approved version over unfreezing it.

## What are you approving?

Decide before creating anything:

| Approving | Do |
|---|---|
| the current contents of the base tables | `mode := 'materialized'` + `scenario_freeze` (§1) |
| an existing what-if variant (a delta scenario) | capture it into a materialized scenario (§7); freezing the variant itself is not enough, its untouched rows still follow the base |
| a variant still to be planned | start from a materialized baseline, branch it, freeze the chosen branch |
| tables in a DuckLake catalog | `base := 'lake'` + freeze: pinned, no copy ([recipes.md](recipes.md)) |

## Critical gotchas

- **A default (delta) scenario is not a snapshot.** It reads the live base for every row it
  did not change, frozen or not.
- **Freezing is a switch, not a seal.** `scenario_unfreeze` reverses it (not for a merged
  scenario). Do an approved correction atomically (see "Audit notes").
- **Frozen is guarded, not sealed.** SQL cannot edit `__anofox_scenario` in a session with the
  extension loaded, but a process without the extension, or with
  `anofox_scenario_allow_internal_writes`, can. `SET lock_configuration = true` after loading
  keeps the setting off (set the telemetry opt-out before it). For tamper evidence keep an
  external copy: `EXPORT DATABASE` plus a checksum stored elsewhere.
- **Materialized creation copies all base tables** of the host database; tables added later
  are not part of it. Copies carry no FOREIGN KEY constraints; a merge still enforces them.
- **A revision is computed from the approved copy**, not today's base. Merging it flags every
  row it touched that the base changed since approval; `'ours'` reverts those base edits.

## The playbook

### 1. Approve the current data: materialize and freeze

```sql
CREATE TABLE budget (cost_center VARCHAR PRIMARY KEY, amount DECIMAL(12,2), owner VARCHAR);
INSERT INTO budget VALUES ('CC-100', 50000, 'ops'), ('CC-200', 72000, 'sales'), ('CC-300', 31000, 'it');

BEGIN;
CALL scenario_create('budget_2026_approved', 'approved by the board 2026-03-14', mode := 'materialized');
CALL scenario_freeze('budget_2026_approved');
COMMIT;
```

### 2. Read the approved state at any time

```sql
ATTACH 'budget_2026_approved' AS approved (TYPE scenario);
SELECT * FROM approved.budget ORDER BY cost_center;
```

```sql
-- fails: is frozen. Unfreeze it with CALL scenario_unfreeze
UPDATE approved.budget SET amount = 0;
```

```sql
-- fails: Cannot modify __anofox_scenario.registry with SQL
UPDATE __anofox_scenario.registry SET frozen = false WHERE name = 'budget_2026_approved';
```

### 3. The base moves on

```sql
UPDATE budget SET amount = 80000 WHERE cost_center = 'CC-200';
INSERT INTO budget VALUES ('CC-400', 12000, 'hr');
DELETE FROM budget WHERE cost_center = 'CC-300';
```

### 4. What changed since approval

Snapshot vs. live table (`old` = approved, `new` = today):

```sql
SELECT * FROM scenario_diff('budget_2026_approved', 'main', 'budget') ORDER BY cost_center;
```

The 2-argument form `scenario_diff('budget_2026_approved', 'budget')` answers another
question: what the scenario itself changed (nothing, for an untouched approval). For a
DuckLake approval `'main'` is the pinned snapshot: compare with a join ([recipes.md](recipes.md)).

KPI view, approved vs. actual:

```sql
SELECT coalesce(a.cost_center, b.cost_center) AS cost_center,
       a.amount AS approved, b.amount AS current, b.amount - a.amount AS delta
FROM approved.budget a FULL JOIN budget b USING (cost_center)
ORDER BY cost_center;
```

### 5. Inventory

```sql
SELECT name, mode, frozen, parent, created_at, merged_at, description
FROM scenario_list() ORDER BY created_at;
```

### 6. A revision on top of the approved plan

Branch it instead of unfreezing it, so the approved version stays as it was:

```sql
CALL scenario_create('budget_2026_rev1', 'Q2 revision', from_scenario := 'budget_2026_approved');
ATTACH 'budget_2026_rev1' AS rev1 (TYPE scenario);
UPDATE rev1.budget SET amount = amount * 1.05 WHERE owner = 'sales';
SELECT * FROM scenario_diff('budget_2026_approved', 'budget_2026_rev1', 'budget');
```

Merging `rev1` into the live table writes values computed from the approved copy: CC-200 was
changed in the base since approval, so it is a conflict. To apply a revision to today's data,
redo it in a fresh delta scenario (`anofox-scenario-safe-change`).

```sql
SELECT key, action, conflict FROM scenario_merge_preview('budget_2026_rev1');
```

## Audit notes

Plain SQL over a plain, append-only table: every approval, correction and revision is a row.

```sql
CREATE SEQUENCE IF NOT EXISTS audit_events_seq;
CREATE TABLE IF NOT EXISTS audit_events (
    event_id BIGINT DEFAULT nextval('audit_events_seq') PRIMARY KEY,
    scenario VARCHAR NOT NULL, event VARCHAR NOT NULL, actor VARCHAR,
    logged_at TIMESTAMP DEFAULT current_timestamp, note VARCHAR);

INSERT INTO audit_events (scenario, event, actor, note) VALUES
    ('budget_2026_approved', 'approved', 'board', 'board meeting 2026-03-14, minutes ref B-26-03'),
    ('budget_2026_rev1', 'created', 'cfo', 'sales headcount moved forward to Q2');
```

An approved correction (after the user's yes): unfreeze, correct, refreeze and log in one
transaction, so the plan is never left writable and never changes without its log entry:

```sql
BEGIN;
CALL scenario_unfreeze('budget_2026_approved');
UPDATE approved.budget SET owner = 'infra' WHERE cost_center = 'CC-300';
CALL scenario_freeze('budget_2026_approved');
INSERT INTO audit_events (scenario, event, actor, note) VALUES
    ('budget_2026_approved', 'corrected', 'controller', 'CC-300 owner it -> infra; unfrozen and refrozen');
COMMIT;

SELECT frozen FROM scenario_list() WHERE name = 'budget_2026_approved';
SELECT cost_center, owner FROM approved.budget WHERE cost_center = 'CC-300';
```

Scenarios, copies and notes all live in the one `.duckdb` file.

## 7. Capture an approved what-if variant

Copy the variant's state into a new materialized scenario and freeze it in one transaction.
Only the tables the variant changed need copying; the fresh copy already holds the rest.

```sql
-- doc-snippets: new session
CREATE TABLE demand (sku VARCHAR, region VARCHAR, qty INTEGER, PRIMARY KEY (sku, region));
INSERT INTO demand VALUES ('A', 'US', 100), ('A', 'EU', 80), ('B', 'US', 50);
CALL scenario_create('optimistic', 'US +10');
ATTACH 'optimistic' AS opt (TYPE scenario);
UPDATE opt.demand SET qty = qty + 10 WHERE region = 'US';

SELECT table_name FROM scenario_diff_summary('optimistic');   -- the tables to copy
```

```sql
CALL scenario_create('optimistic_approved', 'optimistic, approved 2026-03-14', mode := 'materialized');
ATTACH 'optimistic_approved' AS oa (TYPE scenario);
BEGIN;
DELETE FROM oa.demand;
INSERT INTO oa.demand SELECT * FROM opt.demand;
CALL scenario_freeze('optimistic_approved');
COMMIT;

SELECT count(*) AS differences FROM scenario_diff('optimistic', 'optimistic_approved', 'demand');
UPDATE demand SET qty = 0;                            -- the base moves on
SELECT * FROM oa.demand ORDER BY ALL;                 -- still the approved variant
```
