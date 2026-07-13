#include "catalog/scenario_scan.hpp"

#include "catalog/scenario_catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

namespace {

struct ScenarioScanBindData : public TableFunctionData {
	ScenarioScanBindData(ScenarioTableEntry &entry_p, TableCatalogEntry &base_entry_p)
	    : entry(entry_p), base_entry(base_entry_p) {
	}

	//! The scenario table entry: the identity the planner must see
	ScenarioTableEntry &entry;
	//! The base-side source of the scan. Phase 1: the live host table.
	//! (Phase 2: a materialized copy; Phase 4: a versioned DuckLake read.)
	TableCatalogEntry &base_entry;
};

//! Single-threaded scan over the base DataTable via the parallel-scan API
//! (which, unlike DataTable::InitializeScan, copes with freshly created
//! tables and covers transaction-local storage).
struct ScenarioScanGlobalState : public GlobalTableFunctionState {
	ParallelTableScanState parallel_state;
	TableScanState scan_state;
	bool exhausted = false;

	idx_t MaxThreads() const override {
		// v1: correctness first; base-scan parallelism is a reserved
		// optimization (plan WP2.1)
		return 1;
	}
};

unique_ptr<GlobalTableFunctionState> ScenarioScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto &data_table = bind_data.base_entry.GetStorage();

	auto result = make_uniq<ScenarioScanGlobalState>();

	vector<StorageIndex> storage_ids;
	for (auto &col_idx : input.column_indexes) {
		storage_ids.push_back(bind_data.base_entry.GetStorageIndex(col_idx));
	}
	data_table.InitializeParallelScan(context, result->parallel_state, input.column_indexes);
	result->scan_state.Initialize(std::move(storage_ids), context, input.filters.get());

	// Prime the scan state with the first row-group range
	if (data_table.NextParallelScan(context, result->parallel_state, result->scan_state) == 0) {
		result->exhausted = true;
	}
	return std::move(result);
}

void ScenarioScanExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto &gstate = input.global_state->Cast<ScenarioScanGlobalState>();
	if (gstate.exhausted) {
		return;
	}
	auto &data_table = bind_data.base_entry.GetStorage();
	auto &transaction = DuckTransaction::Get(context, bind_data.base_entry.catalog);
	while (true) {
		data_table.Scan(transaction, output, gstate.scan_state);
		if (output.size() > 0) {
			return;
		}
		if (data_table.NextParallelScan(context, gstate.parallel_state, gstate.scan_state) == 0) {
			gstate.exhausted = true;
			return;
		}
	}
}

BindInfo ScenarioScanGetBindInfo(const optional_ptr<FunctionData> bind_data) {
	auto &data = bind_data->Cast<ScenarioScanBindData>();
	return BindInfo(data.entry);
}

} // namespace

TableFunction ScenarioScanFunction::GetFunction(ClientContext &context, ScenarioTableEntry &entry,
                                                unique_ptr<FunctionData> &bind_data) {
	TableFunction function("scenario_scan", {}, ScenarioScanExecute);
	function.init_global = ScenarioScanInitGlobal;
	function.get_bind_info = ScenarioScanGetBindInfo;
	function.projection_pushdown = true;
	// Filters are applied by the engine above the scan in v1 (correct, and
	// the delta side stays negligible); pushdown is reserved headroom (WP2.1).
	function.filter_pushdown = false;

	bind_data = make_uniq<ScenarioScanBindData>(entry, entry.base_entry);
	return function;
}

} // namespace duckdb
