//===----------------------------------------------------------------------===//
// Phase 6: merge-back. scenario_merge_preview streams the planned actions
// with conflict flags (bind-replaced SQL over the delta changelog);
// CALL scenario_merge applies the delta to the base in the caller's
// transaction and marks the scenario merged (frozen + merged_at).
//
// Overlay-tier conflict model (honest for live bases): an 'I' row whose key
// now exists in the base, or a 'U' row whose key vanished, is a conflict.
// 'D' rows whose key vanished are already satisfied and are skipped.
// on_conflict := 'abort' (default) | 'ours' (scenario wins) | 'theirs'
// (base wins - conflicting delta rows are dropped).
//===----------------------------------------------------------------------===//

#include "lifecycle/scenario_lifecycle.hpp"

#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace {

string QM(const string &identifier) {
	return KeywordHelper::WriteOptionallyQuoted(identifier);
}

Catalog &MergeHostCatalog(ClientContext &context) {
	return Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
}

//! Enumerate (delta table name, logical table name) pairs of a scenario
vector<pair<string, string>> ListDeltaTables(ClientContext &context, Catalog &host, int64_t scenario_id) {
	vector<pair<string, string>> result;
	string prefix = "s" + to_string(scenario_id) + "_delta_";
	auto schema = host.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return result;
	}
	schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		if (entry.type == CatalogType::TABLE_ENTRY && StringUtil::StartsWith(entry.name, prefix)) {
			result.emplace_back(entry.name, entry.name.substr(prefix.size()));
		}
	});
	return result;
}

//===----------------------------------------------------------------------===//
// scenario_merge_preview(name): bind-replaced streaming SQL
//===----------------------------------------------------------------------===//

unique_ptr<TableRef> MergePreviewBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto scenario_name = input.inputs[0].GetValue<string>();
	auto &host = MergeHostCatalog(context);
	auto entry = ScenarioRegistry::Lookup(context, host, scenario_name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", scenario_name);
	}

	string sql;
	auto host_prefix = QM(host.GetName()) + ".";
	auto internal_prefix = host_prefix + QM(ScenarioRegistry::SCHEMA_NAME) + ".";
	for (auto &delta_pair : ListDeltaTables(context, host, entry->scenario_id)) {
		auto &logical_name = delta_pair.second;
		auto base_entry =
		    host.GetEntry<TableCatalogEntry>(context, DEFAULT_SCHEMA, logical_name, OnEntryNotFound::RETURN_NULL);
		if (!base_entry) {
			continue; // base table vanished; scenario_merge reports the error
		}
		auto pk_columns = ScenarioDelta::GetPKColumns(*base_entry);
		string key_expr, pk_match;
		if (pk_columns.empty()) {
			key_expr = "CAST('(row)' AS VARCHAR)";
		} else {
			for (auto pk_col : pk_columns) {
				auto &name = base_entry->GetColumn(LogicalIndex(pk_col)).Name();
				if (!key_expr.empty()) {
					key_expr += " || '|' || ";
					pk_match += " AND ";
				}
				key_expr += "CAST(d." + QM(name) + " AS VARCHAR)";
				pk_match += "b." + QM(name) + " = d." + QM(name);
			}
		}
		auto base_name = host_prefix + QM(string(DEFAULT_SCHEMA)) + "." + QM(logical_name);
		string exists_clause = pk_columns.empty()
		                           ? string("false")
		                           : "EXISTS (SELECT 1 FROM " + base_name + " b WHERE " + pk_match + ")";
		if (!sql.empty()) {
			sql += " UNION ALL ";
		}
		sql += "SELECT CAST('" + logical_name + "' AS VARCHAR) AS table_name, " + key_expr +
		       " AS key, CASE d._op WHEN 'I' THEN 'insert' WHEN 'U' THEN 'update' ELSE 'delete' END AS action, "
		       "CASE WHEN d._op = 'I' THEN " +
		       exists_clause + " WHEN d._op = 'U' THEN NOT " + exists_clause +
		       " ELSE false END AS conflict FROM " + internal_prefix + QM(delta_pair.first) + " d";
	}
	if (sql.empty()) {
		sql = "SELECT CAST(NULL AS VARCHAR) AS table_name, CAST(NULL AS VARCHAR) AS key, CAST(NULL AS VARCHAR) AS "
		      "action, CAST(false AS BOOLEAN) AS conflict WHERE 1 = 0";
	}
	Parser parser;
	parser.ParseQuery(sql);
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

//===----------------------------------------------------------------------===//
// CALL scenario_merge(name [, on_conflict])
//===----------------------------------------------------------------------===//

enum class MergeConflictPolicy { ABORT, OURS, THEIRS };

struct MergeBindData : public TableFunctionData {
	string name;
	MergeConflictPolicy policy = MergeConflictPolicy::ABORT;
};

