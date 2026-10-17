#include "catalog/scenario_catalog.hpp"

#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {

void ThrowScenarioDDLError() {
	throw NotImplementedException(
	    "DDL operations are not permitted in scenarios. Modify the base schema instead: tables added to the "
	    "base become readable and writable in the scenario automatically");
}

//! Serializes the write registrations and delta creations of scenario sinks:
//! their global states initialize on worker threads in parallel, and
//! MetaTransaction::ModifyDatabase / DuckTransaction::SetModifications are
//! not synchronized
static mutex &ScenarioWriteLock() {
	static mutex lock;
	return lock;
}

ScenarioCatalog::ScenarioCatalog(AttachedDatabase &db, string scenario_name_p, string host_catalog_name_p,
                                 int64_t scenario_id_p, int64_t mat_base_scenario_id_p, string base_catalog_name_p,
                                 timestamp_t created_at_p)
    : Catalog(db), scenario_name(std::move(scenario_name_p)), host_catalog_name(std::move(host_catalog_name_p)),
      base_catalog_name(std::move(base_catalog_name_p)), created_at(created_at_p), scenario_id(scenario_id_p),
      mat_base_scenario_id(mat_base_scenario_id_p) {
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

ScenarioSchemaEntry &ScenarioCatalog::GetOrCreateMirroredSchema(const string &schema_name) {
	lock_guard<mutex> guard(schema_lock);
	auto existing = mirrored_schemas.find(schema_name);
	if (existing != mirrored_schemas.end()) {
		return *existing->second;
	}
	CreateSchemaInfo info;
	info.schema = schema_name;
	info.internal = true;
	auto entry = make_uniq<ScenarioSchemaEntry>(*this, info);
	auto &result = *entry;
	mirrored_schemas[schema_name] = std::move(entry);
	return result;
}

//! True when a base schema participates in scenario mirroring
static bool ShouldMirrorSchema(const SchemaCatalogEntry &schema) {
	if (schema.name == DEFAULT_SCHEMA || schema.internal) {
		return false;
	}
	return schema.name != ScenarioRegistry::SCHEMA_NAME;
}

optional_ptr<SchemaCatalogEntry> ScenarioCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	const auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	// Non-main schemas mirror the base's schemas (live: any schema the base
	// has right now; the internal metadata schema is never exposed)
	if (transaction.context && schema_name != ScenarioRegistry::SCHEMA_NAME) {
		auto &base_catalog = GetBaseCatalog(*transaction.context);
		auto base_schema = base_catalog.GetSchema(*transaction.context, schema_name, OnEntryNotFound::RETURN_NULL);
		if (base_schema && ShouldMirrorSchema(*base_schema)) {
			return &GetOrCreateMirroredSchema(schema_name);
		}
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Schema \"%s\" does not exist in scenario catalog \"%s\" (schemas mirror the base)",
		                       schema_name, GetName());
	}
	return nullptr;
}

void ScenarioCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
	auto &base_catalog = GetBaseCatalog(context);
	base_catalog.ScanSchemas(context, [&](SchemaCatalogEntry &base_schema) {
		if (ShouldMirrorSchema(base_schema)) {
			callback(GetOrCreateMirroredSchema(base_schema.name));
		}
	});
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
	// Scenario entries mirror the live base schema, so plan caching must be
	// invalidated when the base changes: fold the host catalog's version in.
	// The version also folds in the current transaction's serial: physical
	// DML operators (e.g. INSERT ... VALUES, which has no scenario scan whose
	// bind data could opt out of statement caching) hold references to the
	// per-transaction ScenarioTableEntry, which dies with the transaction. A
	// prepared statement planned in an earlier transaction must therefore be
	// re-bound rather than re-executed against dangling entries.
	auto host_version = GetHostCatalog(context).GetCatalogVersion(context);
	auto &transaction = GetScenarioTransaction(context);
	idx_t base = host_version.IsValid() ? host_version.GetIndex() : 0;
	return optional_idx(base * 1000003ULL + transaction.serial);
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

Catalog &ScenarioCatalog::GetBaseCatalog(ClientContext &context) {
	if (base_catalog_name.empty()) {
		return GetHostCatalog(context);
	}
	auto base = Catalog::GetCatalogEntry(context, base_catalog_name);
	if (!base) {
		throw InvalidInputException(
		    "Scenario '%s': base catalog '%s' is not attached - ATTACH it before using this scenario",
		    scenario_name, base_catalog_name);
	}
	return *base;
}

ScenarioTransaction &ScenarioCatalog::GetScenarioTransaction(ClientContext &context) {
	return Transaction::Get(context, *this).Cast<ScenarioTransaction>();
}

void ScenarioCatalog::BeginWrite(ClientContext &context, const char *operation, DatabaseModificationType type) {
	// Sinks initialize their global state in PipelineInitializeTasks, which
	// may run on worker threads in parallel (e.g. the UNION of INSERT
	// pipelines COPY FROM DATABASE plans). MetaTransaction::ModifyDatabase and
	// DuckTransaction::SetModifications are not synchronized, so registrations
	// - of every scenario, they may share one host transaction - take turns.
	lock_guard<mutex> guard(ScenarioWriteLock());
	ThrowIfNotWritable(context, operation);
	MarkHostWrite(context, type);
}

