#define DUCKDB_EXTENSION_MAIN

#include "anofox_scenario_extension.hpp"
#include "metadata_store.hpp"
#include "scenario_manager.hpp"
#include "snapshot_manager.hpp"
#include "protocol_manager.hpp"
#include "delta_storage_engine.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

// OpenSSL linked through vcpkg (kept for future HTTPS protocol export)
#include <openssl/opensslv.h>

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Initialize metadata tables
	MetadataStore::Initialize(loader.GetDatabaseInstance());

	// Register scenario management functions
	ScenarioManager::RegisterFunctions(loader);

	// Register snapshot management functions
	SnapshotManager::RegisterFunctions(loader);

	// Register protocol management functions
	ProtocolManager::RegisterFunctions(loader);

	// Register delta storage engine functions
	DeltaStorageEngine::RegisterFunctions(loader);
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
