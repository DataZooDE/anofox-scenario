#include "catalog/scenario_delta.hpp"

#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

// C++11: out-of-line definitions for static constexpr members
constexpr idx_t ScenarioDelta::OP_COL;
constexpr idx_t ScenarioDelta::TS_COL;
constexpr idx_t ScenarioDelta::PAYLOAD_START;

string ScenarioDelta::DeltaTableName(int64_t scenario_id, const string &table_name) {
	return "s" + to_string(scenario_id) + "_delta_" + table_name;
}

string ScenarioDelta::MatTableName(int64_t scenario_id, const string &table_name) {
	return "s" + to_string(scenario_id) + "_mat_" + table_name;
}

//! Internal: look up a table in the internal schema by exact name
static optional_ptr<DuckTableEntry> TryGetInternalTable(ClientContext &context, Catalog &host_catalog,
                                                        const string &table_name) {
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry = schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, table_name);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

optional_ptr<DuckTableEntry> ScenarioDelta::TryGetDeltaTable(ClientContext &context, Catalog &host_catalog,
                                                             int64_t scenario_id, const string &table_name) {
	return TryGetInternalTable(context, host_catalog, DeltaTableName(scenario_id, table_name));
}

optional_ptr<DuckTableEntry> ScenarioDelta::TryGetMatTable(ClientContext &context, Catalog &host_catalog,
                                                           int64_t scenario_id, const string &table_name) {
	return TryGetInternalTable(context, host_catalog, MatTableName(scenario_id, table_name));
}

DuckTableEntry &ScenarioDelta::CreateMatTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                              TableCatalogEntry &base_entry) {
	// Full schema copy (columns + constraints, so the PK survives)
	auto create_info = base_entry.GetInfo();
	auto &info = create_info->Cast<CreateTableInfo>();
	info.catalog = host_catalog.GetName();
	info.schema = ScenarioRegistry::SCHEMA_NAME;
	info.table = MatTableName(scenario_id, base_entry.name);
	info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	info.temporary = false;
	host_catalog.CreateTable(context, unique_ptr_cast<CreateInfo, CreateTableInfo>(std::move(create_info)));

	auto created = TryGetMatTable(context, host_catalog, scenario_id, base_entry.name);
	if (!created) {
		throw InternalException("anofox_scenario: failed to create materialized table for '%s'", base_entry.name);
	}
	CopyTableData(context, base_entry.Cast<DuckTableEntry>(), *created);
	return *created;
}

void ScenarioDelta::CopyTableData(ClientContext &context, DuckTableEntry &from_table, DuckTableEntry &to_table,
                                  idx_t target_column_offset) {
	auto &from_storage = from_table.GetStorage();
	auto &to_storage = to_table.GetStorage();
	auto from_types = from_storage.GetTypes();
	auto to_types = to_storage.GetTypes();

	vector<StorageIndex> column_ids;
	for (idx_t i = 0; i < from_types.size(); i++) {
		column_ids.emplace_back(i);
	}
	auto binder = Binder::CreateBinder(context);
	auto to_constraints = binder->BindConstraints(to_table);

	auto &transaction = DuckTransaction::Get(context, from_table.catalog);
	ParallelTableScanState parallel_state;
	from_storage.InitializeParallelScan(context, parallel_state, {});
	TableScanState state;
	state.Initialize(std::move(column_ids), context);

	DataChunk scan_chunk;
	scan_chunk.Initialize(Allocator::Get(context), from_types);
	DataChunk append_chunk;
	if (target_column_offset > 0) {
		append_chunk.Initialize(Allocator::Get(context), to_types);
	}
	while (from_storage.NextParallelScan(context, parallel_state, state) > 0) {
		while (true) {
			scan_chunk.Reset();
			from_storage.Scan(transaction, scan_chunk, state);
			if (scan_chunk.size() == 0) {
				break;
			}
			if (target_column_offset == 0) {
				to_storage.LocalAppend(to_table, context, scan_chunk, to_constraints);
			} else {
				append_chunk.Reset();
				for (idx_t col = 0; col < from_types.size(); col++) {
					append_chunk.data[target_column_offset + col].Reference(scan_chunk.data[col]);
				}
				append_chunk.SetCardinality(scan_chunk.size());
				to_storage.LocalAppend(to_table, context, append_chunk, to_constraints);
			}
		}
	}
}

