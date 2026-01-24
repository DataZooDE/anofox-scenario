#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ScenarioManager {
public:
	//! Register all scenario management functions
	static void RegisterFunctions(ExtensionLoader &loader);

	//! Get the schema prefix for scenarios (default: "_scen_")
	static string GetSchemaPrefix(ClientContext &context);

	//! Get the full schema name for a scenario
	static string GetSchemaName(ClientContext &context, const string &scenario_name);

	//! Validate scenario name (alphanumeric + underscore, max 63 chars)
	static bool ValidateName(const string &name);

	//! Validate table name (same rules as scenario name)
	static bool ValidateTableName(const string &name);

	//! Check if a scenario exists
	static bool ScenarioExists(ClientContext &context, const string &name);

	//! Escape a string for SQL (doubles single quotes)
	static string EscapeSQLString(const string &str);

	//! Quote an identifier for SQL (wraps in double quotes, escapes internal quotes)
	static string QuoteIdentifier(const string &identifier);
};

} // namespace duckdb
