# anofox-scenario API Reference

This document describes the SQL API for the anofox-scenario DuckDB extension.

## Configuration Options

### scenario_schema_prefix

Controls the prefix used for scenario schema names.

**Default:** `_scen_`

**Scope:** Session

**Syntax:**
```sql
-- View current value
SELECT current_setting('scenario_schema_prefix');

-- Change the prefix
SET scenario_schema_prefix = '_my_';

-- Reset to default
RESET scenario_schema_prefix;
```

**Description:**

When creating a scenario, the extension creates a schema named `<prefix><scenario_name>`. By default, a scenario named `test` gets schema `_scen_test`. Changing this setting allows using a custom prefix.

**Validation Rules:**
- Must not be empty
- Must end with an underscore (`_`)
- Must contain only alphanumeric characters and underscores

**Example:**
```sql
-- Create scenario with default prefix
SELECT scenario_create('analysis1', 'First analysis');
-- Creates schema: _scen_analysis1

-- Change prefix for new scenarios
SET scenario_schema_prefix = '_experiment_';

-- Create scenario with custom prefix
SELECT scenario_create('analysis2', 'Second analysis');
-- Creates schema: _experiment_analysis2

-- Both scenarios coexist - scenario_list() shows actual schema names
SELECT scenario_name, schema_name FROM scenario_list();
-- analysis1 | _scen_analysis1
-- analysis2 | _experiment_analysis2
```

**Notes:**
- Changing the prefix only affects new scenarios; existing scenarios keep their original schema names
- When dropping scenarios, ensure the prefix matches the scenario's actual schema
- The `schema_name` column in `scenario_list()` shows the actual schema name stored in the registry

**Errors:**
- `scenario_schema_prefix cannot be empty` - Empty string provided
- `scenario_schema_prefix must end with an underscore` - Prefix doesn't end with `_`
- `scenario_schema_prefix must contain only alphanumeric characters and underscores` - Invalid characters

---

## Scenario Lifecycle Functions

### scenario_create

Creates a new scenario with an isolated schema for what-if analysis.

**Syntax:**
```sql
SELECT scenario_create(scenario_name);
SELECT scenario_create(scenario_name, description);
SELECT scenario_create(scenario_name, description, capture_rowids);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required). Must be alphanumeric with underscores, max 63 characters, cannot start with a digit. |
| description | VARCHAR | Optional description of the scenario's purpose. |
| capture_rowids | BOOLEAN | Whether to capture base table rowids at creation time (default: true). Set to false for faster creation on large tables. |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Create a basic scenario
SELECT scenario_create('price_increase_analysis');

-- Create a scenario with description
SELECT scenario_create('supplier_risk_q4', 'Analysis of Q4 supplier disruption impact');

-- Create a scenario without rowid capture (faster for large tables)
SELECT scenario_create('quick_analysis', 'Fast scenario for testing', false);
```

**Performance Note:**
When `capture_rowids` is set to `false`, the scenario creation is faster because it skips capturing individual rowids for all existing tables. This is useful for large tables with millions of rows. The trade-off is that `scenario_validate()` will report `INFO` instead of performing full rowid validation.

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
| schema_name | VARCHAR | Full schema name (prefix + scenario_name) |
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

### scenario_validate

Validates a scenario's integrity by checking if captured rowids still exist in base tables. Use this to detect stale state after VACUUM operations or deletions.

**Syntax:**
```sql
SELECT * FROM scenario_validate(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to validate (required) |

**Returns:** Table with one row per registered table:
| Column | Type | Description |
|--------|------|-------------|
| table_name | VARCHAR | Name of the table |
| validation_status | VARCHAR | 'OK', 'INFO', 'WARNING', or 'ERROR' |
| base_table_exists | BOOLEAN | Whether the base table still exists |
| delta_table_exists | BOOLEAN | Whether a delta table has been created |
| captured_row_count | BIGINT | Row count when scenario was created |
| current_row_count | BIGINT | Current row count in base table |
| missing_rowids | BIGINT | Count of captured rowids no longer in base table |
| message | VARCHAR | Human-readable validation message |

**Status Levels:**
- `OK` - All validations passed
- `INFO` - Row count changed (rows added to base table)
- `WARNING` - Missing rowids detected (possible VACUUM or DELETE)
- `ERROR` - Base table no longer exists

**Example:**
```sql
-- Validate a scenario
SELECT * FROM scenario_validate('pricing_analysis');

