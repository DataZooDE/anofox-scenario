//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// lifecycle/scenario_diff.hpp
//
// Streaming diff engine (Phase 3): scenario_diff / scenario_diff_summary.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class ExtensionLoader;

class ScenarioDiff {
public:
	static void RegisterFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
