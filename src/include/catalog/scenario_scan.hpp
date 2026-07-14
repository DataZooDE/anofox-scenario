//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_scan.hpp
//
// The scenario table scan. Owns its bind data so the planner sees the
// *scenario* table identity (LogicalGet::GetTable), never the base table.
// Serves passthrough base reads today; merge-on-read (base + delta) is
// layered in with the write path.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ScenarioTableEntry;

struct ScenarioScanFunction {
	//! Build the scan function + bind data for a scenario table entry
	static TableFunction GetFunction(ClientContext &context, ScenarioTableEntry &entry,
	                                 unique_ptr<FunctionData> &bind_data);
};

} // namespace duckdb
