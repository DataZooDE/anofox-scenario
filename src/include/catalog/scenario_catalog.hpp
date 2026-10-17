//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_catalog.hpp
//
// A scenario exposed as an attached catalog: reads merge base + delta,
// writes are planned into delta storage, DDL is rejected. All physical
// state lives in the host catalog; this catalog is a projection over it.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "catalog/scenario_compat.hpp"

namespace duckdb {

class DuckTableEntry;
class ScenarioCatalog;
class ScenarioSchemaEntry;
class ViewCatalogEntry;

//! The one canonical error for any DDL attempted inside a scenario.
[[noreturn]] void ThrowScenarioDDLError();

//===----------------------------------------------------------------------===//
// Transaction shim
//===----------------------------------------------------------------------===//

//! All physical state (registry, delta tables) lives in the host catalog, so
//! ACID is provided by the host DuckTransactionManager inside the same
//! MetaTransaction. This transaction only carries the per-transaction table
//! entry cache.
class ScenarioTransaction : public Transaction {
public:
	ScenarioTransaction(TransactionManager &manager, ClientContext &context);

	//! Table entries materialized for this transaction (name -> entry).
	//! Entries wrap the base table as of this transaction; owning them here
	//! gives correct staleness (fresh per transaction) and lifetime (alive
	//! for as long as the transaction can reference them).
	case_insensitive_map_t<unique_ptr<CatalogEntry>> table_entries;
	//! Guards table_entries: one query can enumerate the catalog from several
	//! pipelines at once (e.g. SHOW ALL TABLES). Entries are never replaced
	//! once published - a reference handed out must stay valid.
	mutex table_entries_lock;
	//! Catalog-enumeration variants of the table entries (ScenarioSchemaEntry::
	//! Scan), kept apart so no logical name can collide with them
	case_insensitive_map_t<unique_ptr<CatalogEntry>> enumeration_entries;
	//! The cached entry for name, if any (enumeration: the Scan variant)
	optional_ptr<CatalogEntry> FindEntry(const string &name, bool enumeration = false);
	//! Publish entry under name unless another thread got there first; returns
	//! whichever entry is cached
	CatalogEntry &PublishEntry(const string &name, unique_ptr<CatalogEntry> entry, bool enumeration = false);
	//! Process-unique id of this transaction; feeds ScenarioCatalog::GetCatalogVersion
	idx_t serial;
};

class ScenarioTransactionManager : public TransactionManager {
public:
	explicit ScenarioTransactionManager(AttachedDatabase &db);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<ScenarioTransaction>> transactions;
};

//===----------------------------------------------------------------------===//
// Table entry
//===----------------------------------------------------------------------===//

//! A scenario table: mirrors the base table's columns and constraints.
//! Reads are served by the base scan (passthrough) until the scenario has a
//! delta for this table (merge-on-read, WP2).
class ScenarioTableEntry : public TableCatalogEntry {
public:
	//! for_binding: the entry handed to the binder (LookupEntry) carries
	//! DuckDB's temporary flag; the one handed to catalog enumeration (Scan:
	//! duckdb_tables, information_schema, SHOW TABLES) does not
	ScenarioTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                   TableCatalogEntry &base_entry, bool for_binding = true);
	ScenarioTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, ColumnList columns,
	                   TableCatalogEntry &base_entry, bool for_binding);

	//! The base table in the host catalog this entry mirrors
	TableCatalogEntry &base_entry;
	//! Row identity: base PK or the key declared at scenario_create
	//! (resolved at entry construction; empty = keyless)
	vector<idx_t> key_columns;

	//! Identity exposed through the virtual columns: the key columns, or
	//! EVERY column for keyless tables (whole-row bag identity)
	vector<idx_t> IdentityColumns() const;

public:
	//! The mirrored columns. 2.0: the entry's column list (pure virtual in
	//! TableCatalogEntry, so no `override` on 1.5.6, where it is non-virtual
	//! and this hides an identical copy held by the base)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#endif
	const ColumnList &GetColumns() const; // NOLINT: see above
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
	DataTable &GetStorage() override;
	//! Reports the entry as persistent (temporary = false) for introspection
	//! and ToSQL; see the constructor for why the flag itself is set
	unique_ptr<CreateInfo> GetInfo() const override;

	//! Adds __scenario_origin and __scenario_key_<k> (PK-based row identity)
	virtual_column_map_t GetVirtualColumns() const override;
	//! Row identity for DML: origin + PK values (falls back to rowid for
	//! no-PK tables, whose UPDATE/DELETE the plan hooks reject in v1)
	vector<column_t> GetRowIdColumns() const override;
	//! Rejects PK-column updates (v1) and projects the full post-image row
	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	ScenarioCatalog &GetScenarioCatalog();

private:
	ColumnList scenario_columns;
};

//===----------------------------------------------------------------------===//
// Schema entry
//===----------------------------------------------------------------------===//

//! The single synthetic schema ("main") of a scenario catalog. Enumerates the
//! host catalog's base tables and wraps them as ScenarioTableEntry on demand.
class ScenarioSchemaEntry : public SchemaCatalogEntry {
public:
	ScenarioSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

public:
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;

