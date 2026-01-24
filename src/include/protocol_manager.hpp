#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class ProtocolManager {
public:
	//! Register all protocol management functions
	static void RegisterFunctions(ExtensionLoader &loader);

	//! Check if a scenario exists
	static bool ScenarioExists(ClientContext &context, const string &name);
};

} // namespace duckdb
