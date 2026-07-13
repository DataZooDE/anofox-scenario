#include "catalog/scenario_scan.hpp"

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
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
	//! The base-side source of the scan. Phase 1: the live host table.
	//! (Phase 2: a materialized copy; Phase 4: a versioned DuckLake read.)
	TableCatalogEntry &base_entry;
	//! The delta table, when this scenario has modified the table
	optional_ptr<DuckTableEntry> delta_table;
	//! Logical PK column ids of the base table (empty when none)
	vector<idx_t> pk_columns;
	//! Phase 4: non-duck bases (e.g. DuckLake) are scanned through their own
	//! table function, bound with an AT (TIMESTAMP => created_at) pin
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

//! Single-threaded merge-on-read scan: stream the (small) delta first, then
//! the base with tombstoned/updated keys suppressed.
struct ScenarioScanGlobalState : public GlobalTableFunctionState {
	// --- base side ---
	ParallelTableScanState parallel_state;
	TableScanState scan_state;
	bool base_exhausted = false;
	//! Output column production from the scanned chunk
	vector<ScenarioScanOutputMapping> out_map;
	//! Scanned-chunk positions of the PK columns (suppression key)
	vector<idx_t> key_positions;
	//! Buffer in scanned layout (used when it differs from the output layout)
	DataChunk scan_chunk;
	//! True when scan_chunk differs from the output layout
	bool needs_projection = false;
	//! Phase 4: foreign-base scan states
	unique_ptr<GlobalTableFunctionState> foreign_global;
	unique_ptr<LocalTableFunctionState> foreign_local;

	// --- delta side ---
	//! Visible delta rows (op I/U) already in output layout
	unique_ptr<ColumnDataCollection> visible_delta;
	ColumnDataScanState delta_scan_state;
	bool delta_exhausted = true;
	//! Keys whose base rows are replaced or deleted by the delta
	unordered_set<string> suppressed_keys;

	idx_t MaxThreads() const override {
		// v1: correctness first; base-scan parallelism is reserved (WP2.1)
		return 1;
	}
};

