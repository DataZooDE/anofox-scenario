#define DUCKDB_EXTENSION_MAIN

#include "anofox_scenario_extension.hpp"
#include "catalog/scenario_storage_extension.hpp"
#include "lifecycle/scenario_diff.hpp"
#include "lifecycle/scenario_lifecycle.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "catalog/scenario_registry.hpp"
#include "anofox_scenario_banner.hpp"

#ifdef HAS_POSTHOG_TELEMETRY
#include "telemetry.hpp"
#endif


// Deliberately outside namespace duckdb: the banner library is DuckDB-agnostic
// and the guard macro refers to this object from every guarded source file.
const datazoo::BannerInfo ANOFOX_SCENARIO_BANNER {
    "anofox_scenario", "0.1.0", "https://github.com/DataZooDE/anofox-scenario"};

namespace duckdb {

//! Every connection carries the scenario client state from the start: its
//! OnFinalizePrepare hook must see a connection's first statement too
//! (ScenarioClientState, write attribution)
class ScenarioConnectionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override {
		ScenarioClientState::Get(context);
	}
};

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Git-like branching for analytical databases: attach isolated what-if scenarios as "
	                      "catalogs, edit them with ordinary SQL on copy-on-write delta storage, branch and "
	                      "freeze them, and diff scenarios against their origin or each other.");

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

	// ATTACH 'name' (TYPE scenario)
	StorageExtension::Register(config, "scenario", make_shared_ptr<ScenarioStorageExtension>());
	ExtensionCallbackManager::Get(loader.GetDatabaseInstance()).Register(make_shared_ptr<ScenarioConnectionCallback>());
	// CALL scenario_create/drop/freeze/unfreeze + scenario_list()
	ScenarioLifecycle::RegisterFunctions(loader);
	// scenario_diff / scenario_diff_summary (streaming, bind-replaced)
	ScenarioDiff::RegisterFunctions(loader);
	// CALL scenario_migrate(): legacy v0.1 layout -> registry v2
	ScenarioMigrate::RegisterFunctions(loader);
	// scenario_merge_preview + CALL scenario_merge (merge-back)
	ScenarioMergeBack::RegisterFunctions(loader);

	// Merge-on-read tuning: up to this many updated/deleted keys, base rows
	// are matched against an in-memory hash set of those keys (lock-free,
	// scales with scan threads); above it, every base row probes the delta's
	// key index instead (bounded memory, serialized on the index lock)
	config.AddExtensionOption("anofox_scenario_suppression_hash_limit",
	                          "Maximum number of updated/deleted scenario keys per table for which scans suppress base "
	                          "rows through an in-memory hash set instead of per-row delta index probes",
	                          LogicalType::UBIGINT, Value::UBIGINT(4194304));

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

	datazoo::RegisterBannerOption(loader);
	// Last, so a load that fails earlier never advertises itself. Silent unless
	// stderr is a terminal and the ~/.duckdb stamp is over a day old.
	datazoo::ShowBanner(ANOFOX_SCENARIO_BANNER);
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
