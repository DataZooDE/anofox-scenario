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

### scenario_schema

Returns the schema name for a scenario. Use this helper to construct the correct search_path for transparent scenario access.

**Syntax:**
```sql
SELECT scenario_schema(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |

**Returns:** VARCHAR (the schema name, e.g., `_scen_myscenario`)

**Example:**
```sql
-- Get schema name for a scenario
SELECT scenario_schema('pricing_analysis');
-- Returns: _scen_pricing_analysis

-- Use with SET search_path for transparent access
SET search_path = '_scen_pricing_analysis,main';

-- Now unqualified queries use the scenario view
SELECT * FROM products;  -- Uses scenario view with deltas applied

-- Reset to main schema
SET search_path = 'main';
```

**Notes:**
- The search_path should include both the scenario schema and `main` to resolve tables
- When search_path is set, unqualified table names resolve to scenario views first
- Base tables in main schema can still be accessed with explicit `main.tablename` qualification

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
- All columns from the base table with same types

**Constraint Inheritance:**
Delta tables inherit the following constraints from the base table:
- **NOT NULL**: All NOT NULL columns remain NOT NULL in the delta table
- **CHECK constraints**: All CHECK constraints are copied to the delta table
- **PRIMARY KEY**: The delta table uses the same primary key as the base table

This ensures data integrity is maintained within scenarios. Inserting invalid data into a delta table will fail with appropriate constraint violation errors.

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

### scenario_write

Writes a row modification (insert, update, or delete) to a scenario's delta table. This is a helper function for programmatic scenario modifications.

**Syntax:**
```sql
SELECT scenario_write(scenario_name, table_name, operation, row_data);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario |
| table_name | VARCHAR | Name of the base table |
| operation | VARCHAR | 'I' (insert), 'U' (update), or 'D' (delete) |
| row_data | STRUCT | A struct containing column name/value pairs |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Insert a new row
SELECT scenario_write('pricing_analysis', 'products', 'I',
    {id: 100, name: 'New Product', price: 49.99});

-- Update an existing row
SELECT scenario_write('pricing_analysis', 'products', 'U',
    {id: 1, name: 'Updated Name', price: 59.99});

-- Delete a row
SELECT scenario_write('pricing_analysis', 'products', 'D',
    {id: 2, name: 'Old Product', price: 29.99});
```

**Notes:**
- A delta table must already exist for the table (use `delta_create` first)
- The row_data struct should include all columns, especially primary key columns
- Changes are visible through the merge-on-read view in the scenario's schema

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name exists
- `Operation must be 'I', 'U', or 'D'` - Invalid operation type

---

## Comparison Functions

### scenario_compare

Compares a table between a scenario and its base state, returning row-level differences.

**Syntax:**
```sql
SELECT * FROM scenario_compare(scenario_name, table_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to compare |
| table_name | VARCHAR | Name of the table to compare |

**Returns:** Table with columns:
| Column | Type | Description |
|--------|------|-------------|
| diff_type | VARCHAR | Type of change: 'added', 'removed', or 'changed' |
| <pk_columns> | varies | Primary key column(s) identifying the row |
| column_name | VARCHAR | Name of changed column (NULL for added/removed) |
| old_value | VARCHAR | Value in base table (NULL for added rows) |
| new_value | VARCHAR | Value in scenario (NULL for removed rows) |

**Example:**
```sql
-- Create scenario and make changes
SELECT scenario_create('pricing_test', 'Price change analysis');
SELECT delta_create('pricing_test', 'products');

-- Make modifications
INSERT INTO _scen_pricing_test._delta_products (_op, id, name, price) VALUES ('I', 100, 'New Product', 49.99);
INSERT INTO _scen_pricing_test._delta_products (_op, id, name, price) VALUES ('U', 1, 'Widget Pro', 14.99);
INSERT INTO _scen_pricing_test._delta_products (_op, id, name, price) VALUES ('D', 5, 'Discontinued', 9.99);

-- Compare to see all changes
SELECT * FROM scenario_compare('pricing_test', 'products');
-- Returns:
-- diff_type | id  | column_name | old_value | new_value
-- added     | 100 | NULL        | NULL      | NULL
-- changed   | 1   | name        | Widget    | Widget Pro
-- changed   | 1   | price       | 9.99      | 14.99
-- removed   | 5   | NULL        | NULL      | NULL
```

**Notes:**
- For 'added' rows: one row per PK, column_name/old_value/new_value are NULL
- For 'removed' rows: one row per PK, column_name/old_value/new_value are NULL
- For 'changed' rows: one row per changed column with old/new values
- Unchanged rows are excluded from output
- Supports compound primary keys (multiple PK columns in output)

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name
- `Base table '%s' does not exist in main schema` - Table not found
- `Delta table for '%s' does not exist in scenario '%s'` - No delta created

---

### scenario_compare (3-argument)

Compares a table between two scenarios, showing differences between their current states.

**Syntax:**
```sql
SELECT * FROM scenario_compare(scenario_a, scenario_b, table_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_a | VARCHAR | First scenario (the "old" state) |
| scenario_b | VARCHAR | Second scenario (the "new" state) |
| table_name | VARCHAR | Name of the table to compare |

**Returns:** Same schema as 2-argument version (diff_type, pk_columns, column_name, old_value, new_value)

**Example:**
```sql
-- Compare two alternative scenarios
SELECT * FROM scenario_compare('optimistic_forecast', 'pessimistic_forecast', 'sales')
ORDER BY diff_type, id;
```

**Notes:**
- Compares current state of both scenarios (not branch origins)
- 'added' means row exists in B but not in A
- 'removed' means row exists in A but not in B
- 'changed' shows column-level differences for rows in both

---

### scenario_compare_all

Returns a summary of changes per table across all delta tables in a scenario.

**Syntax:**
```sql
SELECT * FROM scenario_compare_all(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario |

**Returns:** Table with columns:
| Column | Type | Description |
|--------|------|-------------|
| table_name | VARCHAR | Name of the table with changes |
| added_rows | BIGINT | Count of inserted rows |
| removed_rows | BIGINT | Count of deleted rows |
| changed_rows | BIGINT | Count of updated rows |

**Example:**
```sql
-- Get summary of all changes in a scenario
SELECT * FROM scenario_compare_all('pricing_analysis');
-- Returns:
-- table_name | added_rows | removed_rows | changed_rows
-- products   |          5 |            2 |           10
-- inventory  |          0 |            3 |            0
```

**Notes:**
- Only tables with at least one change are included
- Efficient: counts directly from delta tables without materializing full diffs
- For branched scenarios, counts are relative to branch origin

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
