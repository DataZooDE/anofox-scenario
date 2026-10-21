#include "catalog/scenario_registry.hpp"

#include "catalog/scenario_delta.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

// C++11: out-of-line definitions for static constexpr members
constexpr const char *ScenarioRegistry::SCHEMA_NAME;
constexpr const char *ScenarioRegistry::TABLE_NAME;
constexpr const char *ScenarioRegistry::SEQUENCE_NAME;
constexpr const char *ScenarioRegistry::TABLES_TABLE_NAME;

namespace {

// Registry column positions -- keep in sync with EnsureExists below.
constexpr idx_t REG_COL_SCENARIO_ID = 0;
constexpr idx_t REG_COL_NAME = 1;
constexpr idx_t REG_COL_MODE = 2;
constexpr idx_t REG_COL_FROZEN = 3;
constexpr idx_t REG_COL_PARENT_ID = 4;
constexpr idx_t REG_COL_BASE_SNAPSHOT_ID = 5;
constexpr idx_t REG_COL_CREATED_AT = 6;
constexpr idx_t REG_COL_MERGED_AT = 7;
constexpr idx_t REG_COL_DESCRIPTION = 8;
constexpr idx_t REG_COL_BASE_CATALOG = 9;
constexpr idx_t REG_COL_COUNT = 10;

ScenarioRegistryEntry EntryFromChunk(DataChunk &chunk, idx_t row) {
	ScenarioRegistryEntry entry;
	entry.scenario_id = chunk.GetValue(REG_COL_SCENARIO_ID, row).GetValue<int64_t>();
	entry.name = chunk.GetValue(REG_COL_NAME, row).GetValue<string>();
	entry.mode = chunk.GetValue(REG_COL_MODE, row).GetValue<string>();
	entry.frozen = chunk.GetValue(REG_COL_FROZEN, row).GetValue<bool>();
	auto parent = chunk.GetValue(REG_COL_PARENT_ID, row);
	entry.parent_id = parent.IsNull() ? -1 : parent.GetValue<int64_t>();
	auto snapshot = chunk.GetValue(REG_COL_BASE_SNAPSHOT_ID, row);
	entry.base_snapshot_id = snapshot.IsNull() ? -1 : snapshot.GetValue<int64_t>();
	entry.created_at = chunk.GetValue(REG_COL_CREATED_AT, row).GetValue<timestamp_t>();
	auto merged = chunk.GetValue(REG_COL_MERGED_AT, row);
	entry.has_merged_at = !merged.IsNull();
	if (entry.has_merged_at) {
		entry.merged_at = merged.GetValue<timestamp_t>();
	}
	auto desc = chunk.GetValue(REG_COL_DESCRIPTION, row);
	entry.has_description = !desc.IsNull();
	if (entry.has_description) {
		entry.description = desc.GetValue<string>();
	}
	auto base_catalog = chunk.GetValue(REG_COL_BASE_CATALOG, row);
	if (!base_catalog.IsNull()) {
		entry.base_catalog = base_catalog.GetValue<string>();
	}
	return entry;
}

//! Scan all registry rows (all columns), invoking the callback per row.
//! The callback returns false to stop scanning.
void ScanRegistry(ClientContext &context, Catalog &host_catalog, DuckTableEntry &table,
                  const std::function<bool(DataChunk &, idx_t)> &callback) {
	vector<StorageIndex> column_ids;
	for (idx_t i = 0; i < REG_COL_COUNT; i++) {
		column_ids.emplace_back(i);
	}
	ScenarioDelta::ScanTableRows(context, table, std::move(column_ids), table.GetStorage().GetTypes(), callback);
}

//! Scan (rowid, scenario_id) pairs; returns the rowid for the given scenario
//! id or -1 when not found.
row_t FindRowId(ClientContext &context, Catalog &host_catalog, DuckTableEntry &table, int64_t scenario_id) {
	vector<StorageIndex> column_ids;
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	column_ids.emplace_back(REG_COL_SCENARIO_ID);
	row_t result = -1;
	ScenarioDelta::ScanTableRows(context, table, std::move(column_ids), {LogicalType::ROW_TYPE, LogicalType::BIGINT},
	                             [&](DataChunk &chunk, idx_t row) {
		                             if (chunk.GetValue(1, row).GetValue<int64_t>() == scenario_id) {
			                             result = chunk.GetValue(0, row).GetValue<row_t>();
			                             return false;
		                             }
		                             return true;
	                             });
	return result;
}

void MarkHostModified(ClientContext &context, Catalog &host_catalog, DatabaseModificationType type) {
	MetaTransaction::Get(context).ModifyDatabase(host_catalog.GetAttached(), type);
}

// scenario_tables column positions -- keep in sync with EnsureTablesTable
constexpr idx_t TAB_COL_SCENARIO_ID = 0;
constexpr idx_t TAB_COL_TABLE_NAME = 1;
constexpr idx_t TAB_COL_KEY_COLUMNS = 2;
constexpr idx_t TAB_COL_COUNT = 3;

//! Scan (rowid, scenario_id, table_name, key_columns) of every scenario_tables
//! row of one scenario
void ScanDeclaredKeys(ClientContext &context, DuckTableEntry &table, int64_t scenario_id,
                      const std::function<bool(row_t, const string &, vector<string>)> &callback) {
	vector<StorageIndex> column_ids;
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	for (idx_t i = 0; i < TAB_COL_COUNT; i++) {
		column_ids.emplace_back(i);
	}
	vector<LogicalType> types {LogicalType::ROW_TYPE};
	for (auto &type : table.GetStorage().GetTypes()) {
		types.push_back(type);
	}
	ScenarioDelta::ScanTableRows(context, table, std::move(column_ids), types, [&](DataChunk &chunk, idx_t row) {
		if (chunk.GetValue(1 + TAB_COL_SCENARIO_ID, row).GetValue<int64_t>() != scenario_id) {
			return true;
		}
		vector<string> key_columns;
		auto key_list = chunk.GetValue(1 + TAB_COL_KEY_COLUMNS, row);
		for (auto &column : ListValue::GetChildren(key_list)) {
			key_columns.push_back(column.GetValue<string>());
		}
		return callback(chunk.GetValue(0, row).GetValue<row_t>(),
		                chunk.GetValue(1 + TAB_COL_TABLE_NAME, row).GetValue<string>(), std::move(key_columns));
	});
}

} // namespace

