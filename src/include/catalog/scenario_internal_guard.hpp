//===----------------------------------------------------------------------===//
// anofox_scenario
//
// catalog/scenario_internal_guard.hpp
//
// Rejects user SQL that writes to or changes the internal __anofox_scenario
// schema of any attached database (#55)
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class PreparedStatementData;

//! The internal schema (registry, declared keys, deltas, materialized copies)
//! is changed only by the extension's own code, which goes through the
//! catalog and storage APIs and never through SQL. Every prepared plan is
//! therefore checked once it is planned (ScenarioClientState) and rejected if
//! one of its operators would write to, or change the schema of, a
//! __anofox_scenario entry - unless anofox_scenario_allow_internal_writes is
//! set (backup/restore, manual repair).
class ScenarioInternalGuard {
public:
	static constexpr const char *SETTING_NAME = "anofox_scenario_allow_internal_writes";

	//! Throws a PermissionException if the plan of the statement writes the
	//! internal schema and the setting is off. A statement without a physical
	//! plan (one that is re-planned once its parameters are known) is checked
	//! when that plan is made. An allowed internal write is marked to be
	//! re-planned at every execution, so a prepared statement is checked
	//! again under the setting in force when it runs (DuckDB 2.0 has no
	//! execute-time hook for client states).
	static void Check(ClientContext &context, PreparedStatementData &prepared);
	//! Whether anofox_scenario_allow_internal_writes is set for this connection
	static bool InternalWritesAllowed(ClientContext &context);
};

} // namespace duckdb
