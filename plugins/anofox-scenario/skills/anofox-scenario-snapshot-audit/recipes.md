# Anofox Scenario — Snapshot & audit recipes

Companion to the `anofox-scenario-snapshot-audit` skill: DuckLake snapshots, exporting the
audit log, legacy databases.

## DuckLake: snapshots without copying

Over a DuckLake catalog a scenario is pinned to the lake snapshot at creation
(`AT (TIMESTAMP => created_at)`): creation copies nothing and later lake commits are invisible
to it. Branches keep their parent's snapshot. Tables the lake creates after the snapshot are
not part of the scenario.

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

What changed in the live lake since approval (`scenario_diff` against `'main'` compares with
the pinned snapshot and shows nothing):

```sql
-- requires: ducklake
SELECT coalesce(a.cost_center, l.cost_center) AS cost_center,
       a.amount AS approved, l.amount AS current
FROM lba.budget a FULL JOIN lake.budget l USING (cost_center)
WHERE a.amount IS DISTINCT FROM l.amount
ORDER BY cost_center;
```

A DuckLake-based snapshot is readable only while the lake keeps it: the DuckLake catalog file,
its `DATA_PATH` files and the snapshot itself (not expired or compacted away) must be retained
alongside the `.duckdb` file.

## Export the audit log as Markdown

```sql
-- doc-snippets: new session
CREATE TABLE IF NOT EXISTS audit_events (
    event_id BIGINT PRIMARY KEY, scenario VARCHAR NOT NULL, event VARCHAR NOT NULL,
    actor VARCHAR, logged_at TIMESTAMP DEFAULT current_timestamp, note VARCHAR);
INSERT INTO audit_events (event_id, scenario, event, actor, note) VALUES
    (1, 'budget_2026_approved', 'approved', 'board', 'minutes ref B-26-03');

COPY (SELECT '## ' || scenario || ' / ' || event || ' (' || logged_at || ', ' || coalesce(actor, '?') || ')'
             || chr(10) || coalesce(note, '')
      FROM audit_events ORDER BY event_id)
TO 'audit_events.md' (FORMAT csv, QUOTE '', HEADER false);
```

## Legacy databases (v0.1)

Databases written by v0.1 (`_scenario_registry`, `_scen_*`, `_snap_*` tables) are migrated
once, in place: archived scenarios become frozen, v0.1 snapshots become materialized + frozen
scenarios, and the `_scenario_protocols` notes are kept.

```sql
SELECT * FROM scenario_migrate();
```