	void DropEntry(ClientContext &context, DropInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;

private:
	//! True if the base entry should be exposed inside the scenario
	bool ShouldExpose(CatalogEntry &base_entry);
	//! Wrap a base table in a ScenarioTableEntry cached on the scenario transaction
	CatalogEntry &GetOrCreateTableEntry(ClientContext &context, TableCatalogEntry &base_table,
	                                    bool for_binding = true);
	//! Same, exposing the entry under a different (logical) name than the
	//! physical base table's (materialized copies)
	CatalogEntry &GetOrCreateTableEntryAs(ClientContext &context, TableCatalogEntry &base_table,
	                                      const string &logical_name, bool for_binding = true);
	//! Mirror a base view so its SQL rebinds against the scenario's tables
	CatalogEntry &GetOrCreateViewEntry(ClientContext &context, ViewCatalogEntry &base_view);
	//! The delta/mat naming key (ScenarioDelta::LogicalName): plain for main,
	//! '<schema>.<table>' otherwise, parts with a '.' double-quoted
	string QualifiedName(const string &table_name) const;
	ScenarioCatalog &GetScenarioCatalog();
};

//===----------------------------------------------------------------------===//
// Catalog
//===----------------------------------------------------------------------===//

class ScenarioCatalog : public Catalog {
public:
	ScenarioCatalog(AttachedDatabase &db, string scenario_name, string host_catalog_name, int64_t scenario_id,
	                int64_t mat_base_scenario_id, string base_catalog_name, timestamp_t created_at);

	//! Name of the scenario in the registry
	const string scenario_name;
	//! Name of the host catalog holding registry + delta tables + base tables
	const string host_catalog_name;
	//! Phase 4: attached catalog serving as the base ("" = the host)
	const string base_catalog_name;
	//! Creation time: pins versioned bases (DuckLake) via AT (TIMESTAMP => ...)
	const timestamp_t created_at;
	//! Registry id of the scenario
	const int64_t scenario_id;
	//! BaseSource selector: -1 = live host tables (overlay); otherwise the
	//! scenario id whose materialized copies (s<id>_mat_*) serve as the base
	//! (self for materialized scenarios, nearest materialized ancestor for
	//! branches of one). Phase 4 adds the versioned-DuckLake variant.
	const int64_t mat_base_scenario_id;

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void DropSchema(ClientContext &context, DropInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                              const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;
	unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                              unique_ptr<LogicalOperator> plan,
	                                              unique_ptr<CreateIndexInfo> create_info,
	                                              unique_ptr<AlterTableInfo> alter_info) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	optional_idx GetCatalogVersion(ClientContext &context) override;

	bool InMemory() override;
	string GetDBPath() override;

	//! The host catalog (throws if it has been detached)
	Catalog &GetHostCatalog(ClientContext &context);
	//! The catalog providing the base tables (host unless base := was used)
	Catalog &GetBaseCatalog(ClientContext &context);
	//! The scenario transaction for this catalog in the current context
	ScenarioTransaction &GetScenarioTransaction(ClientContext &context);
	//! Execution-time chokepoint for every write (INSERT/UPDATE/DELETE/MERGE),
	//! called once per scenario write sink from GetGlobalSinkState - before any
	//! Sink runs, so a statement that would write zero rows is checked too, and
	//! at EXECUTE rather than PREPARE (core defers its checks the same way).
	//! Throws when the transaction is read-only, the scenario or its host is
	//! attached read-only, or the scenario is frozen; then registers the HOST
	//! database as the transaction's write target (MarkHostWrite). Serialized
	//! process-wide: sibling sinks of one plan initialize on worker threads.
	void BeginWrite(ClientContext &context, const char *operation, DatabaseModificationType type);
	//! The delta a write sink appends to, created on the scenario table's
	//! first write (#34) - in the caller's transaction (ROLLBACK removes it),
	//! at execution, after BeginWrite. Sinks call it with their first chunk,
	//! so a write that touches no rows creates nothing. The delta's identity
	//! is the entry's (ScenarioDelta::GetKeyColumns). A concurrent first
	//! write of the same table by another transaction surfaces as a
	//! retryable TransactionException; a table of a DuckLake base that is not
	//! in the scenario's pinned snapshot cannot be written.
	DuckTableEntry &GetOrCreateWriteDelta(ClientContext &context, ScenarioTableEntry &entry);

private:
	//! The checks of BeginWrite
	void ThrowIfNotWritable(ClientContext &context, const char *operation);
	//! Attribute the write to the HOST database (the scenario catalog is never
	//! the registered write target), so host DML and scenario DML can share one
	//! transaction. Throws a scenario-named error when the transaction already
	//! modified a different database.
	void MarkHostWrite(ClientContext &context, DatabaseModificationType type);

private:
	//! The synthetic "main" schema
	unique_ptr<ScenarioSchemaEntry> main_schema;
	//! Lazily mirrored non-main base schemas (name -> synthetic schema entry)
	mutex schema_lock;
	case_insensitive_map_t<unique_ptr<ScenarioSchemaEntry>> mirrored_schemas;

	//! Find or create the mirror for a non-main base schema
	ScenarioSchemaEntry &GetOrCreateMirroredSchema(const string &schema_name);
};

} // namespace duckdb
