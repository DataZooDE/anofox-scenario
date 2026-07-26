<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="docs/assets/logo-light.svg">
    <img alt="anofox-scenario" src="docs/assets/logo-light.svg" height="80">
  </picture>
</p>

<h3 align="center">Git-Like Branching for DuckDB</h3>

<p align="center">
  <a href="https://github.com/DataZooDE/anofox-scenario/actions/workflows/MainDistributionPipeline.yml"><img src="https://github.com/DataZooDE/anofox-scenario/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=main" alt="Build Status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-BSL_1.1-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/DuckDB-v1.5.5%2B-green.svg" alt="DuckDB Compatibility">
</p>

---

**anofox-scenario** gives you Git-like branches over your DuckDB tables. Change data in a
branch, see exactly what changed, then keep it or throw it away — without ever copying the
table or touching the original.

```sql
INSTALL anofox_scenario FROM community;
LOAD anofox_scenario;

-- Given any existing table, e.g. forecast(id, region, qty).
-- Branch it. Nothing below modifies the real `forecast` table.
CALL scenario_create('optimistic', 'demand +10%');
ATTACH 'optimistic' AS opt (TYPE scenario);

UPDATE opt.forecast SET qty = qty * 1.1 WHERE region = 'US';

SELECT * FROM opt.forecast;                          -- the what-if world
SELECT * FROM forecast;                              -- the real world, untouched
SELECT * FROM scenario_diff('optimistic', 'forecast');  -- exactly what differs
```

That's the whole idea. Only the rows you actually change are stored, so a scenario over a
100-million-row table costs you the rows you touched — not a copy.

### Is this for you?

**A good fit if** you want to model what-ifs on data that fits on one machine, test a change
before applying it, keep several named variants of a dataset side by side, or hand someone a
single file containing both a baseline and the alternatives. Typical uses:

- **S&OP and demand planning** — model demand shifts, supply disruptions, or price changes
- **Budget planning** — keep optimistic, pessimistic, and baseline variants side by side
- **Data validation** — try an ETL or migration change in a branch before applying it for real
- **Audit trails** — freeze an approved plan as an immutable record you can diff against later

**Not a fit if** you need multiple people writing to the same scenario concurrently, you're
working at data-lake scale across a cluster, or you need to change the *schema* inside a
branch — DDL in scenarios is rejected by design.

---

## Installation

Requires DuckDB v1.5.5 or later.

```sql
INSTALL anofox_scenario FROM community;
LOAD anofox_scenario;
```

<details>
<summary>Build from source instead</summary>

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-scenario.git
cd anofox-scenario
GEN=ninja make
make test
```

The extension is built at
`build/release/extension/anofox_scenario/anofox_scenario.duckdb_extension`.

</details>

---

## Quick Start

### 1. Create a Scenario

```sql
LOAD anofox_scenario;

CREATE TABLE products (id INTEGER PRIMARY KEY, name VARCHAR, price DECIMAL(10,2));
INSERT INTO products VALUES (1, 'Widget', 9.99), (2, 'Gadget', 24.50);

-- Register a scenario for what-if analysis
CALL scenario_create('price_increase', 'Analyzing 10% price increase impact');
```

### 2. Make Modifications

A scenario is an **attached catalog** — you edit it with ordinary SQL, and the base
table is never written to.

```sql
ATTACH 'price_increase' AS pi (TYPE scenario);

UPDATE pi.products SET price = price * 1.1 WHERE id = 1;
INSERT INTO pi.products VALUES (3, 'Doohickey', 5.00);
DELETE FROM pi.products WHERE id = 2;
```

```sql
SELECT count(*) FROM products;     -- 2, base untouched
SELECT count(*) FROM pi.products;  -- 2, merged view (1 updated, 1 added, 1 removed)
```

### 3. Compare Changes

```sql
SELECT * FROM scenario_diff('price_increase', 'products');
```

| id | change_type | column_name | old_value | new_value |
|----|-------------|-------------|-----------|-----------|
| 1  | modified    | price       | 9.99      | 10.99     |
| 2  | removed     | NULL        | NULL      | NULL      |
| 3  | added       | NULL        | NULL      | NULL      |

```sql
-- Per-table rollup
SELECT * FROM scenario_diff_summary('price_increase');

-- Diff any two sides, not just against the base
SELECT * FROM scenario_diff('main', 'price_increase', 'products');
```

### 4. Branch and Freeze

```sql
-- Branch off an existing scenario, inheriting its changes
CALL scenario_create('price_increase_eu', from_scenario := 'price_increase');

-- Full copy instead of a delta: immune to later base changes
CALL scenario_create('q2_approved', mode := 'materialized');

