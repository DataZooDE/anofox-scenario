//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_schema_change.hpp
//
// Base schema changes of tables scenarios track (#62). A scenario stores its
// changes in internal tables that mirror the base table column for column
// (delta, pre-images, materialized copy). An ALTER TABLE or DROP TABLE of the
// base is wrapped so that, in the same statement:
//   - ADD COLUMN adds the column to every internal table that mirrors the
//     table, filled with the value the base rows got;
//   - DROP/RENAME COLUMN, a column type change, RENAME TABLE and DROP TABLE are
//     refused while a scenario has unmerged changes to the table, and drop the
//     empty deltas and pre-images of the old shape otherwise.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ClientContext;
class PreparedStatementData;

class ScenarioBaseSchemaChange {
public:
	//! Wrap the plan of a prepared ALTER TABLE / DROP TABLE statement
	//! (called from ScenarioClientState::OnFinalizePrepare)
	static void WrapPlan(ClientContext &context, PreparedStatementData &prepared);
};

} // namespace duckdb
