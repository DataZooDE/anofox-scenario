#include "catalog/scenario_storage_extension.hpp"

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

namespace duckdb {

static unique_ptr<Catalog> ScenarioAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &options) {
	auto &scenario_name = info.path;
	// The scenario registry lives in the session's default database. This is
	// the database scenario_create ran against (documented Phase 1 scoping).
	auto &host_catalog_name = DatabaseManager::GetDefaultDatabase(context);
	auto &host_catalog = Catalog::GetCatalog(context, host_catalog_name);
	if (host_catalog.GetCatalogType() == "scenario") {
		throw InvalidInputException(
		    "Cannot attach scenario '%s': the current default database '%s' is itself a scenario. "
		    "Switch back to the base database first (USE <database>)",
		    scenario_name, host_catalog_name);
	}

	auto entry = ScenarioRegistry::Lookup(context, host_catalog, scenario_name);
	if (!entry) {
		throw InvalidInputException(
		    "Scenario '%s' not found in database '%s'. Create it first with CALL scenario_create('%s')",
		    scenario_name, host_catalog_name, scenario_name);
	}
	// BaseSource resolution: the nearest materialized scenario in the parent
	// chain (including self) provides the base tables; otherwise the live host.
	int64_t mat_base_scenario_id = -1;
	auto current = make_uniq<ScenarioRegistryEntry>(*entry);
	idx_t depth = 0;
	while (current) {
		if (current->mode == "materialized") {
			mat_base_scenario_id = current->scenario_id;
			break;
		}
		if (current->parent_id < 0) {
			break;
		}
		current = ScenarioRegistry::LookupById(context, host_catalog, current->parent_id);
		if (++depth > 1000) {
			throw InternalException("anofox_scenario: cycle detected in scenario parent chain");
		}
	}
	return make_uniq<ScenarioCatalog>(db, entry->name, host_catalog_name, entry->scenario_id, mat_base_scenario_id);
}

static unique_ptr<TransactionManager> ScenarioCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<ScenarioTransactionManager>(db);
}

ScenarioStorageExtension::ScenarioStorageExtension() {
	attach = ScenarioAttach;
	create_transaction_manager = ScenarioCreateTransactionManager;
}

} // namespace duckdb
