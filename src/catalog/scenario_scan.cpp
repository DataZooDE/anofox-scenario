#include "catalog/scenario_scan.hpp"

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

namespace {

struct ScenarioScanBindData : public TableFunctionData {
	ScenarioScanBindData(ScenarioTableEntry &entry_p, TableCatalogEntry &base_entry_p,
	                     optional_ptr<DuckTableEntry> delta_table_p, vector<idx_t> pk_columns_p)
	    : entry(entry_p), base_entry(base_entry_p), delta_table(delta_table_p), pk_columns(std::move(pk_columns_p)) {
	}

	//! The scenario table entry: the identity the planner must see
	ScenarioTableEntry &entry;
	//! The base-side source of the scan: the live host table, a materialized
	//! copy, or (Phase 4) a versioned foreign table (DuckLake)
	TableCatalogEntry &base_entry;
	//! The delta table, when this scenario has modified the table
	optional_ptr<DuckTableEntry> delta_table;
	//! Logical PK column ids of the base table (empty when none)
	vector<idx_t> pk_columns;
	//! Phase 4: non-duck bases are scanned through their own table function,
	//! bound with an AT (TIMESTAMP => created_at) pin for versioned catalogs
	bool base_is_duck = true;
	TableFunction foreign_function;
	unique_ptr<FunctionData> foreign_bind;

	bool SupportStatementCache() const override {
		// Both entry references are owned by the scenario *transaction*
		// (fresh per transaction, correct staleness). Cached plans / prepared
		// statements must therefore rebind on every execution so they never
		// hold these references across a transaction boundary.
		return false;
	}
};

//! How an output column is produced from the base-side scan
struct ScenarioScanOutputMapping {
	//! Position in the scanned chunk; DConstants::INVALID_INDEX means the
	//! column is the __scenario_origin constant (0 for base rows)
	idx_t scan_pos = DConstants::INVALID_INDEX;
};

//! Merge-on-read scan. The (small) visible delta streams through a parallel
//! ColumnDataCollection scan; base row groups are handed out to threads via
//! DataTable's parallel-scan API with tombstoned/updated keys suppressed.
struct ScenarioScanGlobalState : public GlobalTableFunctionState {
	// --- shared column plumbing ---
	vector<ScenarioScanOutputMapping> out_map;
	vector<idx_t> key_positions;
	bool needs_projection = false;
	vector<StorageIndex> storage_ids;
	vector<LogicalType> scan_types;
	//! Pushed filters remapped from output positions to base scan positions;
	//! filters on __scenario_origin never reach the base scan (see InitGlobal)
	TableFilterSet base_filters;
	//! True when an origin filter excludes base rows entirely (origin = 1)
	bool base_side_disabled = false;

	// --- duck base ---
	ParallelTableScanState parallel_state;

	// --- foreign base (single-threaded) ---
	unique_ptr<GlobalTableFunctionState> foreign_global;
	unique_ptr<LocalTableFunctionState> foreign_local;
	bool foreign_exhausted = false;

	// --- delta side ---
	unique_ptr<ColumnDataCollection> visible_delta;
	ColumnDataParallelScanState delta_parallel_state;
	bool has_visible_delta = false;
	unordered_set<string> suppressed_keys;

	idx_t max_threads = 1;
	idx_t MaxThreads() const override {
		return max_threads;
	}
};

struct ScenarioScanLocalState : public LocalTableFunctionState {
	// duck base
	TableScanState scan_state;
	bool primed = false;
	bool base_done = false;
	DataChunk scan_chunk;
	// delta side
	ColumnDataLocalScanState delta_local_state;
	bool delta_done = false;
};

unique_ptr<GlobalTableFunctionState> ScenarioScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto result = make_uniq<ScenarioScanGlobalState>();
	bool has_delta = bind_data.delta_table != nullptr;

