# Anofox Scenario — Forecast per scenario (with `anofox_forecast`)

Companion to the `anofox-scenario-what-if` skill.

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
SELECT * FROM fc_base LIMIT 1;

SELECT sku, ds, round(b.yhat, 1) AS base, round(s.yhat, 1) AS stockout,
       round(s.yhat - b.yhat, 1) AS delta
FROM fc_base b JOIN fc_stockout s USING (sku, ds)
ORDER BY sku, ds;
```

Load the `anofox-forecast` plugin's skills for model choice and backtesting.
