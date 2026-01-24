**anofox-scenario**  
DuckDB Extension

**Requirements Specification**

Version 0.1-DRAFT  
January 2026

DataZoo GmbH

# **1\. Overview**

## **1.1 Purpose**

anofox-scenario is a DuckDB extension that enables Git-like branching for analytical databases. It allows users and AI agents to create isolated scenarios for what-if analysis, compare scenarios against baselines, capture immutable snapshots, and maintain lightweight documentation of analytical decisions.

## **1.2 Problem Statement**

Enterprise planning processes (S\&OP, demand planning, risk assessment) require exploring alternative futures without corrupting production data. Current approaches either:

* Copy entire databases (expensive, slow, hard to diff)

* Use application-layer versioning (loses SQL ergonomics)

* Rely on manual discipline (error-prone, no audit trail)

## **1.3 MVP Scope**

**In Scope:**

* Scenario lifecycle (create, branch, list, stats, archive, drop)

* Copy-on-write storage with transparent SQL operations

* Scenario comparison (diff against baseline or other scenarios)

* Snapshot management (immutable point-in-time captures)

* Protocol management (embedded table storage, explicit logging)

**Explicitly Deferred:**

* Two-step merge protocol (prepare/confirm promotion to main)

* SLM sub-agent integration (separate anofox-agent extension)

* Multi-database scenarios (scenarios span single database only)

* Conflict resolution (no concurrent scenario edits in MVP)

## **1.4 Success Criteria**

| Metric | Target |
| :---- | :---- |
| Scenario creation time | \< 1 second for databases up to 10GB |
| Storage overhead | \< 5% of base table size for scenarios with \<1% row changes |
| Protocol reliability | Warning on failure, DML succeeds |

# **2\. User Personas & Use Cases**

## **2.1 Personas**

### **Persona A: Recursive Tabular Agent (RTA)**

* Automated AI agent executing analytical workflows

* Creates scenarios programmatically based on user queries

* Needs a deterministic, SQL-native interface

* Maintains protocol files as an audit trail

* High volume: may create/drop dozens of scenarios per session

### **Persona B: Human Analyst**

* Demand planner, procurement analyst, S\&OP coordinator

* Uses DuckDB CLI or SQL client directly

* Needs intuitive SQL ergonomics (write to a scenario like any table)

* Values clear diff output for stakeholder communication

* Lower volume: 2-5 scenarios per analysis session

## **2.2 Primary Use Cases**

### **UC-01: Single-Parameter What-If**

"What happens to coverage if demand increases 10%?"

Flow: Create scenario from main, UPDATE forecast with adjustment, query coverage metrics, compare vs. main, document findings, archive scenario.

### **UC-02: Supplier Risk Assessment**

"What's our exposure if Chemtura fails to deliver starting February?"

Flow: Create a scenario with business context, zero out the supply\_plan for the affected supplier, recalculate coverage, compare the impact vs. the baseline, and document findings and recommendations.

### **UC-03: Pre-Meeting Baseline Capture**

"Freeze current state before S\&OP meeting for later comparison."

Flow: Create a snapshot with a description, conduct a meeting, and later compare the current main vs. the snapshot, create a scenario from the snapshot to explore alternatives.

### **UC-04: Iterative Analysis with Branching**

"I want to try two different approaches from the same starting point."

Flow: Create scenario A from main, make changes, branch scenario B from A, continue different changes in A and B, compare A vs. B vs. main.

# **3\. Functional Requirements**

## **3.1 Scenario Lifecycle Management**

### **REQ-SCEN-001: Create Scenario**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** The system shall create a new named scenario as an isolated branch from a specified base schema, capturing the base table state at creation time.

**Acceptance Criteria:**

* Scenario name must be valid SQL identifier (alphanumeric \+ underscore, max 63 chars)

* Scenario name (with configured prefix) must not collide with existing schemas

* Base schema defaults to 'main' if not specified

* Creation completes in \< 1 second for databases up to 10GB

* Only user tables in base schema available (no views, no system tables)

* Tables added to base after scenario creation are not visible in scenario

* No data is copied at creation time (lazy COW)

* Scenario captures base table state at creation time — subsequent base modifications do not affect scenario reads

