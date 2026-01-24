# **anofox-scenario Architecture Design Document**

**DuckDB Extension for Git-Like Database Branching**

Version 0.1-DRAFT | January 2026 | DataZoo GmbH

---

## **1\. Executive Summary**

This document defines the technical architecture for anofox-scenario, a DuckDB extension that enables Git-like branching for analytical databases. The architecture implements copy-on-write (COW) storage with delta-based row versioning, enabling what-if analysis for S\&OP and demand planning workflows.

### **1.1 Architectural Thesis**

The architecture adopts the **Delta-Main pattern** rather than attempting to replicate version-control systems like Dolt. This decision is driven by a fundamental insight: Dolt's Prolly Tree architecture optimizes for diff-ability and merge-ability at the expense of scan performance, while DuckDB's columnar storage optimizes for analytical queries at the expense of granular updates. These are opposing physics.

Our approach:

* **Keep base data immutable and heavy** (leverages DuckDB's columnar scan performance)  
* **Keep scenario deltas mutable and light** (avoids columnar write amplification)  
* **Merge on read** (combines base \+ delta at query time using DuckDB's query engine)

This architecture delivers O(1) branch creation, storage proportional to modifications only, and maintains DuckDB's analytical performance for the 99%+ of data that remains unchanged.

### **1.2 Key Architectural Decisions**

| Decision | Rationale | Req Reference |
| ----- | ----- | ----- |
| Delta-Main over Prolly Trees | Columnar write amplification makes row-level versioning prohibitive | REQ-NFR-001 |
| OptimizerExtension for write interception | `pre_optimize_function` intercepts LogicalInsert/Update/Delete for transparent SQL | REQ-COW-002/003/004 |
| PK-first row identification | PKs are stable across VACUUM; rowids are physical locators that can shift | REQ-NFR-004 |
| Row versioning via delta tables | Enables snapshot isolation without base table locking | REQ-NFR-004 |
| Hash-based anti-join for merge-on-read | O(N+D) performance vs O(N×D) for subquery approach | REQ-COW-001 |
| Embedded protocol storage | Ensures database portability without external dependencies | REQ-NFR-006 |
| ParserExtension for VACUUM detection | Intercept invalidating operations to mark scenarios for validation | REQ-NFR-004 |

---

## **2\. System Context**

### **2.1 Integration Model**

```
┌─────────────────────────────────────────────────────────────────┐
│                        User / RTA Agent                         │
└─────────────────────────────────────────────────────────────────┘
                                │
                    Standard SQL (transparent)
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                         DuckDB v1.4.x                           │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                   anofox-scenario Extension               │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐   │  │
│  │  │  Scenario   │  │   Storage   │  │    Protocol     │   │  │
│  │  │  Manager    │  │  Extension  │  │    Manager      │   │  │
│  │  └─────────────┘  └─────────────┘  └─────────────────┘   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                   │
│  ┌───────────────────────────┴───────────────────────────────┐  │
│  │                    DuckDB Catalog                         │  │
│  │   main schema    │  _scen_* schemas  │  _scenario_* tables│  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Single .duckdb File                          │
│  (scenarios, snapshots, protocols all embedded)                 │
└─────────────────────────────────────────────────────────────────┘
```

### **2.2 Extension Registration**

The extension registers with DuckDB using the standard extension API:

```c
class ScenarioExtension : public Extension {
public:
    void Load(DuckDB &db) override {
        // 1. Register scenario management functions (CALL procedures)
        RegisterScenarioFunctions(db);
        
        // 2. Register comparison table functions
        RegisterComparisonFunctions(db);
        
        // 3. Register protocol management functions
        RegisterProtocolFunctions(db);
        
        // 4. Register storage extension for write interception
        auto &config = DBConfig::GetConfig(db);
        config.AddExtensionOption("scenario_schema_prefix", 
            "Prefix for scenario schema names", 
            LogicalType::VARCHAR, Value("_scen_"));
    }
};

extern "C" {
    DUCKDB_EXTENSION_API void scenario_init(duckdb::DatabaseInstance &db) {
        LoadInternal(db);
    }
    DUCKDB_EXTENSION_API const char *scenario_version() {
        return "0.1.0";
    }
}
```

**Reference files in DuckDB source:**

* `src/include/duckdb/main/extension.hpp`  
* `src/main/extension/extension_load.cpp`  
* `extension/tpch/tpch_extension.cpp` (registration pattern example)

---

## **3\. Component Architecture**

### **3.1 Component Overview**

```
┌──────────────────────────────────────────────────────────────────────┐
│                      anofox-scenario Extension                        │
├──────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌────────────────────┐    ┌────────────────────┐                    │
│  │  ScenarioManager   │    │  SnapshotManager   │                    │
│  │  ----------------  │    │  ----------------  │                    │
│  │  Create/Branch     │    │  Create/List       │                    │
│  │  List/Stats        │    │  Compare           │                    │
│  │  Archive/Drop      │    │  CreateScenarioFrom│                    │
│  └─────────┬──────────┘    └─────────┬──────────┘                    │
│            │                         │                                │
│            └───────────┬─────────────┘                                │
│                        ▼                                              │
│  ┌─────────────────────────────────────────────────────────────┐     │
│  │                    MetadataStore                             │     │
│  │  ─────────────────────────────────────────────────────────  │     │
│  │  _scenario_registry    (scenario metadata)                   │     │
│  │  _scenario_tables      (table-to-scenario mapping)           │     │
│  │  _scenario_snapshots   (snapshot metadata)                   │     │
│  │  _scenario_protocols   (protocol content)                    │     │
│  └─────────────────────────────────────────────────────────────┘     │
│                        │                                              │
│                        ▼                                              │
│  ┌─────────────────────────────────────────────────────────────┐     │
│  │                 DeltaStorageEngine                           │     │
│  │  ─────────────────────────────────────────────────────────  │     │
│  │  CreateDeltaTable()     - Initialize delta for table         │     │
│  │  ApplyInsert()          - Record insert in delta             │     │
│  │  ApplyUpdate()          - Record update in delta             │     │
│  │  ApplyDelete()          - Record delete marker               │     │
│  │  BuildLogicalView()     - Construct base + delta query       │     │
│  └─────────────────────────────────────────────────────────────┘     │
│                        │                                              │
│                        ▼                                              │
│  ┌─────────────────────────────────────────────────────────────┐     │
│  │                 ComparisonEngine                             │     │
│  │  ─────────────────────────────────────────────────────────  │     │
│  │  CompareTables()        - Diff two table states              │     │
│  │  CompareToOrigin()      - Diff scenario vs branch point      │     │
│  │  CompareAll()           - Summary diff all tables            │     │
│  └─────────────────────────────────────────────────────────────┘     │
│                        │                                              │
│                        ▼                                              │
│  ┌─────────────────────────────────────────────────────────────┐     │
│  │                 ProtocolManager                              │     │
│  │  ─────────────────────────────────────────────────────────  │     │
│  │  SetWhy/Plan/Decision   - Update protocol sections           │     │
│  │  LogChange()            - Append to changes log              │     │
│  │  AddFinding()           - Append finding                     │     │
│  │  ExportMarkdown()       - Generate markdown file             │     │
│  └─────────────────────────────────────────────────────────────┘     │
│                                                                       │
└──────────────────────────────────────────────────────────────────────┘
```

### **3.2 Component Responsibilities**

#### **3.2.1 ScenarioManager**

Implements requirements: REQ-SCEN-001 through REQ-SCEN-008

| Function | SQL Interface | Description |
| ----- | ----- | ----- |
| Create | `CALL scenario_create(name, desc, base)` | Creates scenario schema, initializes metadata |
| Branch | `CALL scenario_branch(source, name, desc)` | Creates scenario from existing scenario |
| List | `SELECT * FROM scenario_list()` | Returns scenario metadata |
| Stats | `SELECT * FROM scenario_stats(name)` | Returns modification statistics |
| Archive | `CALL scenario_archive(name)` | Marks read-only, preserves data |
| Unarchive | `CALL scenario_unarchive(name)` | Restores write capability |
| Drop | `CALL scenario_drop(name)` | Removes scenario and delta storage |

#### **3.2.2 DeltaStorageEngine**

Implements requirements: REQ-COW-001 through REQ-COW-008

Core component responsible for transparent SQL operations. This is the most complex component and requires deep DuckDB integration.

#### **3.2.3 ComparisonEngine**

Implements requirements: REQ-COMP-001 through REQ-COMP-004

Generates structured diff output between schema states.

#### **3.2.4 SnapshotManager**

Implements requirements: REQ-SNAP-001 through REQ-SNAP-005

Manages immutable point-in-time captures.

#### **3.2.5 ProtocolManager**

Implements requirements: REQ-PROT-001 through REQ-PROT-005

Handles structured documentation storage and retrieval.

---

## **4\. Data Architecture**

### **4.1 Metadata Schema**

All metadata stored in `main` schema with `_scenario_` prefix to avoid collisions.

```sql
-- Scenario registry
CREATE TABLE _scenario_registry (
    scenario_id INTEGER PRIMARY KEY,
    scenario_name VARCHAR NOT NULL UNIQUE,
    schema_name VARCHAR NOT NULL UNIQUE,      -- Physical schema name with prefix
    base_schema VARCHAR NOT NULL,             -- 'main' or another scenario
    base_captured_at TIMESTAMP NOT NULL,      -- Snapshot isolation timestamp
    created_at TIMESTAMP DEFAULT current_timestamp,
    status VARCHAR DEFAULT 'active' CHECK (status IN ('active', 'archived')),
    description VARCHAR,
    parent_scenario_id INTEGER REFERENCES _scenario_registry(scenario_id)
);

-- Table registration per scenario
CREATE TABLE _scenario_tables (
    scenario_id INTEGER REFERENCES _scenario_registry(scenario_id),
    table_name VARCHAR NOT NULL,
    base_row_count BIGINT,                    -- Row count at scenario creation
    has_primary_key BOOLEAN,
    primary_key_columns VARCHAR[],            -- Array of PK column names
    created_at TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (scenario_id, table_name)
);

-- Snapshot registry
CREATE TABLE _scenario_snapshots (
    snapshot_id INTEGER PRIMARY KEY,
    snapshot_name VARCHAR NOT NULL UNIQUE,
    source_schema VARCHAR NOT NULL,
    created_at TIMESTAMP DEFAULT current_timestamp,
    description VARCHAR,
    size_bytes BIGINT
);

-- Protocol storage (per REQ-PROT-001)
CREATE TABLE _scenario_protocols (
    entity_type VARCHAR NOT NULL CHECK (entity_type IN ('scenario', 'snapshot')),
    entity_name VARCHAR NOT NULL,
    section VARCHAR NOT NULL CHECK (section IN ('why', 'changes', 'findings', 'plan', 'decision', 'metadata')),
    content VARCHAR,
    updated_at TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (entity_type, entity_name, section)
);

-- Archived protocols (for dropped scenarios)
CREATE TABLE _scenario_protocols_archive (
    archived_at TIMESTAMP DEFAULT current_timestamp,
    entity_type VARCHAR NOT NULL,
    entity_name VARCHAR NOT NULL,
    section VARCHAR NOT NULL,
    content VARCHAR
);
```

### **4.2 Delta Table Schema**

For each base table `T` in a scenario, a delta table `_delta_T` is created in the scenario schema:

```sql
-- Example: Delta table for 'forecast' with PK (material_id, period)
CREATE TABLE _scen_optimistic._delta_forecast (
    -- Operation metadata
    _op VARCHAR NOT NULL CHECK (_op IN ('I', 'U', 'D')),
    _ts TIMESTAMP DEFAULT current_timestamp,
    _version INTEGER DEFAULT 1,
    
    -- All columns from base table (including PK)
    material_id VARCHAR NOT NULL,
    period DATE NOT NULL,
    qty DOUBLE,
    confidence DOUBLE,
    
    -- Constraints inherited from base
    CHECK (qty >= 0),
    CHECK (confidence BETWEEN 0 AND 1),
    
    -- PK on delta ensures single active state per row
    PRIMARY KEY (material_id, period)
);

-- Index for efficient anti-join during merge-on-read
CREATE INDEX idx_delta_forecast_pk ON _scen_optimistic._delta_forecast(material_id, period);
```

**Operation codes:**

* `'I'` \- Insert: New row not in base  
* `'U'` \- Update: Modified row from base  
* `'D'` \- Delete: Tombstone marker for base row

### **4.3 Logical View Construction (Merge-on-Read)**

The logical view combines base \+ delta using hash-based anti-join for O(N+D) performance:

```sql
-- Logical view for scenario.forecast
-- This is generated dynamically, not stored as a persistent view

WITH base_captured AS (
    -- Base data as of scenario creation (snapshot isolation)
    SELECT * FROM main.forecast
    WHERE _rowid IN (
        SELECT _rowid FROM _scenario_base_rowids 
        WHERE scenario_id = :scenario_id AND table_name = 'forecast'
    )
),
delta AS (
    SELECT * FROM _scen_optimistic._delta_forecast
    WHERE _op != 'D'  -- Exclude tombstones from output
),
deleted_pks AS (
    SELECT material_id, period 
    FROM _scen_optimistic._delta_forecast
    WHERE _op = 'D'
)
-- Final logical view
SELECT material_id, period, qty, confidence FROM delta
UNION ALL
SELECT material_id, period, qty, confidence FROM base_captured b
WHERE NOT EXISTS (
    SELECT 1 FROM _scen_optimistic._delta_forecast d
    WHERE d.material_id = b.material_id AND d.period = b.period
)
```

**Performance consideration:** The `NOT EXISTS` pattern with indexed delta PK columns compiles to a hash anti-join in DuckDB, achieving O(N+D) rather than O(N×D).

### **4.4 Snapshot Isolation Implementation**

Per REQ-NFR-004, scenarios must see base state as of creation time, not current state.

**Challenge:** DuckDB lacks native temporal queries or MVCC time-travel.

**Solution:** Capture base row identifiers at scenario creation.

```sql
-- Row ID capture table
CREATE TABLE _scenario_base_rowids (
    scenario_id INTEGER,
    table_name VARCHAR,
    base_rowid BIGINT,  -- DuckDB internal rowid
    PRIMARY KEY (scenario_id, table_name, base_rowid)
);

-- At scenario creation, capture current rowids
INSERT INTO _scenario_base_rowids (scenario_id, table_name, base_rowid)
SELECT :new_scenario_id, 'forecast', rowid FROM main.forecast;
```

**Trade-off:** This requires O(N) storage of row IDs at scenario creation. For a 10M row table with 8-byte rowids, this is \~80MB per scenario per table.

**Alternative approaches considered:**

| Approach | Storage | Complexity | Chosen? |
| ----- | ----- | ----- | ----- |
| Rowid capture | O(N) per scenario | Low | ✓ MVP |
| Timestamp column in base | O(1) per scenario | Medium (schema change) | Future |
| Full table copy | O(N×row\_size) | Low | ✗ Too expensive |
| MVCC snapshot ID | O(1) | High (DuckDB internals) | ✗ Not exposed |

### **4.5 Rowid Stability and Invalidation Detection**

**Challenge:** DuckDB rowids are position-based and can shift during table reorganization.

From `src/storage/table/row_version_manager.cpp`:
```cpp
void RowVersionManager::SetStart(idx_t new_start) {
    lock_guard<mutex> l(version_lock);
    this->start = new_start;  // All rowids in this group shift
    // ...
}
```

**Rowid stability by operation:**

| Operation | Rowid Behavior | Impact on Scenarios |
| --------- | -------------- | ------------------- |
| INSERT | New sequential rowids | Safe — new rows not in captured set |
| UPDATE | Rowid unchanged | Safe — row stays at same position |
| DELETE | Gap created, others unchanged | Safe — captured rowids still valid |
| VACUUM / CHECKPOINT | May reorganize, shifting rowids | **Unsafe — captured set invalidated** |

#### **PK-First Strategy (Recommended)**

**Key insight from Codex analysis:** Rowid is a hidden physical locator, not a logical ID. Scenario correctness should NOT depend on rowid stability.

**Recommended approach:**

| Table Type | Row Identification | Merge-on-Read Join | Storage |
| ---------- | ------------------ | ------------------ | ------- |
| Has PK | Primary key columns | Anti-join on PK | Store only PK in delta |
| No PK | Rowid (fallback) | Anti-join on rowid | Capture rowids + validation |

For tables **with PKs** (the common case in S&OP):
- Use PK equality for UPDATE/DELETE targeting in scenarios
- Use anti-join on PK for merge-on-read, not rowid
- Store only PKs in delta tables — decouples from storage reorganization
- **No O(N) rowid capture needed**

For tables **without PKs** (fallback mode):
- Capture rowids at scenario creation (O(N))
- Flag as "unstable" in `_scenario_tables.has_primary_key = false`
- Require validation before use after VACUUM/CHECKPOINT
- Warn users about brittleness

#### **ParserExtension for VACUUM/CHECKPOINT Detection**

Automatically mark scenarios as needing validation when invalidating operations occur:

```cpp
class ScenarioParserExtension : public ParserExtension {
public:
    ScenarioParserExtension() {
        parse_function = &InterceptInvalidatingStatements;
        plan_function = &PlanInvalidatingStatements;
    }

    static ParserExtensionParseResult InterceptInvalidatingStatements(
            ParserExtensionInfo *info, const string &query) {
        // Check if query is VACUUM or CHECKPOINT
        auto upper = StringUtil::Upper(query);
        if (StringUtil::StartsWith(upper, "VACUUM") ||
            StringUtil::StartsWith(upper, "CHECKPOINT")) {
            // Return custom parse data to mark scenarios
            return ParserExtensionParseResult(
                make_uniq<InvalidationParseData>(query));
        }
        // Let DuckDB handle normally
        return ParserExtensionParseResult();
    }

    static ParserExtensionPlanResult PlanInvalidatingStatements(
            ParserExtensionInfo *info, ClientContext &context,
            unique_ptr<ParserExtensionParseData> parse_data) {
        // Mark all active scenarios as needing validation
        MarkScenariosForValidation(context);
        // Return table function that executes original statement
        // ... (implementation details)
    }
};
```

**Alternative:** Hook `StorageExtension.OnCheckpointStart/End` for checkpoint-specific detection (see `storage_extension.hpp:40-47`).

#### **Validation Metadata**

**Solution:** Add validation metadata to detect stale scenarios:

```sql
-- Enhanced metadata schema
ALTER TABLE _scenario_base_rowids ADD COLUMN pk_hash UBIGINT;

CREATE TABLE _scenario_base_state (
    scenario_id INTEGER,
    table_name VARCHAR,
    capture_timestamp TIMESTAMP NOT NULL,
    base_row_count BIGINT NOT NULL,
    pk_checksum UBIGINT,  -- Hash of sample PK values for validation
    PRIMARY KEY (scenario_id, table_name)
);
```

**Validation function (called on first read after extended idle):**

```cpp
bool ScenarioValidateBase(ClientContext &context, const string &scenario_name) {
    // 1. Get captured state
    auto state = GetScenarioBaseState(context, scenario_name);

    // 2. Compare current row count
    auto current_count = GetTableRowCount(context, state.table_name);
    if (current_count < state.base_row_count) {
        // Rows deleted from base — check if our captured rowids affected
        LogWarning("Base table '%s' has fewer rows than at scenario creation. "
                   "Scenario '%s' may reference deleted rows.",
                   state.table_name, scenario_name);
    }

    // 3. Spot-check: verify sample rowids still exist with expected PKs
    auto sample_valid = ValidateSampleRowids(context, scenario_name, state);
    if (!sample_valid) {
        throw InvalidInputException(
            "Scenario '%s' base state invalidated. Table '%s' appears to have been "
            "reorganized (VACUUM/CHECKPOINT). Consider recreating the scenario.",
            scenario_name, state.table_name);
    }

    return true;
}
```

**Recommendation:** Run validation:
- On first scenario read after >1 hour idle
- Explicitly via `CALL scenario_validate('name')`
- Before critical comparison operations

---

## **5\. DuckDB Integration Architecture**

### **5.1 Integration Strategy**

Based on research of DuckDB internals and extension patterns (Delta, Iceberg, SQLite scanner, DuckLake), we adopt a **multi-extension integration** approach:

**Primary extension points:**

1. **Catalog API** — Schema creation for scenario isolation, view management
2. **OptimizerExtension** — Transparent write interception via `pre_optimize_function`
3. **ParserExtension** — Intercept VACUUM/CHECKPOINT for scenario validation marking
4. **Table Functions** — `scenario_list()`, `scenario_compare()`, `scenario_changes()`

**Rationale:** Full `StorageExtension` (as used by Iceberg for external catalogs) is designed for attaching external databases. Our scenarios live within the same DuckDB file. The lighter-weight approach uses:

1. **Schema creation** via Catalog API for scenario isolation
2. **View replacement** for transparent reads (merge-on-read)
3. **OptimizerExtension** for transparent write interception (no wrapper procedures needed)
4. **ParserExtension** for detecting invalidating operations (VACUUM/CHECKPOINT)

**Future (v0.2):** Add `StorageExtension` for `ATTACH 'scenario:name'` syntax.

### **5.2 Write Interception Options**

DuckDB provides write interception via the `OptimizerExtension` API. The `pre_optimize_function` callback receives the logical query plan before optimization, allowing interception and transformation of `LogicalInsert`, `LogicalUpdate`, and `LogicalDelete` operators.

#### **Option A: OptimizerExtension (MVP Recommended)**

```cpp
// Register optimizer extension during Load()
class ScenarioOptimizerExtension : public OptimizerExtension {
public:
    ScenarioOptimizerExtension() {
        pre_optimize_function = InterceptScenarioWrites;
    }

    static void InterceptScenarioWrites(OptimizerExtensionInput &input,
                                        unique_ptr<LogicalOperator> &plan) {
        // Recursively traverse plan tree
        TransformWriteOperators(input.context, plan);
    }

    static void TransformWriteOperators(ClientContext &context,
                                        unique_ptr<LogicalOperator> &op) {
        // Check if this is a write to a scenario schema
        if (op->type == LogicalOperatorType::LOGICAL_INSERT) {
            auto &insert = op->Cast<LogicalInsert>();
            if (IsScenarioSchema(context, insert.table.schema)) {
                // Transform: redirect to _delta_<table>
                // Add _op='I', _ts=now(), _version=1 columns
                TransformInsertToDelta(context, op);
            }
        } else if (op->type == LogicalOperatorType::LOGICAL_UPDATE) {
            auto &update = op->Cast<LogicalUpdate>();
            if (IsScenarioSchema(context, update.table.schema)) {
                // Transform: upsert into delta table with _op='U'
                TransformUpdateToDelta(context, op);
            }
        } else if (op->type == LogicalOperatorType::LOGICAL_DELETE) {
            auto &del = op->Cast<LogicalDelete>();
            if (IsScenarioSchema(context, del.table.schema)) {
                // Transform: insert tombstone with _op='D'
                TransformDeleteToDelta(context, op);
            }
        }

        // Recursively process children
        for (auto &child : op->children) {
            TransformWriteOperators(context, child);
        }
    }
};

// Registration in extension Load():
auto &config = DBConfig::GetConfig(db);
config.optimizer_extensions.push_back(ScenarioOptimizerExtension());
```

**Pros:** Fully transparent SQL — users write `INSERT INTO scenario.forecast VALUES (...)` directly
**Cons:** Requires understanding LogicalOperator transformation

**Reference files:**
- `src/include/duckdb/optimizer/optimizer_extension.hpp` — API definition
- `src/optimizer/optimizer.cpp:290-297` — Extension invocation point
- `test/extension/loadable_extension_optimizer_demo.cpp` — Working example

#### **Option B: Wrapper Functions (Fallback)**

For compatibility or when optimizer interception is insufficient:

```sql
-- Explicit wrapper procedures
CALL scenario_insert('optimistic', 'forecast',
    [{'material_id': 'X', 'period': '2026-01-01', 'qty': 100}]);

CALL scenario_update('optimistic', 'forecast',
    set_clause := 'qty = qty * 1.10',
    where_clause := 'material_group = ''PLST''');

CALL scenario_delete('optimistic', 'forecast',
    where_clause := 'material_id = ''OBSOLETE''');
```

**Pros:** No DuckDB internals required, fully portable
**Cons:** Not transparent SQL (violates REQ-COW-002/003/004 spirit)

#### **Option C: View \+ Instead-Of Trigger Pattern**

**Status:** Not viable in DuckDB v1.4.x — DuckDB has no trigger system.

#### **Option D: SET SCHEMA \+ Direct Delta Table Access**

```sql
-- Activate scenario
SET SCHEMA _scen_optimistic;

-- Reads resolve to scenario schema first
SELECT * FROM forecast;  -- Resolves to _scen_optimistic.forecast (view)

-- Direct delta table writes (for advanced users)
INSERT INTO _delta_forecast (_op, _ts, _version, material_id, period, qty, confidence)
VALUES ('I', now(), 1, 'X', '2026-01-01', 100, 0.8);
```

**Decision:** Adopt **Option A (OptimizerExtension)** for MVP to achieve transparent SQL. Provide Option B wrappers as documented alternative. Option D available for power users needing direct delta access.

### **5.3 Transparent Read Implementation**

Reads are made transparent via auto-generated views in the scenario schema:

```c
void CreateScenarioTableView(Connection &conn, 
                             const string &scenario_name,
                             const string &table_name,
                             const vector<string> &pk_columns,
                             const vector<ColumnDefinition> &columns) {
    
    string schema = GetSchemaName(scenario_name);
    string delta_table = "_delta_" + table_name;
    
    // Build column list (excluding internal columns)
    string col_list = BuildColumnList(columns);
    
    // Build PK join condition
    string pk_condition = BuildPKCondition(pk_columns, "b", "d");
    
    // Generate merge-on-read view
    string view_sql = StringUtil::Format(R"(
        CREATE OR REPLACE VIEW %s.%s AS
        WITH delta AS (
            SELECT %s FROM %s.%s WHERE _op != 'D'
        )
        SELECT %s FROM delta
        UNION ALL
        SELECT %s FROM main.%s b
        WHERE NOT EXISTS (
            SELECT 1 FROM %s.%s d WHERE %s
        )
        AND b.rowid IN (
            SELECT base_rowid FROM _scenario_base_rowids
            WHERE scenario_id = %d AND table_name = '%s'
        )
    )", schema, table_name,
        col_list, schema, delta_table,
        col_list,
        col_list, table_name,
        schema, delta_table, pk_condition,
        scenario_id, table_name);
    
    conn.Query(view_sql);
}
```

### **5.4 Comparison Query Generation**

For REQ-COMP-001 (flat output schema):

```c
string GenerateComparisonQuery(const string &schema_a,
                               const string &schema_b, 
                               const string &table_name,
                               const vector<string> &pk_columns,
                               const vector<string> &value_columns) {
    
    string pk_select = JoinColumns(pk_columns);
    string pk_join = BuildPKCondition(pk_columns, "a", "b");
    
    // Generate UNPIVOT-style comparison
    return StringUtil::Format(R"(
        WITH a AS (SELECT * FROM %s.%s),
             b AS (SELECT * FROM %s.%s),
        
        -- Added rows (in B, not in A)
        added AS (
            SELECT 'added' as diff_type, %s, NULL as column_name,
                   NULL as old_value, NULL as new_value
            FROM b WHERE NOT EXISTS (SELECT 1 FROM a WHERE %s)
        ),
        
        -- Removed rows (in A, not in B)  
        removed AS (
            SELECT 'removed' as diff_type, %s, NULL as column_name,
                   NULL as old_value, NULL as new_value
            FROM a WHERE NOT EXISTS (SELECT 1 FROM b WHERE %s)
        ),
        
        -- Changed rows (in both, with differences)
        changed AS (
            SELECT 'changed' as diff_type, %s,
                   unnest([%s]) as column_name,
                   unnest([%s]) as old_value,
                   unnest([%s]) as new_value
            FROM a JOIN b ON %s
            WHERE (%s)  -- Any column differs
        )
        
        SELECT * FROM added
        UNION ALL SELECT * FROM removed
        UNION ALL SELECT * FROM changed WHERE old_value != new_value
    )", schema_a, table_name, schema_b, table_name,
        /* ... column generation logic ... */);
}
```

---

## **6\. Detailed Design: Core Workflows**

### **6.1 Scenario Creation (REQ-SCEN-001)**

```
┌─────────────────────────────────────────────────────────────────┐
│ CALL scenario_create('optimistic', 'Testing demand increase')   │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 1. Validate name (alphanumeric + underscore, ≤63 chars)         │
│ 2. Check no collision with prefix + name                        │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. BEGIN TRANSACTION                                            │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. Create scenario schema: CREATE SCHEMA _scen_optimistic       │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 5. Insert into _scenario_registry:                              │
│    (name, schema, base='main', captured_at=now(), status)       │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 6. For each user table T in base schema:                        │
│    a. Capture schema (columns, types, constraints, PK)          │
│    b. Insert into _scenario_tables                              │
│    c. Create _delta_T table in scenario schema                  │
│    d. Create T view in scenario schema (merge-on-read)          │
│    e. Capture rowids: INSERT INTO _scenario_base_rowids         │
│       SELECT scenario_id, 'T', rowid FROM main.T                │
│    f. If T lacks PK: log warning                                │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 7. Initialize protocol: INSERT INTO _scenario_protocols         │
│    (entity_type='scenario', entity_name, section='metadata',    │
│     content=JSON{created, base, status})                        │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 8. COMMIT TRANSACTION                                           │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ 9. Return: scenario_id, schema_name, tables_registered          │
└─────────────────────────────────────────────────────────────────┘
```

**Time complexity:** O(Σ|Tᵢ|) where |Tᵢ| is row count of table i (due to rowid capture)

**Target:** \<1 second for databases up to 10GB (per REQ-SCEN-001)

### **6.2 Transparent Read (REQ-COW-001)**

```
┌─────────────────────────────────────────────────────────────────┐
│ SET SCHEMA _scen_optimistic;                                    │
│ SELECT * FROM forecast WHERE material_group = 'PLST';           │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ DuckDB resolves 'forecast' to _scen_optimistic.forecast (view)  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ View expands to merge-on-read query:                            │
│                                                                 │
│ WITH delta AS (SELECT ... FROM _delta_forecast WHERE _op!='D')  │
│ SELECT ... FROM delta                                           │
│ UNION ALL                                                       │
│ SELECT ... FROM main.forecast b                                 │
│ WHERE NOT EXISTS (SELECT 1 FROM _delta_forecast d               │
│                   WHERE d.material_id = b.material_id           │
│                   AND d.period = b.period)                      │
│ AND b.rowid IN (SELECT base_rowid FROM _scenario_base_rowids    │
│                 WHERE scenario_id = X AND table_name='forecast')│
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ DuckDB optimizer:                                               │
│ - Pushes WHERE material_group='PLST' into both branches         │
│ - Converts NOT EXISTS to hash anti-join                         │
│ - Executes with vectorized engine                               │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ Result: Logical view of base + scenario modifications           │
└─────────────────────────────────────────────────────────────────┘
```

### **6.3 Write Operations (REQ-COW-002/003/004)**

With `OptimizerExtension` (Section 5.2), transparent writes work directly:

```sql
-- Transparent writes via OptimizerExtension interception
INSERT INTO _scen_optimistic.forecast (material_id, period, qty, confidence)
VALUES ('NEW-01', '2026-01-01', 500, 0.9);

UPDATE _scen_optimistic.forecast SET qty = qty * 1.10
WHERE material_group = 'PLST';

DELETE FROM _scen_optimistic.forecast WHERE obsolete = true;
```

The `pre_optimize_function` intercepts these and transforms them to delta table operations.

**Wrapper procedures remain available** as fallback or for explicit control:

#### **INSERT Wrapper**

```sql
-- User calls:
CALL scenario_insert('optimistic', 'forecast', 
    columns := ['material_id', 'period', 'qty', 'confidence'],
    values := [['NEW-01', '2026-01-01', 500, 0.9],
               ['NEW-02', '2026-02-01', 600, 0.85]]);

-- Implementation:
CREATE OR REPLACE PROCEDURE scenario_insert(
    scenario_name VARCHAR,
    table_name VARCHAR,
    columns VARCHAR[],
    values ANY[][]
) AS $$
DECLARE
    schema_name VARCHAR;
    delta_table VARCHAR;
    pk_cols VARCHAR[];
BEGIN
    -- Resolve schema and validate
    SELECT sr.schema_name, st.primary_key_columns 
    INTO schema_name, pk_cols
    FROM _scenario_registry sr
    JOIN _scenario_tables st ON sr.scenario_id = st.scenario_id
    WHERE sr.scenario_name = scenario_name 
    AND st.table_name = table_name
    AND sr.status = 'active';
    
    IF schema_name IS NULL THEN
        RAISE EXCEPTION 'Scenario % not found or not active', scenario_name;
    END IF;
    
    delta_table := schema_name || '._delta_' || table_name;
    
    -- Check PK doesn't exist in base or delta
    -- (constraint will catch this, but better error message)
    
    -- Insert with operation marker
    EXECUTE format(
        'INSERT INTO %s (_op, _ts, _version, %s) 
         SELECT ''I'', current_timestamp, 1, * FROM unnest($1)',
        delta_table, array_to_string(columns, ', ')
    ) USING values;
END;
$$;
```

#### **UPDATE Wrapper**

```sql
CALL scenario_update('optimistic', 'forecast',
    set_clause := 'qty = qty * 1.10',
    where_clause := 'material_group = ''PLST''');

-- Implementation converts to:
-- 1. Find affected rows in logical view
-- 2. For rows from delta: UPDATE _delta_forecast SET ...
-- 3. For rows from base: INSERT INTO _delta_forecast (_op='U', ...)
```

#### **DELETE Wrapper**

```sql
CALL scenario_delete('optimistic', 'forecast',
    where_clause := 'obsolete = true');

-- Implementation:
-- 1. For rows from delta with _op='I': DELETE FROM _delta_forecast
-- 2. For rows from delta with _op='U': UPDATE _delta_forecast SET _op='D'
-- 3. For rows from base: INSERT INTO _delta_forecast (_op='D', pk_cols only)
```

### **6.4 Comparison (REQ-COMP-001)**

```
┌─────────────────────────────────────────────────────────────────┐
│ SELECT * FROM scenario_compare('optimistic', 'forecast');       │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ Resolve comparison targets:                                     │
│ - Single arg = compare to branch origin (base_captured_at)      │
│ - Build logical view for scenario current state                 │
│ - Build logical view for origin state                           │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ Execute diff query (see Section 5.4)                            │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ Output (per REQ-COMP-001):                                      │
│ ┌──────────┬─────────────┬────────┬─────────┬──────────┬──────┐│
│ │diff_type │ material_id │ period │col_name │ old_value│ new  ││
│ ├──────────┼─────────────┼────────┼─────────┼──────────┼──────┤│
│ │ added    │ NEW-01      │ 2026-01│ NULL    │ NULL     │ NULL ││
│ │ changed  │ PLST-07     │ 2026-01│ qty     │ 100      │ 110  ││
│ │ removed  │ OBSOLETE    │ 2026-01│ NULL    │ NULL     │ NULL ││
│ └──────────┴─────────────┴────────┴─────────┴──────────┴──────┘│
└─────────────────────────────────────────────────────────────────┘
```

---

## **7\. Design Constraints and Trade-offs**

### **7.1 Why Not Prolly Trees?**

Dolt's architecture uses Prolly Trees (Probabilistic B-Trees) for content-addressed storage with structural sharing. Analysis of this approach for DuckDB:

| Aspect | Dolt (Prolly Trees) | anofox-scenario (Delta-Main) |
| ----- | ----- | ----- |
| Write amplification | Moderate (4KB chunks) | Low (append to delta) |
| Read performance | Poor for analytics (row-oriented) | Excellent (columnar base \+ small delta) |
| Storage sharing | Cryptographic (content-addressed) | Logical (reference base rowids) |
| Merge granularity | Cell-level | Row-level (MVP) |
| Branch creation | O(1) pointer | O(N) rowid capture |

**Decision:** Delta-Main is correct for analytical workloads. Prolly Trees would destroy DuckDB's vectorized scan performance. We accept O(N) branch creation cost in exchange for O(1) query overhead on unchanged data.

### **7.2 Snapshot Isolation Cost**

Capturing base rowids at scenario creation requires O(N) storage per scenario per table.

**Mitigation strategies:**

1. **Lazy capture:** Only capture rowids for tables actually modified in scenario
2. **Bitmap compression:** Store rowid ranges instead of individual values
3. **Timestamp-based:** Require `_modified_at` column in base tables (detailed below)

**MVP decision:** Accept O(N) cost. For typical S\&OP databases (10M rows), this is \~80MB per scenario \- acceptable for dozens of scenarios.

#### **Timestamp-Based Snapshot Isolation (v0.2 Enhancement)**

For tables with a `_modified_at TIMESTAMP` column (common in S&OP data pipelines), scenarios can use O(1) creation:

```sql
-- At scenario creation: just record timestamp (O(1))
INSERT INTO _scenario_registry (scenario_id, scenario_name, base_captured_at, isolation_mode)
VALUES (:id, 'optimistic', current_timestamp, 'timestamp');

-- Merge-on-read view uses timestamp filter instead of rowid list
CREATE VIEW _scen_optimistic.forecast AS
WITH
    scenario_meta AS (
        SELECT base_captured_at FROM _scenario_registry
        WHERE scenario_name = 'optimistic'
    ),
    delta AS (
        SELECT * FROM _scen_optimistic._delta_forecast WHERE _op != 'D'
    )
SELECT material_id, period, qty, confidence FROM delta
UNION ALL
SELECT material_id, period, qty, confidence FROM main.forecast b
WHERE b._modified_at <= (SELECT base_captured_at FROM scenario_meta)  -- O(1) with index
  AND NOT EXISTS (
      SELECT 1 FROM _scen_optimistic._delta_forecast d
      WHERE d.material_id = b.material_id AND d.period = b.period
  );
```

**Trade-offs:**

| Approach | Scenario Creation | Storage Overhead | Requirements |
| -------- | ----------------- | ---------------- | ------------ |
| PK-based (recommended) | O(1) | O(delta) | Table has PK |
| Rowid capture | O(N) | O(N × 8 bytes) per table | None (fallback for no-PK) |
| Timestamp-based | O(1) | O(1) | `_modified_at` column + index |
| Materialized | O(N × row_size) | O(N × row_size) | None (full isolation) |

**Recommendation:**
- MVP uses PK-based for tables with PKs (most S&OP tables)
- MVP uses rowid capture as fallback for tables without PKs
- v0.2 adds timestamp mode via `scenario_create(..., isolation_mode := 'timestamp')`
- v0.2 adds materialized mode for users needing ironclad isolation

#### **Materialized Scenario Mode (v0.2)**

For users who need **guaranteed isolation** regardless of base table structure or VACUUM operations:

```sql
-- Create a materialized scenario (full table copy)
CALL scenario_create('critical_analysis',
    'Board presentation scenario',
    isolation_mode := 'materialized');

-- Implementation: copies base tables into scenario schema
-- INSERT INTO _scen_critical_analysis.forecast
-- SELECT * FROM main.forecast;
```

**Characteristics:**
- **O(N × row_size)** storage per table — full copy, not delta
- **Complete isolation** — no dependency on base table state or rowids
- **No validation needed** — scenario is self-contained
- **Writes go directly to copied tables** — no delta indirection

**Use cases:**
- Tables without primary keys that need stable scenarios
- Critical analyses where VACUUM/CHECKPOINT cannot be controlled
- Long-lived scenarios spanning weeks/months
- Regulatory/audit scenarios requiring immutable snapshots

**SQL syntax:**
```sql
-- Explicit materialized mode
CALL scenario_create('audit_q4', 'Q4 audit baseline',
    isolation_mode := 'materialized',
    tables := ['forecast', 'supply_plan']);  -- Optional: specify subset

-- Check isolation mode
SELECT scenario_name, isolation_mode FROM scenario_list();
```

### **7.3 No Cell-Level Merge**

Unlike Dolt, we cannot efficiently merge at cell granularity because:

* DuckDB data is not globally sorted by PK  
* No content-addressed structural sharing  
* Merge would require O(N) scan and comparison

**MVP scope:** Two-step merge is deferred (per requirements). When implemented, merge will be row-level with conflict detection on PK collisions.

### **7.4 Write Transparency via OptimizerExtension**

~~DuckDB v1.4.x does not expose write hooks to extensions.~~ **Corrected:** DuckDB's `OptimizerExtension` API provides `pre_optimize_function` that can intercept and transform `LogicalInsert`, `LogicalUpdate`, and `LogicalDelete` operators before optimization.

**MVP approach:**
```sql
-- Transparent writes work directly via OptimizerExtension interception
INSERT INTO _scen_optimistic.forecast VALUES ('X', '2026-01-01', 100, 0.8);
UPDATE _scen_optimistic.forecast SET qty = qty * 1.1 WHERE material_group = 'PLST';
DELETE FROM _scen_optimistic.forecast WHERE obsolete = true;
```

**Fallback options remain available:**
- Wrapper procedures (`scenario_insert`, `scenario_update`, `scenario_delete`)
- Direct delta table access for power users

### **7.5 DuckLake Comparison and Future Architecture**

DuckLake (released May 2025) demonstrates a related architecture pattern worth analyzing:

| Aspect | anofox-scenario (MVP) | DuckLake | Recommendation |
| ------ | --------------------- | -------- | -------------- |
| Activation syntax | `SET SCHEMA _scen_X` | `ATTACH 'ducklake:...' AS db` | Adopt ATTACH for v0.2 |
| Metadata storage | Internal tables | SQL database (same approach) | No change needed |
| Time-travel | Via scenario name | `AT (VERSION => N)` syntax | Add version numbering |
| Change tracking | Implicit in delta tables | `table_changes()` function | Add `scenario_changes()` |
| Write interception | OptimizerExtension | Catalog virtualization | Evolve to Catalog for v0.2 |

**v0.2 Architecture Enhancement: StorageExtension + ScenarioCatalog**

```cpp
// Register storage extension for 'scenario:' protocol
class ScenarioStorageExtension : public StorageExtension {
public:
    ScenarioStorageExtension() {
        attach = AttachScenario;
        create_transaction_manager = CreateScenarioTransactionManager;
    }

    static unique_ptr<Catalog> AttachScenario(
            optional_ptr<StorageExtensionInfo> info,
            ClientContext &context,
            AttachedDatabase &db,
            const string &name,
            AttachInfo &attach_info,
            AttachOptions &options) {
        // Parse scenario name from attach path
        auto scenario_name = ParseScenarioName(attach_info.path);
        // Return custom catalog that virtualizes scenario tables
        return make_uniq<ScenarioCatalog>(db, scenario_name);
    }
};

// Usage in v0.2:
// ATTACH 'scenario:optimistic' AS opt;
// SELECT * FROM opt.forecast;  -- Fully transparent
// INSERT INTO opt.forecast VALUES (...);  -- Intercepted by catalog
```

**Benefits of ATTACH-based approach:**
- Cleaner namespace management
- No `SET SCHEMA` side effects
- Enables `AT (VERSION => N)` syntax extension
- Aligns with DuckDB ecosystem patterns (DuckLake, Iceberg, Delta)

**Reference:** `src/include/duckdb/storage/storage_extension.hpp`

---

## **8\. Design Spike: Merge-on-Read Performance Validation**

Before finalizing implementation, we must validate that merge-on-read query overhead is acceptable.

### **8.1 Spike Objective**

**Question:** What is the query overhead of combining base \+ delta for various table sizes and delta ratios?

### **8.2 Test Methodology**

```sql
-- Setup: Create test tables
CREATE TABLE perf_base AS 
SELECT 
    i as id,
    'MAT-' || lpad(i::VARCHAR, 8, '0') as material_id,
    date '2025-01-01' + (i % 365) as period,
    random() * 1000 as qty,
    random() as confidence
FROM generate_series(1, 10000000) as t(i);  -- 10M rows

-- Create delta tables at various ratios
CREATE TABLE perf_delta_01pct AS SELECT * FROM perf_base USING SAMPLE 0.1%;
CREATE TABLE perf_delta_1pct AS SELECT * FROM perf_base USING SAMPLE 1%;
CREATE TABLE perf_delta_5pct AS SELECT * FROM perf_base USING SAMPLE 5%;
CREATE TABLE perf_delta_10pct AS SELECT * FROM perf_base USING SAMPLE 10%;

-- Add operation markers
ALTER TABLE perf_delta_* ADD COLUMN _op VARCHAR DEFAULT 'U';

-- Benchmark queries
-- 1. Baseline: raw table scan
EXPLAIN ANALYZE SELECT SUM(qty) FROM perf_base;

-- 2. Merge-on-read at each delta ratio
EXPLAIN ANALYZE 
WITH delta AS (SELECT * FROM perf_delta_1pct)
SELECT SUM(qty) FROM (
    SELECT qty FROM delta
    UNION ALL
    SELECT qty FROM perf_base b
    WHERE NOT EXISTS (
        SELECT 1 FROM perf_delta_1pct d WHERE d.id = b.id
    )
);
```

### **8.3 Success Criteria**

| Table Size | Delta Ratio | Max Acceptable Overhead |
| ----- | ----- | ----- |
| 1M rows | 1% | \<10% |
| 10M rows | 1% | \<20% |
| 10M rows | 5% | \<30% |
| 10M rows | 10% | \<50% |

### **8.4 Failure Actions**

If overhead exceeds thresholds:

1. **Bloom filter optimization:** Pre-compute Bloom filter on delta PKs, use for base row filtering  
2. **Materialized scenario views:** Cache merged result for hot scenarios, invalidate on delta change  
3. **Partitioned deltas:** Store deltas partitioned by PK range for partition pruning

### **8.5 Spike Deliverables**

1. Benchmark results table  
2. Query plan analysis (verify hash anti-join is used)  
3. Recommendation: proceed / modify architecture / add optimization

---

## **9\. Implementation Phases**

### **Phase 1: Foundation + Transparent Writes (Weeks 1-4)**

* \[ \] Extension skeleton with registration
* \[ \] Metadata schema creation (including validation fields)
* \[ \] `scenario_create` / `scenario_drop`
* \[ \] Delta table generation
* \[ \] Basic merge-on-read view generation
* \[ \] **OptimizerExtension for transparent write interception** (NEW)
* \[ \] Rowid capture with `pk_hash` validation metadata (NEW)

### **Phase 2: Core Operations (Weeks 5-7)**

* \[ \] `scenario_insert` / `scenario_update` / `scenario_delete` wrapper procedures (fallback)
* \[ \] `scenario_list` / `scenario_stats`
* \[ \] `scenario_archive` / `scenario_unarchive`
* \[ \] `scenario_branch`
* \[ \] `scenario_validate()` function for rowid staleness detection (NEW)

### **Phase 3: Comparison (Weeks 8-9)**

* \[ \] `scenario_compare` (single table)
* \[ \] `scenario_compare_all`
* \[ \] Comparison to branch origin
* \[ \] `scenario_changes()` table function (NEW - similar to DuckLake's `table_changes()`)

### **Phase 4: Snapshots (Weeks 10-11)**

* \[ \] `snapshot_create` / `snapshot_drop`
* \[ \] `snapshot_list` / `snapshot_compare`
* \[ \] `scenario_from_snapshot`

### **Phase 5: Protocols (Weeks 12-13)**

* \[ \] Protocol storage and retrieval
* \[ \] `protocol_set_*` / `protocol_add_*` functions
* \[ \] `protocol_export_markdown`

### **Phase 6: Hardening (Weeks 14-15)**

* \[ \] Performance optimization based on spike results
* \[ \] Error message improvements
* \[ \] Documentation
* \[ \] Integration tests

### **Phase 7: Catalog Integration (v0.2, Weeks 16-18)** (NEW)

* \[ \] `ScenarioCatalog` implementation extending `DuckCatalog`
* \[ \] `StorageExtension` for `ATTACH 'scenario:name'` syntax
* \[ \] Version numbering for scenarios
* \[ \] `AT (VERSION => N)` syntax support
* \[ \] Optional timestamp-based snapshot isolation mode

---

## **10\. Open Questions**

| \# | Question | Impact | Resolution Path |
| ----- | ----- | ----- | ----- |
| 1 | Should rowid capture be lazy (on first write) or eager (at scenario creation)? | Storage vs latency trade-off | Benchmark both approaches |
| 2 | How to handle tables without PK efficiently? | Per REQ-COW-008, use all columns as composite key | May need warning \+ documentation |
| 3 | Should SET SCHEMA affect all sessions or just current? | User ergonomics | Follow DuckDB default (session-scoped) |
| 4 | Concurrent write handling for same scenario? | REQ-NFR-003 says single-writer | Document limitation, use advisory locking |
| 5 | Maximum practical scenario count per database? | Metadata table growth | Benchmark at 100, 1000 scenarios |

---

## **11\. References**

### **11.1 Requirements Document**

* anofox-scenario Requirements Specification v0.1, DataZoo GmbH, January 2026

### **11.2 DuckDB Source References**

| Component | Source Location | Purpose |
| ----- | ----- | ----- |
| Extension API | `src/include/duckdb/main/extension.hpp` | Extension registration |
| Catalog API | `src/include/duckdb/catalog/catalog.hpp` | Schema/table management |
| Schema management | `src/catalog/catalog_entry/schema_catalog_entry.cpp` | Schema operations |
| Table functions | `src/include/duckdb/function/table_function.hpp` | Table-valued functions |
| View creation | `src/catalog/catalog_entry/view_catalog_entry.cpp` | View management |
| **OptimizerExtension** | `src/include/duckdb/optimizer/optimizer_extension.hpp` | **Write interception (NEW)** |
| Optimizer invocation | `src/optimizer/optimizer.cpp:290-297` | Extension callback point |
| LogicalInsert | `src/include/duckdb/planner/operator/logical_insert.hpp` | Insert plan structure |
| LogicalUpdate | `src/include/duckdb/planner/operator/logical_update.hpp` | Update plan structure |
| LogicalDelete | `src/include/duckdb/planner/operator/logical_delete.hpp` | Delete plan structure |
| **StorageExtension** | `src/include/duckdb/storage/storage_extension.hpp` | **ATTACH protocol (v0.2)** |
| Row version manager | `src/storage/table/row_version_manager.cpp` | Rowid stability analysis |
| Extension demo | `test/extension/loadable_extension_optimizer_demo.cpp` | Working pattern example |

### **11.3 Extension Pattern References**

| Extension | Relevance | Source |
| ----- | ----- | ----- |
| Iceberg | Catalog attachment, snapshot semantics | github.com/duckdb/duckdb-iceberg |
| Delta | Multi-file versioning, time travel | github.com/duckdb/duckdb-delta |
| SQLite scanner | Full catalog implementation | github.com/duckdb/duckdb-sqlite |
| **DuckLake** | Metadata catalog, change tracking, ATTACH pattern | github.com/duckdb/ducklake |

### **11.4 Background Analysis**

* "Dolt Internals and DuckDB Feasibility Analysis" \- Validates Delta-Main pattern choice
* DuckLake architecture analysis \- Informs v0.2 ATTACH-based design
* DuckDB optimizer extension patterns \- Enables transparent write interception

---

## **Appendix A: Glossary**

| Term | Definition |
| ----- | ----- |
| Delta-Main | Architecture pattern where bulk data is immutable (Main) and changes accumulate in separate structure (Delta) |
| Merge-on-Read | Query-time combination of base data with delta modifications |
| COW | Copy-on-Write: modifications create new data rather than updating in place |
| Prolly Tree | Probabilistic B-Tree used by Dolt for content-addressed storage |
| Branch origin | The state of base data at the moment a scenario was created |
| Snapshot isolation | Guarantee that scenario sees consistent point-in-time view of base |

## **Appendix B: Requirements Traceability**

| Requirement | Architecture Section | Implementation Component |
| ----- | ----- | ----- |
| REQ-SCEN-001 | 6.1 | ScenarioManager.Create() |
| REQ-SCEN-002 | 6.1 | ScenarioManager.Branch() |
| REQ-COW-001 | 6.2, 5.3 | DeltaStorageEngine.BuildLogicalView() |
| REQ-COW-002 | 5.2, 6.3 | **OptimizerExtension.InterceptScenarioWrites()** (transparent) |
| REQ-COW-003 | 5.2, 6.3 | **OptimizerExtension.InterceptScenarioWrites()** (transparent) |
| REQ-COW-004 | 5.2, 6.3 | **OptimizerExtension.InterceptScenarioWrites()** (transparent) |
| REQ-COMP-001 | 6.4, 5.4 | ComparisonEngine.CompareTables() |
| REQ-NFR-001 | 4.2 | Delta table structure |
| REQ-NFR-004 | 4.4, 4.5 | Rowid capture + validation (NEW) |
| REQ-NFR-004 | 4.4 | \_scenario\_base\_rowids table |
| REQ-NFR-006 | 4.1 | All metadata in embedded tables |
| REQ-PROT-001 | 4.1 | \_scenario\_protocols table |