optional_ptr<DuckTableEntry> ScenarioRegistry::GetRegistryTable(ClientContext &context, Catalog &host_catalog) {
	auto schema = host_catalog.GetSchema(context, SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry = schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, TABLE_NAME);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

DuckTableEntry &ScenarioRegistry::GetRegistryTableOrThrow(ClientContext &context, Catalog &host_catalog) {
	auto table = GetRegistryTable(context, host_catalog);
	if (!table) {
		throw InternalException("anofox_scenario: registry table %s.%s not found", SCHEMA_NAME, TABLE_NAME);
	}
	return *table;
}

optional_ptr<DuckTableEntry> ScenarioRegistry::GetTablesTable(ClientContext &context, Catalog &host_catalog) {
	auto schema = host_catalog.GetSchema(context, SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry =
	    schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, TABLES_TABLE_NAME);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

DuckTableEntry &ScenarioRegistry::EnsureTablesTable(ClientContext &context, Catalog &host_catalog,
                                                    const string &scenario_name) {
	auto existing = GetTablesTable(context, host_catalog);
	if (existing) {
		return *existing;
	}
	EnsureExists(context, host_catalog);
	MarkHostModified(context, host_catalog, DatabaseModificationType::CREATE_CATALOG_ENTRY);
	// column order pinned by the TAB_COL_* constants above
	auto info = make_uniq<CreateTableInfo>();
	dc::SetQualifiedName(*info, dc::Str(host_catalog.GetName()), SCHEMA_NAME, TABLES_TABLE_NAME);
	info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	info->columns.AddColumn(ColumnDefinition("scenario_id", LogicalType::BIGINT));
	info->columns.AddColumn(ColumnDefinition("table_name", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("key_columns", LogicalType::LIST(LogicalType::VARCHAR)));
	info->constraints.push_back(dc::MakeUniqueConstraint({"scenario_id", "table_name"}, true));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(TAB_COL_KEY_COLUMNS)));
	try {
		host_catalog.CreateTable(context, std::move(info));
	} catch (std::exception &ex) {
		if (!scenario_compat::IsException(ex, ExceptionType::TRANSACTION)) {
			throw;
		}
		// a catalog write-write conflict: another transaction declared the
		// first key on this host concurrently (uncommitted, or committed
		// after this transaction started) and created the table itself
		throw TransactionException(
		    "Cannot declare key_columns for scenario '%s': another transaction concurrently declared the first "
		    "key_columns on this database and created %s.%s. ROLLBACK and retry the transaction",
		    scenario_name, SCHEMA_NAME, TABLES_TABLE_NAME);
	}
	auto created = GetTablesTable(context, host_catalog);
	if (!created) {
		throw InternalException("anofox_scenario: failed to create %s.%s", SCHEMA_NAME, TABLES_TABLE_NAME);
	}
	return *created;
}

RebindQueryInfo ScenarioClientState::OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared,
                                                       PreparedStatementMode mode) {
	switch (prepared.statement_type) {
	case StatementType::INSERT_STATEMENT:
	case StatementType::UPDATE_STATEMENT:
	case StatementType::DELETE_STATEMENT:
	case StatementType::MERGE_INTO_STATEMENT:
		break;
	default:
		// e.g. COPY FROM DATABASE ... TO <scenario> keeps registering the
		// scenario itself (see ScenarioCatalog::MarkHostWrite)
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	auto &properties = prepared.properties;
	auto &manager = DatabaseManager::Get(context);
	for (auto entry = properties.modified_databases.begin(); entry != properties.modified_databases.end();) {
		auto database = manager.GetDatabase(context, entry->first);
		if (database && database->GetCatalog().GetCatalogType() == "scenario") {
			properties.read_databases.emplace(entry->first, entry->second.identity);
			entry = properties.modified_databases.erase(entry);
		} else {
			++entry;
		}
	}
	return RebindQueryInfo::DO_NOT_REBIND;
}

ScenarioClientState &ScenarioClientState::Get(ClientContext &context) {
	// the registered state lives as long as the connection
	return *context.registered_state->GetOrCreate<ScenarioClientState>("anofox_scenario_client_state");
}

void ScenarioClientState::ResetIfStale(ClientContext &context) {
	auto transaction_id = MetaTransaction::Get(context).global_transaction_id;
	if (cache_transaction != transaction_id) {
		cache.clear();
		cache_transaction = transaction_id;
	}
}

shared_ptr<const ScenarioClientState::DeclaredKeys>
ScenarioClientState::GetDeclaredKeys(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                     const std::function<DeclaredKeys()> &load) {
	auto key = make_pair(idx_t(reinterpret_cast<uintptr_t>(&host_catalog.GetAttached())), scenario_id);
	{
		lock_guard<mutex> guard(cache_lock);
		ResetIfStale(context);
		auto entry = cache.find(key);
		if (entry != cache.end()) {
			return entry->second;
		}
	}
	// load outside the lock: the scan may be slow, and a duplicate load by
	// a sibling thread of the same transaction yields the same rows
	auto loaded = make_shared_ptr<const DeclaredKeys>(load());
	lock_guard<mutex> guard(cache_lock);
	ResetIfStale(context);
	cache[key] = loaded;
	return loaded;
}

void ScenarioClientState::Invalidate(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto key = make_pair(idx_t(reinterpret_cast<uintptr_t>(&host_catalog.GetAttached())), scenario_id);
	lock_guard<mutex> guard(cache_lock);
	ResetIfStale(context);
	cache.erase(key);
}

bool ScenarioRegistry::LookupDeclaredKey(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                         const string &table_name, vector<string> &key_columns) {
	auto declared = LoadDeclaredKeys(context, host_catalog, scenario_id);
	auto entry = declared->find(table_name);
	if (entry == declared->end()) {
		return false;
	}
	key_columns = entry->second;
	return true;
}

map<string, vector<string>> ScenarioRegistry::ListDeclaredKeys(ClientContext &context, Catalog &host_catalog,
                                                              int64_t scenario_id) {
	return *LoadDeclaredKeys(context, host_catalog, scenario_id);
}

shared_ptr<const map<string, vector<string>>>
ScenarioRegistry::LoadDeclaredKeys(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	return ScenarioClientState::Get(context).GetDeclaredKeys(context, host_catalog, scenario_id, [&]() {
		map<string, vector<string>> result;
		auto table = GetTablesTable(context, host_catalog);
		if (!table) {
			return result;
		}
		ScanDeclaredKeys(context, *table, scenario_id, [&](row_t, const string &name, vector<string> keys) {
			result[name] = std::move(keys);
			return true;
		});
		return result;
	});
}

void ScenarioRegistry::InsertDeclaredKey(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                         const string &scenario_name, const string &table_name,
                                         const vector<string> &key_columns) {
	auto &table = EnsureTablesTable(context, host_catalog, scenario_name);
	ScenarioClientState::Get(context).Invalidate(context, host_catalog, scenario_id);
	MarkHostModified(context, host_catalog, DatabaseModificationType::INSERT_DATA);
	auto &data_table = table.GetStorage();
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), data_table.GetTypes());
	vector<Value> keys;
	for (auto &column : key_columns) {
		keys.emplace_back(column);
	}
	chunk.SetValue(TAB_COL_SCENARIO_ID, 0, Value::BIGINT(scenario_id));
	chunk.SetValue(TAB_COL_TABLE_NAME, 0, Value(table_name));
	chunk.SetValue(TAB_COL_KEY_COLUMNS, 0, Value::LIST(LogicalType::VARCHAR, std::move(keys)));
	chunk.SetChildCardinality(1);
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	data_table.LocalAppend(table, context, chunk, bound_constraints);
}