-- Check for issues only
SELECT table_name, validation_status, message
FROM scenario_validate('pricing_analysis')
WHERE validation_status != 'OK';
```

**Notes:**
- Missing rowids can indicate VACUUM reorganization or direct deletes on base tables
- If base data is modified after scenario creation, consider recreating the scenario
- Scenarios with delta tables use PK-based merging, which is resilient to rowid changes

**Errors:**
- Returns error row if scenario doesn't exist

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

**Tables Without Primary Keys:**
For tables without a primary key, all columns are used as a composite key for row identification. This has the following implications:

- **Updates require DELETE + INSERT**: Since there's no unique identifier, updates must be performed as two operations:
  ```sql
  -- Delete the old row (specify exact old values)
  INSERT INTO _scen_myscenario._delta_products (_op, col1, col2) VALUES ('D', 'old_val1', 'old_val2');
  -- Insert the new row
  INSERT INTO _scen_myscenario._delta_products (_op, col1, col2) VALUES ('I', 'new_val1', 'new_val2');
  ```

- **Duplicate rows**: If the base table contains duplicate rows (identical values in all columns), a single DELETE operation will remove all matching duplicates.

- **NULL handling**: The row matching uses `IS NOT DISTINCT FROM` semantics, so NULL values are handled correctly.

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

## Snapshot Functions

### snapshot_create

Creates a named snapshot of a scenario's current state for later comparison.

**Syntax:**
```sql
SELECT snapshot_create(scenario_name, snapshot_name);
SELECT snapshot_create(scenario_name, snapshot_name, description);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario to snapshot (required) |
| snapshot_name | VARCHAR | Name for the snapshot (required). Must be alphanumeric with underscores, max 63 characters, cannot start with a digit. |
| description | VARCHAR | Optional description of the snapshot |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Create a scenario and make some changes
SELECT scenario_create('pricing_analysis');
SELECT delta_create('pricing_analysis', 'products');
INSERT INTO _scen_pricing_analysis._delta_products (_op, id, name, price) VALUES ('U', 1, 'Widget', 12.99);

-- Create a snapshot to mark this state
SELECT snapshot_create('pricing_analysis', 'baseline_q4', 'Q4 baseline prices');
```

**Errors:**
- `Snapshot '%s' already exists` - A snapshot with this name already exists
- `Scenario '%s' does not exist` - No scenario with this name
- `Invalid snapshot name '%s'` - Name validation failed

---

### snapshot_list

Lists all snapshots with their metadata.

**Syntax:**
```sql
SELECT * FROM snapshot_list();
```

**Parameters:** None

**Returns:** Table with columns:
| Column | Type | Description |
|--------|------|-------------|
| snapshot_name | VARCHAR | Name of the snapshot |
| source_schema | VARCHAR | Schema name of the source scenario |
| created_at | TIMESTAMP | When the snapshot was created |
| description | VARCHAR | Optional description |
| size_bytes | BIGINT | Size of the snapshot (NULL if not computed) |

**Example:**
```sql
-- List all snapshots
SELECT * FROM snapshot_list();

-- Find snapshots for a specific scenario
SELECT * FROM snapshot_list() WHERE source_schema = '_scen_pricing_analysis';

