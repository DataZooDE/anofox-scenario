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
  <img src="https://img.shields.io/badge/DuckDB-v1.5.4%2B-green.svg" alt="DuckDB Compatibility">
</p>

---

**anofox-scenario** is a DuckDB extension that enables Git-like branching for analytical databases. Create isolated scenarios for what-if analysis, compare scenarios against baselines, capture immutable snapshots, and maintain structured audit trails—all within a single `.duckdb` file.

## Key Features

| Feature | Description |
|---------|-------------|
| **Scenario Branching** | Create isolated branches for what-if analysis without duplicating data |
| **Copy-on-Write Storage** | Only modifications are stored; base data remains immutable |
| **Transparent SQL** | Standard INSERT/UPDATE/DELETE work naturally in scenarios |
| **Scenario Comparison** | Row-level diffs between scenarios or against baselines |
| **Immutable Snapshots** | Point-in-time captures for audit trails and rollback points |
| **Embedded Protocols** | Built-in documentation for decisions, findings, and change logs |
| **Database Portability** | Everything stored in a single `.duckdb` file—copy and share freely |

## Use Cases

- **S&OP What-If Analysis**: Model demand changes, supply disruptions, or pricing scenarios
- **Budget Planning**: Create optimistic/pessimistic/baseline variants
- **Data Validation**: Test ETL changes in isolated scenarios before applying
- **Audit Compliance**: Capture snapshots with embedded documentation for regulatory review

---

## Installation

### From DuckDB Community Extensions (Coming Soon)

```sql
INSTALL anofox_scenario FROM community;
LOAD anofox_scenario;
```

### Build from Source

```bash
git clone https://github.com/DataZooDE/anofox-scenario.git
cd anofox-scenario
GEN=ninja make
make test
```

The extension will be built at:
```
build/release/extension/anofox_scenario/anofox_scenario.duckdb_extension
```

---

## Quick Start

### 1. Create a Scenario

```sql
-- Load the extension
LOAD 'anofox_scenario';

-- Create a scenario for what-if analysis
SELECT scenario_create('price_increase', 'Analyzing 10% price increase impact');
-- Returns: true
```

### 2. Make Modifications

```sql
-- Register a table for copy-on-write storage
SELECT delta_create('price_increase', 'products');

-- Modify data in the scenario (base table unchanged)
INSERT INTO _scen_price_increase._delta_products
    (_op, id, name, price)
VALUES ('U', 1, 'Widget Pro', 10.99);  -- Update price from 9.99 to 10.99
```

### 3. Compare Changes

```sql
-- See what changed in the scenario
SELECT * FROM scenario_compare('price_increase', 'products');
```

| diff_type | id | column_name | old_value | new_value |
|-----------|-----|-------------|-----------|-----------|
| changed   | 1   | price       | 9.99      | 10.99     |

### 4. Document Decisions

```sql
-- Embed audit documentation directly in the database
SELECT protocol_set_why('price_increase', 'Testing revenue impact of 10% price increase');
SELECT protocol_add_finding('price_increase', 'Revenue projected to increase 8% with minimal churn');
SELECT protocol_set_decision('price_increase', 'Approved for Q2 rollout');
```

### 5. Create Snapshots

```sql
-- Capture immutable point-in-time snapshot
SELECT snapshot_create('price_increase', 'q2_approved', 'Final approved pricing for Q2');

-- Later, compare current state against the snapshot
SELECT * FROM snapshot_compare('q2_approved', 'products');
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
│   main.products │       │ _delta_products     │
└─────────────────┘       └─────────────────────┘
```

**Benefits:**
- **O(1) scenario creation** — no data copying
- **Storage proportional to changes** — only deltas stored
- **Full analytical performance** — DuckDB columnar scans on unchanged data

### Scenario Lifecycle

```
scenario_create()     Create isolated branch
        │
        ├── delta_create()     Enable COW for specific tables
        │
        ├── scenario_branch()  Create child scenarios
        │
        ├── scenario_compare() Diff against baseline
        │
        ├── snapshot_create()  Capture immutable snapshot
        │
        └── scenario_drop()    Clean up when done
```

---

## API Reference

### Scenario Management

| Function | Description |
|----------|-------------|
| `scenario_create(name, desc)` | Create new scenario |
| `scenario_create(name, desc, capture_rowids)` | Create with optional rowid capture (default: true) |
| `scenario_branch(source, name, desc)` | Branch from existing scenario |
| `scenario_list()` | List all scenarios |
| `scenario_stats(name)` | Get scenario statistics |
| `scenario_validate(name)` | Check scenario integrity |
| `scenario_archive(name)` | Mark as read-only |
| `scenario_unarchive(name)` | Restore write capability |
| `scenario_drop(name)` | Remove scenario |
| `scenario_schema(name)` | Get schema name for SET search_path |

### Delta Storage

| Function | Description |
|----------|-------------|
| `delta_create(scenario, table)` | Enable COW storage for table |
| `delta_drop(scenario, table)` | Remove delta table |
| `scenario_write(scenario, table, op, data)` | Programmatic row modification |

