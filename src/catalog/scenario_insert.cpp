//===----------------------------------------------------------------------===//
// Transparent INSERT into scenario tables: rows land in the delta table with
// _op = 'I' ('U' when re-inserting over a tombstone). PK collisions are
// checked against the *merged* relation: the delta key map, and the base's
// own constraint machinery (DataTable::VerifyAppendConstraints -- the same
// path a real base INSERT takes), with distinct error messages (REQ-NFR-005).
//===----------------------------------------------------------------------===//

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"
#include "catalog/scenario_registry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/constraints/bound_not_null_constraint.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Shared write-path helpers (also used by update/delete sinks)
//===----------------------------------------------------------------------===//

void ScenarioDeltaKeyMap::Load(ClientContext &context, DuckTableEntry &delta_table, const vector<idx_t> &pk_columns) {
	keys.clear();
	vector<StorageIndex> column_ids;
	vector<LogicalType> types;
	column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	types.push_back(LogicalType::ROW_TYPE);
	column_ids.emplace_back(ScenarioDelta::OP_COL);
	types.push_back(LogicalType::VARCHAR);
	vector<idx_t> key_positions;
	for (auto pk_col : pk_columns) {
		key_positions.push_back(column_ids.size());
		column_ids.emplace_back(ScenarioDelta::PAYLOAD_START + pk_col);
		types.push_back(delta_table.GetColumn(LogicalIndex(ScenarioDelta::PAYLOAD_START + pk_col)).Type());
	}
	ScenarioDelta::ScanTableRows(context, delta_table, std::move(column_ids), types,
	                             [&](DataChunk &chunk, idx_t row) {
		                             ScenarioDeltaKeyInfo info;
		                             info.row_id = chunk.GetValue(0, row).GetValue<row_t>();
		                             info.op = chunk.GetValue(1, row).GetValue<string>()[0];
		                             keys[ScenarioDelta::MakeKey(chunk, row, key_positions)] = info;
		                             return true;
	                             });
}

void ScenarioDeltaAppendChunk(ClientContext &context, DuckTableEntry &delta_table,
                              const vector<unique_ptr<BoundConstraint>> &constraints, DataChunk &chunk,
                              const vector<row_t> &deleted_row_ids, optional_ptr<DataChunk> deleted_chunk) {
	auto &storage = delta_table.GetStorage();
	LocalAppendState append_state;
	storage.InitializeLocalAppend(append_state, delta_table, context, constraints);
	if (!deleted_row_ids.empty() && deleted_chunk) {
		Vector row_ids(LogicalType::ROW_TYPE, deleted_row_ids.size());
		for (idx_t i = 0; i < deleted_row_ids.size(); i++) {
			row_ids.SetValue(i, Value::BIGINT(deleted_row_ids[i]));
		}
		append_state.storage->AppendToDeleteIndexes(row_ids, *deleted_chunk);
	}
	storage.LocalAppend(append_state, context, chunk, false);
	storage.FinalizeLocalAppend(append_state);
}

