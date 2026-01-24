# Agent Instructions: anofox-scenario

**DuckDB Extension for Git-Like Database Branching**

This document guides AI agents in developing the anofox-scenario extension using incremental TDD with beads for project management and git worktrees for parallel epic development.

---

## Project Overview

**anofox-scenario** enables Git-like branching for analytical databases. Users create isolated scenarios for what-if analysis, compare scenarios against baselines, and maintain audit trails.

### Core Components (from design doc)

| Component | Responsibility | Key Requirements |
|-----------|---------------|------------------|
| **ScenarioManager** | Lifecycle: create, branch, list, archive, drop | REQ-SCEN-001 to REQ-SCEN-008 |
| **DeltaStorageEngine** | COW storage, transparent SQL operations | REQ-COW-001 to REQ-COW-008 |
| **ComparisonEngine** | Diff between schemas/scenarios | REQ-COMP-001 to REQ-COMP-004 |
| **SnapshotManager** | Immutable point-in-time captures | REQ-SNAP-001 to REQ-SNAP-005 |
| **ProtocolManager** | Embedded documentation storage | REQ-PROT-001 to REQ-PROT-005 |
| **MetadataStore** | Internal registry tables | Supporting infrastructure |

### Architecture Pattern

**Delta-Main Pattern:** Base data stays immutable (DuckDB columnar), scenario modifications stored in delta tables, merged on read.

```
User SQL --> DuckDB --> OptimizerExtension intercepts writes --> Delta tables
                    --> Scenario views merge base + delta on read
```

---

## Development Philosophy

### Incremental Development

Build feature-by-feature with each increment demonstrably working:
1. Skeleton extension that loads and passes tests
2. Metadata tables creation
3. `scenario_create` with basic functionality
4. Each subsequent function builds on working foundation

### Test-Driven Development (TDD)

**Red-Green-Refactor Cycle:**
1. **RED:** Write failing SQLLogicTest specifying expected behavior
2. **GREEN:** Implement minimal code to make test pass
3. **REFACTOR:** Clean up while keeping tests green

### Beads for Epic/Task Management

Track work in beads (`bd`) for cross-session persistence:
- **Epics:** Map to requirement categories (6 epics)
- **Tasks:** Individual functions or features within an epic
- **Dependencies:** Tasks blocked until prerequisites complete

### Git Worktrees for Parallelism

Independent epics developed in separate worktrees to avoid merge conflicts and enable parallel work.

---

## Git Worktree Strategy

### Setup Worktrees for Independent Epics

```bash
# From main repo, create worktrees for each epic
git worktree add ../worktrees/scen-lifecycle feature/scenario-lifecycle
git worktree add ../worktrees/cow-storage feature/cow-storage
git worktree add ../worktrees/comparison feature/comparison
git worktree add ../worktrees/snapshots feature/snapshots
git worktree add ../worktrees/protocols feature/protocols
```

### Directory Structure

```
anofox-scenario/           # Main repo (integration branch)
worktrees/
  scen-lifecycle/          # Epic 1 development
  cow-storage/             # Epic 2 development
  comparison/              # Epic 3 development
  snapshots/               # Epic 4 development
  protocols/               # Epic 5 development
```

### Worktree Workflow

1. **Start work on epic:** `cd ../worktrees/scen-lifecycle`
2. **Build and test:** `GEN=ninja make && make test`
3. **Commit changes:** Normal git workflow
4. **Merge to main:** `git checkout main && git merge feature/scenario-lifecycle`
5. **Remove worktree:** `git worktree remove ../worktrees/scen-lifecycle`

### Cross-Epic Dependencies

Some epics depend on others:
- **COW-Storage** depends on **Scenario-Lifecycle** (needs `scenario_create` first)
- **Comparison** depends on **COW-Storage** (needs delta tables to compare)
- **Snapshots** can proceed independently
- **Protocols** can proceed independently

Track dependencies in beads: `bd dep add <task> <depends-on>`

---

## TDD Workflow

### Per-Task Development Cycle

```bash
# 1. Claim task in beads
bd update <task-id> --status=in_progress

# 2. Write failing test (RED)
# Edit test/sql/scenario_lifecycle.test

# 3. Run test to confirm it fails
GEN=ninja make && make test
# Expected: Test fails with clear error

# 4. Implement minimal code (GREEN)
# Edit src/scenario_manager.cpp

# 5. Run test to confirm it passes
GEN=ninja make && make test
# Expected: Test passes

# 6. Refactor while green
# Clean up code, add logging, improve error messages

# 7. Verify tests still pass
GEN=ninja make && make test

# 8. Close task
bd close <task-id>

# 9. Sync beads
bd sync
```

