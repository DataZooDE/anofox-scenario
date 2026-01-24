#include "include/comparison_engine.hpp"
#include "include/scenario_manager.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// scenario_compare Table Function
//===--------------------------------------------------------------------===//

struct ScenarioCompareBindData : public TableFunctionData {
	string scenario_name;
	string table_name;
	string scenario_schema;
	vector<string> pk_columns;
	vector<string> all_columns;
	vector<LogicalType> column_types;
	bool executed = false;
	vector<vector<Value>> results;
	idx_t result_idx = 0;
};

static void ValidateScenarioCompareInputs(ClientContext &context, const string &scenario_name, const string &table_name,
                                          string &scenario_schema, vector<string> &pk_columns,
                                          vector<string> &all_columns, vector<LogicalType> &column_types) {
	// Check scenario exists
	Connection con(context.db->GetDatabase(context));

	auto result = con.Query(StringUtil::Format(
	    "SELECT schema_name FROM _scenario_registry WHERE scenario_name = '%s'",
	    ScenarioManager::EscapeSQLString(scenario_name)));

	if (result->HasError() || result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}
	scenario_schema = result->GetValue(0, 0).ToString();

	// Check base table exists
	result = con.Query(StringUtil::Format(
	    "SELECT column_name, data_type FROM information_schema.columns "
	    "WHERE table_schema = 'main' AND table_name = '%s' ORDER BY ordinal_position",
	    ScenarioManager::EscapeSQLString(table_name)));

	if (result->HasError() || result->RowCount() == 0) {
		throw InvalidInputException("Base table '%s' does not exist in main schema", table_name);
	}

	// Collect all columns and types
	for (idx_t i = 0; i < result->RowCount(); i++) {
		all_columns.push_back(result->GetValue(0, i).ToString());
		// Store column types for later
		string type_str = result->GetValue(1, i).ToString();
		// We'll cast everything to VARCHAR for comparison output
		column_types.push_back(LogicalType::VARCHAR);
	}

	// Check delta table exists
	result = con.Query(StringUtil::Format(
	    "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '_delta_%s'",
	    ScenarioManager::EscapeSQLString(scenario_schema), ScenarioManager::EscapeSQLString(table_name)));

	if (result->HasError() || result->GetValue(0, 0).GetValue<int64_t>() == 0) {
		throw InvalidInputException("Delta table for '%s' does not exist in scenario '%s'", table_name, scenario_name);
	}

	// Get primary key columns
	result = con.Query(StringUtil::Format(
	    "SELECT primary_key_columns FROM _scenario_tables WHERE "
	    "scenario_id = (SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '%s') "
	    "AND table_name = '%s'",
	    ScenarioManager::EscapeSQLString(scenario_name), ScenarioManager::EscapeSQLString(table_name)));

	if (!result->HasError() && result->RowCount() > 0 && !result->GetValue(0, 0).IsNull()) {
		auto pk_list = ListValue::GetChildren(result->GetValue(0, 0));
		for (auto &pk : pk_list) {
			pk_columns.push_back(pk.ToString());
		}
	}

	// If no PK, use all columns as composite key
	if (pk_columns.empty()) {
		pk_columns = all_columns;
	}
}

static unique_ptr<FunctionData> ScenarioCompareBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ScenarioCompareBindData>();

	if (input.inputs.size() < 2) {
		throw InvalidInputException("scenario_compare requires scenario_name and table_name");
	}

	result->scenario_name = input.inputs[0].GetValue<string>();
	result->table_name = input.inputs[1].GetValue<string>();

	// Validate inputs and get metadata
	ValidateScenarioCompareInputs(context, result->scenario_name, result->table_name, result->scenario_schema,
	                              result->pk_columns, result->all_columns, result->column_types);

	// Build output schema: diff_type, pk_columns..., column_name, old_value, new_value
	names.push_back("diff_type");
	return_types.push_back(LogicalType::VARCHAR);

	for (const auto &pk : result->pk_columns) {
		names.push_back(pk);
		// Look up the actual type for this column
		Connection con(context.db->GetDatabase(context));
		auto type_result = con.Query(StringUtil::Format(
		    "SELECT data_type FROM information_schema.columns WHERE table_schema = 'main' AND table_name = '%s' AND "
		    "column_name = '%s'",
		    ScenarioManager::EscapeSQLString(result->table_name), ScenarioManager::EscapeSQLString(pk)));
		if (!type_result->HasError() && type_result->RowCount() > 0) {
			string type_str = type_result->GetValue(0, 0).ToString();
			if (type_str == "INTEGER" || type_str == "BIGINT" || type_str == "SMALLINT" || type_str == "TINYINT") {
				return_types.push_back(LogicalType::BIGINT);
			} else {
				return_types.push_back(LogicalType::VARCHAR);
			}
		} else {
			return_types.push_back(LogicalType::VARCHAR);
		}
	}

	names.push_back("column_name");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("old_value");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("new_value");
	return_types.push_back(LogicalType::VARCHAR);

	return std::move(result);
}

