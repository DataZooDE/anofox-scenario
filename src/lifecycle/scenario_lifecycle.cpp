#include "lifecycle/scenario_lifecycle.hpp"

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace {

//===----------------------------------------------------------------------===//
// Shared plumbing for single-shot CALL verbs
//===----------------------------------------------------------------------===//

void ValidateScenarioName(const string &name) {
	if (name.empty()) {
		throw InvalidInputException("Scenario name cannot be empty");
	}
	if (name.size() > 63) {
		throw InvalidInputException("Scenario name '%s' exceeds the maximum length of 63 characters", name);
	}
	if (!isalpha(name[0]) && name[0] != '_') {
		throw InvalidInputException("Invalid scenario name '%s': must start with a letter or underscore", name);
	}
	for (auto c : name) {
		if (!isalnum(c) && c != '_') {
			throw InvalidInputException(
			    "Invalid scenario name '%s': only letters, digits and underscores are allowed", name);
		}
	}
}

Catalog &GetHostCatalog(ClientContext &context) {
	return Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
}

struct LifecycleBindData : public TableFunctionData {
	string name;
	string description;
	bool has_description = false;
};

struct LifecycleGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<GlobalTableFunctionState> LifecycleInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<LifecycleGlobalState>();
}

unique_ptr<FunctionData> LifecycleBind(TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                       vector<string> &names) {
	auto result = make_uniq<LifecycleBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("Scenario name cannot be NULL");
	}
	result->name = input.inputs[0].GetValue<string>();
	ValidateScenarioName(result->name);
	if (input.inputs.size() > 1) {
		if (!input.inputs[1].IsNull()) {
			result->description = input.inputs[1].GetValue<string>();
			result->has_description = true;
		}
	}
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("success");
	return std::move(result);
}

unique_ptr<FunctionData> LifecycleBindWrapper(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	return LifecycleBind(input, return_types, names);
}

//! Run the verb exactly once, emitting a single 'true' row.
template <void (*VERB)(ClientContext &, const LifecycleBindData &)>
void LifecycleExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<LifecycleGlobalState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	auto &bind_data = data.bind_data->Cast<LifecycleBindData>();
	VERB(context, bind_data);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetCardinality(1);
	state.done = true;
}

//===----------------------------------------------------------------------===//
// Verbs
//===----------------------------------------------------------------------===//

void ScenarioCreateVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	ScenarioRegistry::EnsureExists(context, host_catalog);
	if (ScenarioRegistry::Lookup(context, host_catalog, bind_data.name)) {
		throw InvalidInputException("Scenario '%s' already exists", bind_data.name);
	}
	ScenarioRegistryEntry entry;
	entry.scenario_id = ScenarioRegistry::NextId(context, host_catalog);
	entry.name = bind_data.name;
	entry.created_at = Timestamp::GetCurrentTimestamp();
	entry.description = bind_data.description;
	entry.has_description = bind_data.has_description;
	ScenarioRegistry::Insert(context, host_catalog, entry);

	// Eagerly create one (empty) delta table per base table, in this same
	// transaction. DuckDB's single-writer-per-transaction rule prevents
	// creating catalog entries in the host from within scenario DML, so all
	// delta DDL happens here, where the host *is* the modified database.
	// Cost: O(#tables) empty tables -- metadata only, no row data copied.
	auto &host_schema = host_catalog.GetSchema(context, DEFAULT_SCHEMA);
	vector<reference<TableCatalogEntry>> base_tables;
	host_schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
		if (table_entry.type != CatalogType::TABLE_ENTRY || table_entry.internal) {
			return;
		}
		if (StringUtil::StartsWith(table_entry.name, "_scenario_")) {
			return; // legacy v0.1 metadata
		}
		base_tables.push_back(table_entry.Cast<TableCatalogEntry>());
	});
	for (auto &base_table : base_tables) {
		ScenarioDelta::EnsureDeltaTable(context, host_catalog, entry.scenario_id, base_table.get());
	}
}

//! Throw if the scenario is currently attached in this database instance
void ThrowIfAttached(ClientContext &context, const string &scenario_name) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db : databases) {
		auto &catalog = db->GetCatalog();
		if (catalog.GetCatalogType() != "scenario") {
			continue;
		}
		auto &scenario_catalog = catalog.Cast<ScenarioCatalog>();
		if (scenario_catalog.scenario_name == scenario_name) {
			throw InvalidInputException("Scenario '%s' is attached as '%s'. DETACH it before dropping the scenario",
			                            scenario_name, catalog.GetName());
		}
	}
}

//! Drop all physical delta tables of a scenario (s<id>_delta_*)
void DropDeltaTables(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return;
	}
	string prefix = "s" + to_string(scenario_id) + "_delta_";
	vector<string> to_drop;
	schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		if (entry.type == CatalogType::TABLE_ENTRY && StringUtil::StartsWith(entry.name, prefix)) {
			to_drop.push_back(entry.name);
		}
	});
	for (auto &table_name : to_drop) {
		DropInfo info;
		info.type = CatalogType::TABLE_ENTRY;
		info.catalog = host_catalog.GetName();
		info.schema = ScenarioRegistry::SCHEMA_NAME;
		info.name = table_name;
		host_catalog.DropEntry(context, info);
	}
}

void ScenarioDropVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.name);
	}
	ThrowIfAttached(context, bind_data.name);
	if (ScenarioRegistry::HasChildren(context, host_catalog, entry->scenario_id)) {
		throw ConstraintException("Cannot drop scenario '%s': other scenarios branch from it", bind_data.name);
	}
	// Mark the host writable before dropping catalog entries
	MetaTransaction::Get(context).ModifyDatabase(host_catalog.GetAttached(),
	                                             DatabaseModificationType::DROP_CATALOG_ENTRY |
	                                                 DatabaseModificationType::DELETE_DATA);
	DropDeltaTables(context, host_catalog, entry->scenario_id);
	ScenarioRegistry::Delete(context, host_catalog, entry->scenario_id);
}

void ScenarioFreezeVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.name);
	}
	ScenarioRegistry::SetFrozen(context, host_catalog, entry->scenario_id, true);
}

void ScenarioUnfreezeVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.name);
	}
	ScenarioRegistry::SetFrozen(context, host_catalog, entry->scenario_id, false);
}

TableFunctionSet MakeVerbSet(const string &name, table_function_t function, bool with_description) {
	TableFunctionSet set(name);
	TableFunction one_arg({LogicalType::VARCHAR}, function, LifecycleBindWrapper, LifecycleInit);
	set.AddFunction(one_arg);
	if (with_description) {
		TableFunction two_arg({LogicalType::VARCHAR, LogicalType::VARCHAR}, function, LifecycleBindWrapper,
		                      LifecycleInit);
		set.AddFunction(two_arg);
	}
	return set;
}

} // namespace

void ScenarioLifecycle::RegisterFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(MakeVerbSet("scenario_create", LifecycleExecute<ScenarioCreateVerb>, true));
	loader.RegisterFunction(MakeVerbSet("scenario_drop", LifecycleExecute<ScenarioDropVerb>, false));
	loader.RegisterFunction(MakeVerbSet("scenario_freeze", LifecycleExecute<ScenarioFreezeVerb>, false));
	loader.RegisterFunction(MakeVerbSet("scenario_unfreeze", LifecycleExecute<ScenarioUnfreezeVerb>, false));
}

} // namespace duckdb