-- Reject further writes; reads keep working. A frozen materialized
-- scenario is effectively an immutable snapshot.
CALL scenario_freeze('q2_approved');
```

### 5. Merge Back

```sql
-- Inspect the planned actions and any conflicts first (no side effects)
SELECT * FROM scenario_merge_preview('price_increase');
```

| table_name | key | action | conflict |
|------------|-----|--------|----------|
| products   | 1   | update | false    |
| products   | 3   | insert | false    |
| products   | 2   | delete | false    |

```sql
-- Merge refuses while branches exist, so drop the child from step 4 first
CALL scenario_drop('price_increase_eu');

-- Apply the scenario's changes to the base table
SELECT * FROM scenario_merge('price_increase', on_conflict := 'abort');
```

On success the scenario ends up frozen with an empty delta. Use `on_conflict := 'ours'` to let
the scenario win or `'theirs'` to let the base win, instead of aborting.

```sql
DETACH pi;  -- handle gone, scenario data persists
```

---

## Core Concepts

### Delta-Main Architecture

anofox-scenario uses the **Delta-Main pattern** for efficient copy-on-write storage:

```
┌─────────────────────────────────────────────────┐
│                   User Query                    │
│         SELECT * FROM products                  │
└─────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────┐
│              Merge-on-Read View                 │
│    Combines base data + scenario deltas         │
└─────────────────────────────────────────────────┘
         │                           │
         ▼                           ▼
┌─────────────────┐       ┌─────────────────────┐
│   Base Table    │       │    Delta Table      │
│   (Immutable)   │       │   (Modifications)   │
│   main.products │       │ s1_delta_products   │
└─────────────────┘       └─────────────────────┘
```

Delta tables live in the host database under the internal `__anofox_scenario` schema, named
`s<scenario_id>_delta_<table>`. Because everything is inside the host database, a `.duckdb` file
carries its scenarios with it — copy the file and the branches come along.

**Benefits:**
- **Cheap scenario creation** — delta mode stores O(#tables) metadata and copies no rows
- **Storage proportional to changes** — only deltas stored
- **Full analytical performance** — DuckDB columnar scans on unchanged data

### Choosing a scenario mode

This is the one real decision at `scenario_create`, and it comes down to whether the scenario
should notice later changes to the base table.

| Mode | Create with | Base changes after creation… | Cost |
|------|-------------|------------------------------|------|
| **Delta** (default) | `scenario_create('s')` | …show through, except on rows the scenario already touched | Metadata only |
| **Materialized** | `mode := 'materialized'` | …are invisible; the scenario is fully isolated | Copies every base table |
| **DuckLake snapshot** | `base := '<attached lake>'` | …are invisible; reads pinned to creation time | No copy |

Use the default for a short-lived what-if against a stable base. Use `materialized` when the
scenario must stay reproducible even while the base keeps moving — for example an approved plan
you'll be audited on later.

### Scenario Lifecycle

```
scenario_create()                      Register a scenario
        │
        ├── ATTACH (TYPE scenario)     Read/write it with ordinary SQL
        │
        ├── scenario_create(           Branch a child scenario
        │       from_scenario := ...)
        │
        ├── scenario_diff()            Diff against baseline or another scenario
        │
        ├── scenario_freeze()          Make it read-only (immutable when materialized)
        │
        ├── scenario_merge()           Apply changes back to the base
        │
        └── scenario_drop()            Clean up when done
