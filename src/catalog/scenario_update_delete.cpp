//===----------------------------------------------------------------------===//
// Transparent UPDATE/DELETE on scenario tables. Row identity is the base PK
// (virtual columns __scenario_origin + __scenario_key_<k>); the op-transition
// matrix is applied against the delta key map:
//
//   | delta state for PK | UPDATE arrives              | DELETE arrives      |
//   | none (base row)    | append ('U', full new row)  | append ('D', keys)  |
//   | 'I'                | rewrite payload, stays 'I'  | remove the row      |
//   | 'U'                | rewrite payload, stays 'U'  | convert to 'D'      |
//   | 'D'                | unreachable (suppressed)    | unreachable         |
//===----------------------------------------------------------------------===//

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/update_state.hpp"

namespace duckdb {

namespace {

//===----------------------------------------------------------------------===//
// Shared sink state
//===----------------------------------------------------------------------===//

class ScenarioDMLGlobalState : public GlobalSinkState {
public:
	optional_ptr<DuckTableEntry> delta_table;
	vector<unique_ptr<BoundConstraint>> delta_constraints;
	ScenarioDeltaKeyMap delta_keys;
	vector<idx_t> pk_columns;
	//! Keys already written by this statement (double-touch detection)
	unordered_set<string> touched_keys;
	idx_t affected_count = 0;
	//! Buffer for delta appends, in delta layout
	DataChunk delta_chunk;

	void LazyInit(ClientContext &client, ScenarioTableEntry &entry) {
		if (delta_table) {
			return;
		}
		auto &scenario_catalog = entry.GetScenarioCatalog();
		auto &host_catalog = scenario_catalog.GetHostCatalog(client);
		auto delta_ptr =
		    ScenarioDelta::TryGetDeltaTable(client, host_catalog, scenario_catalog.scenario_id, entry.name);
		if (!delta_ptr) {
			throw InvalidInputException(
			    "Table '%s' was created in the base after scenario '%s': it is readable but not writable in "
			    "this scenario (v1 limitation). Create a fresh scenario to modify it",
			    entry.name, scenario_catalog.scenario_name);
		}
		delta_table = delta_ptr;
		auto binder = Binder::CreateBinder(client);
		delta_constraints = binder->BindConstraints(*delta_ptr);
		pk_columns = ScenarioDelta::GetPKColumns(entry.base_entry);
		delta_keys.Load(client, *delta_ptr, pk_columns);
		delta_chunk.Initialize(Allocator::Get(client), delta_ptr->GetStorage().GetTypes());
	}

	void CheckDoubleTouch(const string &key, const char *verb) {
		if (!touched_keys.insert(key).second) {
			throw InvalidInputException(
			    "Scenario %s modified the same row twice within one statement - this is not supported (v1)", verb);
		}
	}

	//! Append rows to the delta in one batch; rows given as (op, values).
	//! deleted_row_ids/deleted_keys register same-statement deletions (op
	//! transitions) so the delta PK index accepts the replacement rows.
	void FlushAppends(ClientContext &client, ScenarioTableEntry &entry, const vector<char> &ops,
	                  const vector<vector<Value>> &rows, const vector<row_t> &deleted_row_ids = {},
	                  const vector<vector<Value>> &deleted_keys = {}) {
		if (rows.empty()) {
			return;
		}
		auto now = Timestamp::GetCurrentTimestamp();
		optional_ptr<DataChunk> deleted_chunk_ptr;
		DataChunk deleted_chunk;
		if (!deleted_row_ids.empty()) {
			deleted_chunk.Initialize(Allocator::Get(client), delta_table->GetStorage().GetTypes());
			for (idx_t i = 0; i < deleted_keys.size(); i++) {
				for (idx_t k = 0; k < pk_columns.size(); k++) {
					deleted_chunk.SetValue(ScenarioDelta::PAYLOAD_START + pk_columns[k], i, deleted_keys[i][k]);
				}
			}
			deleted_chunk.SetCardinality(deleted_keys.size());
			deleted_chunk_ptr = &deleted_chunk;
		}
		idx_t offset = 0;
		while (offset < rows.size()) {
			delta_chunk.Reset();
			idx_t batch = MinValue<idx_t>(rows.size() - offset, STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < batch; i++) {
				auto &row = rows[offset + i];
				delta_chunk.SetValue(ScenarioDelta::OP_COL, i, Value(string(1, ops[offset + i])));
				delta_chunk.SetValue(ScenarioDelta::TS_COL, i, Value::TIMESTAMP(now));
				for (idx_t col = 0; col < row.size(); col++) {
					delta_chunk.SetValue(ScenarioDelta::PAYLOAD_START + col, i, row[col]);
				}
			}
			delta_chunk.SetCardinality(batch);
			ScenarioDeltaAppendChunk(client, *delta_table, delta_constraints, delta_chunk,
			                         offset == 0 ? deleted_row_ids : vector<row_t>(), deleted_chunk_ptr);
			offset += batch;
		}
	}