	// --- classify requested columns and build the base-side scan list -------
	// The scan list holds regular/rowid columns; __scenario_origin is a
	// constant (0) on the base side; __scenario_key_<k> maps to the PK column.
	vector<ColumnIndex> scan_columns;
	auto find_or_add_scan_column = [&](const ColumnIndex &col) -> idx_t {
		for (idx_t i = 0; i < scan_columns.size(); i++) {
			if (scan_columns[i].GetPrimaryIndex() == col.GetPrimaryIndex()) {
				return i;
			}
		}
		scan_columns.push_back(col);
		return scan_columns.size() - 1;
	};
	for (auto &col : input.column_indexes) {
		auto id = col.GetPrimaryIndex();
		ScenarioScanOutputMapping mapping;
		if (id == SCENARIO_ORIGIN_COLUMN_ID) {
			// constant on the base side
		} else if (id >= SCENARIO_KEY_COLUMN_START && id < SCENARIO_KEY_COLUMN_START + bind_data.pk_columns.size()) {
			auto k = id - SCENARIO_KEY_COLUMN_START;
			mapping.scan_pos = find_or_add_scan_column(ColumnIndex(bind_data.pk_columns[k]));
		} else {
			mapping.scan_pos = find_or_add_scan_column(col);
		}
		result->out_map.push_back(mapping);
	}
	// PK columns are always scanned when a delta exists (suppression keys)
	if (has_delta) {
		for (auto pk_col : bind_data.pk_columns) {
			result->key_positions.push_back(find_or_add_scan_column(ColumnIndex(pk_col)));
		}
	}
	// --- remap pushed filters onto the base scan list ------------------------
	// input.filters are keyed by *output* position (input.column_indexes).
	// The base scan list diverges from the output layout whenever virtual
	// columns are projected: __scenario_origin has no scan column at all and
	// __scenario_key_<k> dedupes onto its PK column. Forwarding the set
	// unremapped would filter the wrong storage column - so rebuild it keyed
	// by scan position, and fold origin filters against the base-side
	// constant (0) instead of pushing them into storage.
	if (input.filters) {
		for (auto &filter_entry : input.filters->filters) {
			auto out_pos = filter_entry.first;
			if (out_pos >= result->out_map.size()) {
				throw InternalException("anofox_scenario: pushed filter references an unprojected column");
			}
			auto scan_pos = result->out_map[out_pos].scan_pos;
			if (scan_pos == DConstants::INVALID_INDEX) {
				// __scenario_origin: constant 0 for every base row. The filter
				// either rejects all base rows (skip the base side) or none.
				auto origin_ref = make_uniq<BoundConstantExpression>(Value::TINYINT(0));
				auto folded = filter_entry.second->ToExpression(*origin_ref);
				Value fold_result;
				if (!ExpressionExecutor::TryEvaluateScalar(context, *folded, fold_result)) {
					throw InternalException("anofox_scenario: could not fold filter on __scenario_origin");
				}
				if (fold_result.IsNull() || !BooleanValue::Get(fold_result.DefaultCastAs(LogicalType::BOOLEAN))) {
					result->base_side_disabled = true;
				}
				continue;
			}
			// PushFilter conjuncts when two outputs (a key virtual column and
			// its PK column) dedupe onto the same scan position
			result->base_filters.PushFilter(ColumnIndex(scan_pos), filter_entry.second->Copy());
		}
	}

	// projection required unless outputs are exactly the scanned columns
	result->needs_projection = scan_columns.size() != result->out_map.size();
	if (!result->needs_projection) {
		for (idx_t i = 0; i < result->out_map.size(); i++) {
			if (result->out_map[i].scan_pos != i) {
				result->needs_projection = true;
				break;
			}
		}
	}

	// --- base scan init ------------------------------------------------------
	for (auto &col : scan_columns) {
		if (col.IsRowIdColumn()) {
			result->scan_types.push_back(LogicalType::ROW_TYPE);
		} else {
			result->scan_types.push_back(bind_data.entry.GetColumn(LogicalIndex(col.GetPrimaryIndex())).Type());
		}
	}
	if (bind_data.base_is_duck) {
		auto &data_table = bind_data.base_entry.GetStorage();
		for (auto &col : scan_columns) {
			result->storage_ids.push_back(bind_data.base_entry.GetStorageIndex(col));
		}
		data_table.InitializeParallelScan(context, result->parallel_state, scan_columns);
		result->max_threads = data_table.MaxThreads(context);
	} else {
		// Foreign base (Phase 4): drive the base's own table function.
		// Single-threaded, and our filter_pushdown is off for foreign bases,
		// so no filters are forwarded either.
		vector<idx_t> no_projection;
		TableFunctionInitInput foreign_input(bind_data.foreign_bind.get(), scan_columns, no_projection, nullptr);
		if (bind_data.foreign_function.init_global) {
			result->foreign_global = bind_data.foreign_function.init_global(context, foreign_input);
		}
		if (bind_data.foreign_function.init_local) {
			ThreadContext thread_context(context);
			ExecutionContext execution_context(context, thread_context, nullptr);
			result->foreign_local = bind_data.foreign_function.init_local(execution_context, foreign_input,
			                                                              result->foreign_global.get());
		}
		result->max_threads = 1;
	}