void ScenarioRegistry::DeleteDeclaredKeys(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto table = GetTablesTable(context, host_catalog);
	if (!table) {
		return;
	}
	vector<row_t> row_ids;
	ScanDeclaredKeys(context, *table, scenario_id, [&](row_t row_id, const string &, vector<string>) {
		row_ids.push_back(row_id);
		return true;
	});
	DeleteDeclaredKeyRows(context, host_catalog, *table, row_ids);
	ScenarioClientState::Get(context).Invalidate(context, host_catalog, scenario_id);
}

void ScenarioRegistry::ReplaceDeclaredKey(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                          const string &table_name, const vector<string> &key_columns) {
	auto table = GetTablesTable(context, host_catalog);
	if (!table) {
		throw InternalException("anofox_scenario: %s.%s not found", SCHEMA_NAME, TABLES_TABLE_NAME);
	}
	row_t old_row_id = -1;
	vector<string> old_keys;
	ScanDeclaredKeys(context, *table, scenario_id, [&](row_t row_id, const string &name, vector<string> keys) {
		if (name != table_name) {
			return true;
		}
		old_row_id = row_id;
		old_keys = std::move(keys);
		return false;
	});
	if (old_row_id < 0) {
		throw InternalException("anofox_scenario: no declared key recorded for '%s'", table_name);
	}
	// LIST columns cannot be updated in place: delete the row and append its
	// replacement. The (scenario_id, table_name) PK of the deleted row goes
	// into the transaction's delete index first, so the re-insert of the
	// same key does not conflict with it
	MarkHostModified(context, host_catalog, DatabaseModificationType::DELETE_DATA |
	                                            DatabaseModificationType::INSERT_DATA);
	auto &data_table = table->GetStorage();
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(*table);
	auto make_row = [&](DataChunk &chunk, const vector<string> &keys) {
		chunk.Initialize(Allocator::Get(context), data_table.GetTypes());
		vector<Value> key_values;
		for (auto &column : keys) {
			key_values.emplace_back(column);
		}
		chunk.SetValue(TAB_COL_SCENARIO_ID, 0, Value::BIGINT(scenario_id));
		chunk.SetValue(TAB_COL_TABLE_NAME, 0, Value(table_name));
		chunk.SetValue(TAB_COL_KEY_COLUMNS, 0, Value::LIST(LogicalType::VARCHAR, std::move(key_values)));
		chunk.SetChildCardinality(1);
	};
	DataChunk old_row, new_row;
	make_row(old_row, old_keys);
	make_row(new_row, key_columns);
	Vector row_ids(LogicalType::ROW_TYPE);
	row_ids.SetValue(0, Value::BIGINT(old_row_id));
	{
		auto delete_state = data_table.InitializeDelete(*table, context, bound_constraints);
		dc::SetVectorSize(row_ids, 1);
		scenario_compat::TableDelete(data_table, *delete_state, context, *table, row_ids, 1);
	}
	LocalAppendState append_state;
	data_table.InitializeLocalAppend(append_state, *table, context, bound_constraints);
	append_state.storage->AppendToDeleteIndexes(row_ids, old_row);
	scenario_compat::TableLocalAppend(data_table, append_state, *table, context, new_row, false);
	data_table.FinalizeLocalAppend(append_state);
	ScenarioClientState::Get(context).Invalidate(context, host_catalog, scenario_id);
}

