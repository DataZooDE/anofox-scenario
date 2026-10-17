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
#include "duckdb/main/client_context_state.hpp"

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
	//! Phase 4: catalog whose tables are the scenario's base ("" = host default)
	string base_catalog;
};

//! Static helpers around the registry table. The physical layout is
//! __anofox_scenario.registry / __anofox_scenario.registry_seq in the host
//! catalog; created lazily by the first scenario_create.
class ScenarioRegistry {
public:
	static constexpr const char *SCHEMA_NAME = "__anofox_scenario";
	static constexpr const char *TABLE_NAME = "registry";
	static constexpr const char *SEQUENCE_NAME = "registry_seq";
	//! Declared row identity per (scenario, table): one row per table whose
	//! key was declared (scenario_create / scenario_refresh key_columns :=).
	//! Created lazily, like the registry, in the caller's transaction (#34)
	static constexpr const char *TABLES_TABLE_NAME = "scenario_tables";

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

	//===--------------------------------------------------------------===//
	// __anofox_scenario.scenario_tables (declared keys, #34)
	//===--------------------------------------------------------------===//
	//! The declared key of a scenario table (logical name), if one is recorded
	static bool LookupDeclaredKey(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                              const string &table_name, vector<string> &key_columns);
	//! Every declared key of a scenario: logical table name -> key column names
	static map<string, vector<string>> ListDeclaredKeys(ClientContext &context, Catalog &host_catalog,
	                                                   int64_t scenario_id);
	//! Record a declared key (creates scenario_tables on first use); the
	//! caller has checked that none is recorded for the table yet.
	//! scenario_name only names the scenario in errors
	static void InsertDeclaredKey(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                              const string &scenario_name, const string &table_name,
	                              const vector<string> &key_columns);
	//! Remove every declared key of a scenario (scenario_drop)
	static void DeleteDeclaredKeys(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);
	//! Replace the recorded declared key of one table (re-declaring a key
	//! whose column vanished from the base)
	static void ReplaceDeclaredKey(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                               const string &table_name, const vector<string> &key_columns);

private:
	static optional_ptr<DuckTableEntry> GetRegistryTable(ClientContext &context, Catalog &host_catalog);
	static DuckTableEntry &GetRegistryTableOrThrow(ClientContext &context, Catalog &host_catalog);
	static optional_ptr<DuckTableEntry> GetTablesTable(ClientContext &context, Catalog &host_catalog);
	//! Create scenario_tables if it does not exist yet (caller's transaction)
	static DuckTableEntry &EnsureTablesTable(ClientContext &context, Catalog &host_catalog,
	                                         const string &scenario_name);
	//! The declared keys of a scenario, cached per transaction (ScenarioClientState)
	static shared_ptr<const map<string, vector<string>>> LoadDeclaredKeys(ClientContext &context,
	                                                                      Catalog &host_catalog, int64_t scenario_id);
	//! Delete the given scenario_tables rows (caller's transaction)
	static void DeleteDeclaredKeyRows(ClientContext &context, Catalog &host_catalog, DuckTableEntry &table,
	                                  const vector<row_t> &row_ids);
};

//! Per-connection state of the extension (a ClientContextState)
class ScenarioClientState : public ClientContextState {
public:
	static ScenarioClientState &Get(ClientContext &context);

	//! Serializes the write registrations and delta creations of this
	//! connection's scenario sinks: their global states initialize on worker
	//! threads in parallel, and MetaTransaction::ModifyDatabase /
	//! DuckTransaction::SetModifications are not synchronized. Both are
	//! per-transaction state, so a lock per connection suffices: first writes
	//! of different connections (or databases) do not serialize
	mutex write_lock;

	//! The declared keys of one scenario as this transaction sees them,
	//! loaded once per (transaction, host, scenario): a table entry is built
	//! per table and transaction, and scanning scenario_tables for each of
	//! them would cost O(tables x declarations) per enumeration. A
	//! transaction's snapshot only changes through its own writes, which
	//! invalidate the cache (Insert/ReplaceDeclaredKey, DeleteDeclaredKeys)
	using DeclaredKeys = map<string, vector<string>>;
	shared_ptr<const DeclaredKeys> GetDeclaredKeys(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                                               const std::function<DeclaredKeys()> &load);
	void Invalidate(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);

private:
	mutex cache_lock;
	transaction_t cache_transaction = DConstants::INVALID_INDEX;
	map<pair<idx_t, int64_t>, shared_ptr<const DeclaredKeys>> cache;
	void ResetIfStale(ClientContext &context);
};

} // namespace duckdb