-- List snapshots by creation time
SELECT snapshot_name, created_at FROM snapshot_list() ORDER BY created_at DESC;
```

**Notes:**
- Results are ordered by `created_at DESC` (newest first)
- Snapshots persist after scenario deletion (they reference source_schema, not scenario_id)

---

### snapshot_compare

Compares the current state of a table against a snapshot, showing row-level differences.

**Syntax:**
```sql
SELECT * FROM snapshot_compare(snapshot_name, table_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| snapshot_name | VARCHAR | Name of the snapshot to compare against (required) |
| table_name | VARCHAR | Name of the table to compare (required) |

**Returns:** Table with columns:
| Column | Type | Description |
|--------|------|-------------|
| diff_type | VARCHAR | Type of change: 'added', 'removed', or 'changed' |
| <pk_columns> | varies | Primary key column(s) identifying the row |
| column_name | VARCHAR | Name of changed column (NULL for added/removed) |
| old_value | VARCHAR | Value at snapshot time (NULL for added rows) |
| new_value | VARCHAR | Current value in main (NULL for removed rows) |

**Example:**
```sql
-- Create a scenario, make changes, and snapshot
SELECT scenario_create('pricing_analysis');
SELECT delta_create('pricing_analysis', 'products');
INSERT INTO _scen_pricing_analysis._delta_products (_op, id, name, price) VALUES ('U', 1, 'Widget Pro', 14.99);
SELECT snapshot_create('pricing_analysis', 'q4_prices', 'Q4 pricing snapshot');

-- Later, compare current main state against the snapshot
SELECT * FROM snapshot_compare('q4_prices', 'products');
-- Returns:
-- diff_type | id  | column_name | old_value  | new_value
-- changed   | 1   | name        | Widget Pro | Widget
-- changed   | 1   | price       | 14.99      | 9.99
```

**Notes:**
- For 'added' rows: one row per PK, column_name/old_value/new_value are NULL (row added to main since snapshot)
- For 'removed' rows: one row per PK, column_name/old_value/new_value are NULL (row was in snapshot but not in current main)
- For 'changed' rows: one row per changed column with old/new values
- Unchanged rows are excluded from output
- The snapshot captures the scenario's merged view (base + deltas) at snapshot time
- Supports compound primary keys (multiple PK columns in output)

**Errors:**
- `Snapshot '%s' does not exist` - No snapshot with this name
- `Table '%s' not found in snapshot '%s'` - Table wasn't registered in the scenario when snapshot was created

---

### scenario_from_snapshot

Creates a new scenario using a snapshot as its base. The scenario sees the snapshot data as its baseline instead of current main, allowing branching from historical states.

**Syntax:**
```sql
SELECT scenario_from_snapshot(snapshot_name, scenario_name);
SELECT scenario_from_snapshot(snapshot_name, scenario_name, description);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| snapshot_name | VARCHAR | Name of the snapshot to use as base (required) |
| scenario_name | VARCHAR | Name for the new scenario (required) |
| description | VARCHAR | Optional description |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Create a scenario, make changes, and snapshot
SELECT scenario_create('q4_analysis');
SELECT delta_create('q4_analysis', 'sales');
INSERT INTO _scen_q4_analysis._delta_sales (_op, region, amount) VALUES ('U', 'west', 15000);
SELECT snapshot_create('q4_analysis', 'q4_final', 'Q4 final numbers');

-- Later, create new scenario from that snapshot
SELECT scenario_from_snapshot('q4_final', 'q5_planning', 'Start Q5 from Q4 final');

-- New scenario sees snapshot data as baseline
SELECT * FROM _scen_q5_planning.sales;  -- Shows Q4 final data, not current main

-- Modifications are independent
INSERT INTO _scen_q5_planning._delta_sales (_op, region, amount) VALUES ('U', 'west', 18000);
-- This doesn't affect the q4_final snapshot
```

**Notes:**
- The new scenario's `base_schema` points to the snapshot schema (not main)
- Delta tables and merge views are auto-created for all tables in the snapshot
- Modifications in the new scenario don't affect the snapshot
- Useful for branching from historical states or creating "what-if" scenarios from past baselines

**Errors:**
- `Snapshot '%s' does not exist` - No snapshot with this name
- `Scenario '%s' already exists` - A scenario with this name already exists
- `Invalid scenario name '%s'` - Name validation failed

---

### snapshot_drop

Deletes a snapshot and its associated data.

**Syntax:**
```sql
SELECT snapshot_drop(snapshot_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| snapshot_name | VARCHAR | Name of the snapshot to drop (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Drop an unused snapshot
SELECT snapshot_drop('old_snapshot');

-- First check if any scenarios depend on it
SELECT scenario_name FROM _scenario_registry WHERE base_schema = '_snap_old_snapshot';
-- If empty, safe to drop
SELECT snapshot_drop('old_snapshot');
```

**Notes:**
- Drops the snapshot metadata from `_scenario_snapshots`
- Drops the snapshot schema (`_snap_<snapshot_name>`) and all its data
- Cannot drop a snapshot if any scenarios depend on it (created via `scenario_from_snapshot`)
- Operation is irreversible - snapshot data cannot be recovered

**Errors:**
- `Snapshot '%s' does not exist` - No snapshot with this name
- `Cannot drop snapshot '%s': scenario '%s' depends on it` - A scenario uses this snapshot as its base

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

### _scenario_snapshots

Stores information about scenario snapshots.

| Column | Type | Description |
|--------|------|-------------|
| snapshot_id | INTEGER | Unique identifier |
| snapshot_name | VARCHAR | Human-readable name |
| source_schema | VARCHAR | Schema name of the source scenario |
| created_at | TIMESTAMP | When the snapshot was created |
| description | VARCHAR | Optional description |
| size_bytes | BIGINT | Size of the snapshot (NULL if not computed) |

### _scenario_protocols

Stores protocol documentation for scenarios and snapshots.

| Column | Type | Description |
|--------|------|-------------|
| entity_type | VARCHAR | 'scenario' or 'snapshot' |
| entity_name | VARCHAR | Name of the scenario or snapshot |
| section | VARCHAR | Protocol section: 'why', 'changes', 'findings', 'plan', 'decision', 'metadata' |
| content | VARCHAR | Text content for this section |
| updated_at | TIMESTAMP | When this section was last updated |

---

## DDL Restrictions

### Schema Modifications Not Permitted

Scenario schemas (`_scen_*`) are managed by the anofox-scenario extension and do not allow direct DDL operations. The following operations are blocked:

**CREATE TABLE:**
```sql
-- This will fail with an error
CREATE TABLE _scen_myscenario.new_table (id INTEGER);
-- Error: CREATE TABLE is not permitted in scenario schemas
```

**ALTER TABLE:**
```sql
-- This will fail with an error
ALTER TABLE _scen_myscenario.products ADD COLUMN new_col INTEGER;
-- Error: ALTER TABLE is not permitted in scenario schemas
```

**DROP TABLE:**
```sql
-- This will fail with an error
DROP TABLE _scen_myscenario.products;
-- Error: DROP is not permitted in scenario schemas
```

### Rationale

Scenarios use a copy-on-write (COW) pattern where:
- The base table structure remains immutable
- Modifications are stored in delta tables
- Merge-on-read views combine base + delta data

Schema modifications would break this pattern and invalidate existing delta data. Instead:
- Use `delta_create()` to enable modifications on existing tables
- Use `delta_drop()` to remove delta tables
- Use `scenario_drop()` to remove an entire scenario

---

## Protocol Functions

Protocol functions allow embedding documentation within scenarios for audit trails and decision tracking.

### protocol_set_why

Sets or replaces the "why" section of a scenario's protocol, documenting the purpose of the scenario.

**Syntax:**
```sql
SELECT protocol_set_why(scenario_name, why_text);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |
| why_text | VARCHAR | Text describing why this scenario was created (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Document why the scenario was created
SELECT protocol_set_why('pricing_analysis', 'Exploring impact of 15% price increase on Q4 revenue');

-- Update the reason later
SELECT protocol_set_why('pricing_analysis', 'Updated: Now analyzing 10% increase after stakeholder feedback');
```

**Notes:**
- Replaces any existing "why" content (not append)
- Stored in `_scenario_protocols` table with section='why'

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### protocol_log_change

Appends an entry to the "changes" section of a scenario's protocol, creating an audit trail of modifications.

**Syntax:**
```sql
SELECT protocol_log_change(scenario_name, change_text);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |
| change_text | VARCHAR | Description of the change being logged (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Log changes as you modify the scenario
SELECT protocol_log_change('pricing_analysis', 'Increased price by 10% for product category A');
SELECT protocol_log_change('pricing_analysis', 'Added bulk discount rules for orders > 100 units');
SELECT protocol_log_change('pricing_analysis', 'Removed discontinued products from analysis');
```

**Notes:**
- Appends to existing changes (does not replace)
- Each entry is separated by a newline
- Creates the "changes" section if it doesn't exist
- Useful for creating audit trails of scenario modifications

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### protocol_add_finding

Appends an entry to the "findings" section of a scenario's protocol, documenting insights and observations.

**Syntax:**
```sql
SELECT protocol_add_finding(scenario_name, finding_text);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |
| finding_text | VARCHAR | Description of the finding to record (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Record findings as you analyze the scenario
SELECT protocol_add_finding('pricing_analysis', 'Revenue increased by 5% with new pricing');
SELECT protocol_add_finding('pricing_analysis', 'Customer churn remained stable at 2.3%');
SELECT protocol_add_finding('pricing_analysis', 'Premium tier showed strongest response (+12%)');
```

**Notes:**
- Appends to existing findings (does not replace)
- Each entry is separated by a newline
- Creates the "findings" section if it doesn't exist
- Useful for documenting insights discovered during scenario analysis

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### protocol_set_plan

Sets the "plan" section of a scenario's protocol, describing the intended approach or methodology. Replaces any existing plan content.

**Syntax:**
```sql
SELECT protocol_set_plan(scenario_name, plan_text);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |
| plan_text | VARCHAR | Description of the plan or methodology (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Set initial plan
SELECT protocol_set_plan('pricing_analysis', 'Step 1: Update prices by 10%. Step 2: Monitor sales for 2 weeks. Step 3: Adjust based on results.');

-- Update plan (replaces existing)
SELECT protocol_set_plan('pricing_analysis', 'Revised: Test in West region first, then roll out nationally.');
```

**Notes:**
- Replaces existing plan content (does not append)
- Creates the "plan" section if it doesn't exist
- Useful for documenting what you intend to do before making changes

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### protocol_set_decision

Sets the "decision" section of a scenario's protocol, documenting the final decision or outcome. Replaces any existing decision content.

**Syntax:**
```sql
SELECT protocol_set_decision(scenario_name, decision_text);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |
| decision_text | VARCHAR | Description of the decision made (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Record the decision
SELECT protocol_set_decision('pricing_analysis', 'Approved: roll out 10% price increase to all regions starting Q2.');

-- Update decision if it changes
SELECT protocol_set_decision('pricing_analysis', 'Revised: delay rollout pending competitor response analysis.');
```

**Notes:**
- Replaces existing decision content (does not append)
- Creates the "decision" section if it doesn't exist
- Useful for documenting the final outcome of the scenario analysis

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### protocol_read

Reads all protocol sections for a scenario as a table. Returns all sections that have been set (why, changes, findings, plan, decision).

**Syntax:**
```sql
SELECT * FROM protocol_read(scenario_name);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |

**Returns:** Table with columns:
| Column | Type | Description |
|--------|------|-------------|
| section | VARCHAR | The section name (why, changes, findings, plan, decision) |
| content | VARCHAR | The content of the section |
| updated_at | TIMESTAMP | When the section was last updated |

**Example:**
```sql
-- Read all protocol sections for a scenario
SELECT * FROM protocol_read('pricing_analysis');

-- Get specific sections
SELECT section, content FROM protocol_read('pricing_analysis') WHERE section IN ('why', 'decision');

-- Check when sections were last updated
SELECT section, updated_at FROM protocol_read('pricing_analysis') ORDER BY updated_at DESC;
```

**Notes:**
- Returns only sections that have been set (empty sections are not returned)
- Sections are returned in alphabetical order by section name
- Useful for reviewing the complete documentation of a scenario

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name

---

### protocol_export_markdown

Exports all protocol sections for a scenario to a markdown file.

**Syntax:**
```sql
SELECT protocol_export_markdown(scenario_name, file_path);
```

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| scenario_name | VARCHAR | Name of the scenario (required) |
| file_path | VARCHAR | Path to the output markdown file (required) |

**Returns:** BOOLEAN (true on success)

**Example:**
```sql
-- Export protocol to a markdown file
SELECT protocol_export_markdown('pricing_analysis', '/reports/pricing_protocol.md');

-- Export to a relative path
SELECT protocol_export_markdown('q4_forecast', './scenario_docs/q4_forecast.md');
```

**Output Format:**
The generated markdown file includes:
- Title with scenario name
- Sections in logical order: Why, Plan, Changes Made, Findings, Decision
- Only sections that have content are included
- Footer indicating the file was exported from anofox-scenario

**Notes:**
- Creates the file if it doesn't exist
- Overwrites any existing file at the specified path
- Only includes sections that have been set
- Useful for generating documentation reports from scenario protocols

**Errors:**
- `Scenario '%s' does not exist` - No scenario with this name