void ScenarioRegistry::DeleteDeclaredKeyRows(ClientContext &context, Catalog &host_catalog, DuckTableEntry &table,
                                             const vector<row_t> &row_ids) {
	if (row_ids.empty()) {
		return;
	}
	MarkHostModified(context, host_catalog, DatabaseModificationType::DELETE_DATA);
	auto &data_table = table.GetStorage();
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	auto delete_state = data_table.InitializeDelete(table, context, bound_constraints);
	for (idx_t offset = 0; offset < row_ids.size(); offset += STANDARD_VECTOR_SIZE) {
		auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, row_ids.size() - offset);
		Vector row_id_vector(LogicalType::ROW_TYPE);
		for (idx_t i = 0; i < count; i++) {
			row_id_vector.SetValue(i, Value::BIGINT(row_ids[offset + i]));
		}
		dc::SetVectorSize(row_id_vector, count);
		scenario_compat::TableDelete(data_table, *delete_state, context, table, row_id_vector, count);
	}
}

bool ScenarioRegistry::Exists(ClientContext &context, Catalog &host_catalog) {
	return GetRegistryTable(context, host_catalog) != nullptr;
}

void ScenarioRegistry::EnsureExists(ClientContext &context, Catalog &host_catalog) {
	if (Exists(context, host_catalog)) {
		return;
	}
	MarkHostModified(context, host_catalog, DatabaseModificationType::CREATE_CATALOG_ENTRY);

	// Schema
	CreateSchemaInfo schema_info;
	dc::SetCatalog(schema_info, dc::Str(host_catalog.GetName()));
	dc::SetSchema(schema_info, SCHEMA_NAME);
	schema_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	host_catalog.CreateSchema(context, schema_info);

	// Sequence for scenario ids
	auto seq_info = make_uniq<CreateSequenceInfo>();
	dc::SetQualifiedName(*seq_info, dc::Str(host_catalog.GetName()), SCHEMA_NAME, SEQUENCE_NAME);
	seq_info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	host_catalog.CreateSequence(context, *seq_info);

	// Registry table -- column order pinned by the REG_COL_* constants above.
	auto info = make_uniq<CreateTableInfo>();
	dc::SetQualifiedName(*info, dc::Str(host_catalog.GetName()), SCHEMA_NAME, TABLE_NAME);
	info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	info->columns.AddColumn(ColumnDefinition("scenario_id", LogicalType::BIGINT));
	info->columns.AddColumn(ColumnDefinition("name", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("mode", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("frozen", LogicalType::BOOLEAN));
	info->columns.AddColumn(ColumnDefinition("parent_id", LogicalType::BIGINT));
	info->columns.AddColumn(ColumnDefinition("base_snapshot_id", LogicalType::BIGINT));
	info->columns.AddColumn(ColumnDefinition("created_at", LogicalType::TIMESTAMP));
	info->columns.AddColumn(ColumnDefinition("merged_at", LogicalType::TIMESTAMP));
	info->columns.AddColumn(ColumnDefinition("description", LogicalType::VARCHAR));
	// Phase 4: NULL = the host default database; otherwise the attached
	// catalog whose tables serve as the scenario's base (e.g. a DuckLake)
	info->columns.AddColumn(ColumnDefinition("base_catalog", LogicalType::VARCHAR));
	info->constraints.push_back(make_uniq<UniqueConstraint>(LogicalIndex(REG_COL_SCENARIO_ID), true));
	info->constraints.push_back(make_uniq<UniqueConstraint>(LogicalIndex(REG_COL_NAME), false));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(REG_COL_NAME)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(REG_COL_MODE)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(REG_COL_FROZEN)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(REG_COL_CREATED_AT)));
	host_catalog.CreateTable(context, std::move(info));
}