DuckTableEntry &ScenarioDelta::EnsureDeltaTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                                TableCatalogEntry &base_entry, const vector<string> *declared_keys) {
	auto existing = TryGetDeltaTable(context, host_catalog, scenario_id, base_entry.name);
	if (existing) {
		return *existing;
	}
	// NOTE: the caller is responsible for registering the host write
	// (ScenarioCatalog::MarkHostWrite / MetaTransaction::ModifyDatabase).

	auto info = make_uniq<CreateTableInfo>();
	info->catalog = host_catalog.GetName();
	info->schema = ScenarioRegistry::SCHEMA_NAME;
	info->table = DeltaTableName(scenario_id, base_entry.name);
	info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	info->columns.AddColumn(ColumnDefinition("_op", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("_ts", LogicalType::TIMESTAMP));
	for (auto &col : base_entry.GetColumns().Logical()) {
		info->columns.AddColumn(ColumnDefinition(col.Name(), col.Type()));
	}
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(OP_COL)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(TS_COL)));

	// The base PK becomes the delta PK: one delta row per user-visible key.
	// Tables without a PK may declare one via key_columns := at create;
	// otherwise they stay insert-only.
	auto pk_columns = GetPKColumns(base_entry);
	if (!pk_columns.empty()) {
		vector<string> pk_names;
		for (auto col_id : pk_columns) {
			pk_names.push_back(base_entry.GetColumn(LogicalIndex(col_id)).Name());
		}
		info->constraints.push_back(make_uniq<UniqueConstraint>(std::move(pk_names), true));
	} else if (declared_keys && !declared_keys->empty()) {
		vector<string> pk_names = *declared_keys;
		info->constraints.push_back(make_uniq<UniqueConstraint>(std::move(pk_names), true));
	}
	host_catalog.CreateTable(context, std::move(info));

	auto created = TryGetDeltaTable(context, host_catalog, scenario_id, base_entry.name);
	if (!created) {
		throw InternalException("anofox_scenario: failed to create delta table for '%s'", base_entry.name);
	}
	return *created;
}

vector<idx_t> ScenarioDelta::GetPKColumns(const TableCatalogEntry &base_entry) {
	vector<idx_t> result;
	auto pk = base_entry.GetPrimaryKey();
	if (!pk) {
		return result;
	}
	auto &unique = pk->Cast<UniqueConstraint>();
	auto &columns = base_entry.GetColumns();
	if (unique.HasIndex()) {
		result.push_back(unique.GetIndex().index);
		return result;
	}
	for (auto &name : unique.GetColumnNames()) {
		result.push_back(columns.GetColumn(name).Logical().index);
	}
	return result;
}

vector<idx_t> ScenarioDelta::GetKeyColumns(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                           const string &logical_name, TableCatalogEntry &base_entry) {
	auto pk_columns = GetPKColumns(base_entry);
	if (!pk_columns.empty()) {
		return pk_columns;
	}
	// no base PK: a declared key lives as the delta table's PRIMARY KEY
	auto delta = TryGetDeltaTable(context, host_catalog, scenario_id, logical_name);
	if (!delta) {
		return {};
	}
	auto delta_pk = GetPKColumns(*delta);
	vector<idx_t> result;
	for (auto delta_col : delta_pk) {
		if (delta_col < PAYLOAD_START) {
			throw InternalException("anofox_scenario: delta PK references a metadata column");
		}
		result.push_back(delta_col - PAYLOAD_START);
	}
	return result;
}

string ScenarioDelta::MakeKey(DataChunk &chunk, idx_t row, const vector<idx_t> &key_positions) {
	string key;
	for (auto pos : key_positions) {
		auto value = chunk.GetValue(pos, row);
		if (value.IsNull()) {
			key += "N|";
			continue;
		}
		auto str = value.ToString();
		key += to_string(str.size());
		key += ":";
		key += str;
	}
	return key;
}

void ScenarioDelta::ScanTableRows(ClientContext &context, DuckTableEntry &table, vector<StorageIndex> column_ids,
                                  const vector<LogicalType> &scan_types,
                                  const std::function<bool(DataChunk &, idx_t)> &callback) {
	auto &data_table = table.GetStorage();
	auto &transaction = DuckTransaction::Get(context, table.catalog);

	ParallelTableScanState parallel_state;
	data_table.InitializeParallelScan(context, parallel_state, {});
	TableScanState state;
	state.Initialize(std::move(column_ids), context);

	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), scan_types);
	while (data_table.NextParallelScan(context, parallel_state, state) > 0) {
		while (true) {
			chunk.Reset();
			data_table.Scan(transaction, chunk, state);
			if (chunk.size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk.size(); row++) {
				if (!callback(chunk, row)) {
					return;
				}
			}
		}
	}
}

} // namespace duckdb
