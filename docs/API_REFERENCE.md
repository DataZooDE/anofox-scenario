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