### SQLLogicTest Patterns

**Test file structure:**
```sql
# name: test/sql/scenario_lifecycle.test
# description: Test scenario lifecycle operations
# group: [anofox_scenario]

require anofox_scenario

# Setup
statement ok
CREATE TABLE test_data (id INTEGER PRIMARY KEY, value VARCHAR);

statement ok
INSERT INTO test_data VALUES (1, 'hello'), (2, 'world');

# Test: scenario_create
statement ok
CALL scenario_create('test_scenario', 'Test description');

# Verify scenario exists
query ITT
SELECT scenario_name, status, description FROM scenario_list();
----
test_scenario	active	Test description

# Cleanup
statement ok
CALL scenario_drop('test_scenario');
```

**Error testing:**
```sql
# Test: Cannot create scenario with existing name
statement ok
CALL scenario_create('duplicate', 'First');

statement error
CALL scenario_create('duplicate', 'Second');
----
Scenario 'duplicate' already exists
```

**Type indicators:** `I` (integer), `T` (text), `R` (real), `D` (date), `B` (boolean)

---

## Build & Test Commands

### Essential Commands

```bash
# Fast incremental build (use ninja)
GEN=ninja make

# Full release build
make release

# Debug build (with symbols)
make debug

# Run all SQLLogicTests
make test

# Run specific test file
./build/release/test/unittest "test/sql/scenario_lifecycle.test"

# Interactive DuckDB with extension
./build/release/duckdb
> LOAD 'build/release/extension/anofox_scenario/anofox_scenario.duckdb_extension';
```

### Debugging with Logging

```cpp
// In C++ code, add logging:
DUCKDB_LOG_DEBUG(context, "anofox_scenario.ScenarioCreate",
    StringUtil::Format("Creating scenario: %s", scenario_name));
```

```sql
-- In DuckDB, enable and view logs:
CALL enable_logging(level = 'debug');
SELECT * FROM duckdb_logs_parsed('anofox_scenario%');
```

### Clean Build (rare)

```bash
make clean && GEN=ninja make && make test
```

---

## File Organization

### Source Structure

```
src/
  anofox_scenario_extension.cpp   # Entry point, function registration
  include/
    anofox_scenario_extension.hpp # Extension class
    scenario_manager.hpp
    delta_storage_engine.hpp
    comparison_engine.hpp
    snapshot_manager.hpp
    protocol_manager.hpp
    metadata_store.hpp
  scenario_manager.cpp            # Lifecycle operations
  delta_storage_engine.cpp        # COW implementation
  comparison_engine.cpp           # Diff logic
  snapshot_manager.cpp            # Snapshot operations
  protocol_manager.cpp            # Documentation handling
  metadata_store.cpp              # Registry management
```

### Test Structure

```
test/sql/
  scenario_lifecycle.test         # Epic 1 tests
  scenario_read.test              # COW read tests
  scenario_write.test             # COW write tests (INSERT/UPDATE/DELETE)
  scenario_constraints.test       # PK/FK/CHECK enforcement
  scenario_comparison.test        # Diff tests
  scenario_snapshots.test         # Snapshot tests
  scenario_protocols.test         # Protocol tests
  scenario_errors.test            # Error message verification
```

### Configuration Files

```
CMakeLists.txt                    # Build config (add source files here)
extension_config.cmake            # Extension registration
vcpkg.json                        # Dependencies (currently OpenSSL)
Makefile                          # Build orchestration
```

---

## Beads Quick Reference

### Finding Work

```bash
bd ready                          # Show issues ready to work (no blockers)
bd list --status=open             # All open issues
bd show <id>                      # Detailed issue view
```

### Creating Work

```bash
# Create epic
bd create --title="Scenario Lifecycle" --type=epic --priority=0

# Create task within epic context
bd create --title="Implement scenario_create" --type=task --priority=1

# Add dependency (task depends on another)
bd dep add <task-id> <depends-on-id>
```

### Updating Status

```bash
bd update <id> --status=in_progress    # Claim work
bd close <id>                          # Mark complete
bd close <id1> <id2> ...               # Close multiple
```

### Sync with Git

```bash
bd sync                           # Commit beads changes and push
bd sync --status                  # Check sync status
```

---

## Session Protocol

### Starting a Session

```bash
# 1. Check available work
bd ready

# 2. Review current epic status
bd list --status=in_progress

# 3. Verify build works
GEN=ninja make && make test
```

