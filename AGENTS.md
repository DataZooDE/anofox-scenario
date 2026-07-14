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
- **Documentation Tasks:** Every implementation task requires companion doc tasks (API_REFERENCE.md, docs/spec/)

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

# 8. Update docs/API_REFERENCE.md if user-facing function
# Document: function name, parameters, return type, examples

# 9. Update docs/spec/ if architecture changed
# Keep architecture documentation in sync with implementation

# 10. Close task
bd close <task-id>

# 11. Sync beads
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
  anofox_scenario_extension.cpp        # Entry point, registration
  include/anofox_scenario_extension.hpp
  catalog/                             # the ATTACH-based scenario catalog (v2)
    scenario_storage_extension.cpp     #   ATTACH (TYPE scenario) + BaseSource resolution
    scenario_catalog.cpp               #   Catalog impl, MarkHostWrite, freeze chokepoint
    scenario_schema_entry.cpp          #   synthetic main schema, DDL rejection
    scenario_table_entry.cpp           #   base mirror, virtual identity columns
    scenario_scan.cpp                  #   merge-on-read scan (delta first + suppression)
    scenario_insert.cpp                #   insert sink + PK collision policy
    scenario_update_delete.cpp         #   update/delete sinks (op-transition matrix)
    scenario_delta.cpp                 #   delta/mat table management + copy helpers
    scenario_registry.cpp              #   registry v2 (caller-transaction catalog ops)
    scenario_transaction.cpp           #   transaction shim (ACID rides the host)
    include under src/include/catalog/
  lifecycle/
    scenario_lifecycle.cpp             #   CALL create/drop/freeze/unfreeze + scenario_list
    scenario_diff.cpp                  #   scenario_diff/_summary (bind_replace, streaming)
    scenario_migrate.cpp               #   legacy v0.1 -> v2 migration
```

### Test Structure

```
test/sql/
  attach_basic.test               # ATTACH/DETACH, passthrough reads, lifecycle
  attach_write.test               # op-transition matrix (I/U/D transitions)
  attach_isolation.test           # base never written; scenarios independent
  attach_constraints.test         # NOT NULL/CHECK/PK vs merged state; no-PK limits
  attach_ddl.test                 # canonical DDL rejection
  attach_txn.test                 # caller-transaction semantics (ROLLBACK/COMMIT)
  attach_freeze.test              # freeze/unfreeze enforcement
  attach_hardening.test           # re-attach, double attach, introspection
  attach_persistence.test         # durability across restart
  attach_concurrent.test          # write-write conflicts across connections
  attach_perf.test                # 1M-row merge-scan smoke
  attach_unsupported.test         # gated features error with guidance
  scenario_model_v2.test          # materialized mode, branching, scenario_list
  scenario_diff_v2.test           # streaming diff engine (2- and 3-arg)
  scenario_migrate.test           # legacy v0.1 -> v2 migration
```

### Configuration Files

```
CMakeLists.txt                    # Build config (add source files here)
extension_config.cmake            # Extension registration
vcpkg.json                        # Dependencies (currently OpenSSL)
Makefile                          # Build orchestration
```

### Documentation Structure

```
docs/
  API_REFERENCE.md                # User-facing SQL API (MUST update per function)
  spec/                           # Architecture docs (MUST consult before implementing)
  features/requirements.md        # Requirements specification
  features/design.md              # High-level design
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

Before creating tasks, consult `docs/spec/` for architecture guidance. Each epic should include documentation tasks for `docs/API_REFERENCE.md` (user-facing API) and `docs/spec/` (architecture updates).

```bash
# Create epic
bd create --title="Scenario Lifecycle" --type=epic --priority=0

# Create task within epic context
bd create --title="Implement scenario_create" --type=task --priority=1

# Create companion documentation task
bd create --title="Document scenario_create in API_REFERENCE.md" --type=task --priority=2

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

All state lives in the host database under the internal `__anofox_scenario` schema
(created lazily by the first `scenario_create`, in the caller's transaction):

```sql
-- Registry v2 (reserved columns land in later phases)
__anofox_scenario.registry(
    scenario_id BIGINT PRIMARY KEY,      -- from __anofox_scenario.registry_seq
    name VARCHAR NOT NULL UNIQUE,
    mode VARCHAR NOT NULL,               -- 'delta' | 'materialized'
    frozen BOOLEAN NOT NULL,
    parent_id BIGINT,                    -- branches
    base_snapshot_id BIGINT,             -- Phase 4: DuckLake bases
    created_at TIMESTAMP NOT NULL,
    merged_at TIMESTAMP,                 -- Phase 6: merge-back
    description VARCHAR)

-- One delta table per (scenario, base table); the changelog contract
__anofox_scenario.s<id>_delta_<table>(_op VARCHAR, _ts TIMESTAMP, <base columns>,
                                      PRIMARY KEY (<base pk>))

-- Materialized base copies (mode = 'materialized')
__anofox_scenario.s<id>_mat_<table>     -- full schema + data copy of the base table
```

## Getting Unstuck

### After 3+ Failed Fix Attempts

1. **Gather context:** Error messages, relevant code, what you tried
2. **Check logging:** Enable debug logs and analyze output
3. **Consult references:**
   - `docs/spec/` - Architecture documentation (consult before implementation)
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

- **Architecture:** `docs/spec/` - Consult before implementation decisions
- **API Reference:** `docs/API_REFERENCE.md` - Update per user-facing function
- **Requirements:** `docs/features/requirements.md`
- **Design:** `docs/features/design.md`
- **DuckDB Extension Template:** https://github.com/duckdb/extension-template
- **DuckDB Docs:** https://duckdb.org/docs/
- **SQLLogicTest Guide:** https://duckdb.org/docs/stable/dev/sqllogictest/intro