unique_ptr<ScenarioRegistryEntry> ScenarioRegistry::Lookup(ClientContext &context, Catalog &host_catalog,
                                                           const string &name) {
	auto table = GetRegistryTable(context, host_catalog);
	if (!table) {
		return nullptr;
	}
	unique_ptr<ScenarioRegistryEntry> result;
	ScanRegistry(context, host_catalog, *table, [&](DataChunk &chunk, idx_t row) {
		if (chunk.GetValue(REG_COL_NAME, row).GetValue<string>() == name) {
			result = make_uniq<ScenarioRegistryEntry>(EntryFromChunk(chunk, row));
			return false;
		}
		return true;
	});
	return result;
}

unique_ptr<ScenarioRegistryEntry> ScenarioRegistry::LookupById(ClientContext &context, Catalog &host_catalog,
                                                               int64_t id) {
	auto table = GetRegistryTable(context, host_catalog);
	if (!table) {
		return nullptr;
	}
	unique_ptr<ScenarioRegistryEntry> result;
	ScanRegistry(context, host_catalog, *table, [&](DataChunk &chunk, idx_t row) {
		if (chunk.GetValue(REG_COL_SCENARIO_ID, row).GetValue<int64_t>() == id) {
			result = make_uniq<ScenarioRegistryEntry>(EntryFromChunk(chunk, row));
			return false;
		}
		return true;
	});
	return result;
}

