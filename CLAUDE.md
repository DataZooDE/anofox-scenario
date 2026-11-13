# Agent Instructions: anofox-scenario

**DuckDB Extension for Git-Like Database Branching**

This document guides AI agents in developing the anofox-scenario extension using incremental TDD, with GitHub issues for work tracking and git worktrees for parallel development.

---

## Project Overview

**anofox-scenario** enables Git-like branching for analytical databases. Users create isolated scenarios for what-if analysis, compare scenarios against baselines, and maintain audit trails.

### Core Components

| Component | Responsibility |
|-----------|---------------|
| **Scenario catalog** | `ATTACH (TYPE scenario)`, synthetic schema, DDL rejection, freeze enforcement |
| **Merge-on-read scan** | Delta-first scan over the base, suppressing deleted keys |
| **Write sinks** | Insert/update/delete against the delta, op-transition matrix, merged-state constraints |
| **Delta storage** | Per-(scenario, table) delta tables and materialized base copies |
| **Comparison engine** | Streaming typed diffs and per-table summaries |
| **Merge-back** | Preview and apply a scenario's delta to the base with conflict policies |
| **Registry** | Scenario metadata in `__anofox_scenario`, in the caller's transaction |

See `docs/spec/architecture.md` for the full picture.

### Architecture Pattern

**Delta-Main Pattern:** Base data stays immutable (DuckDB columnar), scenario modifications stored in delta tables, merged on read.

```
ATTACH 'name' (TYPE scenario) --> StorageExtension "scenario" --> ScenarioCatalog
User SQL on alias.<table>     --> scenario table entry: merge-on-read scan over base + delta
                              --> insert/update/delete sinks write the delta tables
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

### Work Tracking

Work is tracked in GitHub issues. Every implementation change that touches the
user-facing surface needs a companion documentation update in the same PR —
`docs/API_REFERENCE.md` for the SQL API, `docs/spec/architecture.md` when the
internals change, and **the affected skill(s) under `plugins/anofox-scenario/skills/`**
(see "Claude Code Plugin" below). A PR that touches README.md, `docs/` or a skill must
pass `make check-docs`.

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

Track dependencies in the GitHub issue itself.

---

## TDD Workflow

### Per-Task Development Cycle

```bash
# 1. Write failing test (RED)
# Edit the test/sql/*.test file covering this concern,
# e.g. attach_write.test for write semantics

# 2. Run test to confirm it fails
GEN=ninja make && make test
# Expected: Test fails with clear error

# 3. Implement minimal code (GREEN)
# Edit the relevant file under src/catalog/ or src/lifecycle/

# 4. Run test to confirm it passes
GEN=ninja make && make test
# Expected: Test passes

# 5. Refactor while green
# Clean up code, add logging, improve error messages

# 6. Verify tests still pass
GEN=ninja make && make test

# 7. Update docs/API_REFERENCE.md if user-facing function
# Document: function name, parameters, return type, examples

# 8. Update docs/spec/architecture.md if internals changed
# Keep architecture documentation in sync with implementation
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
    scenario_internal_guard.cpp        #   rejects SQL writes/DDL on __anofox_scenario (#55)
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
  attach_mixed_txn.test           # host + scenario writes in one transaction (#31)
  attach_freeze.test              # freeze/unfreeze enforcement
  attach_hardening.test           # re-attach, double attach, introspection
  attach_persistence.test         # durability across restart
  attach_concurrent.test          # write-write conflicts across connections
  attach_perf.test                # 1M-row merge-scan smoke
  attach_unsupported.test         # gated features error with guidance
  scenario_model_v2.test          # materialized mode, branching, scenario_list
  scenario_diff_v2.test           # streaming diff engine (2- and 3-arg)
  scenario_migrate.test           # legacy v0.1 -> v2 migration
  attach_keyless_unique.test      # UNIQUE on keyless (bag) tables vs the merged relation (#27)
  attach_keyless_merge.test       # MERGE INTO / ON CONFLICT on keyless tables vs a plain-table oracle (#31)
  scenario_diff_keyless.test      # bag diff/summary on keyless tables (#27)
  attach_dotted_names.test        # table/schema names containing . (quote-aware logical names, #27)
  attach_insert_constraint_order.test # base PK probe vs constraint declaration order (#22)
  scenario_materialized_fk.test   # materialized copies over FK schemas; merge-back enforces FKs (#24)
  attach_lazy_delta.test          # deltas created by the first write; scenario_tables identity; races (#34)
  attach_lazy_delta_identity.test # row identity of lazy deltas: refresh/first-write races, base schema changes, key validation, diff names (#34)
  scenario_keyless_upgrade.test   # pre-v0.4.1 keyless deltas (no _count) upgraded in place (#31)
  scenario_ducklake_keyless_merge.test # keyless bag merge-back into DuckLake bases: exact copies, drift (#31)
  attach_delta_key_probe.test     # single-row DML over a 100k-row delta resolves keys via the delta PK index, vs an oracle (#36)
  attach_scan_suppression.test    # hashed base suppression, point lookups, keyless rowid filters after suppression (#39)
  attach_double_touch.test        # repeat touches: DELETE no-ops, UPDATE/MERGE "same row twice", keyed + keyless (#39)
  attach_virtual_columns.test     # __scenario_* virtual columns in user WHERE clauses; DELETE row-id positions (#39)
  attach_write_checks.test        # read-only / frozen / single-writer checks at EXECUTE, not PREPARE (#39)
  attach_internal_guard.test      # host SQL may read __anofox_scenario but not write it / change its schema; escape-hatch setting, IMPORT (#55)
  scenario_merge_keyless_drift.test # 3-way drift for keyless tables in materialized scenarios (#39)
  scenario_ducklake_merge_hardening.test # legacy deltas, rowid columns, net-plan preview, idempotent lake re-merge (#39)
  scenario_delta_drift.test       # delta scenarios: pre-images, drift conflicts at merge/preview, branches, legacy (#53)
  scenario_merge_fk_order.test    # merge applies parents first; in-place updates of referenced rows (#54)