### **REQ-SCEN-002: Branch Scenario**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall create a new scenario branched from an existing scenario, inheriting all modifications made in the source scenario.

**Acceptance Criteria:**

* Source must be an existing scenario (not 'main')

* New scenario sees source's modifications as its baseline

* Further changes in source do not affect the branch

* Further changes in branch do not affect source

* Branch operation completes in \< 1 second

### **REQ-SCEN-003: List Scenarios**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall provide a table-valued function returning metadata about all scenarios in the database.

**Acceptance Criteria:**

* Returns: name, schema\_name, base\_schema, base\_captured\_at, created\_at, status, description

* Status values: 'active', 'archived'

* Excludes dropped scenarios

* Ordered by created\_at descending by default

### **REQ-SCEN-004: Scenario Statistics**

| Category | Functional |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** The system shall provide statistics about modifications within a scenario.

**Acceptance Criteria:**

* Returns per-table: rows\_inserted, rows\_updated, rows\_deleted, delta\_size\_bytes

* Returns totals across all tables

* Reflects current state (not historical)

* Includes base\_captured\_at timestamp

### **REQ-SCEN-005: Archive Scenario**

| Category | Functional |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** The system shall mark a scenario as archived, preventing further modifications while preserving data for reference.

**Acceptance Criteria:**

* Archived scenarios remain queryable (SELECT works)

* INSERT/UPDATE/DELETE on archived scenarios fails with clear error

* Archive operation updates protocol with final status

* Archive is reversible (can un-archive)

### **REQ-SCEN-006: Drop Scenario**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall permanently delete a scenario and its associated storage.

**Acceptance Criteria:**

* Delta tables are deleted

* Schema is dropped

* Protocol data is moved to archive section (not deleted)

* Operation fails if other scenarios are branched from this one (dependency tree enforced)

### **REQ-SCEN-007: Unarchive Scenario**

| Category | Functional |
| :---- | :---- |
| Priority | Low |

**Requirement Statement:** The system shall restore an archived scenario to active status.

**Acceptance Criteria:**

* Status changes from 'archived' to 'active'

* DML operations become permitted again

### **REQ-SCEN-008: Schema Name Prefix Configuration**

| Category | Functional |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** The system shall support a configurable prefix for scenario schema names to avoid collisions with existing schemas.

**Acceptance Criteria:**

* SET scenario\_schema\_prefix \= '\_scen\_' creates scenarios as \_scen\_\<name\>

* Default prefix is empty string (no prefix)

* Prefix persists for session

* scenario\_list() returns logical name and physical schema name

* All API functions accept logical name; system resolves to physical name

## **3.2 Copy-on-Write Storage & Transparent SQL**

### **REQ-COW-001: Transparent Read**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** Queries against scenario tables shall transparently combine base data (as captured at scenario creation) with scenario modifications, returning the logically correct result.

**Acceptance Criteria:**

* SELECT \* FROM scenario.table returns base rows \+ inserts \- deletes, with updates applied

* Base data reflects state at scenario creation, not current main state

* All DuckDB query features work (JOINs, aggregations, window functions, CTEs)

* No special syntax required for reading

### **REQ-COW-002: Transparent Write \- INSERT**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** INSERT statements against scenario tables shall store new rows in the scenario's delta storage without modifying the base table.

**Acceptance Criteria:**

* Standard INSERT syntax works: INSERT INTO scenario.table VALUES (...)

* INSERT ... SELECT works

* Bulk inserts work

* Primary key violations detected (conflicts with base OR delta)

* Base table remains unchanged

### **REQ-COW-003: Transparent Write \- UPDATE**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** UPDATE statements against scenario tables shall record modifications in delta storage without modifying the base table.

**Acceptance Criteria:**

* Standard UPDATE syntax works: UPDATE scenario.table SET col \= val WHERE ...

* Updates to already-modified rows overwrite previous delta

* Updates to base rows create new delta entries

* WHERE clause operates on logical view (base \+ deltas)

### **REQ-COW-004: Transparent Write \- DELETE**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** DELETE statements against scenario tables shall mark rows as deleted in delta storage without modifying the base table.

**Acceptance Criteria:**

* Standard DELETE syntax works: DELETE FROM scenario.table WHERE ...

* Deleting a base row records deletion marker

