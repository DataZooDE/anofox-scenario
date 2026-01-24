#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class ComparisonEngine {
public:
	//! Register comparison functions
	static void RegisterFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
