#include "catalog/scenario_delta.hpp"

#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
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

optional_ptr<DuckTableEntry> ScenarioDelta::TryGetDeltaTable(ClientContext &context, Catalog &host_catalog,
                                                             int64_t scenario_id, const string &table_name) {
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry = schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY,
	                              DeltaTableName(scenario_id, table_name));
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

DuckTableEntry &ScenarioDelta::EnsureDeltaTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                                TableCatalogEntry &base_entry) {
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
	// (No-PK tables get no delta PK; they are insert-only in v1.)
	auto pk_columns = GetPKColumns(base_entry);
	if (!pk_columns.empty()) {
		vector<string> pk_names;
		for (auto col_id : pk_columns) {
			pk_names.push_back(base_entry.GetColumn(LogicalIndex(col_id)).Name());
		}
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
