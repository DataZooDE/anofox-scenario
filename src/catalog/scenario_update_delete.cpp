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
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

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
	//! Base-side unique/PK verification (PK-moving updates)
	vector<unique_ptr<BoundConstraint>> base_constraints;
	unique_ptr<ConstraintState> base_constraint_state;
	//! RETURNING: collected result rows
	unique_ptr<ColumnDataCollection> return_collection;
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
		    ScenarioDelta::TryGetDeltaTable(client, host_catalog, scenario_catalog.scenario_id, ScenarioDelta::LogicalName(entry));
		if (!delta_ptr) {
			throw InvalidInputException(
			    "Table '%s' was created in the base after scenario '%s': it is readable but not writable in "
			    "this scenario (v1 limitation). Create a fresh scenario to modify it",
			    entry.name, scenario_catalog.scenario_name);
		}
		delta_table = delta_ptr;
		auto binder = Binder::CreateBinder(client);
		delta_constraints = binder->BindConstraints(*delta_ptr);
		if (entry.base_entry.IsDuckTable()) {
			auto &base_duck = entry.base_entry.Cast<DuckTableEntry>();
			auto all_base_constraints = binder->BindConstraints(base_duck);
			// Base probe = unique/PK collisions ONLY. NOT NULL/CHECK are
			// evaluated by the sink itself, and FK constraints must NOT be
			// checked against the base alone: a referenced parent row may
			// exist only in the scenario (FKs are enforced at merge-back).
			for (auto &constraint : all_base_constraints) {
				if (constraint->type == ConstraintType::UNIQUE) {
					base_constraints.push_back(std::move(constraint));
				}
			}
			base_constraint_state = base_duck.GetStorage().InitializeConstraintState(base_duck, base_constraints);
		}
		pk_columns = entry.key_columns;
		delta_keys.Load(client, *delta_ptr, pk_columns);
		delta_chunk.Initialize(Allocator::Get(client), delta_ptr->GetStorage().GetTypes());
	}

	//! Verify rows (full table layout) against the base's unique constraints
	void VerifyAgainstBase(ClientContext &client, ScenarioTableEntry &entry, DataChunk &full_chunk,
	                       SelectionVector &sel, idx_t count) {
		if (count == 0 || !base_constraint_state) {
			return;
		}
		DataChunk verify_chunk;
		verify_chunk.Initialize(Allocator::Get(client), entry.GetTypes());
		verify_chunk.Reference(full_chunk);
		verify_chunk.Slice(sel, count);
		auto &base_duck = entry.base_entry.Cast<DuckTableEntry>();
		try {
			base_duck.GetStorage().VerifyAppendConstraints(*base_constraint_state, client, verify_chunk, nullptr,
			                                               nullptr);
		} catch (ConstraintException &) {
			throw ConstraintException(
			    "Duplicate key violates primary key constraint on scenario table \"%s\": the key exists in the "
			    "base table",
			    entry.name);
		}
	}

	void CheckDoubleTouch(const string &key, const char *verb) {
		if (!touched_keys.insert(key).second) {
			throw InvalidInputException(
			    "Scenario %s modified the same row twice within one statement - this is not supported (v1)", verb);
		}
	}

	//! Fetch the full payload of delta rows by rowid (before deleting them),
	//! so the delete-index registration covers every unique index - not just
	//! the PK (secondary UNIQUE columns must be freed for replacement rows)
	vector<vector<Value>> FetchDeltaRows(ClientContext &client, ScenarioTableEntry &entry,
	                                     const vector<row_t> &row_ids) {
		vector<vector<Value>> result;
		if (row_ids.empty()) {
			return result;
		}
		auto column_count = entry.GetColumns().LogicalColumnCount();
		auto &storage = delta_table->GetStorage();
		auto &transaction = DuckTransaction::Get(client, delta_table->catalog);
		vector<StorageIndex> fetch_columns;
		vector<LogicalType> fetch_types;
		for (idx_t col = 0; col < column_count; col++) {
			fetch_columns.emplace_back(ScenarioDelta::PAYLOAD_START + col);
			fetch_types.push_back(entry.GetColumn(LogicalIndex(col)).Type());
		}
		idx_t offset = 0;
		while (offset < row_ids.size()) {
			idx_t batch = MinValue<idx_t>(row_ids.size() - offset, STANDARD_VECTOR_SIZE);
			DataChunk fetched;
			fetched.Initialize(Allocator::Get(client), fetch_types);
			Vector fetch_rowids(LogicalType::ROW_TYPE, batch);
			for (idx_t i = 0; i < batch; i++) {
				fetch_rowids.SetValue(i, Value::BIGINT(row_ids[offset + i]));
			}
			ColumnFetchState fetch_state;
			storage.Fetch(transaction, fetched, fetch_columns, fetch_rowids, batch, fetch_state);
			for (idx_t i = 0; i < batch; i++) {
				vector<Value> row_values;
				for (idx_t col = 0; col < column_count; col++) {
					row_values.push_back(fetched.GetValue(col, i));
				}
				result.push_back(std::move(row_values));
			}
			offset += batch;
		}
		return result;
	}

	//! Append rows to the delta in one batch; rows given as (op, values).
	//! deleted_row_ids/deleted_rows register same-statement deletions (op
	//! transitions) so the delta unique indexes accept the replacement rows.
	void FlushAppends(ClientContext &client, ScenarioTableEntry &entry, const vector<char> &ops,
	                  const vector<vector<Value>> &rows, const vector<row_t> &deleted_row_ids = {},
	                  const vector<vector<Value>> &deleted_rows = {}) {
		if (rows.empty()) {
			return;
		}
		auto now = Timestamp::GetCurrentTimestamp();
		optional_ptr<DataChunk> deleted_chunk_ptr;
		DataChunk deleted_chunk;
		if (!deleted_row_ids.empty()) {
			deleted_chunk.Initialize(Allocator::Get(client), delta_table->GetStorage().GetTypes());
			for (idx_t i = 0; i < deleted_rows.size(); i++) {
				for (idx_t col = 0; col < deleted_rows[i].size(); col++) {
					deleted_chunk.SetValue(ScenarioDelta::PAYLOAD_START + col, i, deleted_rows[i][col]);
				}
			}
			deleted_chunk.SetCardinality(deleted_rows.size());
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

SourceResultType ScenarioDMLGetData(const ScenarioDMLGlobalState &gstate, bool return_chunk, DataChunk &chunk,
                                    OperatorSourceInput &input) {
	auto &source = input.global_state.Cast<ScenarioDMLSourceState>();
	if (return_chunk) {
		if (!gstate.return_collection) {
			return SourceResultType::FINISHED;
		}
		if (!source.initialized) {
			gstate.return_collection->InitializeScan(source.scan_state);
			source.initialized = true;
		}
		gstate.return_collection->Scan(source.scan_state, chunk);
		return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
	}
	if (source.count_emitted) {
		return SourceResultType::FINISHED;
	}
	source.count_emitted = true;
	chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(gstate.affected_count)));
	chunk.SetCardinality(1);
	return SourceResultType::FINISHED;
}

