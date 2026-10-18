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
	scenario_compat::ForEachIndexEntry(base.GetStorage().GetDataTableInfo()->GetIndexes(),
	                                   [&](const scenario_compat::IndexEntryRef &index_entry) {
		                                   if (!scenario_compat::IsBoundUniqueART(index_entry)) {
			                                   return;
		                                   }
		                                   auto column_set = scenario_compat::IndexColumns(index_entry);
		                                   bool is_key_index = !key_set.empty() && column_set.size() == key_set.size();
		                                   for (auto col : column_set) {
			                                   if (key_set.find(col) == key_set.end()) {
				                                   is_key_index = false;
				                                   break;
			                                   }
		                                   }
		                                   if (is_key_index) {
			                                   key_index = index_entry;
		                                   } else {
			                                   secondary_indexes.push_back(index_entry);
		                                   }
	                                   });
}

void ScenarioBaseUniqueProbe::ProbePrimaryKey(DataChunk &rows) {
	if (!key_index || rows.size() == 0) {
		return;
	}
	scenario_compat::VerifyAppend(key_index, rows);
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
			key_chunk.SetChildCardinality(1);
			replaced_keys.emplace(ScenarioDelta::MakeKey(key_chunk, 0, positions), std::move(key_values));
		}
	}
	if (replaced_keys.empty()) {
		return;
	}

	// 2. their base rowids: a point lookup in the base PK index, or (declared
	//    key_columns over a PK-less base) one base scan
	if (key_index) {
		scenario_compat::WithART(key_index, false, [&](auto &art) {
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
				key_rows.SetChildCardinality(count);
				expr_chunk.Reset();
				art.ExecuteExpressions(key_rows, expr_chunk);
				ArenaAllocator arena(Allocator::Get(context));
				unsafe_vector<ARTKey> keys(count);
				art.template GenerateKeys<>(arena, expr_chunk, keys);
				for (idx_t i = 0; i < count; i++) {
					row_t row_id;
					if (scenario_compat::LookupInlinedRowId(art, keys[i], row_id)) {
						row_ids.push_back(row_id);
					}
				}
			}
		});
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
		dc::SetVectorSize(fetch_rowids, batch);
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
			scenario_compat::VerifyAppend(index_entry, rows);
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
		auto mask = scenario_compat::MakeUniqueMask(index_entry);
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
			mask_rows.SetChildCardinality(batch);
			dc::SetVectorSize(mask_row_ids, batch);
			scenario_compat::MaskAppend(mask, mask_rows, mask_row_ids);
			offset += batch;
		}
		scenario_compat::MaskVerifyAppend(mask, rows);
	}
}

void ScenarioBaseUniqueProbe::ProbeKeyless(ClientContext &context, DuckTableEntry &delta_table, DataChunk &rows,
                                           const vector<vector<Value>> &pending_deleted) {
	if (secondary_indexes.empty() || rows.size() == 0) {
		return;
	}
	auto count_column = ScenarioDelta::CountColumnIndex(delta_table);
	if (!count_column.IsValid()) {
		return; // pre-v0.4.1 delta: keyless writes are gated elsewhere
	}
	auto column_count = entry->GetColumns().LogicalColumnCount();
	vector<LogicalType> table_types;
	for (idx_t col = 0; col < column_count; col++) {
		table_types.push_back(entry->GetColumn(LogicalIndex(col)).Type());
	}
	// NULL-free tuple of one unique index in a row ("" = has a NULL: never conflicts)
	auto tuple_of = [](const vector<idx_t> &columns, const std::function<Value(idx_t)> &get) {
		string key;
		for (auto col : columns) {
			auto value = get(col);
			if (value.IsNull()) {
				return string();
			}
			auto str = value.ToString();
			key += to_string(str.size()) + ":" + str;
		}
		return key;
	};

	for (auto &index_entry : secondary_indexes) {
		auto id_set = scenario_compat::IndexColumns(index_entry);
		vector<idx_t> columns(id_set.begin(), id_set.end());
		std::sort(columns.begin(), columns.end());

		// candidate tuples of the new rows; rows with a NULL are exempt
		vector<string> row_tuples(rows.size());
		unordered_map<string, int64_t> net;
		for (idx_t i = 0; i < rows.size(); i++) {
			row_tuples[i] = tuple_of(columns, [&](idx_t col) { return rows.GetValue(col, i); });
			if (!row_tuples[i].empty()) {
				net.emplace(row_tuples[i], 0);
			}
		}
		if (net.empty()) {
			continue;
		}

		// scenario side: sum(I) - sum(D) per tuple, flushed rows + this
		// statement's pending deletes
		vector<StorageIndex> delta_columns;
		vector<LogicalType> delta_types;
		delta_columns.emplace_back(ScenarioDelta::OP_COL);
		delta_types.push_back(LogicalType::VARCHAR);
		delta_columns.emplace_back(count_column.GetIndex());
		delta_types.push_back(LogicalType::BIGINT);
		for (auto col : columns) {
			delta_columns.emplace_back(ScenarioDelta::PAYLOAD_START + col);
			delta_types.push_back(table_types[col]);
		}
		ScenarioDelta::ScanTableRows(context, delta_table, std::move(delta_columns), delta_types,
		                             [&](DataChunk &chunk, idx_t row) {
			                             idx_t pos = 2;
			                             string key;
			                             bool has_null = false;
			                             for (idx_t c = 0; c < columns.size(); c++, pos++) {
				                             auto value = chunk.GetValue(pos, row);
				                             if (value.IsNull()) {
					                             has_null = true;
					                             break;
				                             }
				                             auto str = value.ToString();
				                             key += to_string(str.size()) + ":" + str;
			                             }
			                             if (has_null) {
				                             return true;
			                             }
			                             auto hit = net.find(key);
			                             if (hit != net.end()) {
				                             auto count = chunk.GetValue(1, row).GetValue<int64_t>();
				                             auto op = chunk.GetValue(0, row).GetValue<string>();
				                             hit->second += op == "I" ? count : -count;
			                             }
			                             return true;
		                             });
		for (auto &deleted : pending_deleted) {
			auto hit = net.find(tuple_of(columns, [&](idx_t col) { return deleted[col]; }));
			if (hit != net.end()) {
				hit->second--;
			}
		}

		// base side: a unique index holds each tuple at most once. Fast path:
		// none of the rows collides with a base row.
		vector<bool> in_base(rows.size(), false);
		try {
			scenario_compat::VerifyAppend(index_entry, rows);
		} catch (ConstraintException &) {
			DataChunk single;
			single.Initialize(Allocator::Get(context), table_types);
			for (idx_t i = 0; i < rows.size(); i++) {
				if (row_tuples[i].empty()) {
					continue;
				}
				single.Reset();
				for (idx_t col = 0; col < column_count; col++) {
					single.SetValue(col, 0, rows.GetValue(col, i));
				}
				single.SetChildCardinality(1);
				try {
					scenario_compat::VerifyAppend(index_entry, single);
				} catch (ConstraintException &) {
					in_base[i] = true;
				}
			}
		}

		// each new row needs its tuple to be absent from the merged relation,
		// counting the rows of this batch added before it
		for (idx_t i = 0; i < rows.size(); i++) {
			if (row_tuples[i].empty()) {
				continue;
			}
			auto &held = net[row_tuples[i]];
			if ((in_base[i] ? 1 : 0) + held > 0) {
				throw ConstraintException(
				    "Duplicate value violates a UNIQUE constraint on keyless scenario table \"%s\": the value is "
				    "already held by a row of the scenario",
				    dc::Str(entry->name));
			}
			held++;
		}
	}
}

} // namespace duckdb