unique_ptr<GlobalTableFunctionState> ScenarioScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto result = make_uniq<ScenarioScanGlobalState>();
	bool has_delta = bind_data.delta_table != nullptr;

	// --- classify requested columns and build the base-side scan list -------
	// The scan list holds regular/rowid columns; __scenario_origin is a
	// constant (0) on the base side; __scenario_key_<k> maps to the PK column.
	vector<ColumnIndex> scan_columns;
	auto find_or_add_scan_column = [&](const ColumnIndex &col) {
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
	vector<LogicalType> scan_types;
	for (auto &col : scan_columns) {
		if (col.IsRowIdColumn()) {
			scan_types.push_back(LogicalType::ROW_TYPE);
		} else {
			scan_types.push_back(bind_data.entry.GetColumn(LogicalIndex(col.GetPrimaryIndex())).Type());
		}
	}
	if (bind_data.base_is_duck) {
		auto &data_table = bind_data.base_entry.GetStorage();
		vector<StorageIndex> storage_ids;
		for (auto &col : scan_columns) {
			storage_ids.push_back(bind_data.base_entry.GetStorageIndex(col));
		}
		data_table.InitializeParallelScan(context, result->parallel_state, scan_columns);
		result->scan_state.Initialize(std::move(storage_ids), context, input.filters.get());
		if (data_table.NextParallelScan(context, result->parallel_state, result->scan_state) == 0) {
			result->base_exhausted = true;
		}
	} else {
		// Foreign base (Phase 4): drive the base's own table function
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
	}
	if (result->needs_projection) {
		result->scan_chunk.Initialize(Allocator::Get(context), scan_types);
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

		// Delta scan: _op + payload columns needed for outputs and keys
		vector<StorageIndex> delta_column_ids;
		vector<LogicalType> delta_types;
		delta_column_ids.emplace_back(ScenarioDelta::OP_COL);
		delta_types.push_back(LogicalType::VARCHAR);
		auto find_or_add_delta = [&](idx_t payload_col) {
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
			if (append_count > 0) {
				append_chunk.SetCardinality(append_count);
				result->visible_delta->Append(append_chunk);
				append_chunk.Reset();
				append_count = 0;
			}
		};
		ScenarioDelta::ScanTableRows(
		    context, delta_table, std::move(delta_column_ids), delta_types, [&](DataChunk &chunk, idx_t row) {
			    auto op = chunk.GetValue(0, row).GetValue<string>()[0];
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
			result->visible_delta->InitializeScan(result->delta_scan_state);
			result->delta_exhausted = false;
		}
	}
	return std::move(result);
}

void ScenarioScanExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<ScenarioScanBindData>();
	auto &gstate = input.global_state->Cast<ScenarioScanGlobalState>();

	// 1. stream the visible delta rows
	if (!gstate.delta_exhausted) {
		if (gstate.visible_delta->Scan(gstate.delta_scan_state, output) && output.size() > 0) {
			return;
		}
		gstate.delta_exhausted = true;
		output.Reset();
	}

	// 2. stream the base, masking suppressed keys
	if (gstate.base_exhausted) {
		return;
	}
	auto &scan_target = gstate.needs_projection ? gstate.scan_chunk : output;
	while (true) {
		// Always reset: an exhausted scan leaves the chunk untouched, and a
		// stale non-empty chunk would loop forever when the previous
		// iteration was fully suppressed
		scan_target.Reset();
		if (bind_data.base_is_duck) {
			auto &data_table = bind_data.base_entry.GetStorage();
			auto &transaction = DuckTransaction::Get(context, bind_data.base_entry.catalog);
			data_table.Scan(transaction, scan_target, gstate.scan_state);
			if (scan_target.size() == 0) {
				if (data_table.NextParallelScan(context, gstate.parallel_state, gstate.scan_state) == 0) {
					gstate.base_exhausted = true;
					output.SetCardinality(0);
					return;
				}
				continue;
			}
		} else {
			TableFunctionInput foreign_input(bind_data.foreign_bind.get(), gstate.foreign_local.get(),
			                                 gstate.foreign_global.get());
			bind_data.foreign_function.function(context, foreign_input, scan_target);
			if (scan_target.size() == 0) {
				gstate.base_exhausted = true;
				output.SetCardinality(0);
				return;
			}
		}
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
				continue; // whole chunk suppressed; scan on
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
					output.data[out_idx].Reference(gstate.scan_chunk.data[mapping.scan_pos]);
				}
			}
			output.SetCardinality(count);
		}
		return;
	}
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
	function.get_bind_info = ScenarioScanGetBindInfo;
	function.projection_pushdown = true;
	// Filters are applied by the engine above the scan in v1 (correct, and
	// the delta side stays negligible); pushdown is reserved headroom (WP2.1).
	function.filter_pushdown = false;

	auto &scenario_catalog = entry.GetScenarioCatalog();
	auto &host_catalog = scenario_catalog.GetHostCatalog(context);
	auto delta_table =
	    ScenarioDelta::TryGetDeltaTable(context, host_catalog, scenario_catalog.scenario_id, entry.name);
	auto result = make_uniq<ScenarioScanBindData>(entry, entry.base_entry, delta_table,
	                                              ScenarioDelta::GetPKColumns(entry.base_entry));
	result->base_is_duck = entry.base_entry.IsDuckTable();
	if (!result->base_is_duck) {
		// Phase 4: bind the foreign base's own scan; versioned bases
		// (DuckLake) are pinned to the scenario's creation time
		unique_ptr<BoundAtClause> at_clause;
		auto &base_catalog = scenario_catalog.GetBaseCatalog(context);
		if (base_catalog.GetCatalogType() == "ducklake") {
			at_clause =
			    make_uniq<BoundAtClause>("timestamp", Value::TIMESTAMP(scenario_catalog.created_at));
		}
		EntryLookupInfo foreign_lookup(CatalogType::TABLE_ENTRY, entry.base_entry.name, at_clause.get(),
		                               QueryErrorContext());
		result->foreign_function =
		    entry.base_entry.GetScanFunction(context, result->foreign_bind, foreign_lookup);
	}
	bind_data = std::move(result);
	return function;
}

} // namespace duckdb
