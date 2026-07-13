#include "catalog/scenario_catalog.hpp"

#include "catalog/scenario_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
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

} // namespace duckdb
