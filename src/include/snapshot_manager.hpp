#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class SnapshotManager {
public:
	//! Register all snapshot management functions
	static void RegisterFunctions(ExtensionLoader &loader);

	//! Validate snapshot name (alphanumeric + underscore, max 63 chars)
	static bool ValidateName(const string &name);

	//! Check if a snapshot with the given name exists
	static bool SnapshotExists(ClientContext &context, const string &name);

	//! Check if a scenario with the given name exists
	static bool ScenarioExists(ClientContext &context, const string &name);

	//! Get the schema name for a scenario
	static string GetScenarioSchema(ClientContext &context, const string &scenario_name);
};

} // namespace duckdb
