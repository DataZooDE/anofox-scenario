# Error Handling Policy

This document defines the error handling conventions for the anofox-scenario extension.

## Error Categories

### Fatal Errors

Fatal errors prevent the operation from completing and must be reported to the user immediately. Use `throw` with an appropriate DuckDB exception type.

**When to throw:**
- User input validation failures (invalid names, missing parameters)
- Required resources not found (scenario doesn't exist, table doesn't exist)
- Constraint violations (duplicate names, circular dependencies)
- Core operation failures (CREATE SCHEMA, INSERT into registry)

**Exception types:**
- `InvalidInputException` - User provided invalid parameters
- `ConstraintException` - Constraint violation (duplicates, dependencies)
- `NotImplementedException` - Operation not supported (DDL in scenarios)
- `InternalException` - Bug in extension code (should not occur)

**Pattern:**
```cpp
auto result = con.Query(sql);
if (result->HasError()) {
    throw InvalidInputException("Failed to do X: %s", result->GetError().c_str());
}
```

### Best-Effort Operations

Best-effort operations enhance functionality but are not required for correctness. Failures should be logged but not propagated to the user.

**Examples:**
- Index creation for performance optimization
- Statistics updates
- Non-critical metadata updates

**Pattern:**
```cpp
auto result = con.Query(index_sql);
if (result->HasError()) {
    // Log for debugging, but don't fail the operation
    // Index is for performance only; merge-on-read works without it
}
```

## Error Message Guidelines

All error messages should:

1. **Include context** - Scenario name, table name, operation being performed
2. **Be specific** - What exactly failed and why
3. **Suggest remediation** - What the user can do to fix the issue

**Good:**
```
Scenario 'pricing_analysis' does not exist
Delta table for 'products' does not exist in scenario 'test'. Call delta_create first.
CREATE TABLE is not permitted in scenario schemas. Use delta_create() to enable modifications.
```

**Bad:**
```
Not found
Operation failed
Invalid input
```

## Error Handling by Component

### ScenarioManager

| Operation | Error Type | Notes |
|-----------|------------|-------|
| scenario_create name validation | Fatal | Invalid names rejected immediately |
| scenario_create duplicate check | Fatal | Prevents data corruption |
| schema creation | Fatal | Required for scenario to function |
| registry insertion | Fatal | Required for scenario tracking |
| table registration | Fatal | Required for merge-on-read |
| rowid capture | Fatal | Required for validation |

### DeltaStorageEngine

| Operation | Error Type | Notes |
|-----------|------------|-------|
| delta_create table validation | Fatal | Base table must exist |
| delta table creation | Fatal | Required for modifications |
| index creation | Best-effort | Performance optimization only |
| merge view creation | Fatal | Required for transparent reads |

### ComparisonEngine

| Operation | Error Type | Notes |
|-----------|------------|-------|
| scenario validation | Fatal | Must exist to compare |
| table validation | Fatal | Must exist to compare |
| comparison query | Fatal | Core functionality |

### SnapshotManager

| Operation | Error Type | Notes |
|-----------|------------|-------|
| snapshot_create validation | Fatal | Scenario and name must be valid |
| data copy | Fatal | Required for snapshot integrity |

### ProtocolManager

| Operation | Error Type | Notes |
|-----------|------------|-------|
| scenario validation | Fatal | Must exist to store protocols |
| protocol storage | Fatal | Core functionality |

## Transaction Handling

### Atomic Operations

Operations that modify multiple tables should use transactions:

```cpp
con.Query("BEGIN TRANSACTION");
try {
    // Multiple modifications
    con.Query("COMMIT");
} catch (...) {
    con.Query("ROLLBACK");
    throw;
}
```

### Current Transaction Usage

- `scenario_create` - Uses transaction for schema + registry + tables + rowids
- `scenario_drop` - Uses transaction for cleanup across all tables
- `scenario_branch` - Uses transaction for registry + table copying

## Logging

### When to Log

- Best-effort operation failures (for debugging)
- Performance warnings (large operations)
- Deprecation warnings (future use)

### Log Levels

- `DEBUG` - Detailed operation tracing
- `INFO` - Normal operation milestones
- `WARNING` - Non-fatal issues that may need attention
- `ERROR` - Failures that are caught and handled

### DuckDB Logging API

```cpp
// Note: Logging infrastructure TBD
// For now, best-effort failures are silent
// Future: Use DUCKDB_LOG_* macros when available
```

## Testing Error Handling

All error conditions should be tested in `test/sql/scenario_errors.test`:

```sql
statement error
SELECT scenario_create('123invalid', 'starts with digit');
----
Invalid scenario name '123invalid'
```

Test files verify:
- Error message includes relevant context
- Correct exception type is thrown
- Operation does not leave partial state
