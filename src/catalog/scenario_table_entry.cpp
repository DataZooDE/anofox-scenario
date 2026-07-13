#include "catalog/scenario_catalog.hpp"

#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"
#include "catalog/scenario_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

ScenarioTableEntry::ScenarioTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                       TableCatalogEntry &base_entry_p)
    : TableCatalogEntry(catalog, schema, info), base_entry(base_entry_p) {
}

ScenarioCatalog &ScenarioTableEntry::GetScenarioCatalog() {
	return catalog.Cast<ScenarioCatalog>();
}

unique_ptr<BaseStatistics> ScenarioTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	// Base statistics: conservatively correct for the passthrough case;
	// deltas are small by invariant, so these stay useful with a delta too.
	return base_entry.GetStatistics(context, column_id);
}

TableFunction ScenarioTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	// Always our own scan function: the planner must see the *scenario* table
	// identity (LogicalGet::GetTable), otherwise DDL/DML against the scenario
	// gets misattributed to the base table (e.g. CREATE INDEX would silently
	// build an index on the base).
	return ScenarioScanFunction::GetFunction(context, *this, bind_data);
}

TableStorageInfo ScenarioTableEntry::GetStorageInfo(ClientContext &context) {
	return base_entry.GetStorageInfo(context);
}

DataTable &ScenarioTableEntry::GetStorage() {
	return base_entry.GetStorage();
}

virtual_column_map_t ScenarioTableEntry::GetVirtualColumns() const {
	auto result = TableCatalogEntry::GetVirtualColumns(); // rowid
	result.insert(
	    make_pair(SCENARIO_ORIGIN_COLUMN_ID, TableColumn("__scenario_origin", LogicalType::TINYINT)));
	auto pk_columns = ScenarioDelta::GetPKColumns(base_entry);
	for (idx_t k = 0; k < pk_columns.size(); k++) {
		auto &column = GetColumn(LogicalIndex(pk_columns[k]));
		result.insert(make_pair(SCENARIO_KEY_COLUMN_START + k,
		                        TableColumn("__scenario_key_" + to_string(k), column.Type())));
	}
	return result;
}

vector<column_t> ScenarioTableEntry::GetRowIdColumns() const {
	auto pk_columns = ScenarioDelta::GetPKColumns(base_entry);
	if (pk_columns.empty()) {
		// no-PK tables: default rowid identity; PlanUpdate/PlanDelete throw
		// the v1 limitation error before it is ever used
		return TableCatalogEntry::GetRowIdColumns();
	}
	vector<column_t> result;
	result.push_back(SCENARIO_ORIGIN_COLUMN_ID);
	for (idx_t k = 0; k < pk_columns.size(); k++) {
		result.push_back(SCENARIO_KEY_COLUMN_START + k);
	}
	return result;
}

void ScenarioTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                               LogicalUpdate &update, ClientContext &context) {
	// v1 gate: updating PK columns needs D+I pair semantics (planned v0.4).
	// Checked before extra columns are projected, so update.columns still
	// reflects only the user's SET targets.
	auto pk_columns = ScenarioDelta::GetPKColumns(base_entry);
	for (auto pk_col : pk_columns) {
		auto pk_physical = GetColumns().GetColumn(LogicalIndex(pk_col)).Physical();
		for (auto &updated : update.columns) {
			if (updated == pk_physical) {
				throw NotImplementedException(
				    "Updating PRIMARY KEY columns of scenario tables is not supported yet (planned for v0.4). "
				    "Use DELETE + INSERT instead");
			}
		}
	}
	// The scenario update sink writes complete post-image rows into the
	// delta: project every column (the same mechanism core uses for
	// update_is_del_and_insert)
	physical_index_set_t all_columns;
	for (auto &column : GetColumns().Physical()) {
		all_columns.insert(column.Physical());
	}
	LogicalUpdate::BindExtraColumns(*this, get, proj, update, all_columns);
	update.update_is_del_and_insert = false;
}

} // namespace duckdb