* Deleting an inserted row removes it from delta

* WHERE clause operates on logical view

### **REQ-COW-005: Schema Activation**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall support setting an active scenario schema to simplify SQL syntax.

**Acceptance Criteria:**

* SET SCHEMA scenario\_name makes scenario the default schema

* Unqualified table references resolve to scenario first, then main

* SET SCHEMA main returns to default behavior

* Session-scoped (does not persist)

### **REQ-COW-006: Constraint Inheritance**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** Scenario modifications shall enforce the same constraints defined on base tables, with foreign key checks operating on logical views.

**Acceptance Criteria:**

* CHECK constraints from base table enforced on scenario writes

* NOT NULL constraints enforced

* PRIMARY KEY uniqueness enforced across logical view (base \+ delta)

* FOREIGN KEY constraints enforced against logical view of referenced table

* Constraint violation rolls back the statement (not the scenario)

**Technical Note:** FK validation requires constructing logical view of referenced table for each FK check. This may have performance implications for high-volume inserts.

### **REQ-COW-007: Supported Operations**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** Scenarios shall support DML operations only. DDL operations and schema modifications are not permitted.

**Acceptance Criteria:**

* Supported: SELECT, INSERT, UPDATE, DELETE

* TRUNCATE treated as DELETE FROM table (logged as delete of all rows)

* Not supported with clear error: CREATE TABLE/VIEW/INDEX, ALTER TABLE, DROP TABLE, any other DDL

* Error message: "DDL operations not permitted in scenarios. Modify base schema first, then create new scenario."

**Rationale:** DDL in scenarios creates schema divergence that complicates comparison, merge (future), and mental model. MVP enforces structural consistency.

### **REQ-COW-008: Primary Key Handling**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall use the table's primary key for row identity in COW operations. If no primary key exists, all columns together form the composite key.

**Acceptance Criteria:**

* Tables with explicit PK: use PK for row identity and diff operations

* Tables without PK: all columns treated as composite key

* Warning raised at scenario creation if table lacks PK

* Tables with duplicate rows: behavior for UPDATE/DELETE targeting duplicates is implementation-defined (document it)

## **3.3 Scenario Comparison**

### **REQ-COMP-001: Compare Two Schemas**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** The system shall compare a specific table between two schemas, returning row-level differences in a structured format.

**Output Schema (one row per changed column per row):**

`diff_type VARCHAR,      -- 'added', 'removed', 'changed'`

`<pk_column_1> <type>,   -- Primary key column(s)`

`column_name VARCHAR,    -- Name of changed column`

`old_value VARCHAR,      -- Value in schema_a`

`new_value VARCHAR       -- Value in schema_b`

**Acceptance Criteria:**

* For 'added' rows: one row per PK, column\_name \= NULL

* For 'removed' rows: one row per PK, column\_name \= NULL

* For 'changed' rows: one row per changed column with old/new values

* 'unchanged' rows excluded from output

* Handles compound primary keys

### **REQ-COMP-002: Compare All Tables**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall provide a summary comparison across all tables between two schemas.

**Acceptance Criteria:**

* Returns per-table: added\_rows, removed\_rows, changed\_rows, unchanged\_rows

* Only includes tables with differences (or optionally all tables)

* Executes efficiently (does not materialize full diffs)

### **REQ-COMP-003: Compare to Branch Origin**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** Scenario comparisons shall default to comparing against the branch origin (base state at creation), not current state of the source schema.

**Acceptance Criteria:**

* scenario\_compare('optimistic', 'forecast') (single schema) compares to branch origin

* scenario\_compare('main', 'optimistic', 'forecast') explicitly compares two schemas as-of-now

* For scenario branched from snapshot: comparison against snapshot state

* Branch origin metadata stored with scenario

**Rationale:** Comparing to branch origin answers "what did I change in this scenario?" — the default mental model.

### **REQ-COMP-004: Scenario-to-Scenario Comparison**

| Category | Functional |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** When comparing two scenarios, the comparison uses current state of both scenarios, not historical branch points.

**Acceptance Criteria:**

* scenario\_compare('scenario\_a', 'scenario\_b', 'table') compares current A vs current B

* Even if B was branched from A, comparison sees A's current deltas

