# anofox-scenario Architecture

Version 0.1 | January 2026

---

## 1. Introduction and Goals

### 1.1 Purpose

anofox-scenario is a DuckDB extension that enables Git-like branching for analytical databases. Users create isolated scenarios for what-if analysis without copying data.

### 1.2 Quality Goals

| Priority | Goal | Description |
|----------|------|-------------|
| 1 | Performance | Scenario creation < 1s for 10GB database |
| 2 | Transparency | Standard SQL works without modification |
| 3 | Isolation | Scenarios don't affect base data or each other |
| 4 | Portability | Database files work across machines |

### 1.3 Stakeholders

- **Analysts**: Run what-if scenarios via SQL
- **Applications**: Integrate scenario management programmatically
- **DBAs**: Manage scenarios and snapshots

---

## 2. Constraints

### 2.1 Technical Constraints

| Constraint | Description |
|------------|-------------|
| DuckDB v1.4.3+ | Target extension API version |
| C++17 | DuckDB extension language requirement |
| Single-writer | DuckDB's concurrency model |
| No DDL in scenarios | Schema changes not supported in scenario branches |

### 2.2 Organizational Constraints

| Constraint | Description |
|------------|-------------|
| SQLLogicTest | All features tested via DuckDB's test framework |
| Incremental TDD | Red-Green-Refactor development cycle |

---

## 3. Context and Scope

### 3.1 Business Context

```
┌─────────────────┐     SQL      ┌──────────────────────┐
│                 │─────────────>│                      │
│  User/App       │              │  DuckDB + Extension  │
│                 │<─────────────│                      │
└─────────────────┘   Results    └──────────────────────┘
```

### 3.2 Technical Context

```
┌─────────────────────────────────────────────────────────────┐
│                        DuckDB Process                        │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                  anofox_scenario Extension           │    │
│  │  ┌───────────────┐  ┌───────────────┐  ┌─────────┐  │    │
│  │  │ScenarioManager│  │SnapshotManager│  │Protocol │  │    │
│  │  │               │  │               │  │Manager  │  │    │
│  │  └───────┬───────┘  └───────┬───────┘  └────┬────┘  │    │
│  │          │                  │               │        │    │
│  │          └────────┬─────────┴───────────────┘        │    │
│  │                   │                                  │    │
│  │          ┌────────▼────────┐                         │    │
│  │          │  MetadataStore  │                         │    │
│  │          └────────┬────────┘                         │    │
│  └───────────────────┼──────────────────────────────────┘    │
│                      │                                       │
│  ┌───────────────────▼──────────────────────────────────┐    │
│  │                 DuckDB Catalog                        │    │
│  │  main schema    │  _scen_* schemas  │  metadata tbls  │    │
│  └──────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. Solution Strategy

### 4.1 Delta-Main Pattern

Base data remains immutable in DuckDB's columnar storage. Scenario modifications are stored in delta tables and merged on read.

```
Read Query Flow:
  User Query → Scenario View → UNION (Base + Delta) with anti-join on deleted

Write Query Flow (future):
  User INSERT/UPDATE/DELETE → OptimizerExtension → Delta Table
```

### 4.2 Schema Isolation

Each scenario gets a dedicated DuckDB schema (`_scen_<name>`) containing:
- Delta tables for modified data
- Views that merge base + delta

### 4.3 Metadata in Main Schema

All metadata tables live in `main` schema with `_scenario_` prefix:
- `_scenario_registry` - Scenario tracking
- `_scenario_tables` - Per-scenario table metadata
- `_scenario_base_rowids` - Rowid preservation
- `_scenario_snapshots` - Snapshot metadata
- `_scenario_protocols` - Documentation storage

---

## 5. Building Block View

### 5.1 Level 1: Extension Components

```
┌─────────────────────────────────────────────────────────────┐
│                   anofox_scenario Extension                  │
├─────────────────┬─────────────────┬─────────────────────────┤
│ ScenarioManager │ SnapshotManager │ ProtocolManager         │
│                 │                 │                         │
│ • scenario_     │ • snapshot_     │ • protocol_set_why      │
│   create        │   create        │ • protocol_log_change   │
│ • scenario_drop │ • snapshot_list │ • protocol_add_finding  │
│ • scenario_list │ • snapshot_drop │ • protocol_read         │
│ • scenario_     │                 │ • protocol_export_md    │
│   branch        │                 │                         │
├─────────────────┴─────────────────┴─────────────────────────┤
│                      MetadataStore                          │
│  • Initialize metadata tables on extension load             │
│  • Provide table schemas for registry, tables, rowids, etc. │
├─────────────────────────────────────────────────────────────┤
│                   DeltaStorageEngine (planned)              │
│  • Delta table creation and management                      │
│  • Merge-on-read view generation                            │
├─────────────────────────────────────────────────────────────┤
│                   OptimizerExtension (planned)              │
│  • Intercept writes to scenario schemas                     │
│  • Redirect to delta tables                                 │
├─────────────────────────────────────────────────────────────┤
│                   ComparisonEngine (planned)                │
│  • scenario_compare for single table diff                   │
│  • scenario_compare_all for summary                         │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 Level 2: ScenarioManager