DuckTableEntry &ScenarioCatalog::GetOrCreateWriteDelta(ClientContext &context, ScenarioTableEntry &entry) {
	auto &host_catalog = GetHostCatalog(context);
	auto logical_name = ScenarioDelta::LogicalName(entry);
	// sibling sinks of one statement (MERGE INTO actions, the INSERTs of
	// COPY FROM DATABASE) may get here together for the same table
	lock_guard<mutex> guard(ScenarioWriteLock());
	auto existing = ScenarioDelta::TryGetDeltaTable(context, host_catalog, scenario_id, logical_name);
	if (existing) {
		return *existing;
	}
	if (!base_catalog_name.empty()) {
		auto &base_catalog = GetBaseCatalog(context);
		if (base_catalog.GetCatalogType() == "ducklake") {
			// the scenario reads the lake AT its creation time: a table that
			// is not in that snapshot is not part of the scenario's base
			BoundAtClause at_clause("timestamp", Value::TIMESTAMP(created_at));
			EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, entry.base_entry.name, &at_clause, QueryErrorContext());
			optional_ptr<CatalogEntry> pinned;
			try {
				pinned = base_catalog.GetEntry(context, entry.base_entry.ParentSchema().name, lookup,
				                               OnEntryNotFound::RETURN_NULL);
			} catch (CatalogException &) {
				pinned = nullptr; // e.g. the schema is not in the snapshot either
			}
			if (!pinned) {
				throw InvalidInputException(
				    "Cannot write to table '%s' of scenario '%s': the table is not in the scenario's base snapshot "
				    "(DuckLake '%s' AT %s) - it was created in the lake later. Create a new scenario to modify it",
				    logical_name, scenario_name, base_catalog_name, Timestamp::ToString(created_at));
			}
		}
	}
	// the identity the statement was bound with must still be the table's:
	// a key declared (scenario_refresh) after this transaction cached the
	// entry would otherwise yield a delta of a different shape
	auto key_columns = ScenarioDelta::GetKeyColumns(context, host_catalog, scenario_id, logical_name, entry.base_entry);
	if (key_columns != entry.key_columns) {
		throw TransactionException("Cannot write to table '%s' of scenario '%s': its row identity was declared after "
		                           "this transaction first used the table. Retry in a new transaction",
		                           logical_name, scenario_name);
	}
	vector<string> declared_keys;
	if (ScenarioDelta::GetPKColumns(entry.base_entry).empty()) {
		for (auto key_column : key_columns) {
			declared_keys.push_back(entry.base_entry.GetColumn(LogicalIndex(key_column)).Name());
		}
	}
	// BeginWrite made the host this transaction's writer already (or, for a
	// plan that registered this catalog itself, flagged the host transaction)
	MarkHostWrite(context, DatabaseModificationType::CREATE_CATALOG_ENTRY);
	auto writer = MetaTransaction::Get(context).ModifiedDatabase();
	bool host_is_not_writer = writer && writer.get() == &GetAttached();
	try {
		return ScenarioDelta::EnsureDeltaTable(context, host_catalog, scenario_id, entry.base_entry,
		                                       declared_keys.empty() ? nullptr : &declared_keys, &logical_name,
		                                       host_is_not_writer);
	} catch (TransactionException &) {
		// a catalog write-write conflict: another transaction created this
		// delta concurrently (uncommitted, or committed after we started)
		throw TransactionException(
		    "Cannot write to scenario '%s': another transaction concurrently made the first write to table '%s' "
		    "and created its delta. ROLLBACK and retry the transaction",
		    scenario_name, logical_name);
	}
}

void ScenarioCatalog::MarkHostWrite(ClientContext &context, DatabaseModificationType type) {
	// The single point where a scenario write is attributed to a database.
	// Scenario table entries are flagged temporary (see ScenarioTableEntry), so
	// the binder does not register the scenario catalog as the statement's
	// write target; BeginWrite registers the HOST database instead - where the
	// rows actually land - once per sink, before execution. Host DML and
	// scenario DML (of any number of scenarios on this host) thus modify one
	// database and may share a transaction; a write to a genuinely different
	// database is refused, naming the scenario. Because the binder-time checks
	// no longer cover the scenario catalog, the read-only checks of
	// ClientContext::CheckIfPreparedStatementIsExecutable are mirrored here.
	auto &host_db = GetHostCatalog(context).GetAttached();
	auto &meta = MetaTransaction::Get(context);
	if (meta.IsReadOnly()) {
		throw TransactionException("Cannot write to scenario \"%s\" - transaction is launched in read-only mode",
		                           scenario_name);
	}
	if (GetAttached().IsReadOnly()) {
		throw InvalidInputException("Cannot write to scenario '%s': it is attached in read-only mode (as '%s')",
		                            scenario_name, GetAttached().GetName());
	}
	if (host_db.IsReadOnly()) {
		// scenario writes physically land in the host database
		throw InvalidInputException(
		    "Cannot write to scenario '%s': its host database '%s' is attached in read-only mode", scenario_name,
		    host_db.GetName());
	}
	auto modified = meta.ModifiedDatabase();
	if (modified && modified.get() != &host_db && modified.get() != &GetAttached()) {
		// MetaTransaction would refuse this too, but naming only the host
		throw TransactionException(
		    "Cannot write to scenario '%s': this transaction already modified database \"%s\". Scenario writes "
		    "land in the host database \"%s\", and a single transaction can only write to a single attached "
		    "database",
		    scenario_name, modified->GetName(), host_db.GetName());
	}
	if (modified && modified.get() == &GetAttached()) {
		// Defensive: a code path that registered the scenario catalog itself
		// as the write target (none of the DML binders do, see above). Keep
		// it as the single writer and only flag the host transaction
		// read-write so commit flushes it.
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