class PhysicalScenarioUpdate : public PhysicalOperator {
public:
	PhysicalScenarioUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, ScenarioTableEntry &entry,
	                       vector<PhysicalIndex> columns, vector<unique_ptr<Expression>> expressions,
	                       vector<unique_ptr<Expression>> bound_defaults,
	                       vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t estimated_cardinality,
	                       bool return_chunk, optional_idx explicit_row_id_start)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      entry(entry), columns(std::move(columns)), expressions(std::move(expressions)),
	      bound_defaults(std::move(bound_defaults)), bound_constraints(std::move(bound_constraints)),
	      return_chunk(return_chunk), explicit_row_id_start(explicit_row_id_start) {
	}

	ScenarioTableEntry &entry;
	vector<PhysicalIndex> columns;
	vector<unique_ptr<Expression>> expressions;
	vector<unique_ptr<Expression>> bound_defaults;
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	bool return_chunk;
	//! Position of the row-id columns in the child chunk. For plain UPDATE
	//! they trail the projection; MERGE INTO supplies op.row_id_start.
	optional_idx explicit_row_id_start;

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
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<ScenarioDMLSourceState>();
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		return ScenarioDMLGetData(sink_state->Cast<ScenarioDMLGlobalState>(), return_chunk, chunk, input);
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

	// Row identity: [__scenario_origin, __scenario_key_0..n-1]
	// [__scenario_origin, __scenario_key_0..n-1, (duck bases) rowid]
	idx_t row_id_count = entry.GetRowIdColumns().size();
	idx_t row_id_start =
	    explicit_row_id_start.IsValid() ? explicit_row_id_start.GetIndex() : chunk.ColumnCount() - row_id_count;
	vector<idx_t> old_key_positions;
	for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
		old_key_positions.push_back(row_id_start + 1 + k);
	}

	entry.GetScenarioCatalog().MarkHostWrite(client, DatabaseModificationType::UPDATE_DATA |
	                                                     DatabaseModificationType::INSERT_DATA);

	// Apply the matrix. PK-moving updates decompose into DELETE(old key) +
	// INSERT/tombstone-replace(new key).
	auto column_count = entry.GetColumns().LogicalColumnCount();
	vector<char> append_ops;
	vector<vector<Value>> append_rows;
	vector<row_t> rows_to_remove;
	SelectionVector base_verify_sel(chunk.size());
	idx_t base_verify_count = 0;
	auto null_payload_row = [&](DataChunk &keys_chunk, const vector<idx_t> &positions, idx_t row) {
		vector<Value> row_values;
		for (idx_t col = 0; col < column_count; col++) {
			row_values.push_back(Value(entry.GetColumn(LogicalIndex(col)).Type()));
		}
		for (idx_t k = 0; k < gstate.pk_columns.size(); k++) {
			row_values[gstate.pk_columns[k]] = keys_chunk.GetValue(positions[k], row);
		}
		return row_values;
	};
	vector<idx_t> new_key_positions;
	for (auto pk_col : gstate.pk_columns) {
		new_key_positions.push_back(pk_col);
	}
	for (idx_t i = 0; i < chunk.size(); i++) {
		auto origin = chunk.GetValue(row_id_start, i).GetValue<int8_t>();
		auto old_key = ScenarioDelta::MakeKey(chunk, i, old_key_positions);
		auto new_key = ScenarioDelta::MakeKey(full_chunk, i, new_key_positions);
		gstate.CheckDoubleTouch(old_key, "UPDATE");

		vector<Value> row_values;
		for (idx_t col = 0; col < column_count; col++) {
			row_values.push_back(full_chunk.GetValue(col, i));
		}
		if (new_key == old_key) {
			if (origin == 0) {
				// base row: record the update in the delta
				append_ops.push_back('U');
				append_rows.push_back(std::move(row_values));
				ScenarioDeltaKeyInfo info;
				info.op = 'U';
				info.row_id = -1;
				gstate.delta_keys.keys[old_key] = info;
			} else {
				// delta row ('I' or 'U'): replace it (remove + re-append with
				// the same op - in-place updates cannot touch indexed columns)
				auto existing = gstate.delta_keys.keys.find(old_key);
				if (existing == gstate.delta_keys.keys.end()) {
					throw InternalException("anofox_scenario: delta row for updated key not found in key map");
				}
				rows_to_remove.push_back(existing->second.row_id);
				append_ops.push_back(existing->second.op);
				append_rows.push_back(std::move(row_values));
				existing->second.row_id = -1;
			}
			gstate.affected_count++;
			continue;
		}

		// --- PK move ------------------------------------------------------
		gstate.CheckDoubleTouch(new_key, "UPDATE");
		// old side: vacate the key
		if (origin == 0) {
			append_ops.push_back('D');
			append_rows.push_back(null_payload_row(chunk, old_key_positions, i));
			ScenarioDeltaKeyInfo info;
			info.op = 'D';
			info.row_id = -1;
			gstate.delta_keys.keys[old_key] = info;
		} else {
			auto existing = gstate.delta_keys.keys.find(old_key);
			if (existing == gstate.delta_keys.keys.end()) {
				throw InternalException("anofox_scenario: delta row for moved key not found in key map");
			}
			rows_to_remove.push_back(existing->second.row_id);
			if (existing->second.op == 'U') {
				append_ops.push_back('D');
				append_rows.push_back(null_payload_row(chunk, old_key_positions, i));
				existing->second.op = 'D';
				existing->second.row_id = -1;
			} else {
				gstate.delta_keys.keys.erase(existing);
			}
		}
		// new side: claim the key
		auto target = gstate.delta_keys.keys.find(new_key);
		if (target != gstate.delta_keys.keys.end()) {
			if (target->second.op == 'D') {
				// moving onto a tombstoned base key: net effect is an update
				rows_to_remove.push_back(target->second.row_id);
				append_ops.push_back('U');
				append_rows.push_back(std::move(row_values));
				target->second.op = 'U';
				target->second.row_id = -1;
			} else {
				throw ConstraintException(
				    "Duplicate key violates primary key constraint on scenario table \"%s\": the key was "
				    "already inserted or modified in this scenario",
				    entry.name);
			}
		} else {
			// must not collide with a visible base row
			base_verify_sel.set_index(base_verify_count++, i);
			append_ops.push_back('I');
			append_rows.push_back(std::move(row_values));
			ScenarioDeltaKeyInfo info;
			info.op = 'I';
			info.row_id = -1;
			gstate.delta_keys.keys[new_key] = info;
		}
		gstate.affected_count++;
	}
	gstate.VerifyAgainstBase(client, entry, full_chunk, base_verify_sel, base_verify_count);
	auto removed_rows = gstate.FetchDeltaRows(client, entry, rows_to_remove);
	gstate.FlushDeletes(client, rows_to_remove);
	gstate.FlushAppends(client, entry, append_ops, append_rows, rows_to_remove, removed_rows);
	if (return_chunk) {
		if (!gstate.return_collection) {
			gstate.return_collection = make_uniq<ColumnDataCollection>(Allocator::Get(client), entry.GetTypes());
		}
		gstate.return_collection->Append(full_chunk);
	}
	return SinkResultType::NEED_MORE_INPUT;
}

