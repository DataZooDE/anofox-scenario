#define DUCKDB_EXTENSION_MAIN

#include "anofox_scenario_extension.hpp"
#include "catalog/scenario_storage_extension.hpp"
#include "lifecycle/scenario_diff.hpp"
#include "lifecycle/scenario_lifecycle.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Git-like branching for analytical databases: attach isolated what-if scenarios as "
	                      "catalogs, edit them with ordinary SQL on copy-on-write delta storage, branch and "
	                      "freeze them, and diff scenarios against their origin or each other.");

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

	// ATTACH 'name' (TYPE scenario)
	StorageExtension::Register(config, "scenario", make_shared_ptr<ScenarioStorageExtension>());
	// CALL scenario_create/drop/freeze/unfreeze + scenario_list()
	ScenarioLifecycle::RegisterFunctions(loader);
	// scenario_diff / scenario_diff_summary (streaming, bind-replaced)
	ScenarioDiff::RegisterFunctions(loader);
	// CALL scenario_migrate(): legacy v0.1 layout -> registry v2
	ScenarioMigrate::RegisterFunctions(loader);
}

void AnofoxScenarioExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AnofoxScenarioExtension::Name() {
	return "anofox_scenario";
}

std::string AnofoxScenarioExtension::Version() const {
#ifdef EXT_VERSION_ANOFOX_SCENARIO
	return EXT_VERSION_ANOFOX_SCENARIO;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_scenario, loader) {
	duckdb::LoadInternal(loader);
}
}
