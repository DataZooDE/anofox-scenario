#include "catalog/scenario_registry.hpp"

#include "catalog/scenario_delta.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

// C++11: out-of-line definitions for static constexpr members
constexpr const char *ScenarioRegistry::SCHEMA_NAME;
constexpr const char *ScenarioRegistry::TABLE_NAME;
constexpr const char *ScenarioRegistry::SEQUENCE_NAME;

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
	schema_info.catalog = host_catalog.GetName();
	schema_info.schema = SCHEMA_NAME;
	schema_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	host_catalog.CreateSchema(context, schema_info);

	// Sequence for scenario ids
	auto seq_info = make_uniq<CreateSequenceInfo>();
	seq_info->catalog = host_catalog.GetName();
	seq_info->schema = SCHEMA_NAME;
	seq_info->name = SEQUENCE_NAME;
	seq_info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	host_catalog.CreateSequence(context, *seq_info);

	// Registry table -- column order pinned by the REG_COL_* constants above.
	auto info = make_uniq<CreateTableInfo>();
	info->catalog = host_catalog.GetName();
	info->schema = SCHEMA_NAME;
	info->table = TABLE_NAME;
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
	chunk.SetCardinality(1);

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
	data_table.Delete(*delete_state, context, row_ids, 1);
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
	update_chunk.SetCardinality(1);
	vector<PhysicalIndex> column_ids {PhysicalIndex(REG_COL_FROZEN)};
	data_table.Update(*update_state, context, row_ids, column_ids, update_chunk);
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
	update_chunk.SetCardinality(1);
	vector<PhysicalIndex> column_ids {PhysicalIndex(REG_COL_FROZEN), PhysicalIndex(REG_COL_MERGED_AT)};
	data_table.Update(*update_state, context, row_ids, column_ids, update_chunk);
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
