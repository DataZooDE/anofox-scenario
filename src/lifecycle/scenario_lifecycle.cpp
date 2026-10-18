#include "lifecycle/scenario_lifecycle.hpp"

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "anofox_scenario_banner.hpp"

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
	auto table_name = ScenarioDelta::LogicalName(base_table);
	if (base_table.HasPrimaryKey()) {
		throw InvalidInputException("key_columns declared for table '%s', but it already has a PRIMARY KEY",
		                            table_name);
	}
	case_insensitive_set_t seen;
	for (auto &column_name : columns) {
		if (!base_table.ColumnExists(dc::Ident(column_name))) {
			throw InvalidInputException("key_columns for table '%s': column '%s' does not exist", table_name,
			                            column_name);
		}
		if (!seen.insert(column_name).second) {
			throw InvalidInputException("key_columns for table '%s': column '%s' is listed twice", table_name,
			                            column_name);
		}
	}
}

//! A declared key for a table of a scenario pinned to a DuckLake snapshot
//! must name a table of that snapshot (writes to any other are refused)
void ValidateDeclaredKeyPinned(ClientContext &context, Catalog &base_catalog, const TableCatalogEntry &base_table,
                               const string &scenario_name, timestamp_t pinned_at) {
	if (base_catalog.GetCatalogType() != "ducklake") {
		return;
	}
	if (!ScenarioDelta::GetPinnedBaseEntry(context, base_catalog, dc::Str(base_table.ParentSchema().name),
	                                       dc::Str(base_table.name), pinned_at)) {
		throw InvalidInputException(
		    "key_columns declared for table '%s', but it is not in the base snapshot of scenario '%s' (DuckLake "
		    "'%s' AT %s) - it was created in the lake later. Create a new scenario over the lake's current state "
		    "(base := '%s'; a branch keeps its parent's snapshot)",
		    ScenarioDelta::LogicalName(base_table), scenario_name, dc::Str(base_catalog.GetName()),
		    Timestamp::ToString(pinned_at), dc::Str(base_catalog.GetName()));
	}
}

unique_ptr<GlobalTableFunctionState> LifecycleInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<LifecycleGlobalState>();
}

unique_ptr<FunctionData> LifecycleBind(TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                       dc::BindNames &names) {
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
	names.emplace_back("success");
	return std::move(result);
}

unique_ptr<FunctionData> LifecycleBindWrapper(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, dc::BindNames &names) {
	return LifecycleBind(input, return_types, names);
}

//! Run the verb exactly once, emitting a single 'true' row.
template <void (*VERB)(ClientContext &, const LifecycleBindData &)>
void LifecycleExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<LifecycleGlobalState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	auto &bind_data = data.bind_data->Cast<LifecycleBindData>();
	VERB(context, bind_data);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	output.SetChildCardinality(1);
	state.done = true;
}

//===----------------------------------------------------------------------===//
// Verbs
//===----------------------------------------------------------------------===//

//! Collect every user table of every mirrored schema of a base catalog
void CollectBaseTables(ClientContext &context, Catalog &base_source, vector<reference<TableCatalogEntry>> &out) {
	base_source.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		// built-in schemas (main) are marked internal, so filter by name only
		if (schema.name == ScenarioRegistry::SCHEMA_NAME) {
			return;
		}
		schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
			if (table_entry.type != CatalogType::TABLE_ENTRY || table_entry.internal) {
				return;
			}
			if (StringUtil::StartsWith(dc::Str(table_entry.name), "_scenario_")) {
				return; // legacy v0.1 metadata
			}
			out.push_back(table_entry.Cast<TableCatalogEntry>());
		});
	});
}

//! Names of the PRIMARY KEY columns of a delta table (payload columns only)
vector<string> DeltaKeyNames(TableCatalogEntry &delta) {
	vector<string> names;
	for (auto delta_col : ScenarioDelta::GetPKColumns(delta)) {
		names.push_back(dc::Str(delta.GetColumn(LogicalIndex(delta_col)).Name()));
	}
	return names;
}

void ScenarioCreateVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	ScenarioRegistry::EnsureExists(context, host_catalog);
	if (ScenarioRegistry::Lookup(context, host_catalog, bind_data.name)) {
		throw InvalidInputException("Scenario '%s' already exists", dc::Str(bind_data.name));
	}
	// Branching: resolve the parent first
	unique_ptr<ScenarioRegistryEntry> parent;
	if (!bind_data.from_scenario.empty()) {
		parent = ScenarioRegistry::Lookup(context, host_catalog, bind_data.from_scenario);
		if (!parent) {
			throw InvalidInputException("Cannot branch from scenario '%s': not found", bind_data.from_scenario);
		}
	}

	// Phase 4: cross-catalog base (e.g. a DuckLake attach). Branches inherit
	// the parent's base catalog - and its snapshot pin (created_at), so the
	// child's view equals the parent's at branch time (a fresh pin would
	// leak base churn that happened between the two creations).
	string effective_base = bind_data.base_catalog;
	if (parent && !parent->base_catalog.empty()) {
		effective_base = parent->base_catalog;
	}
	optional_ptr<Catalog> base_catalog;
	if (!effective_base.empty()) {
		base_catalog = Catalog::GetCatalogEntry(context, dc::Ident(effective_base));
		if (!base_catalog) {
			throw InvalidInputException("Base catalog '%s' is not attached", effective_base);
		}
		if (base_catalog->GetCatalogType() == "scenario") {
			throw InvalidInputException("A scenario cannot use another scenario as its base - branch instead "
			                            "(from_scenario := '...')");
		}
	}

	// Delta tables are created lazily, by each table's first write (#34), so
	// a plain delta-mode scenario costs one registry row whatever the size of
	// the schema. The base tables are only enumerated when something needs
	// them: declared keys (validated up front, typo safety), materialized
	// copies, or the parent deltas a branch inherits.
	auto &base_source = base_catalog ? *base_catalog : host_catalog;
	vector<reference<TableCatalogEntry>> base_tables;
	if (!bind_data.key_columns.empty() || bind_data.mode == "materialized" || parent) {
		CollectBaseTables(context, base_source, base_tables);
	}
	for (auto &declared : bind_data.key_columns) {
		bool table_found = false;
		for (auto &base_table : base_tables) {
			if (ScenarioDelta::LogicalName(base_table.get()) != declared.first) {
				continue;
			}
			table_found = true;
			ValidateDeclaredKey(base_table.get(), declared.second);
			if (parent && base_catalog) {
				// a branch of a lake-based parent keeps the parent's pin
				ValidateDeclaredKeyPinned(context, *base_catalog, base_table.get(), parent->name,
				                          parent->created_at);
			}
		}
		if (!table_found) {
			throw InvalidInputException("key_columns declared for unknown table '%s'", declared.first);
		}
	}
	// The row identity a branch records: the parent's declared keys, plus the
	// child's own declarations for tables the parent has not pinned yet. A
	// branch inherits the parent's identity (its delta rows are copied
	// verbatim, so a different child shape would corrupt the changelog).
	auto declared_keys = parent ? ScenarioRegistry::ListDeclaredKeys(context, host_catalog, parent->scenario_id)
	                            : map<string, vector<string>>();
	for (auto &declared : bind_data.key_columns) {
		bool differs = false;
		auto inherited = declared_keys.find(declared.first);
		if (inherited != declared_keys.end()) {
			differs = inherited->second != declared.second;
		} else if (parent) {
			auto parent_delta =
			    ScenarioDelta::TryGetDeltaTable(context, host_catalog, parent->scenario_id, declared.first);
			// a keyed delta carries the declared key as its PK; a bag delta
			// (no PK) is keyless, which no declaration matches
			differs = parent_delta && DeltaKeyNames(*parent_delta) != declared.second;
		}
		if (differs) {
			throw InvalidInputException(
			    "key_columns for table '%s' differ from the parent scenario's declared key - branches "
			    "inherit the parent's row identity",
			    declared.first);
		}
		declared_keys[declared.first] = declared.second;
	}

	ScenarioRegistryEntry entry;
	entry.scenario_id = ScenarioRegistry::NextId(context, host_catalog);
	entry.name = bind_data.name;
	entry.mode = bind_data.mode;
	entry.parent_id = parent ? parent->scenario_id : -1;
	entry.created_at = (parent && !parent->base_catalog.empty()) ? parent->created_at
	                                                             : Timestamp::GetCurrentTimestamp();
	entry.description = bind_data.description;
	entry.has_description = bind_data.has_description;
	entry.base_catalog = effective_base;
	ScenarioRegistry::Insert(context, host_catalog, entry);
	for (auto &declared : declared_keys) {
		ScenarioRegistry::InsertDeclaredKey(context, host_catalog, entry.scenario_id, entry.name, declared.first,
		                                    declared.second);
	}
	if (bind_data.mode == "materialized" || parent) {
		MetaTransaction::Get(context).ModifyDatabase(host_catalog.GetAttached(),
		                                             DatabaseModificationType::CREATE_CATALOG_ENTRY |
		                                                 DatabaseModificationType::INSERT_DATA);
	}

	for (auto &base_table : base_tables) {
		auto logical_name = ScenarioDelta::LogicalName(base_table.get());
		if (bind_data.mode == "materialized") {
			// the point of materialized mode: a full copy, taken now
			ScenarioDelta::CreateMatTable(context, host_catalog, entry.scenario_id, base_table.get());
		}
		if (!parent) {
			continue;
		}
		auto parent_delta = ScenarioDelta::TryGetDeltaTable(context, host_catalog, parent->scenario_id, logical_name);
		if (!parent_delta) {
			continue; // the parent never wrote it: nothing to inherit
		}
		// inherit the parent's modifications by copying its delta rows into a
		// delta of the same shape (cheap - deltas are small by invariant). A
		// pre-v0.4.1 keyless parent delta (no _count) is copied with _count =
		// 1 per row and itself left as is: rewriting it here could break a
		// concurrent writer of the parent
		// (the parent delta's shape is the identity: its PK, or none for a bag)
		auto inherited_keys = DeltaKeyNames(*parent_delta);
		auto &delta = ScenarioDelta::EnsureDeltaTable(context, host_catalog, entry.scenario_id, base_table.get(),
		                                              &inherited_keys);
		ScenarioDelta::CopyTableData(context, *parent_delta, delta);
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
			                            scenario_name, dc::Str(catalog.GetName()));
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
		if (entry.type == CatalogType::TABLE_ENTRY && (StringUtil::StartsWith(dc::Str(entry.name), delta_prefix) ||
		                                               StringUtil::StartsWith(dc::Str(entry.name), mat_prefix))) {
			to_drop.push_back(dc::Str(entry.name));
		}
	});
	for (auto &table_name : to_drop) {
		DropInfo info;
		info.type = CatalogType::TABLE_ENTRY;
		dc::SetQualifiedName(info, dc::Str(host_catalog.GetName()), ScenarioRegistry::SCHEMA_NAME, table_name);
		host_catalog.DropEntry(context, info);
	}
}

void ScenarioDropVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", dc::Str(bind_data.name));
	}
	ThrowIfAttached(context, bind_data.name);
	if (ScenarioRegistry::HasChildren(context, host_catalog, entry->scenario_id)) {
		throw ConstraintException("Cannot drop scenario '%s': other scenarios branch from it", dc::Str(bind_data.name));
	}
	// Mark the host writable before dropping catalog entries
	MetaTransaction::Get(context).ModifyDatabase(host_catalog.GetAttached(),
	                                             DatabaseModificationType::DROP_CATALOG_ENTRY |
	                                                 DatabaseModificationType::DELETE_DATA);
	DropDeltaTables(context, host_catalog, entry->scenario_id);
	ScenarioRegistry::DeleteDeclaredKeys(context, host_catalog, entry->scenario_id);
	ScenarioRegistry::Delete(context, host_catalog, entry->scenario_id);
}

void ScenarioFreezeVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", dc::Str(bind_data.name));
	}
	ScenarioRegistry::SetFrozen(context, host_catalog, entry->scenario_id, true);
}

void ScenarioUnfreezeVerb(ClientContext &context, const LifecycleBindData &bind_data) {
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", dc::Str(bind_data.name));
	}
	if (entry->has_merged_at) {
		throw InvalidInputException("Scenario '%s' was already merged back into the base - its changelog is "
		                            "closed. Create a new scenario instead",
		                            dc::Str(bind_data.name));
	}
	ScenarioRegistry::SetFrozen(context, host_catalog, entry->scenario_id, false);
}

