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
	//! Phase 2: 'delta' (default) or 'materialized'
	string mode = "delta";
	//! Phase 2: branch source (empty = none)
	string from_scenario;
	//! Phase 4: attached catalog serving as the base (empty = host default)
	string base_catalog;
	//! Declared row identity for keyless tables: table -> key column names
	map<string, vector<string>> key_columns;
};

struct LifecycleGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

//! Parse a key_columns := MAP {'table': ['col', ...], ...} parameter value
void ParseKeyColumnsParam(const Value &value, map<string, vector<string>> &out) {
	for (auto &map_entry : ListValue::GetChildren(value)) {
		auto &kv = StructValue::GetChildren(map_entry);
		auto table_name = kv[0].GetValue<string>();
		vector<string> columns;
		for (auto &col : ListValue::GetChildren(kv[1])) {
			columns.push_back(col.GetValue<string>());
		}
		if (columns.empty()) {
			throw InvalidInputException("key_columns for table '%s' must not be empty", table_name);
		}
		out[table_name] = std::move(columns);
	}
}

//! Shared typo-safety checks for a declared key against its base table
void ValidateDeclaredKey(const TableCatalogEntry &base_table, const vector<string> &columns) {
	if (base_table.HasPrimaryKey()) {
		throw InvalidInputException("key_columns declared for table '%s', but it already has a PRIMARY KEY",
		                            base_table.name);
	}
	for (auto &column_name : columns) {
		if (!base_table.ColumnExists(column_name)) {
			throw InvalidInputException("key_columns for table '%s': column '%s' does not exist", base_table.name,
			                            column_name);
		}
	}
}

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
	for (auto &param : input.named_parameters) {
		if (param.second.IsNull()) {
			continue;
		}
		if (param.first == "mode") {
			result->mode = StringUtil::Lower(param.second.GetValue<string>());
			if (result->mode != "delta" && result->mode != "materialized") {
				throw InvalidInputException("Invalid scenario mode '%s': expected 'delta' or 'materialized'",
				                            result->mode);
			}
		} else if (param.first == "from_scenario") {
			result->from_scenario = param.second.GetValue<string>();
		} else if (param.first == "base") {
			result->base_catalog = param.second.GetValue<string>();
		} else if (param.first == "key_columns") {
			ParseKeyColumnsParam(param.second, result->key_columns);
		}
	}
	if (!result->base_catalog.empty() && (result->mode == "materialized" || !result->from_scenario.empty())) {
		throw NotImplementedException(
		    "base := '<catalog>' cannot be combined with materialized mode or branching yet (planned)");
	}
	if (!result->from_scenario.empty() && result->mode == "materialized") {
		throw InvalidInputException(
		    "Branching (from_scenario) and mode := 'materialized' cannot be combined: a branch overlays its "
		    "parent's state");
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
	// Branching: resolve the parent first
	unique_ptr<ScenarioRegistryEntry> parent;
	if (!bind_data.from_scenario.empty()) {
		parent = ScenarioRegistry::Lookup(context, host_catalog, bind_data.from_scenario);
		if (!parent) {
			throw InvalidInputException("Cannot branch from scenario '%s': not found", bind_data.from_scenario);
		}
	}

	// Phase 4: cross-catalog base (e.g. a DuckLake attach)
	optional_ptr<Catalog> base_catalog;
	if (!bind_data.base_catalog.empty()) {
		base_catalog = Catalog::GetCatalogEntry(context, bind_data.base_catalog);
		if (!base_catalog) {
			throw InvalidInputException("Base catalog '%s' is not attached", bind_data.base_catalog);
		}
		if (base_catalog->GetCatalogType() == "scenario") {
			throw InvalidInputException("A scenario cannot use another scenario as its base - branch instead "
			                            "(from_scenario := '...')");
		}
	}

	ScenarioRegistryEntry entry;
	entry.scenario_id = ScenarioRegistry::NextId(context, host_catalog);
	entry.name = bind_data.name;
	entry.mode = bind_data.mode;
	entry.parent_id = parent ? parent->scenario_id : -1;
	entry.created_at = Timestamp::GetCurrentTimestamp();
	entry.description = bind_data.description;
	entry.has_description = bind_data.has_description;
	entry.base_catalog = bind_data.base_catalog;
	ScenarioRegistry::Insert(context, host_catalog, entry);

	// Eagerly create one (empty) delta table per base table, in this same
	// transaction. DuckDB's single-writer-per-transaction rule prevents
	// creating catalog entries in the host from within scenario DML, so all
	// delta DDL happens here, where the host *is* the modified database.
	// Cost: O(#tables) empty tables -- metadata only, no row data copied.
	// Materialized mode additionally CTAS-copies every base table.
	auto &base_source = base_catalog ? *base_catalog : host_catalog;
	auto &host_schema = base_source.GetSchema(context, DEFAULT_SCHEMA);
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
	// validate declared keys up front (typo safety)
	for (auto &declared : bind_data.key_columns) {
		bool table_found = false;
		for (auto &base_table : base_tables) {
			if (base_table.get().name != declared.first) {
				continue;
			}
			table_found = true;
			ValidateDeclaredKey(base_table.get(), declared.second);
		}
		if (!table_found) {
			throw InvalidInputException("key_columns declared for unknown table '%s'", declared.first);
		}
	}
	for (auto &base_table : base_tables) {
		auto declared = bind_data.key_columns.find(base_table.get().name);
		auto &delta = ScenarioDelta::EnsureDeltaTable(
		    context, host_catalog, entry.scenario_id, base_table.get(),
		    declared == bind_data.key_columns.end() ? nullptr : &declared->second);
		if (bind_data.mode == "materialized") {
			ScenarioDelta::CreateMatTable(context, host_catalog, entry.scenario_id, base_table.get());
		}
		if (parent) {
			// Branch: inherit the parent's modifications by copying its
			// delta rows (cheap - deltas are small by invariant)
			auto parent_delta =
			    ScenarioDelta::TryGetDeltaTable(context, host_catalog, parent->scenario_id, base_table.get().name);
			if (parent_delta) {
				ScenarioDelta::CopyTableData(context, *parent_delta, delta);
			}
		}
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

//! Drop all physical tables of a scenario (s<id>_delta_* and s<id>_mat_*)
void DropDeltaTables(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return;
	}
	string delta_prefix = "s" + to_string(scenario_id) + "_delta_";
	string mat_prefix = "s" + to_string(scenario_id) + "_mat_";
	vector<string> to_drop;
	schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		if (entry.type == CatalogType::TABLE_ENTRY && (StringUtil::StartsWith(entry.name, delta_prefix) ||
		                                               StringUtil::StartsWith(entry.name, mat_prefix))) {
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

//===----------------------------------------------------------------------===//
// scenario_refresh: create delta tables for base tables added after creation
//===----------------------------------------------------------------------===//

struct RefreshState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> ScenarioRefreshBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<LifecycleBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("Scenario name cannot be NULL");
	}
	result->name = input.inputs[0].GetValue<string>();
	// key_columns := declares identity for keyless tables that appeared after
	// scenario_create (which rejects declarations for then-unknown tables)
	for (auto &param : input.named_parameters) {
		if (param.second.IsNull()) {
			continue;
		}
		if (param.first == "key_columns") {
			ParseKeyColumnsParam(param.second, result->key_columns);
		}
	}
	return_types = {LogicalType::BIGINT};
	names = {"refreshed_tables"};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ScenarioRefreshInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RefreshState>();
}

void ScenarioRefreshExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RefreshState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	auto &bind_data = data.bind_data->Cast<LifecycleBindData>();
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.name);
	}
	if (entry->mode == "materialized") {
		throw InvalidInputException(
		    "Cannot refresh scenario '%s': materialized scenarios are point-in-time copies and do not pick up "
		    "new base tables",
		    bind_data.name);
	}
	// Phase 4: honor a cross-catalog base
	optional_ptr<Catalog> base_source = &host_catalog;
	if (!entry->base_catalog.empty()) {
		base_source = Catalog::GetCatalogEntry(context, entry->base_catalog);
		if (!base_source) {
			throw InvalidInputException("Scenario '%s': base catalog '%s' is not attached", bind_data.name,
			                            entry->base_catalog);
		}
	}
	MetaTransaction::Get(context).ModifyDatabase(host_catalog.GetAttached(),
	                                             DatabaseModificationType::CREATE_CATALOG_ENTRY);

	auto &base_schema = base_source->GetSchema(context, DEFAULT_SCHEMA);
	vector<reference<TableCatalogEntry>> base_tables;
	base_schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
		if (table_entry.type != CatalogType::TABLE_ENTRY || table_entry.internal ||
		    StringUtil::StartsWith(table_entry.name, "_scenario_")) {
			return;
		}
		base_tables.push_back(table_entry.Cast<TableCatalogEntry>());
	});
	// validate declared keys up front: they must name a base table that is not
	// yet tracked (identity cannot be changed once a delta table exists)
	for (auto &declared : bind_data.key_columns) {
		bool table_found = false;
		for (auto &base_table : base_tables) {
			if (base_table.get().name != declared.first) {
				continue;
			}
			table_found = true;
			ValidateDeclaredKey(base_table.get(), declared.second);
			if (ScenarioDelta::TryGetDeltaTable(context, host_catalog, entry->scenario_id, base_table.get().name)) {
				throw InvalidInputException("key_columns declared for table '%s', but scenario '%s' already tracks "
				                            "it - row identity cannot be changed after the fact",
				                            declared.first, bind_data.name);
			}
		}
		if (!table_found) {
			throw InvalidInputException("key_columns declared for unknown table '%s'", declared.first);
		}
	}
	idx_t refreshed = 0;
	for (auto &base_table : base_tables) {
		if (!ScenarioDelta::TryGetDeltaTable(context, host_catalog, entry->scenario_id, base_table.get().name)) {
			auto declared = bind_data.key_columns.find(base_table.get().name);
			ScenarioDelta::EnsureDeltaTable(context, host_catalog, entry->scenario_id, base_table.get(),
			                                declared == bind_data.key_columns.end() ? nullptr : &declared->second);
			refreshed++;
		}
	}
	output.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(refreshed)));
	output.SetCardinality(1);
}

