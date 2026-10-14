//===----------------------------------------------------------------------===//
// Merged-state UNIQUE probes against a DuckDB base table (see
// ScenarioBaseUniqueProbe in scenario_dml.hpp). The merged relation is
//   delta I/U rows  UNION  base rows without a delta row for their key,
// so uniqueness splits exactly into: among delta rows (the delta's mirrored
// UNIQUE indexes), among untouched base rows (the base's own indexes), and
// delta row vs untouched base row - this probe, with the replaced base rows
// masked out.
//===----------------------------------------------------------------------===//

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

void ScenarioBaseUniqueProbe::Initialize(ScenarioTableEntry &entry_p, const vector<idx_t> &key_columns_p) {
	entry = &entry_p;
	key_columns = key_columns_p;
	if (!entry_p.base_entry.IsDuckTable()) {
		// foreign bases (DuckLake) have no unique indexes to violate
		return;
	}
	auto &base = entry_p.base_entry.Cast<DuckTableEntry>();
	unordered_set<column_t> key_set(key_columns.begin(), key_columns.end());
	for (auto &index_entry : base.GetStorage().GetDataTableInfo()->GetIndexes().IndexEntries()) {
		auto &index = *index_entry.index;
		if (!index.IsUnique() || index.GetIndexType() != ART::TYPE_NAME || !index.IsBound()) {
			continue;
		}
		auto &column_set = index.GetColumnIdSet();
		bool is_key_index = !key_set.empty() && column_set.size() == key_set.size();
		for (auto col : column_set) {
			if (key_set.find(col) == key_set.end()) {
				is_key_index = false;
				break;
			}
		}
		if (is_key_index) {
			key_index = &index_entry;
		} else {
			secondary_indexes.push_back(index_entry);
		}
	}
}

void ScenarioBaseUniqueProbe::ProbePrimaryKey(DataChunk &rows) {
	if (!key_index || rows.size() == 0) {
		return;
	}
	auto &art = key_index->index->Cast<ART>();
	lock_guard<mutex> guard(key_index->lock);
	IndexAppendInfo append_info;
	art.VerifyAppend(rows, append_info, nullptr);
}

void ScenarioBaseUniqueProbe::CollectReplaced(ClientContext &context, DuckTableEntry &delta_table,
                                              const vector<vector<Value>> &pending_replaced, vector<row_t> &row_ids,
                                              vector<vector<Value>> &rows) {
	auto &base = entry->base_entry.Cast<DuckTableEntry>();
	auto column_count = entry->GetColumns().LogicalColumnCount();
	vector<LogicalType> table_types;
	for (idx_t col = 0; col < column_count; col++) {
		table_types.push_back(entry->GetColumn(LogicalIndex(col)).Type());
	}

	// 1. keys the scenario replaced: 'D'/'U' delta rows plus this statement's
	//    not-yet-flushed ones (a key, once D/U, always names a base row)
	unordered_map<string, vector<Value>> replaced_keys;
	vector<idx_t> key_positions;
	for (idx_t k = 0; k < key_columns.size(); k++) {
		key_positions.push_back(k + 1);
	}
	vector<StorageIndex> delta_columns;
	vector<LogicalType> delta_types;
	delta_columns.emplace_back(ScenarioDelta::OP_COL);
	delta_types.push_back(LogicalType::VARCHAR);
	for (auto key_col : key_columns) {
		delta_columns.emplace_back(ScenarioDelta::PAYLOAD_START + key_col);
		delta_types.push_back(table_types[key_col]);
	}
	ScenarioDelta::ScanTableRows(
	    context, delta_table, std::move(delta_columns), delta_types, [&](DataChunk &chunk, idx_t row) {
		    auto op = chunk.GetValue(0, row).GetValue<string>();
		    if (op == "D" || op == "U") {
			    vector<Value> key_values;
			    for (idx_t k = 0; k < key_columns.size(); k++) {
				    key_values.push_back(chunk.GetValue(k + 1, row));
			    }
			    replaced_keys.emplace(ScenarioDelta::MakeKey(chunk, row, key_positions), std::move(key_values));
		    }
		    return true;
	    });
	if (!pending_replaced.empty()) {
		DataChunk key_chunk;
		vector<LogicalType> key_types;
		for (auto key_col : key_columns) {
			key_types.push_back(table_types[key_col]);
		}
		key_chunk.Initialize(Allocator::Get(context), key_types);
		vector<idx_t> positions;
		for (idx_t k = 0; k < key_columns.size(); k++) {
			positions.push_back(k);
		}
		for (auto &pending : pending_replaced) {
			key_chunk.Reset();
			vector<Value> key_values;
			for (idx_t k = 0; k < key_columns.size(); k++) {
				key_chunk.SetValue(k, 0, pending[key_columns[k]]);
				key_values.push_back(pending[key_columns[k]]);
			}
			key_chunk.SetCardinality(1);
			replaced_keys.emplace(ScenarioDelta::MakeKey(key_chunk, 0, positions), std::move(key_values));
		}
	}
	if (replaced_keys.empty()) {
		return;
	}

	// 2. their base rowids: a point lookup in the base PK index, or (declared
	//    key_columns over a PK-less base) one base scan
	if (key_index) {
		auto &art = key_index->index->Cast<ART>();
		lock_guard<mutex> guard(key_index->lock);
		DataChunk key_rows;
		key_rows.Initialize(Allocator::Get(context), table_types);
		DataChunk expr_chunk;
		expr_chunk.Initialize(Allocator::Get(context), art.logical_types);
		auto it = replaced_keys.begin();
		while (it != replaced_keys.end()) {
			key_rows.Reset();
			idx_t count = 0;
			for (; it != replaced_keys.end() && count < STANDARD_VECTOR_SIZE; ++it, count++) {
				for (idx_t col = 0; col < column_count; col++) {
					key_rows.SetValue(col, count, Value(table_types[col]));
				}
				for (idx_t k = 0; k < key_columns.size(); k++) {
					key_rows.SetValue(key_columns[k], count, it->second[k]);
				}
			}
			key_rows.SetCardinality(count);
			expr_chunk.Reset();
			art.ExecuteExpressions(key_rows, expr_chunk);
			ArenaAllocator arena(Allocator::Get(context));
			unsafe_vector<ARTKey> keys(count);
			art.GenerateKeys<>(arena, expr_chunk, keys);
			for (idx_t i = 0; i < count; i++) {
				if (keys[i].Empty()) {
					continue;
				}
				auto leaf = ARTOperator::Lookup(art, art.tree, keys[i], 0);
				if (leaf && leaf->GetType() == NType::LEAF_INLINED) {
					row_ids.push_back(leaf->GetRowId());
				}
			}
		}
	} else {
		vector<StorageIndex> base_columns;
		vector<LogicalType> base_types;
		base_columns.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
		base_types.push_back(LogicalType::ROW_TYPE);
		for (auto key_col : key_columns) {
			base_columns.emplace_back(key_col);
			base_types.push_back(table_types[key_col]);
		}
		ScenarioDelta::ScanTableRows(context, base, std::move(base_columns), base_types,
		                             [&](DataChunk &chunk, idx_t row) {
			                             if (replaced_keys.count(ScenarioDelta::MakeKey(chunk, row, key_positions))) {
				                             row_ids.push_back(chunk.GetValue(0, row).GetValue<row_t>());
			                             }
			                             return true;
		                             });
	}

	// 3. their full base rows (the unique values to mask)
	auto &storage = base.GetStorage();
	auto &transaction = DuckTransaction::Get(context, base.catalog);
	vector<StorageIndex> fetch_columns;
	for (idx_t col = 0; col < column_count; col++) {
		fetch_columns.emplace_back(col);
	}
	idx_t offset = 0;
	while (offset < row_ids.size()) {
		idx_t batch = MinValue<idx_t>(row_ids.size() - offset, STANDARD_VECTOR_SIZE);
		DataChunk fetched;
		fetched.Initialize(Allocator::Get(context), table_types);
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
			rows.push_back(std::move(row_values));
		}
		offset += batch;
	}
}

