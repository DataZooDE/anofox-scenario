# anofox-scenario API Reference

This document describes the SQL API for the anofox-scenario DuckDB extension.

## Scenario Lifecycle Functions

### scenario_create

Creates a new scenario with an isolated schema for what-if analysis.

**Syntax:**
```sql
SELECT scenario_create(scenario_name);
SELECT scenario_create(scenario_name, description);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required). Must be alphanumeric with underscores, max 63 characters, cannot start with a digit. |
| description | VARCHAR | Optional description of the scenario's purpose. |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Create a basic scenario
SELECT scenario_create('price_increase_analysis');

-- Create a scenario with description
SELECT scenario_create('supplier_risk_q4', 'Analysis of Q4 supplier disruption impact');
```

**Errors:**
- `Scenario '%s' already exists` - A scenario with this name already exists
- `Invalid scenario name '%s'` - Name contains invalid characters or exceeds length limit

---

### scenario_drop

Drops an existing scenario and its associated schema.

**Syntax:**
```sql
SELECT scenario_drop(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to drop (required). |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Drop a scenario
SELECT scenario_drop('price_increase_analysis');
```

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name exists
- `Cannot drop scenario '%s': scenario '%s' depends on it` - Another scenario was branched from this one

---

### scenario_list

Lists all scenarios with their metadata.

**Syntax:**
```sql
SELECT * FROM scenario_list();
```

**Parameters:** None

**Returns:** Table with columns:
| Column | Type | Description |
|--------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario |
| status | VARCHAR | 'active' or 'archived' |
| description | VARCHAR | Optional description |
| created_at | TIMESTAMP | When the scenario was created |
| base_schema | VARCHAR | Schema the scenario branches from |
| parent_scenario | VARCHAR | Name of parent scenario (NULL if not branched) |

**Example:**
```sql
-- List all scenarios
SELECT * FROM scenario_list();

-- List only active scenarios
SELECT scenario_name, description FROM scenario_list() WHERE status = 'active';

-- Count scenarios
SELECT COUNT(*) FROM scenario_list();
```

---

### scenario_branch

Creates a new scenario that branches from an existing scenario, inheriting table registrations and base rowids.

**Syntax:**
```sql
SELECT scenario_branch(source_scenario, new_name);
SELECT scenario_branch(source_scenario, new_name, description);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| source_scenario | VARCHAR | Name of the scenario to branch from (required) |
| new_name | VARCHAR | Name for the new branched scenario (required) |
| description | VARCHAR | Optional description |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Branch from existing scenario
SELECT scenario_branch('baseline_q4', 'optimistic_q4', 'Optimistic forecast variant');
```

**Errors:**
- `Scenario '%s' does not exist` - Source scenario doesn't exist
- `Scenario '%s' already exists` - New name already taken
- `Invalid scenario name '%s'` - Name validation failed

---

### scenario_stats

Returns statistics about a scenario.

**Syntax:**
```sql
SELECT * FROM scenario_stats(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |

**Returns:** Table with single row:
| Column | Type | Description |
|--------|------|-------------|
| table_count | BIGINT | Number of registered tables |
| total_base_rows | BIGINT | Sum of base row counts |
| delta_row_count | BIGINT | Count of delta modifications (0 if no deltas) |
| created_at | TIMESTAMP | When the scenario was created |
| status | VARCHAR | 'active' or 'archived' |

**Example:**
```sql
-- Get scenario statistics
SELECT * FROM scenario_stats('pricing_analysis');
```

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### scenario_archive

Marks a scenario as archived (read-only).

**Syntax:**
```sql
SELECT scenario_archive(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to archive (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Archive a completed scenario
SELECT scenario_archive('q4_final_analysis');
```

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### scenario_unarchive

Restores an archived scenario to active status.

**Syntax:**
```sql
SELECT scenario_unarchive(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to unarchive (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Restore an archived scenario
SELECT scenario_unarchive('q4_final_analysis');
```

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

## Delta Storage Functions

### delta_create

Creates a delta table for a base table in a scenario's schema. Delta tables store modifications (inserts, updates, deletes) for copy-on-write storage.

**Syntax:**
```sql
SELECT delta_create(scenario_name, table_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to create delta table in |
| table_name | VARCHAR | Name of the base table in main schema |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Create delta table for products in a scenario
SELECT delta_create('pricing_analysis', 'products');
```

**Delta Table Structure:**
The created delta table (`_delta_<tablename>`) contains:
- `_op VARCHAR NOT NULL` - Operation type: 'I' (insert), 'U' (update), 'D' (delete)
- `_ts TIMESTAMP` - Timestamp of the modification
- `_version INTEGER` - Version counter (default: 1)
- All columns from the base table with same types and constraints

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name exists
- `Delta table for '%s' already exists in scenario '%s'` - Delta already created
- `Base table '%s' not found in main schema` - No such table in main

---

### delta_drop

Drops a delta table from a scenario's schema.

**Syntax:**
```sql
SELECT delta_drop(scenario_name, table_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario |
| table_name | VARCHAR | Name of the base table whose delta to drop |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Drop delta table
SELECT delta_drop('pricing_analysis', 'products');
```

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name exists
- `Delta table for '%s' does not exist in scenario '%s'` - No delta table

---

## Metadata Tables

### _scenario_registry

Stores information about all scenarios.

| Column | Type | Description |
|--------|------|-------------|
| scenario_id | INTEGER | Unique identifier |
| scenario_name | VARCHAR | Human-readable name |
| schema_name | VARCHAR | Associated DuckDB schema |
| base_schema | VARCHAR | Schema scenarios branch from (default: 'main') |
| base_captured_at | TIMESTAMP | When the base data was captured |
| created_at | TIMESTAMP | When the scenario was created |
| status | VARCHAR | 'active' or 'archived' |
| description | VARCHAR | Optional description |
| parent_scenario_id | INTEGER | ID of parent scenario (for branching) |

### _scenario_tables

Stores per-scenario table registration information.

| Column | Type | Description |
|--------|------|-------------|
| scenario_id | INTEGER | Reference to scenario |
| table_name | VARCHAR | Name of the registered table |
| base_row_count | BIGINT | Row count at scenario creation |
| has_primary_key | BOOLEAN | Whether table has a primary key |
| primary_key_columns | VARCHAR[] | List of primary key column names |

### _scenario_base_rowids

Stores row identifiers from base tables at scenario creation.

| Column | Type | Description |
|--------|------|-------------|
| scenario_id | INTEGER | Reference to scenario |
| table_name | VARCHAR | Name of the base table |
| row_id | BIGINT | Row identifier |