//===----------------------------------------------------------------------===//
// scenario_list: the registry v2 view
//===----------------------------------------------------------------------===//

struct ScenarioListState : public GlobalTableFunctionState {
	vector<ScenarioRegistryEntry> entries;
	//! parent names resolved by id
	unordered_map<int64_t, string> names_by_id;
	idx_t offset = 0;
};

unique_ptr<FunctionData> ScenarioListBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::BIGINT,  LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::BOOLEAN,
	                LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::VARCHAR};
	names = {"scenario_id", "name", "mode", "frozen", "parent", "created_at", "description"};
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> ScenarioListInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<ScenarioListState>();
	result->entries = ScenarioRegistry::List(context, GetHostCatalog(context));
	for (auto &entry : result->entries) {
		result->names_by_id[entry.scenario_id] = entry.name;
	}
	return std::move(result);
}

void ScenarioListExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ScenarioListState>();
	idx_t count = 0;
	while (state.offset < state.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = state.entries[state.offset++];
		output.SetValue(0, count, Value::BIGINT(entry.scenario_id));
		output.SetValue(1, count, Value(entry.name));
		output.SetValue(2, count, Value(entry.mode));
		output.SetValue(3, count, Value::BOOLEAN(entry.frozen));
		if (entry.parent_id >= 0) {
			auto parent = state.names_by_id.find(entry.parent_id);
			output.SetValue(4, count,
			                parent != state.names_by_id.end() ? Value(parent->second) : Value(LogicalType::VARCHAR));
		} else {
			output.SetValue(4, count, Value(LogicalType::VARCHAR));
		}
		output.SetValue(5, count, Value::TIMESTAMP(entry.created_at));
		output.SetValue(6, count, entry.has_description ? Value(entry.description) : Value(LogicalType::VARCHAR));
		count++;
	}
	output.SetCardinality(count);
}

