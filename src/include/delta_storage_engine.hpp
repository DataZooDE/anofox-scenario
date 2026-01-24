#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class DeltaStorageEngine {
public:
	//! Register delta storage functions
	static void RegisterFunctions(ExtensionLoader &loader);

	//! Create a delta table for a base table in a scenario schema
	//! Returns true on success
	static bool CreateDeltaTable(ClientContext &context, const string &scenario_schema,
	                             const string &base_table_name);

	//! Get the delta table name for a base table
	static string GetDeltaTableName(const string &base_table_name);

	//! Check if a delta table exists
	static bool DeltaTableExists(ClientContext &context, const string &scenario_schema,
	                             const string &table_name);

	//! Create a merge-on-read view that combines base table with delta modifications
	static bool CreateMergeView(ClientContext &context, const string &scenario_schema,
	                            const string &base_table_name, const vector<string> &pk_columns);

	//! Drop a merge-on-read view
	static bool DropMergeView(ClientContext &context, const string &scenario_schema,
	                          const string &table_name);

	//! Get the merge view name for a table
	static string GetMergeViewName(const string &base_table_name);
};

} // namespace duckdb