	//! Delete delta rows by rowid in one batch
	void FlushDeletes(ClientContext &client, const vector<row_t> &row_ids_to_delete) {
		if (row_ids_to_delete.empty()) {
			return;
		}
		auto &storage = delta_table->GetStorage();
		auto delete_state = storage.InitializeDelete(*delta_table, client, delta_constraints);
		idx_t offset = 0;
		while (offset < row_ids_to_delete.size()) {
			idx_t batch = MinValue<idx_t>(row_ids_to_delete.size() - offset, STANDARD_VECTOR_SIZE);
			Vector row_ids(LogicalType::ROW_TYPE);
			for (idx_t i = 0; i < batch; i++) {
				row_ids.SetValue(i, Value::BIGINT(row_ids_to_delete[offset + i]));
			}
			storage.Delete(*delete_state, client, row_ids, batch);
			offset += batch;
		}
	}
};

//===----------------------------------------------------------------------===//
// PhysicalScenarioUpdate
//===----------------------------------------------------------------------===//

class PhysicalScenarioUpdate : public PhysicalOperator {
public:
	PhysicalScenarioUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, ScenarioTableEntry &entry,
	                       vector<PhysicalIndex> columns, vector<unique_ptr<Expression>> expressions,
	                       vector<unique_ptr<Expression>> bound_defaults,
	                       vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      entry(entry), columns(std::move(columns)), expressions(std::move(expressions)),
	      bound_defaults(std::move(bound_defaults)), bound_constraints(std::move(bound_constraints)) {
	}

	ScenarioTableEntry &entry;
	vector<PhysicalIndex> columns;
	vector<unique_ptr<Expression>> expressions;
	vector<unique_ptr<Expression>> bound_defaults;
	vector<unique_ptr<BoundConstraint>> bound_constraints;

public:
	string GetName() const override {
		return "SCENARIO_UPDATE";
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<ScenarioDMLGlobalState>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		auto &gstate = sink_state->Cast<ScenarioDMLGlobalState>();
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(gstate.affected_count)));
		chunk.SetCardinality(1);
		return SourceResultType::FINISHED;
	}
};