//===----------------------------------------------------------------------===//
// scenario_refresh: deprecated (#34). Delta tables are created by each
// table's first write, so tables added to the base after scenario_create are
// writable without it. What remains is declaring the row identity of keyless
// tables the scenario does not track yet (no declared key, no delta).
//===----------------------------------------------------------------------===//

struct RefreshState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> ScenarioRefreshBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, dc::BindNames &names) {
	auto result = make_uniq<LifecycleBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("Scenario name cannot be NULL");
	}
	result->name = input.inputs[0].GetValue<string>();
	// key_columns := declares identity for keyless tables the scenario does
	// not track yet (e.g. tables that appeared after scenario_create, which
	// rejects declarations for then-unknown tables)
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
		output.SetChildCardinality(0);
		return;
	}
	state.done = true;
	auto &bind_data = data.bind_data->Cast<LifecycleBindData>();
	auto &host_catalog = GetHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", dc::Str(bind_data.name));
	}
	if (entry->frozen) {
		throw InvalidInputException("Cannot refresh scenario '%s': it is frozen. Unfreeze it first",
		                            dc::Str(bind_data.name));
	}
	if (entry->mode == "materialized") {
		throw InvalidInputException(
		    "Cannot refresh scenario '%s': materialized scenarios are point-in-time copies and do not pick up "
		    "new base tables",
		    dc::Str(bind_data.name));
	}
	idx_t refreshed = 0;
	if (!bind_data.key_columns.empty()) {
		// Phase 4: honor a cross-catalog base
		optional_ptr<Catalog> base_source = &host_catalog;
		if (!entry->base_catalog.empty()) {
			base_source = Catalog::GetCatalogEntry(context, dc::Ident(entry->base_catalog));
			if (!base_source) {
				throw InvalidInputException("Scenario '%s': base catalog '%s' is not attached", dc::Str(bind_data.name),
				                            entry->base_catalog);
			}
		}
		vector<reference<TableCatalogEntry>> base_tables;
		CollectBaseTables(context, *base_source, base_tables);
		// validate up front: a declaration must name a base table the scenario
		// does not track yet (identity cannot change once declared or written).
		// The one exception: a declared key whose column has since vanished
		// from the base (renamed or dropped) and that no delta was built from
		// may be re-declared - nothing depends on it yet
		set<string> replaced;
		for (auto &declared : bind_data.key_columns) {
			bool table_found = false;
			for (auto &base_table : base_tables) {
				if (ScenarioDelta::LogicalName(base_table.get()) != declared.first) {
					continue;
				}
				table_found = true;
				ValidateDeclaredKey(base_table.get(), declared.second);
				ValidateDeclaredKeyPinned(context, *base_source, base_table.get(), bind_data.name, entry->created_at);
				if (ScenarioDelta::TryGetDeltaTable(context, host_catalog, entry->scenario_id, declared.first)) {
					throw InvalidInputException("key_columns declared for table '%s', but scenario '%s' already "
					                            "tracks it - row identity cannot be changed after the fact",
					                            declared.first, dc::Str(bind_data.name));
				}
				vector<string> recorded;
				if (ScenarioRegistry::LookupDeclaredKey(context, host_catalog, entry->scenario_id, declared.first,
				                                        recorded)) {
					bool broken = false;
					for (auto &column : recorded) {
						broken = broken || !base_table.get().ColumnExists(dc::Ident(column));
					}
					if (!broken) {
						throw InvalidInputException("key_columns declared for table '%s', but scenario '%s' already "
						                            "tracks it - row identity cannot be changed after the fact",
						                            declared.first, dc::Str(bind_data.name));
					}
					replaced.insert(declared.first);
				}
			}
			if (!table_found) {
				throw InvalidInputException("key_columns declared for unknown table '%s'", declared.first);
			}
		}
		for (auto &declared : bind_data.key_columns) {
			if (replaced.count(declared.first)) {
				ScenarioRegistry::ReplaceDeclaredKey(context, host_catalog, entry->scenario_id, declared.first,
				                                     declared.second);
			} else {
				ScenarioRegistry::InsertDeclaredKey(context, host_catalog, entry->scenario_id, bind_data.name,
				                                    declared.first, declared.second);
			}
			refreshed++;
		}
	}
	output.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(refreshed)));
	output.SetChildCardinality(1);
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
                                          vector<LogicalType> &return_types, dc::BindNames &names) {
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
	output.SetChildCardinality(count);
}