	// --- delta side: materialize visible rows + suppression keys -------------
	if (has_delta) {
		auto delta_table_ptr = bind_data.delta_table;
		auto &delta_table = *delta_table_ptr;

		// Output layout types (for the visible-delta collection)
		vector<LogicalType> output_types;
		for (auto &col : input.column_indexes) {
			auto id = col.GetPrimaryIndex();
			if (col.IsRowIdColumn()) {
				output_types.push_back(LogicalType::ROW_TYPE);
			} else if (id == SCENARIO_ORIGIN_COLUMN_ID) {
				output_types.push_back(LogicalType::TINYINT);
			} else if (id >= SCENARIO_KEY_COLUMN_START) {
				auto k = id - SCENARIO_KEY_COLUMN_START;
				output_types.push_back(
				    bind_data.entry.GetColumn(LogicalIndex(bind_data.pk_columns[k])).Type());
			} else {
				output_types.push_back(bind_data.entry.GetColumn(LogicalIndex(id)).Type());
			}
		}
		result->visible_delta = make_uniq<ColumnDataCollection>(Allocator::Get(context), output_types);

		// Pushed-down filters must also apply to the delta side, or filtered
		// reads would resurrect filtered-out delta rows. Rebuild them as an
		// expression over the output layout (filters are keyed by scan-list
		// position, which matches the output position for requested columns).
		unique_ptr<Expression> delta_filter;
		if (input.filters) {
			for (auto &filter_entry : input.filters->filters) {
				auto scan_pos = filter_entry.first;
				if (scan_pos >= result->out_map.size()) {
					throw InternalException("anofox_scenario: pushed filter references an unprojected column");
				}
				auto column_ref = make_uniq<BoundReferenceExpression>(output_types[scan_pos], scan_pos);
				auto filter_expr = filter_entry.second->ToExpression(*column_ref);
				if (delta_filter) {
					auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
					conjunction->children.push_back(std::move(delta_filter));
					conjunction->children.push_back(std::move(filter_expr));
					delta_filter = std::move(conjunction);
				} else {
					delta_filter = std::move(filter_expr);
				}
			}
		}
		unique_ptr<ExpressionExecutor> filter_executor;
		if (delta_filter) {
			filter_executor = make_uniq<ExpressionExecutor>(context, *delta_filter);
		}

		// Delta scan: _op + payload columns needed for outputs and keys
		vector<StorageIndex> delta_column_ids;
		vector<LogicalType> delta_types;
		delta_column_ids.emplace_back(ScenarioDelta::OP_COL);
		delta_types.push_back(LogicalType::VARCHAR);
		auto find_or_add_delta = [&](idx_t payload_col) -> idx_t {
			for (idx_t i = 1; i < delta_column_ids.size(); i++) {
				if (delta_column_ids[i].GetPrimaryIndex() == ScenarioDelta::PAYLOAD_START + payload_col) {
					return i;
				}
			}
			delta_column_ids.emplace_back(ScenarioDelta::PAYLOAD_START + payload_col);
			delta_types.push_back(
			    delta_table.GetColumn(LogicalIndex(ScenarioDelta::PAYLOAD_START + payload_col)).Type());
			return delta_column_ids.size() - 1;
		};
		// per output: position in the delta chunk, INVALID = NULL rowid,
		// INVALID-1 = origin constant 1
		constexpr idx_t DELTA_OUT_NULL = DConstants::INVALID_INDEX;
		const idx_t DELTA_OUT_ORIGIN = DConstants::INVALID_INDEX - 1;
		vector<idx_t> delta_pos_for_output;
		for (auto &col : input.column_indexes) {
			auto id = col.GetPrimaryIndex();
			if (col.IsRowIdColumn()) {
				delta_pos_for_output.push_back(DELTA_OUT_NULL); // delta rows: NULL rowid
			} else if (id == SCENARIO_ORIGIN_COLUMN_ID) {
				delta_pos_for_output.push_back(DELTA_OUT_ORIGIN);
			} else if (id >= SCENARIO_KEY_COLUMN_START) {
				auto k = id - SCENARIO_KEY_COLUMN_START;
				delta_pos_for_output.push_back(find_or_add_delta(bind_data.pk_columns[k]));
			} else {
				delta_pos_for_output.push_back(find_or_add_delta(id));
			}
		}
		vector<idx_t> delta_key_positions;
		for (auto pk_col : bind_data.pk_columns) {
			delta_key_positions.push_back(find_or_add_delta(pk_col));
		}

		DataChunk append_chunk;
		append_chunk.Initialize(Allocator::Get(context), output_types);
		idx_t append_count = 0;
		auto flush = [&]() {
			if (append_count == 0) {
				return;
			}
			append_chunk.SetCardinality(append_count);
			if (filter_executor) {
				SelectionVector sel(append_count);
				idx_t kept = filter_executor->SelectExpression(append_chunk, sel);
				if (kept == 0) {
					append_chunk.Reset();
					append_count = 0;
					return;
				}
				append_chunk.Slice(sel, kept);
			}
			result->visible_delta->Append(append_chunk);
			append_chunk.Reset();
			append_count = 0;
		};
		ScenarioDelta::ScanTableRows(
		    context, delta_table, std::move(delta_column_ids), delta_types, [&](DataChunk &chunk, idx_t row) {
			    auto op = chunk.GetValue(0, row).GetValue<string>()[0];
			    // Suppression keys come from ALL U/D rows, independent of any
			    // pushed filter (a filtered-out update still hides its base row)
			    if (op == 'U' || op == 'D') {
				    if (!delta_key_positions.empty()) {
					    result->suppressed_keys.insert(ScenarioDelta::MakeKey(chunk, row, delta_key_positions));
				    }
			    }
			    if (op == 'I' || op == 'U') {
				    for (idx_t out_idx = 0; out_idx < delta_pos_for_output.size(); out_idx++) {
					    auto pos = delta_pos_for_output[out_idx];
					    if (pos == DELTA_OUT_NULL) {
						    append_chunk.SetValue(out_idx, append_count, Value(LogicalType::ROW_TYPE));
					    } else if (pos == DELTA_OUT_ORIGIN) {
						    append_chunk.SetValue(out_idx, append_count, Value::TINYINT(1));
					    } else {
						    append_chunk.SetValue(out_idx, append_count, chunk.GetValue(pos, row));
					    }
				    }
				    if (++append_count == STANDARD_VECTOR_SIZE) {
					    flush();
				    }
			    }
			    return true;
		    });
		flush();
		if (result->visible_delta->Count() > 0) {
			result->visible_delta->InitializeScan(result->delta_parallel_state);
			result->has_visible_delta = true;
		}
	}
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> ScenarioScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                          GlobalTableFunctionState *global_state) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto &gstate = global_state->Cast<ScenarioScanGlobalState>();
	auto result = make_uniq<ScenarioScanLocalState>();
	if (bind_data.base_is_duck) {
		auto storage_ids = gstate.storage_ids;
		optional_ptr<TableFilterSet> base_filters;
		if (!gstate.base_filters.filters.empty()) {
			base_filters = &gstate.base_filters;
		}
		result->scan_state.Initialize(std::move(storage_ids), context.client, base_filters);
	}
	result->base_done = gstate.base_side_disabled;
	if (gstate.needs_projection) {
		result->scan_chunk.Initialize(Allocator::Get(context.client), gstate.scan_types);
	}
	result->delta_done = !gstate.has_visible_delta;
	return std::move(result);
}