test/cpp/                         # Catch tests linked into unittest (see test/README.md)
  test_prepared_insert.cpp        # client-API prepared INSERT across transactions (#22)
  test_internal_guard.cpp         # internal-schema guard on client-API prepared statements; connections open before LOAD (#55)
```

### Claude Code Plugin

```
.claude-plugin/marketplace.json                 # marketplace manifest (repo = marketplace)
plugins/anofox-scenario/.claude-plugin/plugin.json  # plugin manifest; version = CalVer release
plugins/anofox-scenario/skills/<skill>/SKILL.md # one skill per usage pattern
scripts/check_doc_snippets.py                   # `make check-docs`: runs every ```sql block
scripts/doc_snippets/                           #   fixtures, expected errors, synopses, allowed names
```

Skills are organised by **usage pattern**, not by API area: scenario's few functions are
used together in every task, so each pattern skill is a complete playbook and edge cases
live once in the reference skill.

| Skill | Covers |
|-------|--------|
| `anofox-scenario-what-if` | plan variants, branches, KPI / row comparisons, forecast per scenario |
| `anofox-scenario-safe-change` | dry run, diff, merge preview, merge + conflict policies, discard, DuckLake merge |
| `anofox-scenario-snapshot-audit` | materialized + frozen snapshots, DuckLake pinning, diff vs. approved, audit notes, migration |
| `anofox-scenario-reference` | signatures, row identity, constraints, transactions, concurrency, error -> cause -> fix |

Rules:
- A change to the SQL surface or to an error message updates the skill that teaches it.
  Error texts quoted in skills are pinned in `scripts/doc_snippets/expected_errors.txt`
  (third column), so rewording an error fails `make check-docs` until the docs follow.
- `make check-docs` needs a release build (`GEN=ninja make release`). It runs each page's
  blocks as one session (a block sees what earlier blocks created); start a fresh session
  with a first line `-- doc-snippets: new session`, or, where a visible comment would be
  noise (README), a `<!-- doc-snippets: new session -->` line before the fence. Tables
  headed `Error contains` are checked too: each quoted error text must exist in `src/` or
  `duckdb/src/`, and so are prose quotes like "fails with `...`". Blocks run sandboxed
  (file access only in a temp dir, locked configuration, no `.` dot-commands).
- Blocks needing DuckLake or another extension (`INSTALL x`, `ducklake:`, a first line
  `-- requires: <ext>`) are skipped by `make check-docs` and run by `make check-docs-all`
  and in CI (DuckLake integration job). Examples whose point is a result (conflict policy,
  diff direction, rollback) get an entry in `scripts/doc_snippets/expected_output.txt`;
  `--show-output FILE` prints block outputs to write one.
- `plugin.json`'s version must match every SKILL.md `version:` and banner (checked).
- On a release, bump `version` in `plugin.json` and in every SKILL.md frontmatter and banner
  to the new CalVer tag.
- In-repo testing: `claude --plugin-dir ./plugins/anofox-scenario`.

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
  spec/architecture.md            # Internal design (MUST consult before implementing)
  UPDATING.md                     # DuckDB version-bump procedure
```

The v0.1 requirements and design documents were removed once v2 shipped; the
implementation plus `spec/architecture.md` are now the source of truth.

---

## Issue Tracking

Work is tracked in GitHub issues via the `gh` CLI.

```bash
gh issue list                                  # Open issues
gh issue list --label bug                      # Filter by label
gh issue view <number>                         # Detailed view
gh issue create --title="..." --body="..."     # File new work
gh issue close <number>
```

