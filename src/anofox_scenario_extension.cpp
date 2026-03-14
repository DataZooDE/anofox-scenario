#define DUCKDB_EXTENSION_MAIN

#include "anofox_scenario_extension.hpp"
#include "metadata_store.hpp"
#include "scenario_manager.hpp"
#include "snapshot_manager.hpp"
#include "protocol_manager.hpp"
#include "delta_storage_engine.hpp"
#include "comparison_engine.hpp"
#include "ddl_blocker.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"


namespace duckdb {

// Validation callback for scenario_schema_prefix
static void SetScenarioSchemaPrefix(ClientContext &context, SetScope scope, Value &parameter) {
	auto prefix = parameter.ToString();

	// Validate not empty
	if (prefix.empty()) {
		throw InvalidInputException("scenario_schema_prefix cannot be empty");
	}

	// Validate ends with underscore
	if (prefix.back() != '_') {
		throw InvalidInputException("scenario_schema_prefix must end with an underscore");
	}

	// Validate characters: alphanumeric and underscore only
	for (char c : prefix) {
		if (!std::isalnum(c) && c != '_') {
			throw InvalidInputException("scenario_schema_prefix must contain only alphanumeric characters and underscores");
		}
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register configuration options
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("scenario_schema_prefix",
	                          "Prefix for scenario schema names (default: '_scen_')",
	                          LogicalType::VARCHAR,
	                          Value("_scen_"),
	                          SetScenarioSchemaPrefix,
	                          SetScope::SESSION);

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

	// Register comparison engine functions
	ComparisonEngine::RegisterFunctions(loader);

	// Register DDL blocker to prevent schema modifications in scenarios
	DDLBlocker::Register(loader.GetDatabaseInstance());
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
