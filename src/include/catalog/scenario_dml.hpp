//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_dml.hpp
//
// Shared machinery of the scenario write path (insert/update/delete sinks).
//===----------------------------------------------------------------------===//

#pragma once

#include "catalog/scenario_delta.hpp"
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
//! Keyless tables only: __scenario_rowno, a per-scan unique row number. The
//! whole-row identity cannot tell duplicate copies apart from one copy that a
//! join (MERGE / UPDATE ... FROM) emits twice; the row number can.
static constexpr column_t SCENARIO_ROWNO_COLUMN_ID = SCENARIO_ORIGIN_COLUMN_ID - 1;

//! Target rows already modified by one statement (double-touch detection).
//! One instance per UPDATE/DELETE sink, or one shared by all action sinks of a
//! MERGE INTO. Keyed tables track the serialized key, keyless tables the
//! per-scan __scenario_rowno (dense from 0, so two bitmaps suffice).
//! A DELETE touching a row a DELETE of the same statement already removed is
//! a no-op (the outcome is the same whichever match wins); every other repeat
//! - an UPDATE, or an UPDATE and a DELETE of one MERGE - throws.
class ScenarioTouchedRows {
public:
	//! true: first touch, apply it. false: repeated DELETE, skip the row.
	bool TouchKey(const string &key, bool is_delete, const char *verb);
	bool TouchRowno(const Value &rowno, bool is_delete, const char *verb);

	[[noreturn]] static void ThrowDoubleTouch(const char *verb);

private:
	//! key -> touched by a DELETE
	unordered_map<string, bool> keys;
	vector<bool> rowno_touched;
	vector<bool> rowno_deleted;
};

//! Can the physical child of an UPDATE/DELETE emit one target row more than
//! once (a join: UPDATE ... FROM, DELETE ... USING, IN-subqueries)? A bare
//! scan + filter + projection cannot, so it needs no touch tracking (and no
//! per-row memory).
bool ScenarioChildCanRepeatRows(const PhysicalOperator &child);

//! State of one PK in the delta table
struct ScenarioDeltaKeyInfo {
	char op;      // 'I', 'U' or 'D'
	row_t row_id; // rowid within the delta table (valid in this transaction)
};

//! A statement's view of a keyed delta table: PK -> delta row. A key absent
//! from `keys` is not in the delta. Keys are resolved on demand by probing
//! the delta's PRIMARY KEY index (committed + transaction-local), so a
//! statement costs O(rows it touches), not O(delta) (#36). Sinks must
//! Resolve() every key a chunk references before reading or writing `keys`
//! for it: each key is then probed once, before the statement changed it, and
//! `keys` equals a start-of-statement snapshot restricted to touched keys.
struct ScenarioDeltaKeyMap {
	unordered_map<string, ScenarioDeltaKeyInfo> keys;

	void Initialize(ClientContext &context, DuckTableEntry &delta_table, const vector<idx_t> &pk_columns);
	//! Resolve the keys of `chunk` (key values at `key_positions`, in PK order)
	void Resolve(ClientContext &context, DataChunk &chunk, const vector<idx_t> &key_positions);

private:
	//! Fallback when the delta has no bound PK index: snapshot every key
	void LoadAll(ClientContext &context);

	optional_ptr<DuckTableEntry> delta_table;
	vector<idx_t> pk_columns;
	ScenarioDeltaIndexProbe index_probe;
	unordered_set<string> resolved;
};

//! Evaluate the base table's NOT NULL and CHECK constraints on scenario rows
void ScenarioVerifyNotNullAndCheck(ClientContext &context, TableCatalogEntry &entry,
                                   const vector<unique_ptr<BoundConstraint>> &bound_constraints, DataChunk &chunk);

class PhysicalPlanGenerator;
class ScenarioTableEntry;
class Expression;
struct IndexEntry;