vector<ScenarioRegistryEntry> ScenarioRegistry::List(ClientContext &context, Catalog &host_catalog) {
	vector<ScenarioRegistryEntry> result;
	auto table = GetRegistryTable(context, host_catalog);
	if (!table) {
		return result;
	}
	ScanRegistry(context, host_catalog, *table, [&](DataChunk &chunk, idx_t row) {
		result.push_back(EntryFromChunk(chunk, row));
		return true;
	});
	return result;
}

int64_t ScenarioRegistry::NextId(ClientContext &context, Catalog &host_catalog) {
	auto &seq = host_catalog.GetEntry<SequenceCatalogEntry>(context, SCHEMA_NAME, SEQUENCE_NAME);
	return seq.NextValue(DuckTransaction::Get(context, host_catalog));
}

void ScenarioRegistry::Insert(ClientContext &context, Catalog &host_catalog, const ScenarioRegistryEntry &entry) {
	auto &table = GetRegistryTableOrThrow(context, host_catalog);
	MarkHostModified(context, host_catalog, DatabaseModificationType::INSERT_DATA);

	auto &data_table = table.GetStorage();
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), data_table.GetTypes());
	chunk.SetValue(REG_COL_SCENARIO_ID, 0, Value::BIGINT(entry.scenario_id));
	chunk.SetValue(REG_COL_NAME, 0, Value(entry.name));
	chunk.SetValue(REG_COL_MODE, 0, Value(entry.mode));
	chunk.SetValue(REG_COL_FROZEN, 0, Value::BOOLEAN(entry.frozen));
	chunk.SetValue(REG_COL_PARENT_ID, 0,
	               entry.parent_id < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(entry.parent_id));
	chunk.SetValue(REG_COL_BASE_SNAPSHOT_ID, 0,
	               entry.base_snapshot_id < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(entry.base_snapshot_id));
	chunk.SetValue(REG_COL_CREATED_AT, 0, Value::TIMESTAMP(entry.created_at));
	chunk.SetValue(REG_COL_MERGED_AT, 0,
	               entry.has_merged_at ? Value::TIMESTAMP(entry.merged_at) : Value(LogicalType::TIMESTAMP));
	chunk.SetValue(REG_COL_DESCRIPTION, 0,
	               entry.has_description ? Value(entry.description) : Value(LogicalType::VARCHAR));
	chunk.SetValue(REG_COL_BASE_CATALOG, 0,
	               entry.base_catalog.empty() ? Value(LogicalType::VARCHAR) : Value(entry.base_catalog));
	chunk.SetChildCardinality(1);

	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	data_table.LocalAppend(table, context, chunk, bound_constraints);
}