TableFunctionSet MakeVerbSet(const string &name, table_function_t function, bool is_create) {
	TableFunctionSet set(name);
	TableFunction one_arg({LogicalType::VARCHAR}, function, LifecycleBindWrapper, LifecycleInit);
	if (is_create) {
		one_arg.named_parameters["mode"] = LogicalType::VARCHAR;
		one_arg.named_parameters["from_scenario"] = LogicalType::VARCHAR;
		one_arg.named_parameters["base"] = LogicalType::VARCHAR;
		one_arg.named_parameters["key_columns"] =
		    LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR));
	}
	set.AddFunction(one_arg);
	if (is_create) {
		TableFunction two_arg({LogicalType::VARCHAR, LogicalType::VARCHAR}, function, LifecycleBindWrapper,
		                      LifecycleInit);
		two_arg.named_parameters["mode"] = LogicalType::VARCHAR;
		two_arg.named_parameters["from_scenario"] = LogicalType::VARCHAR;
		two_arg.named_parameters["base"] = LogicalType::VARCHAR;
		two_arg.named_parameters["key_columns"] =
		    LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR));
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
	// Registry v2 listing (replaces legacy scenario_list in v0.2)
	loader.RegisterFunction(TableFunction("scenario_list", {}, ScenarioListExecute, ScenarioListBind,
	                                      ScenarioListInit));
	// Pick up base tables created after the scenario
	TableFunction refresh("scenario_refresh", {LogicalType::VARCHAR}, ScenarioRefreshExecute, ScenarioRefreshBind,
	                      ScenarioRefreshInit);
	refresh.named_parameters["key_columns"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR));
	loader.RegisterFunction(refresh);
}

} // namespace duckdb
