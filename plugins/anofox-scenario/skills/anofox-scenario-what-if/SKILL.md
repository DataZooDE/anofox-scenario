---
name: anofox-scenario-what-if
description: >
  What-if planning with the anofox_scenario DuckDB extension: branch tables into named
  plan variants (optimistic / pessimistic / baseline), change each with ordinary SQL, and
  compare them side by side or against the real data. Use when the user wants to model a
  demand, price, budget or capacity change without touching the real table, keep several
  plan variants next to each other (S&OP, budgeting), compare KPIs across variants, or run
  anofox_forecast forecasts per scenario. Main tools: scenario_create (with from_scenario
  for variants of a variant), ATTACH ... (TYPE scenario), and scenario_diff between variants.
version: 2026.10.21
user-invocable: false
---

# Anofox Scenario — What-If Planning Playbook

**Extension:** `anofox_scenario` v2026.10.21 | **DuckDB:** v1.5.5+ and v2.0 | **Install:** `INSTALL anofox_scenario FROM community; LOAD anofox_scenario;`

A scenario is a named, Git-like branch of the tables in your database. You attach it as a
catalog and use plain SQL on it; only the rows you change are stored, the real tables are
never written. This playbook builds plan variants and compares them. For edge cases and
error messages see the `anofox-scenario-reference` skill; to apply a variant to the real
data see `anofox-scenario-safe-change`.

## Critical gotchas

- **Create, then attach.** `CALL scenario_create('optimistic')` only registers the scenario.
  Reading and writing needs `ATTACH 'optimistic' AS opt (TYPE scenario)`. The string is the
  scenario name, not a file path.
- **Always qualify with the alias.** `opt.demand` is the scenario's view of the table;
  plain `demand` is still the real table. Writes through `opt.` never reach the base.
- **Default (delta) scenarios see later base changes** on every row the scenario did not
  touch itself. That is what you want for a quick what-if on a stable table. For a variant
  that must not move while the base is updated (a plan you will be held to), create it with
  `mode := 'materialized'` (full copy, see "Choosing a mode").
- **No DDL inside a scenario.** `CREATE TABLE opt.x`, `ALTER TABLE opt.demand ...` fail with
  `DDL operations are not permitted in scenarios`. Change the schema in the base (new base
  tables show up in delta scenarios automatically), or keep helper tables in `main` / `TEMP`.
- **Direction of the 3-argument diff:** `scenario_diff(a, b, 'table')` reports `old_value`
  from `a` and `new_value` from `b`. `'main'` stands for the real table.
- **Cleanup order:** `DETACH` an alias before `scenario_drop`, and drop branches before the
  scenario they branch from.

## The playbook

### 1. The base table

Any table works. A PRIMARY KEY gives row-level diffs keyed by that key; a table without one
can declare a key at `scenario_create` (`key_columns := MAP {'demand': ['sku', 'month']}`) or
is compared as a bag of rows (see the reference skill).

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

Creating a delta scenario costs one registry row, however large the database.

```sql
CALL scenario_create('optimistic', 'US demand +10%');
CALL scenario_create('pessimistic', 'EU demand -20%, SKU B delisted in EU');

ATTACH 'optimistic' AS opt (TYPE scenario);
ATTACH 'pessimistic' AS pes (TYPE scenario);
```

### 3. Apply the assumptions with ordinary SQL

`INSERT`, `UPDATE`, `DELETE`, `TRUNCATE`, `MERGE INTO` and `INSERT ... ON CONFLICT` all work
on scenario tables, with the base's constraints enforced.

```sql
UPDATE opt.demand SET qty = qty * 1.1 WHERE region = 'US';

UPDATE pes.demand SET qty = qty * 0.8 WHERE region = 'EU';
DELETE FROM pes.demand WHERE sku = 'B' AND region = 'EU';
```

Assumptions kept in a table apply as a join:

```sql
CREATE TEMP TABLE uplift (region VARCHAR, factor DOUBLE);
INSERT INTO uplift VALUES ('US', 1.05), ('EU', 1.02);

CALL scenario_create('moderate', 'uplift table applied');
ATTACH 'moderate' AS mod (TYPE scenario);
UPDATE mod.demand SET qty = demand.qty * u.factor FROM uplift u WHERE demand.region = u.region;
```

### 4. Compare KPIs across variants

Each attached scenario is a full, queryable copy of the data, so a KPI comparison is a
`UNION ALL` of the same query over each variant (the base included).

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

### 5. Row-level differences

Against the real table (2 arguments): one row per changed key and column.

```sql
SELECT * FROM scenario_diff('pessimistic', 'demand') ORDER BY ALL;
```

Output columns: the key columns in their own types (`sku`, `region`, `month` here), then
`change_type` (`added` / `removed` / `modified`), `column_name` (the changed column for
`modified`, NULL otherwise), `old_value`, `new_value` (as text).