void ScenarioVerifyNotNullAndCheck(ClientContext &context, TableCatalogEntry &entry,
                                   const vector<unique_ptr<BoundConstraint>> &bound_constraints, DataChunk &chunk) {
	for (auto &constraint : bound_constraints) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &not_null = constraint->Cast<BoundNotNullConstraint>();
			if (VectorOperations::HasNull(chunk.data[not_null.index.index], chunk.size())) {
				throw ConstraintException("NOT NULL constraint failed: %s.%s", entry.name,
				                          entry.GetColumn(LogicalIndex(not_null.index.index)).Name());
			}
			break;
		}
		case ConstraintType::CHECK: {
			auto &check = constraint->Cast<BoundCheckConstraint>();
			ExpressionExecutor executor(context, *check.expression);
			Vector result(LogicalType::INTEGER);
			executor.ExecuteExpression(chunk, result);
			UnifiedVectorFormat format;
			result.ToUnifiedFormat(chunk.size(), format);
			auto result_data = UnifiedVectorFormat::GetData<int32_t>(format);
			for (idx_t i = 0; i < chunk.size(); i++) {
				auto idx = format.sel->get_index(i);
				// NULL check results pass; only FALSE violates
				if (format.validity.RowIsValid(idx) && result_data[idx] == 0) {
					throw ConstraintException("CHECK constraint failed on table %s", entry.name);
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

//===----------------------------------------------------------------------===//
// PhysicalScenarioInsert
//===----------------------------------------------------------------------===//

namespace {

class ScenarioInsertGlobalState : public GlobalSinkState {
public:
	optional_ptr<DuckTableEntry> delta_table;
	vector<unique_ptr<BoundConstraint>> delta_constraints;
	ScenarioDeltaKeyMap delta_keys;
	vector<idx_t> pk_columns;
	//! Base-side unique/PK verification (same machinery as a real base INSERT)
	vector<unique_ptr<BoundConstraint>> base_constraints;
	unique_ptr<ConstraintState> base_constraint_state;
	//! RETURNING: collected result rows
	unique_ptr<ColumnDataCollection> return_collection;
	idx_t insert_count = 0;
	//! Chunk in delta layout, reused across Sink calls
	DataChunk delta_chunk;
	//! Buffer for the rows that need base-side verification
	DataChunk verify_chunk;
};

class PhysicalScenarioInsert : public PhysicalOperator {
public:
	PhysicalScenarioInsert(PhysicalPlan &physical_plan, vector<LogicalType> types, ScenarioTableEntry &entry,
	                       vector<unique_ptr<BoundConstraint>> bound_constraints, idx_t estimated_cardinality,
	                       bool return_chunk)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      entry(entry), bound_constraints(std::move(bound_constraints)), return_chunk(return_chunk) {
	}

	ScenarioTableEntry &entry;
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	bool return_chunk;

public:
	string GetName() const override {
		return "SCENARIO_INSERT";
	}

	// Sink interface
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	// Source interface (insert count, or the RETURNING rows)
	bool IsSource() const override {
		return true;
	}
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<ScenarioDMLSourceState>();
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

unique_ptr<GlobalSinkState> PhysicalScenarioInsert::GetGlobalSinkState(ClientContext &context) const {
	auto result = make_uniq<ScenarioInsertGlobalState>();
	result->pk_columns = entry.key_columns;
	return std::move(result);
}

SinkResultType PhysicalScenarioInsert::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<ScenarioInsertGlobalState>();
	auto &client = context.client;
	auto &scenario_catalog = entry.GetScenarioCatalog();
	auto &host_catalog = scenario_catalog.GetHostCatalog(client);

	// Lazy setup on the first chunk. The delta table was created eagerly by
	// scenario_create; DML never creates catalog entries (single-writer rule).
	if (!gstate.delta_table) {
		auto delta_ptr =
		    ScenarioDelta::TryGetDeltaTable(client, host_catalog, scenario_catalog.scenario_id, ScenarioDelta::LogicalName(entry));
		if (!delta_ptr) {
			throw InvalidInputException(
			    "Table '%s' was created in the base after scenario '%s': it is readable but not writable in "
			    "this scenario (v1 limitation). Create a fresh scenario to modify it",
			    entry.name, scenario_catalog.scenario_name);
		}
		auto &delta = *delta_ptr;
		gstate.delta_table = &delta;
		auto binder = Binder::CreateBinder(client);
		gstate.delta_constraints = binder->BindConstraints(delta);
		if (entry.base_entry.IsDuckTable()) {
			auto &base_duck = entry.base_entry.Cast<DuckTableEntry>();
			auto all_base_constraints = binder->BindConstraints(base_duck);
			// Base probe = unique/PK collisions ONLY. NOT NULL/CHECK are
			// evaluated by the sink itself, and FK constraints must NOT be
			// checked against the base alone: a referenced parent row may
			// exist only in the scenario (FKs are enforced at merge-back).
			for (auto &constraint : all_base_constraints) {
				if (constraint->type == ConstraintType::UNIQUE) {
					gstate.base_constraints.push_back(std::move(constraint));
				}
			}
			gstate.base_constraint_state =
			    base_duck.GetStorage().InitializeConstraintState(base_duck, gstate.base_constraints);
		}
		if (!gstate.pk_columns.empty()) {
			gstate.delta_keys.Load(client, delta, gstate.pk_columns);
		}
		gstate.delta_chunk.Initialize(Allocator::Get(client), delta.GetStorage().GetTypes());
		gstate.verify_chunk.Initialize(Allocator::Get(client), entry.GetTypes());
	}
	auto &delta_table = *gstate.delta_table;

	// Base NOT NULL + CHECK constraints apply to scenario rows
	ScenarioVerifyNotNullAndCheck(client, entry, bound_constraints, chunk);

	// PK collision policy against the merged relation
	vector<string> row_keys(chunk.size());
	vector<char> row_ops(chunk.size(), 'I');
	vector<row_t> tombstones_to_remove;
	vector<idx_t> tombstone_source_rows;
	if (!gstate.pk_columns.empty()) {
		vector<idx_t> key_positions;
		for (auto pk_col : gstate.pk_columns) {
			if (VectorOperations::HasNull(chunk.data[pk_col], chunk.size())) {
				throw ConstraintException("NOT NULL constraint failed: %s.%s (PRIMARY KEY)", entry.name,
				                          entry.GetColumn(LogicalIndex(pk_col)).Name());
			}
			key_positions.push_back(pk_col);
		}
		SelectionVector base_verify_sel(chunk.size());
		idx_t base_verify_count = 0;
		for (idx_t i = 0; i < chunk.size(); i++) {
			auto key = ScenarioDelta::MakeKey(chunk, i, key_positions);
			auto existing = gstate.delta_keys.keys.find(key);
			if (existing != gstate.delta_keys.keys.end()) {
				if (existing->second.op == 'D') {
					// Re-insert over a tombstone: net effect is an update of
					// the base row -> replace the tombstone with a 'U' row
					tombstones_to_remove.push_back(existing->second.row_id);
					tombstone_source_rows.push_back(i);
					row_ops[i] = 'U';
				} else {
					throw ConstraintException(
					    "Duplicate key violates primary key constraint on scenario table \"%s\": the key was "
					    "already inserted or modified in this scenario",
					    entry.name);
				}
			} else {
				// Not touched by this scenario: must not collide with a base row
				base_verify_sel.set_index(base_verify_count++, i);
			}
			// Record immediately: a later row of this same chunk with the
			// same PK must hit the duplicate branch above (and a tombstone
			// must not be replaced twice)
			ScenarioDeltaKeyInfo info;
			info.op = row_ops[i];
			info.row_id = -1;
			gstate.delta_keys.keys[key] = info;
			row_keys[i] = std::move(key);
		}

		// Probe the base through its own constraint machinery (duck bases
		// only; foreign bases like DuckLake have no PK indexes to violate)
		if (base_verify_count > 0 && gstate.base_constraint_state) {
			gstate.verify_chunk.Reset();
			gstate.verify_chunk.Reference(chunk);
			gstate.verify_chunk.Slice(base_verify_sel, base_verify_count);
			auto &base_duck = entry.base_entry.Cast<DuckTableEntry>();
			try {
				base_duck.GetStorage().VerifyAppendConstraints(*gstate.base_constraint_state, client,
				                                               gstate.verify_chunk, nullptr, nullptr);
			} catch (ConstraintException &) {
				throw ConstraintException(
				    "Duplicate key violates primary key constraint on scenario table \"%s\": the key exists in "
				    "the base table",
				    entry.name);
			}
		}

		// Apply D -> U transitions: drop the tombstone rows first
		if (!tombstones_to_remove.empty()) {
			auto &delta_storage = delta_table.GetStorage();
			auto delete_state = delta_storage.InitializeDelete(delta_table, client, gstate.delta_constraints);
			Vector row_ids(LogicalType::ROW_TYPE);
			for (idx_t i = 0; i < tombstones_to_remove.size(); i++) {
				row_ids.SetValue(i, Value::BIGINT(tombstones_to_remove[i]));
			}
			delta_storage.Delete(*delete_state, client, row_ids, tombstones_to_remove.size());
		}
	}

	// Build the delta chunk: _op, _ts, payload
	auto &delta_chunk = gstate.delta_chunk;
	delta_chunk.Reset();
	auto now = Timestamp::GetCurrentTimestamp();
	for (idx_t i = 0; i < chunk.size(); i++) {
		delta_chunk.SetValue(ScenarioDelta::OP_COL, i, Value(string(1, row_ops[i])));
	}
	delta_chunk.data[ScenarioDelta::TS_COL].Reference(Value::TIMESTAMP(now));
	for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
		delta_chunk.data[ScenarioDelta::PAYLOAD_START + col].Reference(chunk.data[col]);
	}
	auto count_column = ScenarioDelta::CountColumnIndex(delta_table);
	if (count_column.IsValid()) {
		// keyless bag deltas: every appended row carries multiplicity 1
		delta_chunk.data[count_column.GetIndex()].Reference(Value::BIGINT(1));
	}
	delta_chunk.SetCardinality(chunk.size());

	// (delta_keys entries for this chunk were already recorded in the
	// classification loop above)
	scenario_catalog.MarkHostWrite(client, DatabaseModificationType::INSERT_DATA);
	if (tombstones_to_remove.empty()) {
		delta_table.GetStorage().LocalAppend(delta_table, client, delta_chunk, gstate.delta_constraints);
	} else {
		// Register the removed tombstones in the delete indexes so the delta
		// PK index accepts the replacement rows within this statement
		DataChunk deleted_chunk;
		deleted_chunk.Initialize(Allocator::Get(client), delta_table.GetStorage().GetTypes());
		for (idx_t i = 0; i < tombstone_source_rows.size(); i++) {
			for (auto pk_col : gstate.pk_columns) {
				deleted_chunk.SetValue(ScenarioDelta::PAYLOAD_START + pk_col, i,
				                       chunk.GetValue(pk_col, tombstone_source_rows[i]));
			}
		}
		deleted_chunk.SetCardinality(tombstone_source_rows.size());
		ScenarioDeltaAppendChunk(client, delta_table, gstate.delta_constraints, delta_chunk, tombstones_to_remove,
		                         &deleted_chunk);
	}

	if (return_chunk) {
		if (!gstate.return_collection) {
			gstate.return_collection = make_uniq<ColumnDataCollection>(Allocator::Get(client), entry.GetTypes());
		}
		gstate.return_collection->Append(chunk);
	}
	gstate.insert_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SourceResultType PhysicalScenarioInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<ScenarioInsertGlobalState>();
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
	chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(gstate.insert_count)));
	chunk.SetCardinality(1);
	return SourceResultType::FINISHED;
}

} // namespace