### Landing the Plane (Session Completion)

**MANDATORY WORKFLOW - Work is NOT complete until `git push` succeeds:**

```bash
# 1. File issues for remaining work
bd create --title="..." --type=task

# 2. Run quality gates (if code changed)
GEN=ninja make && make test

# 3. Update issue status
bd close <completed-ids>

# 4. PUSH TO REMOTE
git pull --rebase
bd sync
git push
git status  # MUST show "up to date with origin"

# 5. Hand off context
# Summarize: what was done, what's next, any blockers
```

**Critical Rules:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- If push fails, resolve and retry until it succeeds
- NEVER add Co-Authored-By or AI attribution lines to commits

---

## DuckDB Extension Development Patterns

### Function Registration (Modern API)

```cpp
// In LoadInternal():
ExtensionLoader::RegisterFunction(loader, ScalarFunction(
    "scenario_create",                              // Function name
    {LogicalType::VARCHAR, LogicalType::VARCHAR},   // Parameters
    LogicalType::BOOLEAN,                           // Return type
    ScenarioCreateFunction                          // Implementation
));
```

### Table Functions

```cpp
// For scenario_list():
ExtensionLoader::RegisterFunction(loader, TableFunction(
    "scenario_list",
    {},                                  // No parameters
    ScenarioListBind,                    // Bind function
    ScenarioListInit,                    // Init function
    ScenarioListScan                     // Scan function
));
```

### OptimizerExtension for Write Interception

```cpp
class ScenarioOptimizerExtension : public OptimizerExtension {
public:
    ScenarioOptimizerExtension() {
        pre_optimize_function = InterceptScenarioWrites;
    }

    static void InterceptScenarioWrites(OptimizerExtensionInput &input,
                                        unique_ptr<LogicalOperator> &plan) {
        // Transform INSERT/UPDATE/DELETE to scenario schemas
        // into delta table operations
    }
};
```

### Error Handling

```cpp
// Use DuckDB exception types for clear errors
throw InvalidInputException("Scenario '%s' not found", scenario_name);
throw ConstraintException("Cannot drop scenario '%s': other scenarios depend on it", name);
throw NotImplementedException("DDL operations not permitted in scenarios");
```

---

## Metadata Schema Reference

From design doc, internal tables in `main` schema:

```sql
-- Scenario registry
CREATE TABLE _scenario_registry (
    scenario_id INTEGER PRIMARY KEY,
    scenario_name VARCHAR NOT NULL UNIQUE,
    schema_name VARCHAR NOT NULL UNIQUE,
    base_schema VARCHAR NOT NULL,
    base_captured_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP DEFAULT current_timestamp,
    status VARCHAR DEFAULT 'active',
    description VARCHAR,
    parent_scenario_id INTEGER
);

-- Table registration per scenario
CREATE TABLE _scenario_tables (
    scenario_id INTEGER,
    table_name VARCHAR NOT NULL,
    base_row_count BIGINT,
    has_primary_key BOOLEAN,
    primary_key_columns VARCHAR[],
    PRIMARY KEY (scenario_id, table_name)
);

-- Protocol storage
CREATE TABLE _scenario_protocols (
    entity_type VARCHAR NOT NULL,
    entity_name VARCHAR NOT NULL,
    section VARCHAR NOT NULL,
    content VARCHAR,
    updated_at TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (entity_type, entity_name, section)
);
```

---

## Getting Unstuck

### After 3+ Failed Fix Attempts

1. **Gather context:** Error messages, relevant code, what you tried
2. **Check logging:** Enable debug logs and analyze output
3. **Consult references:**
   - `docs/features/requirements.md` - What should happen
   - `docs/features/design.md` - How it should work
   - DuckDB source: `duckdb/src/include/duckdb/`

### Common Issues

| Problem | Solution |
|---------|----------|
| Extension won't load | Check `DUCKDB_CPP_EXTENSION_ENTRY` macro |
| Function not found | Verify `ExtensionLoader::RegisterFunction` called |
| Test fails unexpectedly | Check type indicators match actual return types |
| Linker errors | Verify dependencies in CMakeLists.txt |
| Rowid instability | Use PK-based identification, validate on read |

---

## References

- **Requirements:** `docs/features/requirements.md`
- **Design:** `docs/features/design.md`
- **DuckDB Extension Template:** https://github.com/duckdb/extension-template
- **DuckDB Docs:** https://duckdb.org/docs/
- **SQLLogicTest Guide:** https://duckdb.org/docs/stable/dev/sqllogictest/intro