//! Unique-constraint probes against a DuckDB base table, with merged-state
//! semantics. The base's UNIQUE values must not collide with scenario rows -
//! except values held by base rows the scenario REPLACED (a 'D' or 'U' delta
//! row for their key): those base rows are not part of the merged relation,
//! and a 'U' row's values are covered by the delta's mirrored UNIQUE indexes.
//! Replaced rows are masked through a delete index holding their (values,
//! rowid), the same mechanism core uses for a transaction's own deletes.
class ScenarioBaseUniqueProbe {
public:
	void Initialize(ScenarioTableEntry &entry, const vector<idx_t> &key_columns);

	bool HasSecondary() const {
		return !secondary_indexes.empty();
	}
	//! Probe rows (full table layout) against the base PRIMARY KEY index.
	//! Throws the raw ConstraintException on a collision.
	void ProbePrimaryKey(DataChunk &rows);
	//! Probe rows (full table layout) against the base's secondary UNIQUE
	//! indexes, ignoring base rows the scenario replaced. pending_replaced
	//! lists keys (full rows, key columns set) this statement turns into 'D'/'U'
	//! that are not in the delta table yet. Throws the raw ConstraintException.
	void ProbeSecondary(ClientContext &context, DuckTableEntry &delta_table, DataChunk &rows,
	                    const vector<vector<Value>> &pending_replaced);
	//! Keyless tables (bag deltas, no mirrored UNIQUE index): the merged count
	//! of a non-NULL unique tuple is base_has(v) + sum(I) - sum(D) over the
	//! delta rows holding it. Rows (full table layout) are the new rows to
	//! add; pending_deleted are this statement's not-yet-flushed 'D' rows.
	//! Throws a ConstraintException if a row would duplicate a value.
	void ProbeKeyless(ClientContext &context, DuckTableEntry &delta_table, DataChunk &rows,
	                  const vector<vector<Value>> &pending_deleted);

private:
	//! Base rowids and full rows of every base row the scenario replaced
	void CollectReplaced(ClientContext &context, DuckTableEntry &delta_table,
	                     const vector<vector<Value>> &pending_replaced, vector<row_t> &row_ids,
	                     vector<vector<Value>> &rows);

	optional_ptr<ScenarioTableEntry> entry;
	vector<idx_t> key_columns;
	optional_ptr<IndexEntry> key_index;
	vector<reference<IndexEntry>> secondary_indexes;
};

//! Operator factories (used by both the DML plan hooks and PlanMergeInto)
PhysicalOperator &MakeScenarioInsertOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry,
                                             vector<unique_ptr<BoundConstraint>> bound_constraints,
                                             idx_t estimated_cardinality, bool return_chunk,
                                             const char *operation = "INSERT");
PhysicalOperator &MakeScenarioUpdateOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry, vector<PhysicalIndex> columns,
                                             vector<unique_ptr<Expression>> expressions,
                                             vector<unique_ptr<Expression>> bound_defaults,
                                             vector<unique_ptr<BoundConstraint>> bound_constraints,
                                             idx_t estimated_cardinality, bool return_chunk,
                                             optional_idx row_id_start,
                                             shared_ptr<ScenarioTouchedRows> shared_touched = nullptr,
                                             const char *operation = "UPDATE");
//! The row-id columns (GetRowIdColumns order) are contiguous from row_id_start,
//! as in MERGE INTO's projection
PhysicalOperator &MakeScenarioDeleteOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry, idx_t row_id_start,
                                             idx_t estimated_cardinality, bool return_chunk,
                                             shared_ptr<ScenarioTouchedRows> shared_touched = nullptr,
                                             const char *operation = "DELETE");

//! Operation name the write sinks of a MERGE INTO report in BeginWrite errors
static constexpr const char *SCENARIO_MERGE_OPERATION = "MERGE INTO";

//! Keyless tables need a v0.4.1+ bag delta (with _count) for UPDATE, DELETE
//! and MERGE INTO; throws for a legacy delta (a missing one is created in the
//! current layout by the first write)
void ThrowIfKeylessUnsupported(ClientContext &context, ScenarioTableEntry &entry);

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