bool EmitBaseChunk(ScenarioScanGlobalState &gstate, ScenarioScanLocalState &lstate, DataChunk &output,
                   DataChunk &scan_target);

void ScenarioScanExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto &gstate = input.global_state->Cast<ScenarioScanGlobalState>();
	auto &lstate = input.local_state->Cast<ScenarioScanLocalState>();

	// 1. stream the visible delta rows (parallel collection scan)
	if (!lstate.delta_done) {
		if (gstate.visible_delta->Scan(gstate.delta_parallel_state, lstate.delta_local_state, output) &&
		    output.size() > 0) {
			return;
		}
		lstate.delta_done = true;
		output.Reset();
	}

	// 2. stream the base, masking suppressed keys
	auto &scan_target = gstate.needs_projection ? lstate.scan_chunk : output;
	if (bind_data.base_is_duck) {
		if (lstate.base_done) {
			return;
		}
		auto &data_table = bind_data.base_entry.GetStorage();
		auto &transaction = DuckTransaction::Get(context, bind_data.base_entry.catalog);
		if (!lstate.primed) {
			lstate.primed = true;
			if (data_table.NextParallelScan(context, gstate.parallel_state, lstate.scan_state) == 0) {
				lstate.base_done = true;
				return;
			}
		}
		while (true) {
			// Always reset: an exhausted scan leaves the chunk untouched, and
			// a stale non-empty chunk would loop forever when the previous
			// iteration was fully suppressed
			scan_target.Reset();
			data_table.Scan(transaction, scan_target, lstate.scan_state);
			if (scan_target.size() == 0) {
				if (data_table.NextParallelScan(context, gstate.parallel_state, lstate.scan_state) == 0) {
					lstate.base_done = true;
					output.SetCardinality(0);
					return;
				}
				continue;
			}
			if (!EmitBaseChunk(gstate, lstate, output, scan_target)) {
				continue; // whole chunk suppressed; scan on
			}
			return;
		}
	}
	// foreign base: single-threaded
	if (gstate.foreign_exhausted) {
		return;
	}
	while (true) {
		scan_target.Reset();
		TableFunctionInput foreign_input(bind_data.foreign_bind.get(), gstate.foreign_local.get(),
		                                 gstate.foreign_global.get());
		bind_data.foreign_function.function(context, foreign_input, scan_target);
		if (scan_target.size() == 0) {
			gstate.foreign_exhausted = true;
			output.SetCardinality(0);
			return;
		}
		if (!EmitBaseChunk(gstate, lstate, output, scan_target)) {
			continue;
		}
		return;
	}
}