```
┌─────────────────────────────────────────────────────────────┐
│                      ScenarioManager                         │
├─────────────────────────────────────────────────────────────┤
│ Public Functions (SQL API):                                 │
│                                                             │
│ scenario_create(name, description?) → BOOLEAN               │
│   • Validates name (alphanumeric + underscore, ≤63 chars)   │
│   • Creates schema _scen_<name>                             │
│   • Registers in _scenario_registry                         │
│   • Registers all main tables in _scenario_tables           │
│   • Captures rowids in _scenario_base_rowids                │
│                                                             │
│ scenario_drop(name) → BOOLEAN                               │
│   • Validates scenario exists                               │
│   • Deletes from _scenario_base_rowids (cascade)            │
│   • Deletes from _scenario_tables (cascade)                 │
│   • Deletes from _scenario_registry                         │
│   • Drops schema _scen_<name>                               │
│                                                             │
│ scenario_list() → TABLE                                     │
│   • Returns all scenarios with metadata                     │
│   • Columns: name, status, description, created_at,         │
│              base_schema, parent_scenario                   │
├─────────────────────────────────────────────────────────────┤
│ Helper Functions:                                           │
│                                                             │
│ GetSchemaPrefix(context) → string                           │
│   • Returns "_scen_" (configurable in future)               │
│                                                             │
│ GetSchemaName(context, name) → string                       │
│   • Returns prefix + name                                   │
│                                                             │
│ ValidateName(name) → bool                                   │
│   • Checks: alphanumeric + underscore, ≤63 chars,           │
│     doesn't start with digit                                │
│                                                             │
│ ScenarioExists(context, name) → bool                        │
│   • Queries _scenario_registry                              │
└─────────────────────────────────────────────────────────────┘
```

### 5.3 Level 2: SnapshotManager

```
┌─────────────────────────────────────────────────────────────┐
│                      SnapshotManager                         │
├─────────────────────────────────────────────────────────────┤
│ Public Functions (SQL API):                                 │
│                                                             │
│ snapshot_create(name, source_schema, description?) → BOOL   │
│   • Validates name uniqueness                               │
│   • Records in _scenario_snapshots                          │
│   • (Data copy implementation pending)                      │
│                                                             │
│ snapshot_list() → TABLE (planned)                           │
│ snapshot_drop(name) → BOOLEAN (planned)                     │
│ snapshot_compare(name, other) → TABLE (planned)             │
└─────────────────────────────────────────────────────────────┘
```

### 5.4 Level 2: ProtocolManager

```
┌─────────────────────────────────────────────────────────────┐
│                      ProtocolManager                         │
├─────────────────────────────────────────────────────────────┤
│ Public Functions (SQL API):                                 │
│                                                             │
│ protocol_set_why(scenario_name, why_text) → BOOLEAN         │
│   • Validates scenario exists                               │
│   • Upserts into _scenario_protocols with section='why'     │
│                                                             │
│ protocol_log_change(name, change_text) → BOOLEAN (planned)  │
│ protocol_add_finding(name, finding) → BOOLEAN (planned)     │
│ protocol_set_plan(name, plan) → BOOLEAN (planned)           │
│ protocol_set_decision(name, decision) → BOOLEAN (planned)   │
│ protocol_read(name) → TABLE (planned)                       │
│ protocol_export_markdown(name) → VARCHAR (planned)          │
└─────────────────────────────────────────────────────────────┘
```

