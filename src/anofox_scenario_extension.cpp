#define DUCKDB_EXTENSION_MAIN

#include "anofox_scenario_extension.hpp"
#include "catalog/scenario_storage_extension.hpp"
#include "lifecycle/scenario_diff.hpp"
#include "lifecycle/scenario_lifecycle.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"

#ifdef HAS_POSTHOG_TELEMETRY
#include "telemetry.hpp"
#endif


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
	// scenario_merge_preview + CALL scenario_merge (merge-back)
	ScenarioMergeBack::RegisterFunctions(loader);

	// PostHog telemetry (schema 2): envelope-only extension_loaded event.
	// Respects DATAZOO_DISABLE_TELEMETRY and the opt-out setting below.
#ifdef HAS_POSTHOG_TELEMETRY
	auto &telemetry = PostHogTelemetry::Instance();

	// Auto-disable telemetry in CI environments
	if (std::getenv("CI") || std::getenv("GITHUB_ACTIONS") || std::getenv("GITLAB_CI") ||
	    std::getenv("CIRCLECI") || std::getenv("TRAVIS") || std::getenv("JENKINS_URL") ||
	    std::getenv("BUILDKITE") || std::getenv("TEAMCITY_VERSION") ||
	    std::getenv("TF_BUILD") || std::getenv("CODEBUILD_BUILD_ID")) {
		telemetry.SetEnabled(false);
	}

	telemetry.SetAPIKey("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t");
	telemetry.SetDuckDBVersion(DuckDB::LibraryVersion());
	telemetry.SetProduct("anofox_scenario", AnofoxScenarioExtension().Version(), "oss");
	telemetry.AssociateGroup("deployment", PostHogTelemetry::GetDistinctId());
	telemetry.CaptureExtensionLoad("anofox_scenario", AnofoxScenarioExtension().Version());

	// Register telemetry opt-out setting
	config.AddExtensionOption(
	    "anofox_scenario_telemetry_enabled",
	    "Enable or disable anonymous usage telemetry for anofox_scenario",
	    LogicalType::BOOLEAN,
	    Value::BOOLEAN(true),
	    [](ClientContext &context, SetScope scope, Value &parameter) {
		    PostHogTelemetry::Instance().SetEnabled(BooleanValue::Get(parameter));
	    });
#endif
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
