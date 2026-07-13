//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// lifecycle/scenario_lifecycle.hpp
//
// CALL-style lifecycle verbs running in the caller's transaction
// (registry v2 path; the legacy scalar functions remain until Phase 2).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

class ScenarioLifecycle {
public:
	static void RegisterFunctions(ExtensionLoader &loader);
};

//! CALL scenario_migrate(): legacy v0.1 layout -> registry v2 (one-way)
class ScenarioMigrate {
public:
	static void RegisterFunctions(ExtensionLoader &loader);
};

//! Phase 6: scenario_merge_preview + CALL scenario_merge
class ScenarioMergeBack {
public:
	static void RegisterFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