//===----------------------------------------------------------------------===//
// PhysicalScenarioDelete
//===----------------------------------------------------------------------===//

class PhysicalScenarioDelete : public PhysicalOperator {
public:
	PhysicalScenarioDelete(PhysicalPlan &physical_plan, vector<LogicalType> types, ScenarioTableEntry &entry,
	                       idx_t row_id_start, idx_t estimated_cardinality, bool return_chunk)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      entry(entry), row_id_start(row_id_start), return_chunk(return_chunk) {
	}

	ScenarioTableEntry &entry;
	//! Position of __scenario_origin in the child chunk; keys follow, then
	//! (duck bases) the base rowid
	idx_t row_id_start;
	bool return_chunk;

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
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<ScenarioDMLSourceState>();
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		return ScenarioDMLGetData(sink_state->Cast<ScenarioDMLGlobalState>(), return_chunk, chunk, input);
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

	// RETURNING: fetch the visible values of the doomed rows *before* the
	// delta is mutated. Base rows come by rowid (trails the keys in the child
	// chunk); scenario rows come from their delta row (full payload for I/U).
	vector<vector<Value>> returning_rows;
	if (return_chunk) {
		returning_rows.resize(chunk.size());
		idx_t rowid_pos = row_id_start + 1 + gstate.pk_columns.size();
		vector<row_t> base_rowids, delta_rowids;
		vector<idx_t> base_slots, delta_slots;
		for (idx_t i = 0; i < chunk.size(); i++) {
			auto origin = chunk.GetValue(row_id_start, i).GetValue<int8_t>();
			if (origin == 0) {
				base_rowids.push_back(chunk.GetValue(rowid_pos, i).GetValue<row_t>());
				base_slots.push_back(i);
			} else {
				auto key = ScenarioDelta::MakeKey(chunk, i, key_positions);
				auto existing = gstate.delta_keys.keys.find(key);
				if (existing == gstate.delta_keys.keys.end()) {
					throw InternalException("anofox_scenario: delta row for deleted key not found in key map");
				}
				delta_rowids.push_back(existing->second.row_id);
				delta_slots.push_back(i);
			}
		}
		auto fetch_rows = [&](TableCatalogEntry &table, const vector<row_t> &rowids, idx_t first_storage_col,
		                      const vector<idx_t> &slots) {
			if (rowids.empty()) {
				return;
			}
			auto &storage = table.GetStorage();
			auto &transaction = DuckTransaction::Get(client, table.catalog);
			vector<StorageIndex> fetch_columns;
			vector<LogicalType> fetch_types;
			for (idx_t col = 0; col < column_count; col++) {
				fetch_columns.emplace_back(first_storage_col + col);
				fetch_types.push_back(entry.GetColumn(LogicalIndex(col)).Type());
			}
			DataChunk fetched;
			fetched.Initialize(Allocator::Get(client), fetch_types);
			Vector fetch_rowids(LogicalType::ROW_TYPE, rowids.size());
			for (idx_t i = 0; i < rowids.size(); i++) {
				fetch_rowids.SetValue(i, Value::BIGINT(rowids[i]));
			}
			ColumnFetchState fetch_state;
			storage.Fetch(transaction, fetched, fetch_columns, fetch_rowids, rowids.size(), fetch_state);
			for (idx_t i = 0; i < rowids.size(); i++) {
				vector<Value> row_values;
				for (idx_t col = 0; col < column_count; col++) {
					row_values.push_back(fetched.GetValue(col, i));
				}
				returning_rows[slots[i]] = std::move(row_values);
			}
		};
		fetch_rows(entry.base_entry, base_rowids, 0, base_slots);
		fetch_rows(*gstate.delta_table, delta_rowids, ScenarioDelta::PAYLOAD_START, delta_slots);
	}

