#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class DDLBlocker {
public:
	//! Register the optimizer extension to block DDL in scenario schemas
	static void Register(DatabaseInstance &db);
};

} // namespace duckdb