//! Apply suppression + projection to a scanned base chunk; false = fully suppressed
bool EmitBaseChunk(ScenarioScanGlobalState &gstate, ScenarioScanLocalState &lstate, DataChunk &output,
                   DataChunk &scan_target) {
	idx_t count = scan_target.size();
	if (!gstate.suppressed_keys.empty()) {
		SelectionVector sel(count);
		idx_t kept = 0;
		for (idx_t i = 0; i < count; i++) {
			auto key = ScenarioDelta::MakeKey(scan_target, i, gstate.key_positions);
			if (gstate.suppressed_keys.find(key) == gstate.suppressed_keys.end()) {
				sel.set_index(kept++, i);
			}
		}
		if (kept == 0) {
			return false;
		}
		scan_target.Slice(sel, kept);
		count = kept;
	}
	if (gstate.needs_projection) {
		for (idx_t out_idx = 0; out_idx < gstate.out_map.size(); out_idx++) {
			auto &mapping = gstate.out_map[out_idx];
			if (mapping.scan_pos == DConstants::INVALID_INDEX) {
				// __scenario_origin: base rows are origin 0
				output.data[out_idx].Reference(Value::TINYINT(0));
			} else {
				output.data[out_idx].Reference(lstate.scan_chunk.data[mapping.scan_pos]);
			}
		}
		output.SetCardinality(count);
	}
	return true;
}

BindInfo ScenarioScanGetBindInfo(const optional_ptr<FunctionData> bind_data) {
	auto &data = bind_data->Cast<ScenarioScanBindData>();
	return BindInfo(data.entry);
}

} // namespace

TableFunction ScenarioScanFunction::GetFunction(ClientContext &context, ScenarioTableEntry &entry,
                                                unique_ptr<FunctionData> &bind_data) {
	TableFunction function("scenario_scan", {}, ScenarioScanExecute);
	function.init_global = ScenarioScanInitGlobal;
	function.init_local = ScenarioScanInitLocal;
	function.get_bind_info = ScenarioScanGetBindInfo;
	function.projection_pushdown = true;

	auto &scenario_catalog = entry.GetScenarioCatalog();
	auto &host_catalog = scenario_catalog.GetHostCatalog(context);
	auto delta_table =
	    ScenarioDelta::TryGetDeltaTable(context, host_catalog, scenario_catalog.scenario_id, entry.name);
	auto result = make_uniq<ScenarioScanBindData>(entry, entry.base_entry, delta_table, entry.key_columns);
	result->base_is_duck = entry.base_entry.IsDuckTable();
	// WP2.1: duck bases honor pushed filters on both merge sides; foreign
	// scans cannot be trusted to apply someone else's filters, so pushdown
	// stays off there (the engine filters above the scan instead)
	function.filter_pushdown = result->base_is_duck;
	if (!result->base_is_duck) {
		// Phase 4: bind the foreign base's own scan; versioned bases
		// (DuckLake) are pinned to the scenario's creation time
		unique_ptr<BoundAtClause> at_clause;
		auto &base_catalog = scenario_catalog.GetBaseCatalog(context);
		if (base_catalog.GetCatalogType() == "ducklake") {
			at_clause = make_uniq<BoundAtClause>("timestamp", Value::TIMESTAMP(scenario_catalog.created_at));
		}
		EntryLookupInfo foreign_lookup(CatalogType::TABLE_ENTRY, entry.base_entry.name, at_clause.get(),
		                               QueryErrorContext());
		result->foreign_function = entry.base_entry.GetScanFunction(context, result->foreign_bind, foreign_lookup);
	}
	bind_data = std::move(result);
	return function;
}

} // namespace duckdb
