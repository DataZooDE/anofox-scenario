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
};

} // namespace duckdb