void ScenarioRegistry::Delete(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto &table = GetRegistryTableOrThrow(context, host_catalog);
	auto row_id = FindRowId(context, host_catalog, table, scenario_id);
	if (row_id < 0) {
		throw InternalException("anofox_scenario: registry row for scenario id %lld not found", scenario_id);
	}
	MarkHostModified(context, host_catalog, DatabaseModificationType::DELETE_DATA);

	auto &data_table = table.GetStorage();
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	auto delete_state = data_table.InitializeDelete(table, context, bound_constraints);
	Vector row_ids(LogicalType::ROW_TYPE);
	row_ids.SetValue(0, Value::BIGINT(row_id));
	dc::SetVectorSize(row_ids, 1);
	scenario_compat::TableDelete(data_table, *delete_state, context, table, row_ids, 1);
}

void ScenarioRegistry::SetFrozen(ClientContext &context, Catalog &host_catalog, int64_t scenario_id, bool frozen) {
	auto &table = GetRegistryTableOrThrow(context, host_catalog);
	auto row_id = FindRowId(context, host_catalog, table, scenario_id);
	if (row_id < 0) {
		throw InternalException("anofox_scenario: registry row for scenario id %lld not found", scenario_id);
	}
	MarkHostModified(context, host_catalog, DatabaseModificationType::UPDATE_DATA);

	auto &data_table = table.GetStorage();
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	auto update_state = data_table.InitializeUpdate(table, context, bound_constraints);

	Vector row_ids(LogicalType::ROW_TYPE);
	row_ids.SetValue(0, Value::BIGINT(row_id));
	DataChunk update_chunk;
	update_chunk.Initialize(Allocator::Get(context), {LogicalType::BOOLEAN});
	update_chunk.SetValue(0, 0, Value::BOOLEAN(frozen));
	update_chunk.SetChildCardinality(1);
	vector<PhysicalIndex> column_ids {PhysicalIndex(REG_COL_FROZEN)};
	dc::SetVectorSize(row_ids, 1);
	scenario_compat::TableUpdate(data_table, *update_state, context, table, row_ids, column_ids, update_chunk);
}

void ScenarioRegistry::MarkMerged(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto &table = GetRegistryTableOrThrow(context, host_catalog);
	auto row_id = FindRowId(context, host_catalog, table, scenario_id);
	if (row_id < 0) {
		throw InternalException("anofox_scenario: registry row for scenario id %lld not found", scenario_id);
	}
	MarkHostModified(context, host_catalog, DatabaseModificationType::UPDATE_DATA);

	auto &data_table = table.GetStorage();
	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(table);
	auto update_state = data_table.InitializeUpdate(table, context, bound_constraints);

	Vector row_ids(LogicalType::ROW_TYPE);
	row_ids.SetValue(0, Value::BIGINT(row_id));
	DataChunk update_chunk;
	update_chunk.Initialize(Allocator::Get(context), {LogicalType::BOOLEAN, LogicalType::TIMESTAMP});
	update_chunk.SetValue(0, 0, Value::BOOLEAN(true));
	update_chunk.SetValue(1, 0, Value::TIMESTAMP(Timestamp::GetCurrentTimestamp()));
	update_chunk.SetChildCardinality(1);
	vector<PhysicalIndex> column_ids {PhysicalIndex(REG_COL_FROZEN), PhysicalIndex(REG_COL_MERGED_AT)};
	dc::SetVectorSize(row_ids, 1);
	scenario_compat::TableUpdate(data_table, *update_state, context, table, row_ids, column_ids, update_chunk);
}

bool ScenarioRegistry::HasChildren(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto table = GetRegistryTable(context, host_catalog);
	if (!table) {
		return false;
	}
	bool found = false;
	ScanRegistry(context, host_catalog, *table, [&](DataChunk &chunk, idx_t row) {
		auto parent = chunk.GetValue(REG_COL_PARENT_ID, row);
		if (!parent.IsNull() && parent.GetValue<int64_t>() == scenario_id) {
			found = true;
			return false;
		}
		return true;
	});
	return found;
}

} // namespace duckdb
