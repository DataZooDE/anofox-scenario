//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_registry.hpp
//
// Registry v2: one lazily-created table in the host catalog that records all
// scenarios. All operations run in the caller's transaction through the
// catalog / DataTable APIs -- never through a side connection.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class Catalog;
class ClientContext;
class DuckTableEntry;

//! One row of __anofox_scenario.registry
struct ScenarioRegistryEntry {
	int64_t scenario_id = -1;
	string name;
	//! 'delta' (default) or 'materialized' (Phase 2)
	string mode = "delta";
	bool frozen = false;
	//! -1 = no parent
	int64_t parent_id = -1;
	//! -1 = no base snapshot recorded (Phase 4: DuckLake bases)
	int64_t base_snapshot_id = -1;
	timestamp_t created_at;
	//! Phase 6: set when the scenario has been merged back into base
	timestamp_t merged_at = timestamp_t(0);
	bool has_merged_at = false;
	string description;
	bool has_description = false;
};

//! Static helpers around the registry table. The physical layout is
//! __anofox_scenario.registry / __anofox_scenario.registry_seq in the host
//! catalog; created lazily by the first scenario_create.
class ScenarioRegistry {
public:
	static constexpr const char *SCHEMA_NAME = "__anofox_scenario";
	static constexpr const char *TABLE_NAME = "registry";
	static constexpr const char *SEQUENCE_NAME = "registry_seq";

	//! Create schema + sequence + registry table if they do not exist yet.
	//! Runs in the caller's transaction.
	static void EnsureExists(ClientContext &context, Catalog &host_catalog);
	//! True if the registry table exists in the host catalog.
	static bool Exists(ClientContext &context, Catalog &host_catalog);

	//! Look up a scenario by name; returns nullptr when the registry or the
	//! scenario does not exist.
	static unique_ptr<ScenarioRegistryEntry> Lookup(ClientContext &context, Catalog &host_catalog,
	                                                const string &name);
	//! Look up a scenario by id; returns nullptr when not found.
	static unique_ptr<ScenarioRegistryEntry> LookupById(ClientContext &context, Catalog &host_catalog, int64_t id);
	//! List all scenarios.
	static vector<ScenarioRegistryEntry> List(ClientContext &context, Catalog &host_catalog);

	//! Allocate the next scenario id from the registry sequence.
	static int64_t NextId(ClientContext &context, Catalog &host_catalog);
	//! Append a registry row in the caller's transaction.
	static void Insert(ClientContext &context, Catalog &host_catalog, const ScenarioRegistryEntry &entry);
	//! Delete the registry row for the given scenario id.
	static void Delete(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);
	//! Flip the frozen flag for the given scenario id.
	static void SetFrozen(ClientContext &context, Catalog &host_catalog, int64_t scenario_id, bool frozen);
	//! Phase 6: mark a scenario merged (frozen = true, merged_at = now).
	static void MarkMerged(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);
	//! True if any scenario records the given id as its parent.
	static bool HasChildren(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);

private:
	static optional_ptr<DuckTableEntry> GetRegistryTable(ClientContext &context, Catalog &host_catalog);
	static DuckTableEntry &GetRegistryTableOrThrow(ClientContext &context, Catalog &host_catalog);
};

} // namespace duckdb