### Comparison

| Function | Description |
|----------|-------------|
| `scenario_compare(scenario, table)` | Diff scenario vs base |
| `scenario_compare(scenario_a, scenario_b, table)` | Diff two scenarios |
| `scenario_compare_all(scenario)` | Summary diff all tables |

### Snapshots

| Function | Description |
|----------|-------------|
| `snapshot_create(scenario, name, desc)` | Capture point-in-time snapshot |
| `snapshot_list()` | List all snapshots |
| `snapshot_compare(snapshot, table)` | Diff current state vs snapshot |
| `scenario_from_snapshot(snapshot, scenario, desc)` | Create scenario from snapshot |
| `snapshot_drop(name)` | Remove snapshot |

### Protocol Documentation

| Function | Description |
|----------|-------------|
| `protocol_set_why(scenario, text)` | Document scenario purpose |
| `protocol_set_plan(scenario, text)` | Document planned approach |
| `protocol_log_change(scenario, text)` | Append to change log |
| `protocol_add_finding(scenario, text)` | Record analysis findings |
| `protocol_set_decision(scenario, text)` | Document final decision |
| `protocol_read(scenario)` | Read all protocol sections |
| `protocol_export_markdown(scenario, path)` | Export to markdown file |

### Configuration

| Setting | Description | Default |
|---------|-------------|---------|
| `scenario_schema_prefix` | Prefix for scenario schemas | `_scen_` |

```sql
SET scenario_schema_prefix = '_my_';  -- Custom prefix
```

For complete API documentation, see [docs/API_REFERENCE.md](docs/API_REFERENCE.md).

---

## Building from Source

### Prerequisites

- CMake 3.5+
- C++17 compatible compiler
- Ninja (recommended) or Make
- OpenSSL development libraries

### Build Commands

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/DataZooDE/anofox-scenario.git
cd anofox-scenario

# Fast incremental build (recommended)
GEN=ninja make

# Debug build
make debug

# Release build
make release
```

### Running Tests

```bash
# Run all tests (889+ assertions across 20 test files)
make test

# Run specific test file
./build/release/test/unittest "test/sql/scenario_lifecycle.test"
```

### Platform Support

| Platform | Status |
|----------|--------|
| Linux (x86_64) | Supported |
| macOS (x86_64, ARM64) | Supported |
| Windows | Supported |

---

## Interactive Usage

```bash
# Start DuckDB with the extension
./build/release/duckdb

# Load the extension
D LOAD 'build/release/extension/anofox_scenario/anofox_scenario.duckdb_extension';

# Verify it's loaded
D SELECT * FROM duckdb_extensions() WHERE extension_name = 'anofox_scenario';
```

---

## Architecture

The extension consists of six core components:

| Component | Responsibility |
|-----------|---------------|
| **ScenarioManager** | Lifecycle: create, branch, list, archive, drop |
| **DeltaStorageEngine** | COW storage, merge-on-read views, constraint inheritance |
| **ComparisonEngine** | Row-level diffs between states |
| **SnapshotManager** | Immutable point-in-time captures |
| **ProtocolManager** | Embedded documentation storage |
| **MetadataStore** | Internal registry tables |

For detailed architecture documentation, see [docs/spec/architecture.md](docs/spec/architecture.md).

---

## Limitations

- **Single-writer per scenario**: Concurrent writes to the same scenario are not supported
- **DDL restrictions**: Schema modifications not allowed in scenario schemas
- **Rowid stability**: VACUUM/CHECKPOINT may invalidate scenarios; use `scenario_validate()` to check

---

## Contributing

Contributions are welcome! Please read our contributing guidelines before submitting PRs.

```bash
# Development workflow
GEN=ninja make           # Build
make test                # Run tests
```

---

## License

This project is licensed under the **Business Source License 1.1 (BSL)**.

| | |
|---|---|
| 📜 **License** | [BSL 1.1](LICENSE) |
| ✅ **Non-Production Use** | Allowed — development, testing, evaluation |
| ✅ **Modification** | Allowed |
| ✅ **Distribution** | Allowed — under BSL terms |
| 🏢 **Production Use** | Requires commercial license from [DataZoo GmbH](https://datazoo.de) |
| 📅 **Change Date** | 4 years from release → Apache 2.0 |
| ℹ️ **Attribution** | Required — include copyright notice |

**In short:** Free for non-production use. Contact [DataZoo GmbH](https://datazoo.de) for production licensing.

See the [LICENSE](LICENSE) file for the complete license text.

---

## Related Projects

- [DuckDB](https://duckdb.org/) - The in-process analytical database
- [anofox-forecast](https://github.com/anofox/anofox-forecast) - Demand forecasting extension
- [DuckLake](https://github.com/duckdb/ducklake) - Lakehouse metadata management

---

<p align="center">
  <sub>Built with care by <a href="https://datazoo.de">DataZoo GmbH</a></sub>
</p>