	vector<char> append_ops;
	vector<vector<Value>> append_rows;
	vector<row_t> rows_to_remove;
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
	auto removed_rows = gstate.FetchDeltaRows(client, entry, rows_to_remove);
	gstate.FlushDeletes(client, rows_to_remove);
	gstate.FlushAppends(client, entry, append_ops, append_rows, rows_to_remove, removed_rows);

	if (return_chunk) {
		if (!gstate.return_collection) {
			gstate.return_collection = make_uniq<ColumnDataCollection>(client, entry.GetTypes());
		}
		DataChunk out_chunk;
		out_chunk.Initialize(Allocator::Get(client), entry.GetTypes());
		for (idx_t i = 0; i < returning_rows.size(); i++) {
			for (idx_t col = 0; col < column_count; col++) {
				out_chunk.SetValue(col, i, returning_rows[i][col]);
			}
		}
		out_chunk.SetCardinality(returning_rows.size());
		gstate.return_collection->Append(out_chunk);
	}
	return SinkResultType::NEED_MORE_INPUT;
}

} // namespace

PhysicalOperator &MakeScenarioUpdateOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry, vector<PhysicalIndex> columns,
                                             vector<unique_ptr<Expression>> expressions,
                                             vector<unique_ptr<Expression>> bound_defaults,
                                             vector<unique_ptr<BoundConstraint>> bound_constraints,
                                             idx_t estimated_cardinality, bool return_chunk,
                                             optional_idx row_id_start) {
	return planner.Make<PhysicalScenarioUpdate>(std::move(types), entry, std::move(columns), std::move(expressions),
	                                            std::move(bound_defaults), std::move(bound_constraints),
	                                            estimated_cardinality, return_chunk, row_id_start);
}

