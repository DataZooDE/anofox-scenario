---
name: anofox-scenario-what-if
description: >
  What-if planning and sensitivity analysis on DuckDB tables with the anofox_scenario
  extension: clone, fork or sandbox tables into named plan variants (optimistic /
  pessimistic, +5% / +10% / +20%), change each with ordinary SQL, and compare KPIs and rows
  across variants or with the current data, without touching the real tables. Use when the
  user asks "what happens if" demand, prices, costs, budget or capacity change, wants to
  clone a table to tweak numbers and see the effect, runs a sensitivity analysis, keeps
  several plan variants side by side (S&OP, budgeting), or forecasts per scenario with
  anofox_forecast. Not for applying a variant to the real tables
  (anofox-scenario-safe-change) or for approved, signed-off versions
  (anofox-scenario-snapshot-audit).
version: 2026.11.13
user-invocable: false
---

# Anofox Scenario — What-If Planning Playbook

**Extension:** `anofox_scenario` v2026.11.13 | **DuckDB:** v1.5.5+ and v2.0 | **Install:** `INSTALL anofox_scenario FROM community; LOAD anofox_scenario;`

A scenario is a named, Git-like branch of the tables in your database. Attach it as a catalog
and use plain SQL on it; only changed rows are stored, the real tables are never written.
Errors and edge cases: `anofox-scenario-reference`. Forecasting per scenario:
[forecast.md](forecast.md).

## Ask first

Applying a variant to the real tables is a merge: follow `anofox-scenario-safe-change` and get
the user's yes first. Drop only scenarios created in this conversation, or after asking.

## Before creating the variants

Decide these first; changing them later means new scenarios.

1. **May untouched rows follow later base changes?**

   | Need | Create with | Base changes after creation | Cost |
   |---|---|---|---|
   | Quick what-if on a stable base | `scenario_create('s')` (delta, default) | show through, except on rows the scenario touched | one registry row |
   | Variant that must not move while the base changes | `mode := 'materialized'` | invisible | copies every base table |
   | Same isolation, base in DuckLake | `base := '<attached ducklake>'` | invisible (pinned to creation time) | none |

2. **Keyless tables?** Without a PRIMARY KEY, diffs compare whole rows. If the table has a
   logical key, declare it at creation: `key_columns := MAP {'demand': ['sku', 'month']}`.
3. **Variants of a variant?** `from_scenario := 'parent'` branches; the branch copies the
   parent's existing deltas, so K branches of a heavily edited parent cost K copies of its
   delta. A branch cannot be materialized.
4. **Very large edits** (more than ~4M updated or deleted keys in one table) switch scans to
   index lookups; see `anofox_scenario_suppression_hash_limit` in the reference.

## Critical gotchas

- **Create, then attach.** `scenario_create` only registers. Reading and writing needs
  `ATTACH 'optimistic' AS opt (TYPE scenario)`; the string is the scenario name, not a file.
- **Always qualify with the alias.** `opt.demand` is the scenario's view; plain `demand` is
  the real table. Avoid `USE opt`: the `scenario_*` functions then find no scenarios.
- **Views:** `opt.my_view` evaluates a host view against the scenario, unless the view names
  the catalog (`mydb.main.demand`): then it silently reads the real table.
- **No DDL inside a scenario** (`DDL operations are not permitted in scenarios`): keep helper
  tables in `main` or `TEMP`.
- **Direction of the 3-argument diff:** `scenario_diff(a, b, 'table')`: `old_value` from `a`,
  `new_value` from `b`; `'main'` is the real table.
- **Cleanup order:** `DETACH` before `scenario_drop`; drop branches before their parent.

## The playbook

### 1. The base table

```sql
CREATE TABLE demand (
    sku VARCHAR, region VARCHAR, month DATE, qty DECIMAL(12, 2),
    PRIMARY KEY (sku, region, month));
INSERT INTO demand VALUES
    ('A', 'US', DATE '2026-01-01', 100), ('A', 'EU', DATE '2026-01-01', 80),
    ('B', 'US', DATE '2026-01-01', 50),  ('B', 'EU', DATE '2026-01-01', 40),
    ('A', 'US', DATE '2026-02-01', 110), ('A', 'EU', DATE '2026-02-01', 85),
    ('B', 'US', DATE '2026-02-01', 55),  ('B', 'EU', DATE '2026-02-01', 45);
```

### 2. One scenario per variant

```sql
CALL scenario_create('optimistic', 'US demand +10%');
CALL scenario_create('pessimistic', 'EU demand -20%, SKU B delisted in EU');

ATTACH 'optimistic' AS opt (TYPE scenario);
ATTACH 'pessimistic' AS pes (TYPE scenario);
```

### 3. Apply the assumptions with ordinary SQL