struct ScenarioCompareState : public GlobalTableFunctionState {
	idx_t current_row = 0;
};

static unique_ptr<GlobalTableFunctionState> ScenarioCompareInit(ClientContext &context,
                                                                TableFunctionInitInput &input) {
	return make_uniq<ScenarioCompareState>();
}

static void ExecuteComparison(ClientContext &context, ScenarioCompareBindData &bind_data) {
	if (bind_data.executed) {
		return;
	}
	bind_data.executed = true;

	Connection con(context.db->GetDatabase(context));

	// Build PK condition for joins
	string pk_list;
	string pk_join_cond;
	for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
		if (i > 0) {
			pk_list += ", ";
			pk_join_cond += " AND ";
		}
		string pk_quoted = ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]);
		pk_list += pk_quoted;
		pk_join_cond += "base." + pk_quoted + " = scen." + pk_quoted;
	}

	// Build non-PK columns list for change detection
	vector<string> non_pk_columns;
	for (const auto &col : bind_data.all_columns) {
		bool is_pk = false;
		for (const auto &pk : bind_data.pk_columns) {
			if (col == pk) {
				is_pk = true;
				break;
			}
		}
		if (!is_pk) {
			non_pk_columns.push_back(col);
		}
	}

	string base_table = "main." + ScenarioManager::QuoteIdentifier(bind_data.table_name);
	string scen_view = ScenarioManager::QuoteIdentifier(bind_data.scenario_schema) + "." +
	                   ScenarioManager::QuoteIdentifier(bind_data.table_name);

	// Query 1: Find ADDED rows (in scenario but not in base)
	string added_query = StringUtil::Format(
	    "SELECT %s FROM %s scen WHERE NOT EXISTS (SELECT 1 FROM %s base WHERE %s)", pk_list, scen_view, base_table,
	    pk_join_cond);

	auto added_result = con.Query(added_query);
	if (!added_result->HasError()) {
		for (idx_t row = 0; row < added_result->RowCount(); row++) {
			vector<Value> result_row;
			result_row.push_back(Value("added"));
			for (idx_t pk_idx = 0; pk_idx < bind_data.pk_columns.size(); pk_idx++) {
				result_row.push_back(added_result->GetValue(pk_idx, row));
			}
			result_row.push_back(Value()); // column_name = NULL
			result_row.push_back(Value()); // old_value = NULL
			result_row.push_back(Value()); // new_value = NULL
			bind_data.results.push_back(std::move(result_row));
		}
	}

	// Query 2: Find REMOVED rows (in base but not in scenario - marked as 'D' in delta)
	string delta_table_name = "_delta_" + bind_data.table_name;
	string delta_table = ScenarioManager::QuoteIdentifier(bind_data.scenario_schema) + "." +
	                     ScenarioManager::QuoteIdentifier(delta_table_name);

	// A row is removed if it exists in the delta with _op = 'D'
	string removed_pk_cond;
	for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
		if (i > 0) {
			removed_pk_cond += " AND ";
		}
		string pk_quoted = ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]);
		removed_pk_cond += "base." + pk_quoted + " = delta." + pk_quoted;
	}

	// Build SELECT list for PK columns, all qualified with the base alias
	string removed_pk_select;
	for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
		if (i > 0) {
			removed_pk_select += ", ";
		}
		string pk_quoted = ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]);
		removed_pk_select += "base." + pk_quoted;
	}

	string removed_query = StringUtil::Format("SELECT %s FROM %s base "
	                                          "INNER JOIN %s delta ON %s "
	                                          "WHERE delta._op = 'D'",
	                                          removed_pk_select, base_table, delta_table, removed_pk_cond);

	auto removed_result = con.Query(removed_query);
	if (!removed_result->HasError()) {
		for (idx_t row = 0; row < removed_result->RowCount(); row++) {
			vector<Value> result_row;
			result_row.push_back(Value("removed"));
			for (idx_t pk_idx = 0; pk_idx < bind_data.pk_columns.size(); pk_idx++) {
				result_row.push_back(removed_result->GetValue(pk_idx, row));
			}
			result_row.push_back(Value()); // column_name = NULL
			result_row.push_back(Value()); // old_value = NULL
			result_row.push_back(Value()); // new_value = NULL
			bind_data.results.push_back(std::move(result_row));
		}
	}

	// Query 3: Find CHANGED rows (same PK, different values) - only for non-PK columns
	if (!non_pk_columns.empty()) {
		// Get rows that exist in both base and scenario (via update)
		string update_pk_cond;
		for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
			if (i > 0) {
				update_pk_cond += " AND ";
			}
			string pk_quoted = ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]);
			update_pk_cond += "base." + pk_quoted + " = delta." + pk_quoted;
		}

		// Build select list for all columns
		string all_cols_select;
		for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
			if (i > 0) {
				all_cols_select += ", ";
			}
			string pk_quoted = ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]);
			all_cols_select += "base." + pk_quoted + " AS pk_" + std::to_string(i);
		}

		for (const auto &col : non_pk_columns) {
			string col_quoted = ScenarioManager::QuoteIdentifier(col);
			all_cols_select += ", base." + col_quoted + " AS base_" + col;
			all_cols_select += ", delta." + col_quoted + " AS delta_" + col;
		}

		string changed_query =
		    StringUtil::Format("SELECT %s FROM %s base "
		                       "INNER JOIN %s delta ON %s "
		                       "WHERE delta._op = 'U'",
		                       all_cols_select, base_table, delta_table, update_pk_cond);

		auto changed_result = con.Query(changed_query);
		if (!changed_result->HasError()) {
			for (idx_t row = 0; row < changed_result->RowCount(); row++) {
				// Extract PK values
				vector<Value> pk_values;
				for (idx_t pk_idx = 0; pk_idx < bind_data.pk_columns.size(); pk_idx++) {
					pk_values.push_back(changed_result->GetValue(pk_idx, row));
				}

				// Check each non-PK column for changes
				idx_t col_offset = bind_data.pk_columns.size();
				for (idx_t col_idx = 0; col_idx < non_pk_columns.size(); col_idx++) {
					Value base_val = changed_result->GetValue(col_offset + col_idx * 2, row);
					Value delta_val = changed_result->GetValue(col_offset + col_idx * 2 + 1, row);

					// Compare values (both as strings for simplicity)
					string base_str = base_val.IsNull() ? "" : base_val.ToString();
					string delta_str = delta_val.IsNull() ? "" : delta_val.ToString();

					if (base_str != delta_str) {
						vector<Value> result_row;
						result_row.push_back(Value("changed"));
						for (const auto &pk_val : pk_values) {
							result_row.push_back(pk_val);
						}
						result_row.push_back(Value(non_pk_columns[col_idx]));
						result_row.push_back(base_val.IsNull() ? Value() : Value(base_str));
						result_row.push_back(delta_val.IsNull() ? Value() : Value(delta_str));
						bind_data.results.push_back(std::move(result_row));
					}
				}
			}
		}
	}
}

static void ScenarioCompareFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ScenarioCompareBindData>();
	auto &state = data_p.global_state->Cast<ScenarioCompareState>();

	// Execute comparison on first call
	ExecuteComparison(context, bind_data);

	idx_t count = 0;
	idx_t max_count = STANDARD_VECTOR_SIZE;

	while (state.current_row < bind_data.results.size() && count < max_count) {
		const auto &result_row = bind_data.results[state.current_row];

		for (idx_t col = 0; col < result_row.size(); col++) {
			output.data[col].SetValue(count, result_row[col]);
		}

		state.current_row++;
		count++;
	}

	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Register Functions
//===--------------------------------------------------------------------===//

void ComparisonEngine::RegisterFunctions(ExtensionLoader &loader) {
	// scenario_compare(scenario_name, table_name) -> TABLE
	TableFunction scenario_compare("scenario_compare", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                               ScenarioCompareFunction, ScenarioCompareBind, ScenarioCompareInit);
	loader.RegisterFunction(scenario_compare);
}

} // namespace duckdb