TableFunctionSet MakeVerbSet(const string &name, table_function_t function, bool is_create) {
	TableFunctionSet set(dc::FunctionName(name));
	auto add_create_parameters = [](TableFunction &fn) {
		dc::AddNamedParameter(fn, "mode", LogicalType::VARCHAR);
		dc::AddNamedParameter(fn, "from_scenario", LogicalType::VARCHAR);
		dc::AddNamedParameter(fn, "base", LogicalType::VARCHAR);
		dc::AddNamedParameter(fn, "key_columns",
		                      LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)));
	};
	TableFunction one_arg({LogicalType::VARCHAR}, function, LifecycleBindWrapper, LifecycleInit);
	dc::NameParameters(one_arg, {"scenario"});
	if (is_create) {
		add_create_parameters(one_arg);
	}
	set.AddFunction(one_arg);
	if (is_create) {
		TableFunction two_arg({LogicalType::VARCHAR, LogicalType::VARCHAR}, function, LifecycleBindWrapper,
		                      LifecycleInit);
		dc::NameParameters(two_arg, {"scenario", "description"});
		add_create_parameters(two_arg);
		set.AddFunction(two_arg);
	}
	return set;
}

} // namespace

namespace {

// Descriptions are lifted from the function table in README.md so the two cannot drift
// apart silently.
//
// On parameter_names: duckdb_functions() replaces the WHOLE parameter list once
// parameter_names is non-empty, walking positional arguments FOLLOWED BY named ones and
// padding any shortfall with "col1", "col2". So a function with named parameters must
// list those too, or naming its positional argument would overwrite them. The named
// parameters come out in map order, which is not sorted and not guaranteed stable --
// test/sql/scenario_function_docs.test pins the rendered list so a reordering fails
// loudly instead of silently misnaming every argument.
FunctionDescription ScenarioDoc(string description, vector<string> examples,
                                vector<string> parameter_names = {},
                                vector<LogicalType> parameter_types = {}) {
	FunctionDescription desc;
	desc.description = std::move(description);
	desc.examples = std::move(examples);
	desc.parameter_names = std::move(parameter_names);
	desc.parameter_types = std::move(parameter_types);
	desc.categories = {"scenario"};
	return desc;
}

//! Register a lifecycle verb that takes a single scenario name and nothing else.
void RegisterVerb(ExtensionLoader &loader, TableFunctionSet set, string description, string example) {
	CreateTableFunctionInfo info(std::move(set));
	info.descriptions.push_back(
	    ScenarioDoc(std::move(description), {std::move(example)}, {"scenario"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace

void ScenarioLifecycle::RegisterFunctions(ExtensionLoader &loader) {
	{
		// Two overloads that mean different things -- with and without a description --
		// so each gets its own FunctionDescription keyed by parameter_types. Note those
		// types cover the POSITIONAL arguments only: that is the list DuckDB matches
		// against, even though the name list above also spans the named parameters.
		CreateTableFunctionInfo info(MakeVerbSet("scenario_create", LifecycleExecute<ScenarioCreateVerb>, true));
		const string create_doc =
		    "Register a scenario. 'materialized' mode copies every base table, the default 'delta' mode "
		    "stores only what changed; from_scenario branches off an existing scenario; base uses another "
		    "attached catalog; key_columns declares row identity for tables without a primary key.";
		info.descriptions.push_back(ScenarioDoc(
		    create_doc, {"CALL scenario_create('optimistic');"},
		    {"scenario", "key_columns", "base", "from_scenario", "mode"}, {LogicalType::VARCHAR}));
		info.descriptions.push_back(ScenarioDoc(
		    create_doc, {"CALL scenario_create('price_increase', 'Analyzing 10% price increase impact');"},
		    {"scenario", "description", "key_columns", "base", "from_scenario", "mode"},
		    {LogicalType::VARCHAR, LogicalType::VARCHAR}));
		loader.RegisterFunction(std::move(info));
	}
	RegisterVerb(loader, MakeVerbSet("scenario_drop", LifecycleExecute<ScenarioDropVerb>, false),
	             "Remove a scenario and its delta or materialized tables. Refuses while the scenario is "
	             "attached or while branches of it still exist.",
	             "CALL scenario_drop('price_increase_eu');");
	RegisterVerb(loader, MakeVerbSet("scenario_freeze", LifecycleExecute<ScenarioFreezeVerb>, false),
	             "Reject writes to a scenario while leaving reads working; a frozen materialized scenario "
	             "is a snapshot.",
	             "CALL scenario_freeze('q2_approved');");
	RegisterVerb(loader, MakeVerbSet("scenario_unfreeze", LifecycleExecute<ScenarioUnfreezeVerb>, false),
	             "Allow writes to a scenario again, reversing scenario_freeze.",
	             "CALL scenario_unfreeze('q2_approved');");
	// Registry v2 listing (replaces legacy scenario_list in v0.2)
	{
		CreateTableFunctionInfo info(TableFunction("scenario_list", {},
		                                           DATAZOO_GUARD(ANOFOX_SCENARIO_BANNER, ScenarioListExecute),
		                                           DATAZOO_GUARD(ANOFOX_SCENARIO_BANNER, ScenarioListBind),
		                                           ScenarioListInit));
		info.descriptions.push_back(ScenarioDoc(
		    "List every registered scenario as (scenario_id, name, mode, frozen, parent, created_at, description).",
		    {"SELECT * FROM scenario_list();"}));
		loader.RegisterFunction(std::move(info));
	}
	// Deprecated (#34): only declares keys for untracked keyless tables
	{
		TableFunction refresh("scenario_refresh", {LogicalType::VARCHAR}, DATAZOO_GUARD(ANOFOX_SCENARIO_BANNER, ScenarioRefreshExecute),
		                      DATAZOO_GUARD(ANOFOX_SCENARIO_BANNER, ScenarioRefreshBind),
		                      ScenarioRefreshInit);
		dc::NameParameters(refresh, {"scenario"});
		dc::AddNamedParameter(refresh, "key_columns",
		                      LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)));
		CreateTableFunctionInfo info(std::move(refresh));
		info.descriptions.push_back(ScenarioDoc(
		    "Deprecated: tables added to the base after scenario_create are writable without it. Only declares "
		    "key_columns for keyless tables the scenario does not track yet.",
		    {"SELECT * FROM scenario_refresh('price_increase');"}, {"scenario", "key_columns"}));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace duckdb
