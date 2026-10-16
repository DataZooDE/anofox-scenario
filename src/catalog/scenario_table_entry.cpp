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
                                       TableCatalogEntry &base_entry_p, bool for_binding)
    : TableCatalogEntry(catalog, schema, info), base_entry(base_entry_p) {
	// Write attribution (#31). The binder registers DML against a non-temporary
	// table as a modification of the table's catalog (bind_insert/update/delete/
	// merge_into: RegisterDBModify), and MetaTransaction::ModifyDatabase allows
	// one modified database per transaction. A scenario catalog owns no storage:
	// every row it writes lands in the HOST database's delta tables. Registering
	// the scenario database as the write target would therefore forbid host
	// writes (and writes to other scenarios) in the same transaction, although
	// physically only the host is ever modified. Flagging the entry temporary
	// makes the binder skip that registration (INSERT registers a read instead,
	// keeping prepared-statement rebinding intact); the sinks then register the
	// host via ScenarioCatalog::MarkHostWrite before their first physical write.
	// The flag has no other effect on the binder's entry: it only matters for
	// entries stored in a CatalogSet / the WAL, and scenario entries live in
	// neither. Introspection reads the flag too (duckdb_tables.temporary,
	// information_schema.tables.table_type), so catalog enumeration
	// (ScenarioSchemaEntry::Scan) gets a variant without it.
	temporary = for_binding;
}

unique_ptr<CreateInfo> ScenarioTableEntry::GetInfo() const {
	auto info = TableCatalogEntry::GetInfo();
	info->temporary = false;
	return info;
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

vector<idx_t> ScenarioTableEntry::IdentityColumns() const {
	if (!key_columns.empty()) {
		return key_columns;
	}
	// keyless: whole-row identity (bag semantics via the delta's _count)
	vector<idx_t> result;
	for (idx_t col = 0; col < GetColumns().LogicalColumnCount(); col++) {
		result.push_back(col);
	}
	return result;
}

virtual_column_map_t ScenarioTableEntry::GetVirtualColumns() const {
	auto result = TableCatalogEntry::GetVirtualColumns(); // rowid
	result.insert(
	    make_pair(SCENARIO_ORIGIN_COLUMN_ID, TableColumn("__scenario_origin", LogicalType::TINYINT)));
	auto identity = IdentityColumns();
	for (idx_t k = 0; k < identity.size(); k++) {
		auto &column = GetColumn(LogicalIndex(identity[k]));
		result.insert(make_pair(SCENARIO_KEY_COLUMN_START + k,
		                        TableColumn("__scenario_key_" + to_string(k), column.Type())));
	}
	if (key_columns.empty()) {
		result.insert(make_pair(SCENARIO_ROWNO_COLUMN_ID, TableColumn("__scenario_rowno", LogicalType::BIGINT)));
	}
	return result;
}

vector<column_t> ScenarioTableEntry::GetRowIdColumns() const {
	auto identity = IdentityColumns();
	vector<column_t> result;
	result.push_back(SCENARIO_ORIGIN_COLUMN_ID);
	for (idx_t k = 0; k < identity.size(); k++) {
		result.push_back(SCENARIO_KEY_COLUMN_START + k);
	}
	if (base_entry.IsDuckTable() && !key_columns.empty()) {
		// base rowid trails the identity columns: DELETE ... RETURNING fetches
		// the doomed base rows by rowid (delta rows carry NULL here). Keyless
		// tables carry their whole row in the identity columns instead.
		result.push_back(COLUMN_IDENTIFIER_ROW_ID);
	}
	if (key_columns.empty()) {
		// keyless: the per-scan row number tells one copy matched twice by a
		// join apart from two identical copies (double-touch detection)
		result.push_back(SCENARIO_ROWNO_COLUMN_ID);
	}
	return result;
}

void ScenarioTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                               LogicalUpdate &update, ClientContext &context) {
	// The scenario update sink writes complete post-image rows into the
	// delta (and decomposes PK-moving updates into D+I): project every
	// column (the same mechanism core uses for update_is_del_and_insert)
	physical_index_set_t all_columns;
	for (auto &column : GetColumns().Physical()) {
		all_columns.insert(column.Physical());
	}
	LogicalUpdate::BindExtraColumns(*this, get, proj, update, all_columns);
	update.update_is_del_and_insert = false;
}

} // namespace duckdb