---

## 6. Runtime View

### 6.1 Scenario Creation Flow

```
User: SELECT scenario_create('pricing_test', 'Q1 price analysis');

┌──────────────┐    ┌─────────────────┐    ┌──────────────────┐
│ ScalarFunc   │───>│ ScenarioManager │───>│ MetadataStore    │
│ Bind         │    │ CreateFunction  │    │ (tables exist?)  │
└──────────────┘    └────────┬────────┘    └──────────────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
         ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ CREATE SCHEMA   │ │ INSERT INTO     │ │ INSERT INTO     │
│ _scen_pricing_  │ │ _scenario_      │ │ _scenario_      │
│ test            │ │ registry        │ │ tables          │
└─────────────────┘ └─────────────────┘ └─────────────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │ INSERT INTO     │
                    │ _scenario_base_ │
                    │ rowids          │
                    └─────────────────┘
```

### 6.2 Scenario Query Flow (Future - COW Read)

```
User: SET SCHEMA '_scen_pricing_test';
User: SELECT * FROM products WHERE price > 100;

┌──────────────┐    ┌─────────────────┐    ┌──────────────────┐
│ Query        │───>│ Scenario View   │───>│ UNION ALL        │
│ Planner      │    │ (auto-created)  │    │ Base + Delta     │
└──────────────┘    └─────────────────┘    │ with anti-join   │
                                           └──────────────────┘
```

---

## 7. Data Model

### 7.1 Metadata Tables

```
┌─────────────────────────────────────────────────────────────┐
│                    _scenario_registry                        │
├─────────────────────────────────────────────────────────────┤
│ scenario_id      INTEGER PRIMARY KEY                        │
│ scenario_name    VARCHAR NOT NULL UNIQUE                    │
│ schema_name      VARCHAR NOT NULL UNIQUE                    │
│ base_schema      VARCHAR NOT NULL (default: 'main')         │
│ base_captured_at TIMESTAMP NOT NULL                         │
│ created_at       TIMESTAMP DEFAULT current_timestamp        │
│ status           VARCHAR DEFAULT 'active' CHECK(active|     │
│                  archived)                                  │
│ description      VARCHAR                                    │
│ parent_scenario_id INTEGER → _scenario_registry             │
└─────────────────────────────────────────────────────────────┘
         │
         │ 1:N (app-layer integrity)
         ▼
┌─────────────────────────────────────────────────────────────┐
│                    _scenario_tables                          │
├─────────────────────────────────────────────────────────────┤
│ scenario_id         INTEGER NOT NULL                        │
│ table_name          VARCHAR NOT NULL                        │
│ base_row_count      BIGINT                                  │
│ has_primary_key     BOOLEAN                                 │
│ primary_key_columns VARCHAR[]                               │
│ created_at          TIMESTAMP DEFAULT current_timestamp     │
│ PRIMARY KEY (scenario_id, table_name)                       │
└─────────────────────────────────────────────────────────────┘
         │
         │ 1:N (app-layer integrity)
         ▼
┌─────────────────────────────────────────────────────────────┐
│                   _scenario_base_rowids                      │
├─────────────────────────────────────────────────────────────┤
│ scenario_id  INTEGER NOT NULL                               │
│ table_name   VARCHAR NOT NULL                               │
│ base_rowid   BIGINT NOT NULL                                │
│ PRIMARY KEY (scenario_id, table_name, base_rowid)           │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                   _scenario_snapshots                        │
├─────────────────────────────────────────────────────────────┤
│ snapshot_id    INTEGER PRIMARY KEY                          │
│ snapshot_name  VARCHAR NOT NULL UNIQUE                      │
│ source_schema  VARCHAR NOT NULL                             │
│ created_at     TIMESTAMP DEFAULT current_timestamp          │
│ description    VARCHAR                                      │
│ size_bytes     BIGINT                                       │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                   _scenario_protocols                        │
├─────────────────────────────────────────────────────────────┤
│ entity_type  VARCHAR NOT NULL CHECK(scenario|snapshot)      │
│ entity_name  VARCHAR NOT NULL                               │
│ section      VARCHAR NOT NULL CHECK(why|changes|findings|   │
│              plan|decision|metadata)                        │
│ content      VARCHAR                                        │
│ updated_at   TIMESTAMP DEFAULT current_timestamp            │
│ PRIMARY KEY (entity_type, entity_name, section)             │
└─────────────────────────────────────────────────────────────┘
```

