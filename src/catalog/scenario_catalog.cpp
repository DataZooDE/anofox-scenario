#include "catalog/scenario_catalog.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {

void ThrowScenarioDDLError() {
	throw NotImplementedException(
	    "DDL operations are not permitted in scenarios. Modify the base schema, then create a new scenario "
	    "(REQ-COW-007)");
}

ScenarioCatalog::ScenarioCatalog(AttachedDatabase &db, string scenario_name_p, string host_catalog_name_p,
                                 int64_t scenario_id_p)
    : Catalog(db), scenario_name(std::move(scenario_name_p)), host_catalog_name(std::move(host_catalog_name_p)),
      scenario_id(scenario_id_p) {
}

void ScenarioCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	info.schema = DEFAULT_SCHEMA;
	info.internal = true;
	main_schema = make_uniq<ScenarioSchemaEntry>(*this, info);
}

string ScenarioCatalog::GetCatalogType() {
	return "scenario";
}

optional_ptr<CatalogEntry> ScenarioCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	ThrowScenarioDDLError();
}

void ScenarioCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<SchemaCatalogEntry> ScenarioCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	const auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Scenario catalog \"%s\" only has a \"%s\" schema - schema \"%s\" does not exist",
		                       GetName(), DEFAULT_SCHEMA, schema_name);
	}
	return nullptr;
}

void ScenarioCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

PhysicalOperator &ScenarioCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op, PhysicalOperator &plan) {
	ThrowScenarioDDLError();
}

unique_ptr<LogicalOperator> ScenarioCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                             TableCatalogEntry &table,
                                                             unique_ptr<LogicalOperator> plan) {
	// Without this override, the default implementation would build the index
	// on the *base* table's storage (GetStorage() forwards to the base).
	ThrowScenarioDDLError();
}

unique_ptr<LogicalOperator> ScenarioCatalog::BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
                                                               unique_ptr<LogicalOperator> plan,
                                                               unique_ptr<CreateIndexInfo> create_info,
                                                               unique_ptr<AlterTableInfo> alter_info) {
	ThrowScenarioDDLError();
}

DatabaseSize ScenarioCatalog::GetDatabaseSize(ClientContext &context) {
	// Real numbers (sum of delta table sizes) land with WP3/WP5.
	return DatabaseSize();
}

optional_idx ScenarioCatalog::GetCatalogVersion(ClientContext &context) {
	// Forward the host catalog's version: scenario entries mirror the live
	// base schema, so plan caching must be invalidated when the base changes.
	return GetHostCatalog(context).GetCatalogVersion(context);
}

bool ScenarioCatalog::InMemory() {
	return false;
}

string ScenarioCatalog::GetDBPath() {
	return "scenario:" + scenario_name;
}

Catalog &ScenarioCatalog::GetHostCatalog(ClientContext &context) {
	auto host = Catalog::GetCatalogEntry(context, host_catalog_name);
	if (!host) {
		throw InvalidInputException(
		    "Scenario '%s': host database '%s' is no longer attached - reattach it to use this scenario",
		    scenario_name, host_catalog_name);
	}
	return *host;
}

ScenarioTransaction &ScenarioCatalog::GetScenarioTransaction(ClientContext &context) {
	return Transaction::Get(context, *this).Cast<ScenarioTransaction>();
}

void ScenarioCatalog::MarkHostWrite(ClientContext &context, DatabaseModificationType type) {
	auto &host_db = GetHostCatalog(context).GetAttached();
	auto &meta = MetaTransaction::Get(context);
	if (meta.IsReadOnly()) {
		throw TransactionException("Cannot write to scenario \"%s\" - transaction is launched in read-only mode",
		                           scenario_name);
	}
	if (host_db.IsReadOnly()) {
		// Mirror the check MetaTransaction::ModifyDatabase's callers perform:
		// scenario writes physically land in the host database
		throw InvalidInputException(
		    "Cannot write to scenario '%s': its host database '%s' is attached in read-only mode", scenario_name,
		    host_db.GetName());
	}
	auto modified = meta.ModifiedDatabase();
	if (modified && modified.get() == &GetAttached()) {
		// DML targeting this scenario: the binder already registered the
		// scenario catalog as the transaction's single write target. The
		// physical rows land in the host database within the same
		// meta-transaction; mark its transaction read-write so commit
		// flushes it, without violating the single-writer rule.
		auto &host_transaction = meta.GetTransaction(host_db);
		if (host_transaction.IsReadOnly()) {
			host_transaction.SetReadWrite();
		}
		host_transaction.SetModifications(type);
		return;
	}
	meta.ModifyDatabase(host_db, type);
}

} // namespace duckdb