Between two variants (3 arguments; `old` = first, `new` = second):

```sql
SELECT * FROM scenario_diff('optimistic', 'pessimistic', 'demand') ORDER BY ALL;
```

Per-table counts for a whole scenario:

```sql
SELECT * FROM scenario_diff_summary('pessimistic');
```

Diffs are ordinary table functions, so filter and aggregate them like any table:

```sql
SELECT region, count(*) AS changed_cells
FROM scenario_diff('optimistic', 'demand')
WHERE change_type = 'modified'
GROUP BY region;
```

### 6. Variants of a variant (branches)

`from_scenario` starts a new scenario from another one's current state; it inherits the
parent's changes and adds its own.

```sql
CALL scenario_create('optimistic_promo', from_scenario := 'optimistic');
ATTACH 'optimistic_promo' AS promo (TYPE scenario);
UPDATE promo.demand SET qty = qty + 20 WHERE sku = 'A' AND month = DATE '2026-02-01';

-- only the promo's own changes relative to its parent
SELECT * FROM scenario_diff('optimistic', 'optimistic_promo', 'demand');
```

A scenario with branches cannot be dropped or merged until its branches are dropped. To
apply the chosen variant to the real data, merge it (a branch carries its parent's changes)
and then drop it and its ancestors: see "Merging a branch" in `anofox-scenario-safe-change`.

### 7. Inventory and cleanup

```sql
SELECT name, mode, frozen, parent, description FROM scenario_list() ORDER BY name;
```

```sql
DETACH promo;
CALL scenario_drop('optimistic_promo');
DETACH mod;
CALL scenario_drop('moderate');
```

Scenarios live inside the database file (`__anofox_scenario` schema), so they survive a
restart and travel with the `.duckdb` file. Re-`ATTACH` them in a new session.

## Choosing a mode

| Need | Create with | Base changes after creation | Cost |
|---|---|---|---|
| Quick what-if on a stable base | `scenario_create('s')` (delta, default) | show through, except on rows the scenario touched | one registry row |
| Variant that must stay reproducible while the base keeps changing | `mode := 'materialized'` | invisible | copies every base table |
| Same isolation without the copy, base in DuckLake | `base := '<attached ducklake>'` | invisible (reads pinned to creation time) | none |

Materialized and DuckLake scenarios are covered in `anofox-scenario-snapshot-audit`.

## Forecast per scenario (with `anofox_forecast`)

The two extensions compose through plain tables: the scenario changes the *inputs* (history,
assumptions), `ts_forecast_by` reads a scenario's merged table like any other, and the
forecast output goes to a normal table per variant (or into a scenario table that exists in
the base, so `scenario_diff` compares forecasts too).

```sql
-- requires: anofox_forecast
INSTALL anofox_forecast FROM community;
LOAD anofox_forecast;

CREATE TABLE sales (sku VARCHAR, ds DATE, y DOUBLE);
INSERT INTO sales
SELECT CASE WHEN i % 2 = 0 THEN 'A' ELSE 'B' END, DATE '2025-01-01' + INTERVAL (i // 2) DAY,
       100 + (i // 2) * 0.5 + 10 * sin(2 * pi() * (i // 2) / 7.0)
FROM range(0, 240) t(i);

CALL scenario_create('stockout', 'SKU A lost 10 days of sales');
ATTACH 'stockout' AS so (TYPE scenario);
UPDATE so.sales SET y = 0 WHERE sku = 'A' AND ds BETWEEN DATE '2025-03-01' AND DATE '2025-03-10';

-- ts_forecast_by takes the source as a table name: a scenario table is 'so.sales'
CREATE TABLE fc_base AS SELECT * FROM ts_forecast_by('sales', sku, ds, y, 'AutoETS', 14, '1d');
CREATE TABLE fc_stockout AS SELECT * FROM ts_forecast_by('so.sales', sku, ds, y, 'AutoETS', 14, '1d');

-- output: <group col>, forecast_step, <date col>, yhat, yhat_lower, yhat_upper, model_name
SELECT sku, ds, round(b.yhat, 1) AS base, round(s.yhat, 1) AS stockout,
       round(s.yhat - b.yhat, 1) AS delta
FROM fc_base b JOIN fc_stockout s USING (sku, ds)
ORDER BY sku, ds;
```

Load the `anofox-forecast` plugin's skills for model choice and backtesting.

## When not to use a scenario

- Several people writing the *same* scenario at the same time: concurrent writes to the same
  rows conflict like in any DuckDB table, and the first write to a table from two
  transactions at once makes one of them retry.
- Schema experiments (new columns, new tables inside the branch): DDL is rejected by design.
- Data that does not fit one DuckDB process.