struct MergeState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> MergeBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MergeBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("Scenario name cannot be NULL");
	}
	result->name = input.inputs[0].GetValue<string>();
	for (auto &param : input.named_parameters) {
		if (param.first == "on_conflict" && !param.second.IsNull()) {
			auto policy = StringUtil::Lower(param.second.GetValue<string>());
			if (policy == "abort") {
				result->policy = MergeConflictPolicy::ABORT;
			} else if (policy == "ours") {
				result->policy = MergeConflictPolicy::OURS;
			} else if (policy == "theirs") {
				result->policy = MergeConflictPolicy::THEIRS;
			} else {
				throw InvalidInputException(
				    "Invalid on_conflict policy '%s': expected 'abort', 'ours' or 'theirs'", policy);
			}
		}
	}
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
	names = {"tables_changed", "rows_applied", "conflicts_resolved"};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> MergeInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<MergeState>();
}

//! One decoded delta row
struct MergeRow {
	char op;
	row_t delta_row_id;
	vector<Value> payload; // base-table layout
};

void MergeExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<MergeState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	auto &bind_data = data.bind_data->Cast<MergeBindData>();
	auto &host = MergeHostCatalog(context);

	auto entry = ScenarioRegistry::Lookup(context, host, bind_data.name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.name);
	}
	if (entry->has_merged_at) {
		throw InvalidInputException("Scenario '%s' was already merged back into the base", bind_data.name);
	}
	if (entry->mode == "materialized") {
		throw NotImplementedException(
		    "Merging a materialized scenario back is not supported (v1): materialized scenarios are full "
		    "copies, not changelogs");
	}
	if (entry->frozen) {
		throw InvalidInputException("Scenario '%s' is frozen. Unfreeze it before merging", bind_data.name);
	}
	if (ScenarioRegistry::HasChildren(context, host, entry->scenario_id)) {
		throw InvalidInputException("Cannot merge scenario '%s': other scenarios branch from it", bind_data.name);
	}
	MetaTransaction::Get(context).ModifyDatabase(
	    host.GetAttached(), DatabaseModificationType::INSERT_DATA | DatabaseModificationType::UPDATE_DATA |
	                            DatabaseModificationType::DELETE_DATA);

	idx_t tables_changed = 0;
	idx_t rows_applied = 0;
	idx_t conflicts_resolved = 0;

	for (auto &delta_pair : ListDeltaTables(context, host, entry->scenario_id)) {
		auto &logical_name = delta_pair.second;
		auto delta_entry =
		    ScenarioDelta::TryGetDeltaTable(context, host, entry->scenario_id, logical_name);
		if (!delta_entry) {
			continue;
		}
		auto &delta_table = *delta_entry;

		// load the delta rows
		auto delta_types = delta_table.GetStorage().GetTypes();
		idx_t payload_count = delta_types.size() - ScenarioDelta::PAYLOAD_START;
		vector<StorageIndex> column_ids;
		vector<LogicalType> scan_types;
		column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
		scan_types.push_back(LogicalType::ROW_TYPE);
		for (idx_t i = 0; i < delta_types.size(); i++) {
			column_ids.emplace_back(i);
			scan_types.push_back(delta_types[i]);
		}
		vector<MergeRow> rows;
		ScenarioDelta::ScanTableRows(context, delta_table, std::move(column_ids), scan_types,
		                             [&](DataChunk &chunk, idx_t row) {
			                             MergeRow merge_row;
			                             merge_row.delta_row_id = chunk.GetValue(0, row).GetValue<row_t>();
			                             merge_row.op = chunk.GetValue(1, row).GetValue<string>()[0];
			                             for (idx_t i = 0; i < payload_count; i++) {
				                             merge_row.payload.push_back(
				                                 chunk.GetValue(1 + ScenarioDelta::PAYLOAD_START + i, row));
			                             }
			                             rows.push_back(std::move(merge_row));
			                             return true;
		                             });
		if (rows.empty()) {
			continue;
		}

		auto base_entry =
		    host.GetEntry<TableCatalogEntry>(context, DEFAULT_SCHEMA, logical_name, OnEntryNotFound::RETURN_NULL);
		if (!base_entry) {
			throw InvalidInputException(
			    "Cannot merge scenario '%s': base table '%s' no longer exists", bind_data.name, logical_name);
		}
		auto &base_duck = base_entry->Cast<DuckTableEntry>();
		auto pk_columns = ScenarioDelta::GetPKColumns(*base_entry);
		if (pk_columns.empty() ) {
			// no-PK tables carry only 'I' rows (v1 write limits): plain appends
			for (auto &row : rows) {
				if (row.op != 'I') {
					throw InternalException("anofox_scenario: unexpected op '%c' in no-PK delta", row.op);
				}
			}
		}

		// resolve base rowids for the delta's keys
		unordered_map<string, row_t> base_row_ids;
		if (!pk_columns.empty()) {
			unordered_set<string> wanted;
			vector<idx_t> payload_key_positions; // pk positions within payload
			for (auto pk_col : pk_columns) {
				payload_key_positions.push_back(pk_col);
			}
			auto key_of = [&](const MergeRow &row) {
				string key;
				for (auto pos : payload_key_positions) {
					auto &value = row.payload[pos];
					if (value.IsNull()) {
						key += "N|";
					} else {
						auto str = value.ToString();
						key += to_string(str.size()) + ":" + str;
					}
				}
				return key;
			};
			for (auto &row : rows) {
				wanted.insert(key_of(row));
			}
			vector<StorageIndex> base_cols;
			vector<LogicalType> base_scan_types;
			base_cols.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
			base_scan_types.push_back(LogicalType::ROW_TYPE);
			vector<idx_t> key_positions;
			for (auto pk_col : pk_columns) {
				key_positions.push_back(base_cols.size());
				base_cols.emplace_back(pk_col);
				base_scan_types.push_back(base_entry->GetColumn(LogicalIndex(pk_col)).Type());
			}
			ScenarioDelta::ScanTableRows(context, base_duck, std::move(base_cols), base_scan_types,
			                             [&](DataChunk &chunk, idx_t row) {
				                             auto key = ScenarioDelta::MakeKey(chunk, row, key_positions);
				                             if (wanted.find(key) != wanted.end()) {
					                             base_row_ids[key] = chunk.GetValue(0, row).GetValue<row_t>();
				                             }
				                             return true;
			                             });

			// classify, resolve conflicts, and apply
			vector<row_t> base_rows_to_delete;      // rows replaced or deleted
			vector<row_t> replaced_rows;            // subset re-inserted (delete-index registration)
			vector<idx_t> append_rows;              // indexes into `rows` to append
			idx_t table_conflicts = 0;
			for (idx_t row_idx = 0; row_idx < rows.size(); row_idx++) {
				auto &row = rows[row_idx];
				auto key = key_of(row);
				auto base_hit = base_row_ids.find(key);
				bool in_base = base_hit != base_row_ids.end();
				bool conflict = (row.op == 'I' && in_base) || (row.op == 'U' && !in_base);
				if (conflict) {
					table_conflicts++;
					if (bind_data.policy == MergeConflictPolicy::ABORT) {
						throw ConstraintException(
						    "Cannot merge scenario '%s': the base changed underneath it (e.g. key %s of table "
						    "'%s'). Rerun with on_conflict := 'ours' (scenario wins) or 'theirs' (base wins)",
						    bind_data.name, key, logical_name);
					}
					if (bind_data.policy == MergeConflictPolicy::THEIRS) {
						continue; // base wins: drop the delta row
					}
					// OURS: apply anyway - I over an existing row replaces it;
					// U of a vanished row re-inserts it
				}
				switch (row.op) {
				case 'I':
					if (in_base) {
						base_rows_to_delete.push_back(base_hit->second);
						replaced_rows.push_back(base_hit->second);
					}
					append_rows.push_back(row_idx);
					rows_applied++;
					break;
				case 'U':
					if (in_base) {
						base_rows_to_delete.push_back(base_hit->second);
						replaced_rows.push_back(base_hit->second);
					}
					append_rows.push_back(row_idx);
					rows_applied++;
					break;
				case 'D':
					if (in_base) {
						base_rows_to_delete.push_back(base_hit->second);
						rows_applied++;
					}
					break;
				default:
					throw InternalException("anofox_scenario: unknown delta op");
				}
			}
			conflicts_resolved += table_conflicts;

			auto binder = Binder::CreateBinder(context);
			auto base_constraints = binder->BindConstraints(base_duck);
			auto &base_storage = base_duck.GetStorage();
			auto &transaction = DuckTransaction::Get(context, base_duck.catalog);

			// fetch the replaced rows first (delete-index registration needs
			// their old values)
			DataChunk replaced_chunk;
			if (!replaced_rows.empty()) {
				vector<StorageIndex> all_cols;
				for (idx_t i = 0; i < base_entry->GetColumns().LogicalColumnCount(); i++) {
					all_cols.emplace_back(i);
				}
				replaced_chunk.Initialize(Allocator::Get(context), base_storage.GetTypes());
				Vector fetch_row_ids(LogicalType::ROW_TYPE, replaced_rows.size());
				for (idx_t i = 0; i < replaced_rows.size(); i++) {
					fetch_row_ids.SetValue(i, Value::BIGINT(replaced_rows[i]));
				}
				ColumnFetchState fetch_state;
				base_storage.Fetch(transaction, replaced_chunk, all_cols, fetch_row_ids, replaced_rows.size(),
				                   fetch_state);
			}

			// deletes
			if (!base_rows_to_delete.empty()) {
				auto delete_state = base_storage.InitializeDelete(base_duck, context, base_constraints);
				idx_t offset = 0;
				while (offset < base_rows_to_delete.size()) {
					idx_t batch = MinValue<idx_t>(base_rows_to_delete.size() - offset, STANDARD_VECTOR_SIZE);
					Vector row_ids(LogicalType::ROW_TYPE);
					for (idx_t i = 0; i < batch; i++) {
						row_ids.SetValue(i, Value::BIGINT(base_rows_to_delete[offset + i]));
					}
					base_storage.Delete(*delete_state, context, row_ids, batch);
					offset += batch;
				}
			}

			// appends (with replaced-row registration in the delete indexes)
			if (!append_rows.empty()) {
				DataChunk append_chunk;
				append_chunk.Initialize(Allocator::Get(context), base_storage.GetTypes());
				idx_t count = 0;
				bool registered = false;
				auto flush = [&]() {
					if (count == 0) {
						return;
					}
					append_chunk.SetCardinality(count);
					ScenarioDeltaAppendChunk(context, base_duck, base_constraints, append_chunk,
					                         registered ? vector<row_t>() : replaced_rows,
					                         replaced_rows.empty() ? nullptr : &replaced_chunk);
					registered = true;
					append_chunk.Reset();
					count = 0;
				};
				for (auto row_idx : append_rows) {
					auto &row = rows[row_idx];
					for (idx_t col = 0; col < row.payload.size(); col++) {
						append_chunk.SetValue(col, count, row.payload[col]);
					}
					if (++count == STANDARD_VECTOR_SIZE) {
						flush();
					}
				}
				flush();
			}
			if (!append_rows.empty() || !base_rows_to_delete.empty()) {
				tables_changed++;
			}
		} else {
			// no-PK: plain appends
			auto binder = Binder::CreateBinder(context);
			auto base_constraints = binder->BindConstraints(base_duck);
			DataChunk append_chunk;
			append_chunk.Initialize(Allocator::Get(context), base_duck.GetStorage().GetTypes());
			idx_t count = 0;
			for (auto &row : rows) {
				for (idx_t col = 0; col < row.payload.size(); col++) {
					append_chunk.SetValue(col, count, row.payload[col]);
				}
				if (++count == STANDARD_VECTOR_SIZE) {
					append_chunk.SetCardinality(count);
					base_duck.GetStorage().LocalAppend(base_duck, context, append_chunk, base_constraints);
					append_chunk.Reset();
					count = 0;
				}
				rows_applied++;
			}
			if (count > 0) {
				append_chunk.SetCardinality(count);
				base_duck.GetStorage().LocalAppend(base_duck, context, append_chunk, base_constraints);
			}
			tables_changed++;
		}

		// empty the delta (the scenario has been absorbed)
		vector<row_t> delta_row_ids;
		for (auto &row : rows) {
			delta_row_ids.push_back(row.delta_row_id);
		}
		auto binder = Binder::CreateBinder(context);
		auto delta_constraints = binder->BindConstraints(delta_table);
		auto delete_state = delta_table.GetStorage().InitializeDelete(delta_table, context, delta_constraints);
		idx_t offset = 0;
		while (offset < delta_row_ids.size()) {
			idx_t batch = MinValue<idx_t>(delta_row_ids.size() - offset, STANDARD_VECTOR_SIZE);
			Vector row_ids(LogicalType::ROW_TYPE);
			for (idx_t i = 0; i < batch; i++) {
				row_ids.SetValue(i, Value::BIGINT(delta_row_ids[offset + i]));
			}
			delta_table.GetStorage().Delete(*delete_state, context, row_ids, batch);
			offset += batch;
		}
	}

	ScenarioRegistry::MarkMerged(context, host, entry->scenario_id);

	output.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(tables_changed)));
	output.SetValue(1, 0, Value::BIGINT(NumericCast<int64_t>(rows_applied)));
	output.SetValue(2, 0, Value::BIGINT(NumericCast<int64_t>(conflicts_resolved)));
	output.SetCardinality(1);
}

} // namespace

void ScenarioMergeBack::RegisterFunctions(ExtensionLoader &loader) {
	TableFunction preview("scenario_merge_preview", {LogicalType::VARCHAR}, nullptr);
	preview.bind_replace = MergePreviewBindReplace;
	loader.RegisterFunction(preview);

	TableFunction merge("scenario_merge", {LogicalType::VARCHAR}, MergeExecute, MergeBind, MergeInit);
	merge.named_parameters["on_conflict"] = LogicalType::VARCHAR;
	loader.RegisterFunction(merge);
}

} // namespace duckdb