* This differs from single-argument compare which uses origin

## **3.4 Snapshot Management**

### **REQ-SNAP-001: Create Snapshot**

| Category | Functional |
| :---- | :---- |
| Priority | Critical |

**Requirement Statement:** The system shall create a point-in-time copy of a schema's data, immutable after creation.

**Acceptance Criteria:**

* Snapshot name must be unique

* Source defaults to 'main'

* Snapshot is read-only (all writes fail with clear error)

* Snapshot captures tables in rapid succession; for guaranteed consistency, caller should ensure no concurrent writes during snapshot creation

* Storage uses COW where possible (space-efficient)

* Protocol entry created with timestamp, source, description

**Rationale:** True atomic snapshots require either global locking or MVCC snapshots (not available in DuckDB). Best-effort with documented limitation is pragmatic for MVP.

### **REQ-SNAP-002: List Snapshots**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall provide a table-valued function returning metadata about all snapshots.

**Acceptance Criteria:**

* Returns: snapshot\_name, source\_schema, created\_at, description, size\_bytes

* Ordered by created\_at descending

### **REQ-SNAP-003: Compare Snapshot**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall compare current state of a table against a snapshot.

**Acceptance Criteria:**

* Same output format as scenario\_compare

* Works for any table in the snapshot

### **REQ-SNAP-004: Create Scenario from Snapshot**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall create a new scenario using a snapshot as its base.

**Acceptance Criteria:**

* New scenario sees snapshot data as baseline (not current main)

* Modifications in scenario do not affect snapshot

* Protocol references source snapshot

* Comparisons default to snapshot (the branch origin)

### **REQ-SNAP-005: Drop Snapshot**

| Category | Functional |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** The system shall permanently delete a snapshot and its storage.

**Acceptance Criteria:**

* Fails if any scenario is based on this snapshot (dependency tree enforced)

* Protocol data moved to archive

## **3.5 Protocol Management**

### **REQ-PROT-001: Protocol Storage**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** Protocol data shall be stored in an embedded database table.

**Acceptance Criteria:**

* Protocols stored in internal table \_scenario\_protocols

* Schema: (entity\_type, entity\_name, section, content, updated\_at)

* entity\_type: 'scenario' | 'snapshot'

* section: 'why' | 'changes' | 'findings' | 'plan' | 'decision' | 'metadata'

* Protocol data included when database file is copied (portability)

* Protocol data participates in database transactions

**Rationale:** Embedded storage ensures portability and transactional consistency. Single storage mechanism simplifies MVP.

### **REQ-PROT-002: Change Logging**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall provide explicit functions for logging changes to protocol; automatic logging is not performed.

**Acceptance Criteria:**

* CALL protocol\_log\_change(scenario, table\_name, operation, row\_count, description) appends to Changes section

* No automatic interception of DML

* Agent or user responsible for calling after DML

* If protocol\_log\_change is not called, Changes section remains empty

**Rationale:** Automatic logging requires invasive DML interception and complicates transaction handling. Explicit logging gives agent/user control.

### **REQ-PROT-003: Manual Protocol Sections**

| Category | Functional |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** The system shall provide functions to update Why, Findings, Plan, and Decision sections of protocol.

**Acceptance Criteria:**

* protocol\_set\_why('scenario', 'text') sets Why section

* protocol\_add\_finding('scenario', 'text') appends to Findings

* protocol\_set\_plan('scenario', 'markdown checklist') sets Plan section

* protocol\_set\_decision('scenario', 'text') sets Decision section

* Sections accept multi-line markdown text

* Set functions overwrite; add functions append

### **REQ-PROT-004: Protocol Reading**

| Category | Functional |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** The system shall provide functions to read protocol content programmatically.

**Acceptance Criteria:**

* SELECT \* FROM protocol\_read('scenario') returns structured protocol content

* Returns: section\_name, content, updated\_at

### **REQ-PROT-005: Protocol Export**

| Category | Functional |
| :---- | :---- |
| Priority | Low |

**Requirement Statement:** The system shall provide a convenience function to export protocol as markdown file.

**Acceptance Criteria:**

* CALL protocol\_export\_markdown('scenario', '/path/file.md') writes formatted markdown

* Uses DuckDB filesystem abstraction