### 7.2 Delta Table Structure (Planned)

For each registered table, delta tables will be created:

```
┌─────────────────────────────────────────────────────────────┐
│          _scen_<scenario>.<table>_delta                      │
├─────────────────────────────────────────────────────────────┤
│ _op       VARCHAR NOT NULL CHECK(I|U|D)  -- operation       │
│ _ts       TIMESTAMP DEFAULT current_timestamp               │
│ _version  INTEGER DEFAULT 1                                 │
│ <all columns from base table>                               │
└─────────────────────────────────────────────────────────────┘
```

---

## 8. Design Decisions

### 8.1 ADR-001: No Foreign Keys in Metadata Tables

**Context**: ScalarFunctions in DuckDB create separate Connection objects for queries. FK constraints fail with DELETE operations in this context.

**Decision**: Remove FK constraints from `_scenario_tables`. Referential integrity managed at application layer (scenario_drop cascades deletes explicitly).

**Consequences**:
- Positive: ScalarFunctions work correctly
- Negative: Must maintain cascade logic in code

### 8.2 ADR-002: ScalarFunction vs Procedure

**Context**: DuckDB supports both ScalarFunctions and Procedures. Procedures would seem natural for side-effecting operations.

**Decision**: Use ScalarFunctions with `FunctionStability::VOLATILE` for scenario management functions.

**Rationale**:
- Cleaner bind-time validation
- Consistent return type (BOOLEAN)
- Better integration with SELECT statements

### 8.3 ADR-003: Schema Prefix Convention

**Context**: Scenario schemas need to be distinguishable from user schemas.

**Decision**: Use `_scen_` prefix for all scenario schemas.

**Rationale**:
- Underscore prefix signals internal/system use
- Short enough to not hit identifier limits
- Configurable via future `scenario_schema_prefix` setting

---

## 9. Implementation Status

### 9.1 Completed Components

| Component | Functions | Status |
|-----------|-----------|--------|
| MetadataStore | Initialize, all Create*Table | ✅ Complete |
| ScenarioManager | scenario_create, scenario_drop, scenario_list, scenario_branch, scenario_archive, scenario_unarchive, scenario_stats | ✅ Complete |
| SnapshotManager | snapshot_create | ✅ Complete |
| ProtocolManager | protocol_set_why | ✅ Complete |
| DeltaStorageEngine | delta_create, delta_drop | ✅ Complete |

### 9.2 Planned Components

| Component | Functions | Status |
|-----------|-----------|--------|
| DeltaStorageEngine | Merge-on-read views, write interception | 🔲 Planned |
| OptimizerExtension | Write interception | 🔲 Planned |
| ComparisonEngine | scenario_compare, scenario_compare_all | 🔲 Planned |
| SnapshotManager | snapshot_list, snapshot_drop, snapshot_compare | 🔲 Planned |
| ProtocolManager | protocol_log_change, protocol_read, etc. | 🔲 Planned |

### 9.3 Test Coverage

- **Total**: 373 assertions across 6 test cases
- **Files**:
  - anofox_scenario_load.test
  - scenario_metadata.test
  - scenario_lifecycle.test
  - scenario_snapshots.test
  - scenario_protocols.test
  - scenario_write.test

---

## 10. Glossary

| Term | Definition |
|------|------------|
| **Scenario** | An isolated branch for what-if analysis |
| **Delta Table** | Storage for modifications (INSERT/UPDATE/DELETE) |
| **Base Table** | Original immutable data in main schema |
| **Merge View** | View combining base + delta with anti-join |
| **Snapshot** | Immutable point-in-time capture |
| **Protocol** | Embedded documentation (why, changes, findings) |
| **COW** | Copy-on-Write - only modifications are stored |