Before filing implementation work, consult `docs/spec/architecture.md`. Any issue
that changes the user-facing SQL surface should call out the companion update to
`docs/API_REFERENCE.md` in its description.

---

## Session Protocol

### Starting a Session

```bash
# 1. Check available work
gh issue list

# 2. Verify build works
GEN=ninja make && make test
```

### Landing the Plane (Session Completion)

**MANDATORY WORKFLOW - Work is NOT complete until `git push` succeeds:**

```bash
# 1. File issues for remaining work
gh issue create --title="..." --body="..."

# 2. Run quality gates (if code changed)
GEN=ninja make && make test
make check-docs   # if README.md, docs/ or a skill changed (needs a release build)

# 3. Close completed issues
gh issue close <number>

# 4. PUSH TO REMOTE
git pull --rebase
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

All SQL verbs (`scenario_create`, `scenario_drop`, `scenario_merge`, ...) are table
functions, so they work with both `CALL` and `SELECT * FROM`. See
`src/lifecycle/scenario_lifecycle.cpp` (`ScenarioLifecycle::RegisterFunctions`):

```cpp
TableFunction fn("scenario_freeze", {LogicalType::VARCHAR}, Execute, Bind, Init);
CreateTableFunctionInfo info(std::move(fn));
loader.RegisterFunction(std::move(info));
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

### Storage Extension for the Scenario Catalog

Writes are not intercepted by an optimizer rule: `ATTACH ... (TYPE scenario)` goes through a
`StorageExtension` registered as `"scenario"` (`src/anofox_scenario_extension.cpp`,
`src/catalog/scenario_storage_extension.cpp`), which returns a `ScenarioCatalog`. Its table
entries plan the merge-on-read scan and the insert/update/delete sinks
(`src/catalog/scenario_*.cpp`).

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

-- Declared keys (key_columns :=), created lazily like the registry (#34)
__anofox_scenario.scenario_tables(scenario_id BIGINT, table_name VARCHAR, key_columns VARCHAR[],
                                  PRIMARY KEY (scenario_id, table_name))

-- One delta table per (scenario, base table), created by the table's FIRST WRITE
-- (not by scenario_create, #34); the changelog contract. Identity: scenario_tables
-- row, else the existing delta's PK, else the base PK, else keyless.
-- <table> is the logical name: plain for main, '<schema>.<table>' otherwise; a part
-- containing '.' is double-quoted ('"a.b"' = main table a.b).
__anofox_scenario.s<id>_delta_<table>(_op VARCHAR, _ts TIMESTAMP, <base columns>,
                                      PRIMARY KEY (<base pk or declared key_columns>))
-- Keyless tables (no PK, no declared key): bag changelog instead - no PK,
-- trailing _count BIGINT multiplicity, readers aggregate I/D nets per value

-- Materialized base copies (mode = 'materialized')
__anofox_scenario.s<id>_mat_<table>     -- schema (minus FOREIGN KEYs) + data copy of the base table

-- Pre-images (#53): keyed tables of non-materialized scenarios over the host; created with
-- the delta, one row per base row the scenario updated/deleted, as it was at that first
-- touch (the drift reference of merge-back); copied by branching, emptied by the merge
__anofox_scenario.s<id>_pre_<table>     -- <base columns>, PRIMARY KEY (<delta key>)
```

## Getting Unstuck

### After 3+ Failed Fix Attempts

1. **Gather context:** Error messages, relevant code, what you tried
2. **Check logging:** Enable debug logs and analyze output
3. **Consult references:**
   - `docs/spec/architecture.md` - Internal design: read/write paths, delta contract, tiers
   - `docs/API_REFERENCE.md` - Behaviour users are promised, plus current limitations
   - `test/sql/` - The executable specification; each file covers one concern
   - DuckDB source: `duckdb/src/include/duckdb/`

### Common Issues

| Problem | Solution |
|---------|----------|
| Extension won't load | Check `DUCKDB_CPP_EXTENSION_ENTRY` macro |
| Function not found | Verify `ExtensionLoader::RegisterFunction` called |
| Test fails unexpectedly | Check type indicators match actual return types |
| Linker errors | Verify dependencies in CMakeLists.txt |
| Keyless table needs a key | Table has no PK; declare `key_columns :=` at `scenario_create` (or `scenario_refresh` before its first write) |

---

## References

- **Architecture:** `docs/spec/architecture.md` - Consult before implementation decisions
- **API Reference:** `docs/API_REFERENCE.md` - Update per user-facing function
- **Updating DuckDB:** `docs/UPDATING.md`
- **DuckDB Extension Template:** https://github.com/duckdb/extension-template
- **DuckDB Docs:** https://duckdb.org/docs/
- **SQLLogicTest Guide:** https://duckdb.org/docs/stable/dev/sqllogictest/intro