* Includes all sections in standard template format

# **4\. Non-Functional Requirements**

### **REQ-NFR-001: Storage Efficiency**

| Category | Non-Functional (Performance) |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** Scenario storage overhead shall be proportional to modifications, not base table size.

**Acceptance Criteria:**

* Scenario with 0 modifications: \< 1KB overhead

* Scenario with N modified rows: storage ≈ N × average\_row\_size × 1.2

* No full table copies at any point in scenario lifecycle

### **REQ-NFR-003: Concurrent Access**

| Category | Non-Functional (Reliability) |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** The system shall handle concurrent read access to scenarios safely.

**Acceptance Criteria:**

* Multiple sessions can read same scenario concurrently

* Write access: single-writer per scenario (locking or advisory)

### **REQ-NFR-004: Base Table Mutation Handling**

| Category | Non-Functional (Reliability) |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** Scenarios shall operate on snapshot isolation: base table state is captured at scenario creation, and subsequent base modifications do not affect scenario reads.

**Acceptance Criteria:**

* Scenario creation captures logical "version" of each base table

* Reads in scenario see: base-as-captured \+ scenario deltas

* Base table INSERT/UPDATE/DELETE after scenario creation: allowed, no effect on scenario

* Base table DDL after scenario creation: scenario continues to see captured schema

* No locking of base tables due to scenario existence

* scenario\_stats() includes base\_captured\_at timestamp

**Rationale:** Strict locking is impractical for multi-user environments. Snapshot isolation provides intuitive "branch from a point in time" semantics matching git mental model.

### **REQ-NFR-005: Error Messages**

| Category | Non-Functional (Usability) |
| :---- | :---- |
| Priority | High |

**Requirement Statement:** Error messages shall clearly indicate scenario-specific context and remediation.

**Acceptance Criteria:**

* Errors include scenario name when relevant

* Constraint violations indicate whether conflict is with base or delta

* Protocol write failures include location and suggest remediation

### **REQ-NFR-006: Database Portability**

| Category | Non-Functional (Reliability) |
| :---- | :---- |
| Priority | Medium |

**Requirement Statement:** Scenarios, snapshots, and protocols shall be fully contained within the database file.

**Acceptance Criteria:**

* Copying database.duckdb to new location preserves all scenarios

* All protocol data travels with database (embedded storage)

* No external file dependencies

* Scenario functionality works immediately after copy without configuration

### **REQ-NFR-007: No Size Limits**

| Category | Non-Functional (Scalability) |
| :---- | :---- |
| Priority | Low |

**Requirement Statement:** The system shall not impose artificial size limits on tables participating in scenarios.

**Acceptance Criteria:**

* No row count limits

* No table size limits

* Performance degrades gracefully with scale

* Documentation includes guidance for large tables (\>10M rows with \>5% modification rate)

# **5\. API Reference**

## **5.1 Scenario Lifecycle**

`CALL scenario_create(name VARCHAR, description VARCHAR,`

                     `base_schema VARCHAR DEFAULT 'main')`

`CALL scenario_branch(source VARCHAR, new_name VARCHAR, description VARCHAR)`

`SELECT * FROM scenario_list()`

`SELECT * FROM scenario_stats(scenario_name VARCHAR)`

`CALL scenario_archive(scenario_name VARCHAR)`

`CALL scenario_unarchive(scenario_name VARCHAR)`

`CALL scenario_drop(scenario_name VARCHAR)`

## **5.2 Configuration**

`SET scenario_schema_prefix = ''  -- Default empty`

`SET SCHEMA scenario_name`

`SET SCHEMA main`

## **5.3 Comparison**

`-- Compare to branch origin (single-arg)`

`SELECT * FROM scenario_compare(scenario VARCHAR, table_name VARCHAR)`

`-- Explicit two-schema comparison`

`SELECT * FROM scenario_compare(schema_a VARCHAR, schema_b VARCHAR,`

                               `table_name VARCHAR)`

`-- Summary across all tables`

`SELECT * FROM scenario_compare_all(scenario VARCHAR)`

`SELECT * FROM scenario_compare_all(schema_a VARCHAR, schema_b VARCHAR)`

## **5.4 Snapshots**