`INSERT`, `UPDATE`, `DELETE`, `TRUNCATE`, `MERGE INTO` and `INSERT ... ON CONFLICT` all work,
with the base's constraints enforced.

```sql
UPDATE opt.demand SET qty = qty * 1.1 WHERE region = 'US';

UPDATE pes.demand SET qty = qty * 0.8 WHERE region = 'EU';
DELETE FROM pes.demand WHERE sku = 'B' AND region = 'EU';
```

Assumptions kept in a table apply as a join; a sensitivity sweep is one scenario per step:

```sql
CREATE TEMP TABLE uplift (region VARCHAR, factor DOUBLE);
INSERT INTO uplift VALUES ('US', 1.05), ('EU', 1.02);

CALL scenario_create('moderate', 'uplift table applied');
ATTACH 'moderate' AS mod (TYPE scenario);
UPDATE mod.demand SET qty = demand.qty * u.factor FROM uplift u WHERE demand.region = u.region;
```

### 4. Compare KPIs across variants

Each attached scenario is a full, queryable version of the data, so a KPI comparison is a
`UNION ALL` of the same query over each variant, the base included:

```sql
SELECT variant, region, sum(qty) AS total_qty
FROM (
    SELECT 'base' AS variant, * FROM demand
    UNION ALL SELECT 'optimistic', * FROM opt.demand
    UNION ALL SELECT 'pessimistic', * FROM pes.demand
)
GROUP BY ALL
ORDER BY region, variant;
```

Side by side, one column per variant:

```sql
PIVOT (
    SELECT 'base' AS variant, * FROM demand
    UNION ALL SELECT 'optimistic', * FROM opt.demand
    UNION ALL SELECT 'pessimistic', * FROM pes.demand
) ON variant USING sum(qty) GROUP BY region ORDER BY region;
```

An existing KPI view works per variant when it names the table without a catalog:

```sql
CREATE VIEW demand_by_region AS SELECT region, sum(qty) AS total_qty FROM demand GROUP BY region;
SELECT 'optimistic' AS variant, * FROM opt.demand_by_region
UNION ALL SELECT 'pessimistic', * FROM pes.demand_by_region
ORDER BY region, variant;
```

### 5. Row-level differences

Counts first (one row per changed table), then the rows:

```sql
SELECT * FROM scenario_diff_summary('pessimistic');
```

Against the real table (2 arguments): one row per changed key and column. Add `LIMIT` or a
filter for large changes.

```sql
SELECT * FROM scenario_diff('pessimistic', 'demand') ORDER BY ALL;
```

Columns: the key columns in their own types, `change_type` (`added` / `removed` /
`modified`), `column_name` (for `modified`), `old_value`, `new_value` (text).

Between two variants (3 arguments; `old` = first, `new` = second):

```sql
SELECT * FROM scenario_diff('optimistic', 'pessimistic', 'demand') ORDER BY ALL;
```

Diffs are table functions, so filter and aggregate them:

```sql
SELECT region, count(*) AS changed_cells
FROM scenario_diff('optimistic', 'demand')
WHERE change_type = 'modified'
GROUP BY region;
```

### 6. Variants of a variant (branches)

```sql
CALL scenario_create('optimistic_promo', from_scenario := 'optimistic');
ATTACH 'optimistic_promo' AS promo (TYPE scenario);
UPDATE promo.demand SET qty = qty + 20 WHERE sku = 'A' AND month = DATE '2026-02-01';

-- only the promo's own changes relative to its parent
SELECT * FROM scenario_diff('optimistic', 'optimistic_promo', 'demand');
```

A scenario with branches cannot be dropped or merged until its branches are dropped. To apply
the chosen variant, merge it (it carries its parent's changes): "Merging a branch" in
`anofox-scenario-safe-change`. To keep it as the approved plan: `anofox-scenario-snapshot-audit`.

### 7. Inventory and cleanup

```sql
SELECT name, mode, frozen, parent, merged_at, description FROM scenario_list() ORDER BY name;
```

After the user agrees which variants to discard:

```sql
DETACH promo;
CALL scenario_drop('optimistic_promo');
DETACH mod;
CALL scenario_drop('moderate');
```

Scenarios live inside the database file (`__anofox_scenario` schema): they survive a restart
and travel with the `.duckdb` file. Re-`ATTACH` them in each new session. Python and sharing:
the reference skill's [behaviour.md](../anofox-scenario-reference/behaviour.md).

## When not to use a scenario

- Several people writing the *same* scenario at once: concurrent writes to the same rows
  conflict like in any DuckDB table.
- Schema experiments (new columns or tables inside the branch): DDL is rejected by design. A
  column added to the base table (`ALTER TABLE ... ADD COLUMN`) does reach every scenario.
- Data that does not fit one DuckDB process.
