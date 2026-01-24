#pragma once

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

class MetadataStore {
public:
	//! Initialize all metadata tables. Called once when extension loads.
	static void Initialize(DatabaseInstance &db);

	//! Check if metadata tables already exist
	static bool TablesExist(DatabaseInstance &db);

private:
	//! Create the _scenario_registry table
	static void CreateRegistryTable(DatabaseInstance &db);

	//! Create the _scenario_tables table
	static void CreateTablesTable(DatabaseInstance &db);

	//! Create the _scenario_base_rowids table
	static void CreateBaseRowidsTable(DatabaseInstance &db);

	//! Create the _scenario_snapshots table
	static void CreateSnapshotsTable(DatabaseInstance &db);

	//! Create the _scenario_protocols table
	static void CreateProtocolsTable(DatabaseInstance &db);
};

} // namespace duckdb