PhysicalOperator &MakeScenarioDeleteOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry, idx_t row_id_start,
                                             idx_t estimated_cardinality, bool return_chunk) {
	return planner.Make<PhysicalScenarioDelete>(std::move(types), entry, row_id_start, estimated_cardinality,
	                                            return_chunk);
}

//===----------------------------------------------------------------------===//
// Plan hooks
//===----------------------------------------------------------------------===//

PhysicalOperator &ScenarioCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalUpdate &op, PhysicalOperator &plan) {
	ThrowIfFrozen(context, "UPDATE");
	auto &entry = op.table.Cast<ScenarioTableEntry>();
	if (entry.key_columns.empty()) {
		throw NotImplementedException("UPDATE/DELETE in scenarios requires a PRIMARY KEY on the base table or "
		                              "key_columns declared at scenario_create (v1 limitation)");
	}
	auto &update = planner.Make<PhysicalScenarioUpdate>(
	    op.types, entry, op.columns, std::move(op.expressions), std::move(op.bound_defaults),
	    std::move(op.bound_constraints), op.estimated_cardinality, op.return_chunk, optional_idx());
	update.children.push_back(plan);
	return update;
}

PhysicalOperator &ScenarioCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalDelete &op, PhysicalOperator &plan) {
	ThrowIfFrozen(context, "DELETE");
	auto &entry = op.table.Cast<ScenarioTableEntry>();
	if (op.return_chunk && !entry.base_entry.IsDuckTable()) {
		throw NotImplementedException("DELETE ... RETURNING on scenarios over non-DuckDB bases (e.g. DuckLake) is "
		                              "not supported yet - SELECT the rows before deleting them");
	}
	if (entry.key_columns.empty()) {
		throw NotImplementedException("UPDATE/DELETE in scenarios requires a PRIMARY KEY on the base table or "
		                              "key_columns declared at scenario_create (v1 limitation)");
	}
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &del = planner.Make<PhysicalScenarioDelete>(op.types, entry, bound_ref.index, op.estimated_cardinality,
	                                                 op.return_chunk);
	del.children.push_back(plan);
	return del;
}

} // namespace duckdb