`CALL snapshot_create(name VARCHAR, description VARCHAR,`

                     `source_schema VARCHAR DEFAULT 'main')`

`SELECT * FROM snapshot_list()`

`SELECT * FROM snapshot_compare(snapshot_name VARCHAR, table_name VARCHAR)`

`CALL scenario_from_snapshot(snapshot VARCHAR, scenario_name VARCHAR,`

                            `description VARCHAR)`

`CALL snapshot_drop(snapshot_name VARCHAR)`

## **5.5 Protocol Management**

`CALL protocol_set_why(entity_name VARCHAR, content VARCHAR)`

`CALL protocol_log_change(entity_name VARCHAR, table_name VARCHAR,`

                         `operation VARCHAR, row_count INT,`

                         `description VARCHAR)`

`CALL protocol_add_finding(entity_name VARCHAR, content VARCHAR)`

`CALL protocol_set_plan(entity_name VARCHAR, content VARCHAR)`

`CALL protocol_set_decision(entity_name VARCHAR, content VARCHAR)`

`SELECT * FROM protocol_read(entity_name VARCHAR)`

`CALL protocol_export_markdown(entity_name VARCHAR, filepath VARCHAR)`

# **6\. Deferred Items**

The following items are explicitly out of scope for MVP and targeted for future releases:

| Item | Rationale | Target |
| :---- | :---- | :---- |
| Two-step merge | Complexity; MVP focuses on analysis, not promotion | v0.2 |
| Concurrent scenario writes | Requires conflict resolution strategy | v0.2 |
| Cross-database scenarios | Requires federation; unclear use case priority | v0.3+ |
| Protocol templates | Nice-to-have; standard template sufficient | v0.2 |
| Scenario permissions | Enterprise feature (row-level security) | v0.3+ |

# **7\. Decisions Log**

Key design decisions made during requirements development:

| \# | Decision | Rationale |
| :---- | :---- | :---- |
| 1 | No DDL in scenarios | Structural consistency; avoids schema divergence |
| 2 | FK checks use logical views | Correctness over simplicity; document perf implications |
| 3 | Snapshot atomicity relaxed | DuckDB limitation; document caller responsibility |
| 4 | No automatic change logging | Simplicity; agent controls what's meaningful |
| 5 | Snapshot isolation for base mutations | Git-like mental model; no blocking |
| 6 | Dependency tree for drops | Explicit relationship tracking; prevents orphans |
| 7 | Flat output schema (no JSON) | SQL-native; easier to query/aggregate |
| 8 | Embedded protocol storage only | Portability; transactional consistency |
| 9 | Tables only (no views) | Simplicity; views can be recreated |
| 10 | Configurable prefix | Collision avoidance without mandating convention |
| 11 | Default comparison to origin | Answers "what did I change?" naturally |
| 12 | No PK → all columns as key | Pragmatic; don't exclude real tables |
| 13 | No size limits | Graceful degradation over hard failures |
| 14 | Scenario-to-scenario \= current vs current | "How do they differ now?" is the useful question |

# **Appendix A: Glossary**

| Term | Definition |
| :---- | :---- |
| Scenario | An isolated branch of the database for what-if analysis |
| Snapshot | An immutable point-in-time capture of schema state |
| COW | Copy-on-Write: storage pattern where modifications are stored as deltas |
| Delta table | Internal table storing scenario modifications (inserts, updates, deletes) |
| Logical view | The combined result of base table \+ delta modifications |
| Branch origin | The state of the base at the time a scenario was created |
| Protocol | Structured documentation of scenario purpose, changes, findings, and decisions |
| RTA | Recursive Tabular Agent: AI agent executing analytical workflows |
| S\&OP | Sales and Operations Planning |

# **Appendix B: Requirements Traceability**

Requirements are organised by category with the following ID scheme:

| Prefix | Category | Count |
| :---- | :---- | :---- |
| REQ-SCEN- | Scenario Lifecycle | 8 requirements |
| REQ-COW- | Copy-on-Write Storage | 8 requirements |
| REQ-COMP- | Comparison | 4 requirements |
| REQ-SNAP- | Snapshot Management | 5 requirements |
| REQ-PROT- | Protocol Management | 5 requirements |
| REQ-NFR- | Non-Functional | 6 requirements |

Total: 36 requirements

