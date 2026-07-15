//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_storage_extension.hpp
//
// ATTACH 'name' (TYPE scenario) support.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class ScenarioStorageExtension : public StorageExtension {
public:
	ScenarioStorageExtension();
};

} // namespace duckdb