```

---

## API Reference

### Reading and Writing

| Statement | Description |
|-----------|-------------|
| `ATTACH 'name' AS alias (TYPE scenario)` | Expose the scenario as a catalog — this is the entire read/write UX |
| `SELECT / INSERT / UPDATE / DELETE / TRUNCATE` on `alias.<table>` | Ordinary SQL; reads merge base + delta, writes go to the delta |
| `DETACH alias` | Drop the handle; scenario data persists |

### Scenario Management

| Function | Description |
|----------|-------------|
| `CALL scenario_create(name, [desc], [mode := 'delta'\|'materialized'], [from_scenario := parent], [base := catalog], [key_columns := MAP {...}])` | Register a scenario. `materialized` copies every base table; `from_scenario` branches; `base` uses another attached catalog (DuckLake bases pin to creation time); `key_columns` declares row identity for tables without a PK |
| `CALL scenario_drop(name)` | Remove the scenario and its delta/materialized tables. Refuses while attached or while branches exist |
| `CALL scenario_freeze(name)` / `scenario_unfreeze(name)` | Reject/allow writes; reads keep working. A frozen materialized scenario is a snapshot |
| `SELECT * FROM scenario_list()` | `scenario_id, name, mode, frozen, parent, created_at, description` |
| `SELECT * FROM scenario_refresh(name, [key_columns := MAP {...}])` | Create delta tables for base tables added after `scenario_create` |
| `SELECT * FROM scenario_migrate()` | One-way migration of a legacy v0.1 database into the v2 layout |

### Comparison

| Function | Description |
|----------|-------------|
| `SELECT * FROM scenario_diff(scenario, table)` | Diff against origin: `<pk cols>`, `change_type` (`added`/`removed`/`modified`), `column_name`, `old_value`, `new_value` |
| `SELECT * FROM scenario_diff(a, b, table)` | Diff any two sides (`'main'` or any scenario); `old` = side a, `new` = side b |
| `SELECT * FROM scenario_diff_summary(scenario)` | Per-table `rows_added / rows_modified / rows_removed` |

Diffs stream through the engine, so filters and aggregates compose normally and PK columns
keep their native types. Requires a PK or a declared `key_columns` on the table.

### Merge-Back

| Function | Description |
|----------|-------------|
| `SELECT * FROM scenario_merge_preview(scenario)` | Planned actions: `table_name, key, action, conflict`. Streaming, no side effects |
| `SELECT * FROM scenario_merge(scenario, [on_conflict := 'abort'\|'ours'\|'theirs'])` | Apply the delta to the base. `abort` (default) throws on conflict; `ours` = scenario wins; `theirs` = base wins. On success the scenario ends frozen with an empty delta |

### Snapshots and Audit Notes

There is no separate snapshot or protocol API. For an immutable point-in-time capture, create a
materialized scenario and freeze it:

```sql
CALL scenario_create('q2_approved', mode := 'materialized');
CALL scenario_freeze('q2_approved');
```

For audit notes, use a plain table — see the recipe in
[docs/API_REFERENCE.md](docs/API_REFERENCE.md#protocols--audit-notes).

For complete API documentation, including isolation tiers and current limitations, see
[docs/API_REFERENCE.md](docs/API_REFERENCE.md).

---

## Development

Prerequisites: CMake 3.5+, a C++17 compiler, Ninja (recommended), and OpenSSL development
libraries. Supported on Linux (x86_64), macOS (x86_64 and ARM64), and Windows.

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-scenario.git
cd anofox-scenario

GEN=ninja make          # fast incremental build
make debug              # debug build with symbols
make test               # 1,100+ assertions across 28 test files

./build/release/test/unittest "test/sql/attach_basic.test"   # one test file
```

To try a locally built extension:

```bash
./build/release/duckdb
```

```sql
LOAD 'build/release/extension/anofox_scenario/anofox_scenario.duckdb_extension';
SELECT * FROM duckdb_extensions() WHERE extension_name = 'anofox_scenario';
```

For how the extension works internally — the read and write paths, the delta contract, the
isolation tiers, and a map of the source tree — see
[docs/spec/architecture.md](docs/spec/architecture.md).

---

## Limitations

- **Single-writer**: host writes and scenario writes cannot share one explicit transaction
- **DDL restrictions**: schema modifications are not permitted inside scenarios
- **Keyless tables**: tables with no PRIMARY KEY need `key_columns :=` declared to unlock
  `scenario_diff`, `MERGE INTO`, and `ON CONFLICT`; UPDATE/DELETE work with bag semantics
- **Overlay isolation**: in the default `delta` mode, base changes show through unless the
  scenario touched the same key — use `mode := 'materialized'` or a DuckLake `base :=` for
  full isolation

See [docs/API_REFERENCE.md](docs/API_REFERENCE.md) for the complete list.

---

## Contributing

Contributions are welcome. Development is test-first: add a failing case in `test/sql/`, make it
pass, and keep `make test` green. See [Development](#development) for build commands and
[docs/spec/architecture.md](docs/spec/architecture.md) for how the internals fit together.

Bug reports are most useful with a minimal reproducer — the `CREATE TABLE`, the
`scenario_create`, and the statement that misbehaves.

---

## License

This project is licensed under the **Business Source License 1.1 (BSL)**.

| | |
|---|---|
| 📜 **License** | [BSL 1.1](LICENSE) |
| ✅ **Non-Production Use** | Allowed — development, testing, evaluation |
| ✅ **Modification** | Allowed |
| ✅ **Distribution** | Allowed — under BSL terms |
| 🏢 **Production Use** | Requires commercial license from [DataZoo GmbH](https://www.datazoo.de) |
| 📅 **Change Date** | 4 years from release → Apache 2.0 |
| ℹ️ **Attribution** | Required — include copyright notice |

**In short:** Free for non-production use. Contact [DataZoo GmbH](https://www.datazoo.de) for production licensing.

See the [LICENSE](LICENSE) file for the complete license text.

---

## Related Projects

- [DuckDB](https://duckdb.org/) - The in-process analytical database
- [anofox-forecast](https://github.com/DataZooDE/anofox-forecast) - Statistical timeseries forecasting
- [DuckLake](https://github.com/duckdb/ducklake) - Lakehouse metadata management

---

<p align="center">
  <sub>Built with care by <a href="https://www.datazoo.de">DataZoo GmbH</a></sub>
</p>