PhysicalOperator &MakeScenarioInsertOperator(PhysicalPlanGenerator &planner, vector<LogicalType> types,
                                             ScenarioTableEntry &entry,
                                             vector<unique_ptr<BoundConstraint>> bound_constraints,
                                             idx_t estimated_cardinality, bool return_chunk) {
	return planner.Make<PhysicalScenarioInsert>(std::move(types), entry, std::move(bound_constraints),
	                                            estimated_cardinality, return_chunk);
}

//===----------------------------------------------------------------------===//
// ScenarioCatalog::PlanInsert
//===----------------------------------------------------------------------===//

void ScenarioCatalog::ThrowIfFrozen(ClientContext &context, const char *operation) {
	auto entry = ScenarioRegistry::LookupById(context, GetHostCatalog(context), scenario_id);
	if (entry && entry->frozen) {
		throw InvalidInputException(
		    "Cannot %s: scenario '%s' is frozen. Unfreeze it with CALL scenario_unfreeze('%s')", operation,
		    scenario_name, scenario_name);
	}
}

PhysicalOperator &ScenarioCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
                                              LogicalInsert &op, optional_ptr<PhysicalOperator> plan) {
	if (!plan) {
		throw NotImplementedException("INSERT without a source is not supported for scenario tables");
	}
	ThrowIfFrozen(context, "INSERT");
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw NotImplementedException(
		    "ON CONFLICT clauses on scenario tables are not supported yet (planned for v0.4)");
	}
	auto &entry = op.table.Cast<ScenarioTableEntry>();
	if (!op.column_index_map.empty()) {
		plan = planner.ResolveDefaultsProjection(op, *plan);
	}
	auto &insert = planner.Make<PhysicalScenarioInsert>(op.types, entry, std::move(op.bound_constraints),
	                                                    op.estimated_cardinality, op.return_chunk);
	insert.children.push_back(*plan);
	return insert;
}

} // namespace duckdb
