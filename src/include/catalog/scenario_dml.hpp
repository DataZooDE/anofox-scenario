//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_dml.hpp
//
// Shared machinery of the scenario write path (insert/update/delete sinks).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class BoundConstraint;
class ClientContext;
class DuckTableEntry;
class TableCatalogEntry;

//! Virtual identity columns exposed by scenario tables for DML binding.
//! __scenario_origin: 0 = row comes from base, 1 = row comes from the delta.
//! __scenario_key_<k>: the k-th PK column value of the row.
static constexpr column_t SCENARIO_ORIGIN_COLUMN_ID = UINT64_C(9223372036854775808) /*VIRTUAL_COLUMN_START*/ + 4096;
static constexpr column_t SCENARIO_KEY_COLUMN_START = SCENARIO_ORIGIN_COLUMN_ID + 1;

//! State of one PK in the delta table
struct ScenarioDeltaKeyInfo {
	char op;      // 'I', 'U' or 'D'
	row_t row_id; // rowid within the delta table (valid in this transaction)
};

//! In-memory view of a delta table keyed by PK; deltas are small by invariant
struct ScenarioDeltaKeyMap {
	unordered_map<string, ScenarioDeltaKeyInfo> keys;

	void Load(ClientContext &context, DuckTableEntry &delta_table, const vector<idx_t> &pk_columns);
};

//! Evaluate the base table's NOT NULL and CHECK constraints on scenario rows
void ScenarioVerifyNotNullAndCheck(ClientContext &context, TableCatalogEntry &entry,
                                   const vector<unique_ptr<BoundConstraint>> &bound_constraints, DataChunk &chunk);

class PhysicalPlanGenerator;
class ScenarioTableEntry;
class Expression;

//! Operator factories (used by both the DML plan hooks and PlanMergeInto)
PhysicalOperator &MakeScenarioInsertOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry,
                                             vector<unique_ptr<BoundConstraint>> bound_constraints,
                                             idx_t estimated_cardinality, bool return_chunk);
PhysicalOperator &MakeScenarioUpdateOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry, vector<PhysicalIndex> columns,
                                             vector<unique_ptr<Expression>> expressions,
                                             vector<unique_ptr<Expression>> bound_defaults,
                                             vector<unique_ptr<BoundConstraint>> bound_constraints,
                                             idx_t estimated_cardinality, bool return_chunk,
                                             optional_idx row_id_start);
PhysicalOperator &MakeScenarioDeleteOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry, idx_t row_id_start,
                                             idx_t estimated_cardinality);

//! Source-side state for DML operators: emits either the affected-count row
//! or, with RETURNING, the collected result rows
class ScenarioDMLSourceState : public GlobalSourceState {
public:
	ColumnDataScanState scan_state;
	bool initialized = false;
	bool count_emitted = false;
};

//! Append a delta-layout chunk. When rows with the same PK were deleted from
//! the delta earlier in this statement (op transitions D->U and U->D), their
//! rowids/keys must be registered in the transaction-local delete indexes
//! first, or the delta PK index reports a false duplicate (the same pattern
//! core uses for update_is_del_and_insert).
void ScenarioDeltaAppendChunk(ClientContext &context, DuckTableEntry &delta_table,
                              const vector<unique_ptr<BoundConstraint>> &constraints, DataChunk &chunk,
                              const vector<row_t> &deleted_row_ids, optional_ptr<DataChunk> deleted_chunk);

} // namespace duckdb