void ScenarioBaseUniqueProbe::ProbeSecondary(ClientContext &context, DuckTableEntry &delta_table, DataChunk &rows,
                                             const vector<vector<Value>> &pending_replaced) {
	if (secondary_indexes.empty() || rows.size() == 0) {
		return;
	}
	// Fast path: nothing collides with any base row at all
	try {
		for (auto &index_entry : secondary_indexes) {
			auto &art = index_entry.get().index->Cast<ART>();
			lock_guard<mutex> guard(index_entry.get().lock);
			IndexAppendInfo append_info;
			art.VerifyAppend(rows, append_info, nullptr);
		}
		return;
	} catch (ConstraintException &) {
		// a collision: it only counts if the owning base row is still live
	}

	vector<row_t> replaced_row_ids;
	vector<vector<Value>> replaced_rows;
	CollectReplaced(context, delta_table, pending_replaced, replaced_row_ids, replaced_rows);

	auto column_count = entry->GetColumns().LogicalColumnCount();
	vector<LogicalType> table_types;
	for (idx_t col = 0; col < column_count; col++) {
		table_types.push_back(entry->GetColumn(LogicalIndex(col)).Type());
	}
	for (auto &index_entry : secondary_indexes) {
		auto &art = index_entry.get().index->Cast<ART>();
		auto mask = art.CreateDeltaIndex(DeltaIndexType::LOCAL_DELETE);
		idx_t offset = 0;
		while (offset < replaced_rows.size()) {
			idx_t batch = MinValue<idx_t>(replaced_rows.size() - offset, STANDARD_VECTOR_SIZE);
			DataChunk mask_rows;
			mask_rows.Initialize(Allocator::Get(context), table_types);
			Vector mask_row_ids(LogicalType::ROW_TYPE, batch);
			for (idx_t i = 0; i < batch; i++) {
				for (idx_t col = 0; col < column_count; col++) {
					mask_rows.SetValue(col, i, replaced_rows[offset + i][col]);
				}
				mask_row_ids.SetValue(i, Value::BIGINT(replaced_row_ids[offset + i]));
			}
			mask_rows.SetCardinality(batch);
			IndexAppendInfo mask_info(IndexAppendMode::IGNORE_DUPLICATES, nullptr);
			auto error = mask->Append(mask_rows, mask_row_ids, mask_info);
			if (error.HasError()) {
				throw InternalException("anofox_scenario: unexpected error building the unique mask: %s",
				                        error.Message());
			}
			offset += batch;
		}
		lock_guard<mutex> guard(index_entry.get().lock);
		IndexAppendInfo append_info(IndexAppendMode::DEFAULT, mask.get());
		art.VerifyAppend(rows, append_info, nullptr);
	}
}

} // namespace duckdb