SinkResultType PhysicalScenarioUpdate::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<ScenarioDMLGlobalState>();
	auto &client = context.client;
	gstate.LazyInit(client, entry);
	chunk.Flatten();

	// Evaluate the SET expressions (BindUpdateConstraints projected every
	// column, so this yields the complete post-image row)
	auto table_column_count = entry.GetColumns().PhysicalColumnCount();
	DataChunk full_chunk;
	full_chunk.Initialize(Allocator::Get(client), entry.GetTypes());
	full_chunk.SetCardinality(chunk.size());
	ExpressionExecutor default_executor(client, bound_defaults);
	default_executor.SetChunk(chunk);
	for (idx_t i = 0; i < expressions.size(); i++) {
		auto target_column = columns[i].index;
		if (expressions[i]->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
			default_executor.ExecuteExpression(target_column, full_chunk.data[target_column]);
			continue;
		}
		auto &binding = expressions[i]->Cast<BoundReferenceExpression>();
		full_chunk.data[target_column].Reference(chunk.data[binding.index]);
	}
	(void)table_column_count;

	// Base NOT NULL + CHECK constraints hold for the post-image
	ScenarioVerifyNotNullAndCheck(client, entry, bound_constraints, full_chunk);

	// Row identity: [__scenario_origin, __scenario_key_0..n-1] at the tail
	idx_t row_id_count = 1 + gstate.pk_columns.size();
	idx_t row_id_start = chunk.ColumnCount() - row_id_count;
	vector<idx_t> old_key_positions;
	for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
		old_key_positions.push_back(row_id_start + 1 + k);
	}

	entry.GetScenarioCatalog().MarkHostWrite(client, DatabaseModificationType::UPDATE_DATA |
	                                                     DatabaseModificationType::INSERT_DATA);

	// Apply the matrix
	vector<char> append_ops;
	vector<vector<Value>> append_rows;
	vector<row_t> rewrite_row_ids;
	vector<vector<Value>> rewrite_rows;
	for (idx_t i = 0; i < chunk.size(); i++) {
		auto origin = chunk.GetValue(row_id_start, i).GetValue<int8_t>();
		auto key = ScenarioDelta::MakeKey(chunk, i, old_key_positions);
		gstate.CheckDoubleTouch(key, "UPDATE");

		vector<Value> row_values;
		for (idx_t col = 0; col < entry.GetColumns().LogicalColumnCount(); col++) {
			row_values.push_back(full_chunk.GetValue(col, i));
		}
		if (origin == 0) {
			// base row: record the update in the delta
			append_ops.push_back('U');
			append_rows.push_back(std::move(row_values));
			ScenarioDeltaKeyInfo info;
			info.op = 'U';
			info.row_id = -1;
			gstate.delta_keys.keys[key] = info;
		} else {
			// delta row ('I' or 'U'): rewrite its payload in place
			auto existing = gstate.delta_keys.keys.find(key);
			if (existing == gstate.delta_keys.keys.end()) {
				throw InternalException("anofox_scenario: delta row for updated key not found in key map");
			}
			rewrite_row_ids.push_back(existing->second.row_id);
			rewrite_rows.push_back(std::move(row_values));
		}
		gstate.affected_count++;
	}
	gstate.FlushAppends(client, entry, append_ops, append_rows);

	// In-place payload rewrite for delta rows (op unchanged)
	if (!rewrite_row_ids.empty()) {
		auto &storage = gstate.delta_table->GetStorage();
		auto update_state = storage.InitializeUpdate(*gstate.delta_table, client, gstate.delta_constraints);
		auto delta_types = storage.GetTypes();
		auto now = Timestamp::GetCurrentTimestamp();
		// PK payload columns are covered by the delta's PK index and cannot
		// change here (PK updates are gated at bind): exclude them so the
		// in-place update never touches an indexed column.
		unordered_set<idx_t> pk_set(gstate.pk_columns.begin(), gstate.pk_columns.end());
		vector<PhysicalIndex> update_columns;
		vector<idx_t> value_columns; // table-layout column served by each update column (after _ts)
		update_columns.emplace_back(ScenarioDelta::TS_COL);
		for (idx_t col = 0; col < entry.GetColumns().LogicalColumnCount(); col++) {
			if (pk_set.find(col) != pk_set.end()) {
				continue;
			}
			update_columns.emplace_back(ScenarioDelta::PAYLOAD_START + col);
			value_columns.push_back(col);
		}
		DataChunk update_chunk;
		vector<LogicalType> update_types;
		for (auto &col : update_columns) {
			update_types.push_back(delta_types[col.index]);
		}
		update_chunk.Initialize(Allocator::Get(client), update_types);
		idx_t offset = 0;
		while (offset < rewrite_row_ids.size()) {
			idx_t batch = MinValue<idx_t>(rewrite_row_ids.size() - offset, STANDARD_VECTOR_SIZE);
			update_chunk.Reset();
			Vector row_ids(LogicalType::ROW_TYPE);
			for (idx_t i = 0; i < batch; i++) {
				row_ids.SetValue(i, Value::BIGINT(rewrite_row_ids[offset + i]));
				update_chunk.SetValue(0, i, Value::TIMESTAMP(now));
				for (idx_t out_col = 0; out_col < value_columns.size(); out_col++) {
					update_chunk.SetValue(1 + out_col, i, rewrite_rows[offset + i][value_columns[out_col]]);
				}
			}
			update_chunk.SetCardinality(batch);
			storage.Update(*update_state, client, row_ids, update_columns, update_chunk);
			offset += batch;
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

//===----------------------------------------------------------------------===//
// PhysicalScenarioDelete
//===----------------------------------------------------------------------===//

class PhysicalScenarioDelete : public PhysicalOperator {
public:
	PhysicalScenarioDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, ScenarioTableEntry &entry,
	                       idx_t row_id_start, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      entry(entry), row_id_start(row_id_start) {
	}

	ScenarioTableEntry &entry;
	//! Position of __scenario_origin in the child chunk; keys follow
	idx_t row_id_start;

public:
	string GetName() const override {
		return "SCENARIO_DELETE";
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<ScenarioDMLGlobalState>();
	}
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		auto &gstate = sink_state->Cast<ScenarioDMLGlobalState>();
		chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(gstate.affected_count)));
		chunk.SetCardinality(1);
		return SourceResultType::FINISHED;
	}
};

