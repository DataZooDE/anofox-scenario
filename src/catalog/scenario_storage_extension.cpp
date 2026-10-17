#include "catalog/scenario_storage_extension.hpp"

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
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
	// connections opened before the extension was loaded get the client
	// state here (see ScenarioConnectionCallback)
	ScenarioClientState::Get(context);
	// The scenario registry lives in the session's default database. This is
	// the database scenario_create ran against (documented Phase 1 scoping).
	const string host_catalog_name = dc::Str(DatabaseManager::GetDefaultDatabase(context));
	auto &host_catalog = Catalog::GetCatalog(context, dc::Ident(host_catalog_name));
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
	// Keyless deltas written before v0.4.1 lack _count: upgrade them in place
	// now, before any statement binds against this scenario (bound plans hold
	// the delta entry, so a later swap would leave them dangling). Only when
	// that is safe - not for a READ_ONLY attach, not inside an explicit
	// transaction that does not already write the host, not while another
	// transaction may be writing the delta; readers otherwise treat the
	// legacy layout as _count = 1. Freezing does not block it: the logical
	// content is unchanged.
	ScenarioDelta::TryUpgradeLegacyKeylessDeltas(context, host_catalog, entry->scenario_id,
	                                             options.access_mode == AccessMode::READ_ONLY);

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
	if (!entry->base_catalog.empty() && !Catalog::GetCatalogEntry(context, dc::Ident(entry->base_catalog))) {
		throw InvalidInputException(
		    "Scenario '%s' uses '%s' as its base catalog, which is not attached. ATTACH it first",
		    scenario_name, entry->base_catalog);
	}
	return make_uniq<ScenarioCatalog>(db, entry->name, host_catalog_name, entry->scenario_id, mat_base_scenario_id,
	                                  entry->base_catalog, entry->created_at);
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
