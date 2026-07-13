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

namespace duckdb {

class ScenarioCatalog;
class ScenarioSchemaEntry;

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
	ScenarioTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                   TableCatalogEntry &base_entry);

	//! The base table in the host catalog this entry mirrors
	TableCatalogEntry &base_entry;

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
	DataTable &GetStorage() override;

	ScenarioCatalog &GetScenarioCatalog();
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
	CatalogEntry &GetOrCreateTableEntry(ClientContext &context, TableCatalogEntry &base_table);
	ScenarioCatalog &GetScenarioCatalog();
};

//===----------------------------------------------------------------------===//
// Catalog
//===----------------------------------------------------------------------===//

class ScenarioCatalog : public Catalog {
public:
	ScenarioCatalog(AttachedDatabase &db, string scenario_name, string host_catalog_name, int64_t scenario_id);

	//! Name of the scenario in the registry
	const string scenario_name;
	//! Name of the host catalog holding registry + delta tables + base tables
	const string host_catalog_name;
	//! Registry id of the scenario
	const int64_t scenario_id;

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

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	optional_idx GetCatalogVersion(ClientContext &context) override;

	bool InMemory() override;
	string GetDBPath() override;

	//! The host catalog (throws if it has been detached)
	Catalog &GetHostCatalog(ClientContext &context);
	//! The scenario transaction for this catalog in the current context
	ScenarioTransaction &GetScenarioTransaction(ClientContext &context);

private:
	//! The single synthetic schema ("main")
	unique_ptr<ScenarioSchemaEntry> main_schema;
};

} // namespace duckdb