SinkResultType PhysicalScenarioDelete::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<ScenarioDMLGlobalState>();
	auto &client = context.client;
	gstate.LazyInit(client, entry);
	chunk.Flatten();

	vector<idx_t> key_positions;
	for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
		key_positions.push_back(row_id_start + 1 + k);
	}
	auto column_count = entry.GetColumns().LogicalColumnCount();

	entry.GetScenarioCatalog().MarkHostWrite(client, DatabaseModificationType::DELETE_DATA |
	                                                     DatabaseModificationType::INSERT_DATA);

	vector<char> append_ops;
	vector<vector<Value>> append_rows;
	vector<row_t> rows_to_remove;
	vector<vector<Value>> removed_keys;
	for (idx_t i = 0; i < chunk.size(); i++) {
		auto origin = chunk.GetValue(row_id_start, i).GetValue<int8_t>();
		auto key = ScenarioDelta::MakeKey(chunk, i, key_positions);
		gstate.CheckDoubleTouch(key, "DELETE");

		if (origin == 0) {
			// base row: tombstone (keys set, other payload NULL)
			vector<Value> row_values;
			for (idx_t col = 0; col < column_count; col++) {
				row_values.push_back(Value(entry.GetColumn(LogicalIndex(col)).Type()));
			}
			for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
				row_values[gstate.pk_columns[k]] = chunk.GetValue(key_positions[k], i);
			}
			append_ops.push_back('D');
			append_rows.push_back(std::move(row_values));
			ScenarioDeltaKeyInfo info;
			info.op = 'D';
			info.row_id = -1;
			gstate.delta_keys.keys[key] = info;
		} else {
			auto existing = gstate.delta_keys.keys.find(key);
			if (existing == gstate.delta_keys.keys.end()) {
				throw InternalException("anofox_scenario: delta row for deleted key not found in key map");
			}
			// remove the delta row; 'U' rows get a fresh tombstone ('I' rows
			// simply vanish - the scenario never saw them)
			rows_to_remove.push_back(existing->second.row_id);
			vector<Value> key_values;
			for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
				key_values.push_back(chunk.GetValue(key_positions[k], i));
			}
			removed_keys.push_back(std::move(key_values));
			if (existing->second.op == 'U') {
				vector<Value> row_values;
				for (idx_t col = 0; col < column_count; col++) {
					row_values.push_back(Value(entry.GetColumn(LogicalIndex(col)).Type()));
				}
				for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
					row_values[gstate.pk_columns[k]] = chunk.GetValue(key_positions[k], i);
				}
				append_ops.push_back('D');
				append_rows.push_back(std::move(row_values));
				existing->second.op = 'D';
				existing->second.row_id = -1;
			} else {
				gstate.delta_keys.keys.erase(existing);
			}
		}
		gstate.affected_count++;
	}
	gstate.FlushDeletes(client, rows_to_remove);
	gstate.FlushAppends(client, entry, append_ops, append_rows, rows_to_remove, removed_keys);
	return SinkResultType::NEED_MORE_INPUT;
}

} // namespace

//===----------------------------------------------------------------------===//
// Plan hooks
//===----------------------------------------------------------------------===//

PhysicalOperator &ScenarioCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalUpdate &op, PhysicalOperator &plan) {
	ThrowIfFrozen(context, "UPDATE");
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING on scenario tables is not supported yet (planned for v0.4)");
	}
	auto &entry = op.table.Cast<ScenarioTableEntry>();
	if (ScenarioDelta::GetPKColumns(entry.base_entry).empty()) {
		throw NotImplementedException(
		    "UPDATE/DELETE in scenarios requires a PRIMARY KEY on the base table (v1 limitation)");
	}
	auto &update = planner.Make<PhysicalScenarioUpdate>(op.types, entry, op.columns, std::move(op.expressions),
	                                                    std::move(op.bound_defaults),
	                                                    std::move(op.bound_constraints), op.estimated_cardinality);
	update.children.push_back(plan);
	return update;
}

PhysicalOperator &ScenarioCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalDelete &op, PhysicalOperator &plan) {
	ThrowIfFrozen(context, "DELETE");
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING on scenario tables is not supported yet (planned for v0.4)");
	}
	auto &entry = op.table.Cast<ScenarioTableEntry>();
	if (ScenarioDelta::GetPKColumns(entry.base_entry).empty()) {
		throw NotImplementedException(
		    "UPDATE/DELETE in scenarios requires a PRIMARY KEY on the base table (v1 limitation)");
	}
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &del = planner.Make<PhysicalScenarioDelete>(op.types, entry, bound_ref.index, op.estimated_cardinality);
	del.children.push_back(plan);
	return del;
}

} // namespace duckdb
