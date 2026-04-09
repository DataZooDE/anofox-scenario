#include "include/comparison_engine.hpp"
#include "include/scenario_manager.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
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
	string base_schema;  // "main" or parent scenario's schema for branched scenarios
	vector<string> pk_columns;
	vector<string> all_columns;
	vector<LogicalType> column_types;
	bool executed = false;
	vector<vector<Value>> results;
	idx_t result_idx = 0;
};

static void ValidateScenarioCompareInputs(ClientContext &context, const string &scenario_name, const string &table_name,
                                          string &scenario_schema, string &base_schema, vector<string> &pk_columns,
                                          vector<string> &all_columns, vector<LogicalType> &column_types) {
	// Check scenario exists and get parent info
	Connection con(context.db->GetDatabase(context));

	auto result = con.Query(StringUtil::Format(
	    "SELECT schema_name, parent_scenario_id FROM _scenario_registry WHERE scenario_name = '%s'",
	    ScenarioManager::EscapeSQLString(scenario_name)));

	if (result->HasError() || result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}
	scenario_schema = result->GetValue(0, 0).ToString();

	// Check if this is a branched scenario
	Value parent_id_val = result->GetValue(1, 0);
	if (!parent_id_val.IsNull()) {
		// Branched scenario - get parent's schema for comparison
		int64_t parent_id = parent_id_val.GetValue<int64_t>();
		auto parent_result = con.Query(StringUtil::Format(
		    "SELECT schema_name FROM _scenario_registry WHERE scenario_id = %d", parent_id));
		if (!parent_result->HasError() && parent_result->RowCount() > 0) {
			base_schema = parent_result->GetValue(0, 0).ToString();
		} else {
			base_schema = "main";  // Fallback
		}
	} else {
		// Not branched - compare against main
		base_schema = "main";
	}

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
	                              result->base_schema, result->pk_columns, result->all_columns, result->column_types);

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

	// Use the appropriate base for comparison (main or parent scenario's schema for branched)
	string base_table = ScenarioManager::QuoteIdentifier(bind_data.base_schema) + "." +
	                    ScenarioManager::QuoteIdentifier(bind_data.table_name);
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
// scenario_compare_all Table Function
//===--------------------------------------------------------------------===//

struct ScenarioCompareAllBindData : public TableFunctionData {
	string scenario_name;
	string scenario_schema;
	string base_schema;
	vector<string> table_names;
	bool executed = false;
	vector<vector<Value>> results;
};

static unique_ptr<FunctionData> ScenarioCompareAllBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ScenarioCompareAllBindData>();

	if (input.inputs.empty()) {
		throw InvalidInputException("scenario_compare_all requires scenario_name");
	}

	result->scenario_name = input.inputs[0].GetValue<string>();

	// Validate scenario exists and get metadata
	Connection con(context.db->GetDatabase(context));

	auto scenario_result = con.Query(StringUtil::Format(
	    "SELECT schema_name, parent_scenario_id FROM _scenario_registry WHERE scenario_name = '%s'",
	    ScenarioManager::EscapeSQLString(result->scenario_name)));

	if (scenario_result->HasError() || scenario_result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", result->scenario_name);
	}
	result->scenario_schema = scenario_result->GetValue(0, 0).ToString();

	// Determine base schema (main or parent for branched scenarios)
	Value parent_id_val = scenario_result->GetValue(1, 0);
	if (!parent_id_val.IsNull()) {
		int64_t parent_id = parent_id_val.GetValue<int64_t>();
		auto parent_result = con.Query(StringUtil::Format(
		    "SELECT schema_name FROM _scenario_registry WHERE scenario_id = %d", parent_id));
		if (!parent_result->HasError() && parent_result->RowCount() > 0) {
			result->base_schema = parent_result->GetValue(0, 0).ToString();
		} else {
			result->base_schema = "main";
		}
	} else {
		result->base_schema = "main";
	}

	// Get list of tables with delta tables in this scenario
	auto tables_result = con.Query(StringUtil::Format(
	    "SELECT table_name FROM _scenario_tables WHERE "
	    "scenario_id = (SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '%s')",
	    ScenarioManager::EscapeSQLString(result->scenario_name)));

	if (!tables_result->HasError()) {
		for (idx_t i = 0; i < tables_result->RowCount(); i++) {
			result->table_names.push_back(tables_result->GetValue(0, i).ToString());
		}
	}

	// Output schema: table_name, added_rows, removed_rows, changed_rows
	names.push_back("table_name");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("added_rows");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("removed_rows");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("changed_rows");
	return_types.push_back(LogicalType::BIGINT);

	return std::move(result);
}

struct ScenarioCompareAllState : public GlobalTableFunctionState {
	idx_t current_row = 0;
};

static unique_ptr<GlobalTableFunctionState> ScenarioCompareAllInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<ScenarioCompareAllState>();
}

static void ExecuteCompareAll(ClientContext &context, ScenarioCompareAllBindData &bind_data) {
	if (bind_data.executed) {
		return;
	}
	bind_data.executed = true;

	Connection con(context.db->GetDatabase(context));

	for (const auto &table_name : bind_data.table_names) {
		string delta_table_name = "_delta_" + table_name;

		// Check if delta table exists
		auto check_result = con.Query(StringUtil::Format(
		    "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
		    ScenarioManager::EscapeSQLString(bind_data.scenario_schema),
		    ScenarioManager::EscapeSQLString(delta_table_name)));

		if (check_result->HasError() || check_result->GetValue(0, 0).GetValue<int64_t>() == 0) {
			continue;  // No delta table for this table
		}

		string delta_table = ScenarioManager::QuoteIdentifier(bind_data.scenario_schema) + "." +
		                     ScenarioManager::QuoteIdentifier(delta_table_name);

		// Count added rows (_op = 'I')
		auto added_result = con.Query(StringUtil::Format(
		    "SELECT COUNT(*) FROM %s WHERE _op = 'I'", delta_table));
		int64_t added_count = 0;
		if (!added_result->HasError() && added_result->RowCount() > 0) {
			added_count = added_result->GetValue(0, 0).GetValue<int64_t>();
		}

		// Count removed rows (_op = 'D')
		auto removed_result = con.Query(StringUtil::Format(
		    "SELECT COUNT(*) FROM %s WHERE _op = 'D'", delta_table));
		int64_t removed_count = 0;
		if (!removed_result->HasError() && removed_result->RowCount() > 0) {
			removed_count = removed_result->GetValue(0, 0).GetValue<int64_t>();
		}

		// Count changed rows (_op = 'U')
		auto changed_result = con.Query(StringUtil::Format(
		    "SELECT COUNT(*) FROM %s WHERE _op = 'U'", delta_table));
		int64_t changed_count = 0;
		if (!changed_result->HasError() && changed_result->RowCount() > 0) {
			changed_count = changed_result->GetValue(0, 0).GetValue<int64_t>();
		}

		// Only include tables with changes
		if (added_count > 0 || removed_count > 0 || changed_count > 0) {
			vector<Value> row;
			row.push_back(Value(table_name));
			row.push_back(Value::BIGINT(added_count));
			row.push_back(Value::BIGINT(removed_count));
			row.push_back(Value::BIGINT(changed_count));
			bind_data.results.push_back(std::move(row));
		}
	}
}

static void ScenarioCompareAllFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ScenarioCompareAllBindData>();
	auto &state = data_p.global_state->Cast<ScenarioCompareAllState>();

	ExecuteCompareAll(context, bind_data);

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
// scenario_compare 3-arg (scenario-to-scenario) Table Function
//===--------------------------------------------------------------------===//

struct ScenarioCompare3BindData : public TableFunctionData {
	string scenario_a;
	string scenario_b;
	string table_name;
	string schema_a;
	string schema_b;
	vector<string> pk_columns;
	vector<string> all_columns;
	bool executed = false;
	vector<vector<Value>> results;
};

static unique_ptr<FunctionData> ScenarioCompare3Bind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ScenarioCompare3BindData>();

	result->scenario_a = input.inputs[0].GetValue<string>();
	result->scenario_b = input.inputs[1].GetValue<string>();
	result->table_name = input.inputs[2].GetValue<string>();

	Connection con(context.db->GetDatabase(context));

	// Validate scenario A exists and get schema
	auto result_a = con.Query(StringUtil::Format(
	    "SELECT schema_name FROM _scenario_registry WHERE scenario_name = '%s'",
	    ScenarioManager::EscapeSQLString(result->scenario_a)));
	if (result_a->HasError() || result_a->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", result->scenario_a);
	}
	result->schema_a = result_a->GetValue(0, 0).ToString();

	// Validate scenario B exists and get schema
	auto result_b = con.Query(StringUtil::Format(
	    "SELECT schema_name FROM _scenario_registry WHERE scenario_name = '%s'",
	    ScenarioManager::EscapeSQLString(result->scenario_b)));
	if (result_b->HasError() || result_b->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", result->scenario_b);
	}
	result->schema_b = result_b->GetValue(0, 0).ToString();

	// Get columns from base table
	auto columns_result = con.Query(StringUtil::Format(
	    "SELECT column_name FROM information_schema.columns "
	    "WHERE table_schema = 'main' AND table_name = '%s' ORDER BY ordinal_position",
	    ScenarioManager::EscapeSQLString(result->table_name)));
	if (columns_result->HasError() || columns_result->RowCount() == 0) {
		throw InvalidInputException("Base table '%s' does not exist in main schema", result->table_name);
	}
	for (idx_t i = 0; i < columns_result->RowCount(); i++) {
		result->all_columns.push_back(columns_result->GetValue(0, i).ToString());
	}

	// Get PK columns (check scenario A's registration first, then B)
	auto pk_result = con.Query(StringUtil::Format(
	    "SELECT primary_key_columns FROM _scenario_tables WHERE "
	    "scenario_id = (SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '%s') "
	    "AND table_name = '%s'",
	    ScenarioManager::EscapeSQLString(result->scenario_a),
	    ScenarioManager::EscapeSQLString(result->table_name)));
	if (!pk_result->HasError() && pk_result->RowCount() > 0 && !pk_result->GetValue(0, 0).IsNull()) {
		auto pk_list = ListValue::GetChildren(pk_result->GetValue(0, 0));
		for (auto &pk : pk_list) {
			result->pk_columns.push_back(pk.ToString());
		}
	}
	if (result->pk_columns.empty()) {
		result->pk_columns = result->all_columns;
	}

	// Output schema: diff_type, pk_columns..., column_name, old_value, new_value
	names.push_back("diff_type");
	return_types.push_back(LogicalType::VARCHAR);

	for (const auto &pk : result->pk_columns) {
		names.push_back(pk);
		auto type_result = con.Query(StringUtil::Format(
		    "SELECT data_type FROM information_schema.columns WHERE table_schema = 'main' AND table_name = '%s' AND column_name = '%s'",
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

static void ExecuteCompare3(ClientContext &context, ScenarioCompare3BindData &bind_data) {
	if (bind_data.executed) {
		return;
	}
	bind_data.executed = true;

	Connection con(context.db->GetDatabase(context));

	// Build PK list and join condition
	string pk_list;
	string pk_join_cond;
	for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
		if (i > 0) {
			pk_list += ", ";
			pk_join_cond += " AND ";
		}
		string pk_quoted = ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]);
		pk_list += pk_quoted;
		pk_join_cond += "a." + pk_quoted + " = b." + pk_quoted;
	}

	// Build non-PK columns list
	vector<string> non_pk_columns;
	for (const auto &col : bind_data.all_columns) {
		bool is_pk = false;
		for (const auto &pk : bind_data.pk_columns) {
			if (col == pk) { is_pk = true; break; }
		}
		if (!is_pk) non_pk_columns.push_back(col);
	}

	string view_a = ScenarioManager::QuoteIdentifier(bind_data.schema_a) + "." +
	                ScenarioManager::QuoteIdentifier(bind_data.table_name);
	string view_b = ScenarioManager::QuoteIdentifier(bind_data.schema_b) + "." +
	                ScenarioManager::QuoteIdentifier(bind_data.table_name);

	// Added: in B but not in A
	string added_query = StringUtil::Format(
	    "SELECT %s FROM %s b WHERE NOT EXISTS (SELECT 1 FROM %s a WHERE %s)",
	    pk_list, view_b, view_a, pk_join_cond);
	auto added_result = con.Query(added_query);
	if (!added_result->HasError()) {
		for (idx_t row = 0; row < added_result->RowCount(); row++) {
			vector<Value> result_row;
			result_row.push_back(Value("added"));
			for (idx_t pk_idx = 0; pk_idx < bind_data.pk_columns.size(); pk_idx++) {
				result_row.push_back(added_result->GetValue(pk_idx, row));
			}
			result_row.push_back(Value());
			result_row.push_back(Value());
			result_row.push_back(Value());
			bind_data.results.push_back(std::move(result_row));
		}
	}

	// Removed: in A but not in B
	string removed_query = StringUtil::Format(
	    "SELECT %s FROM %s a WHERE NOT EXISTS (SELECT 1 FROM %s b WHERE %s)",
	    pk_list, view_a, view_b, pk_join_cond);
	auto removed_result = con.Query(removed_query);
	if (!removed_result->HasError()) {
		for (idx_t row = 0; row < removed_result->RowCount(); row++) {
			vector<Value> result_row;
			result_row.push_back(Value("removed"));
			for (idx_t pk_idx = 0; pk_idx < bind_data.pk_columns.size(); pk_idx++) {
				result_row.push_back(removed_result->GetValue(pk_idx, row));
			}
			result_row.push_back(Value());
			result_row.push_back(Value());
			result_row.push_back(Value());
			bind_data.results.push_back(std::move(result_row));
		}
	}

	// Changed: in both A and B with different values
	if (!non_pk_columns.empty()) {
		string select_cols;
		for (idx_t i = 0; i < bind_data.pk_columns.size(); i++) {
			if (i > 0) select_cols += ", ";
			select_cols += "a." + ScenarioManager::QuoteIdentifier(bind_data.pk_columns[i]) + " AS pk_" + std::to_string(i);
		}
		for (const auto &col : non_pk_columns) {
			string col_q = ScenarioManager::QuoteIdentifier(col);
			select_cols += ", a." + col_q + " AS a_" + col + ", b." + col_q + " AS b_" + col;
		}

		string changed_query = StringUtil::Format(
		    "SELECT %s FROM %s a INNER JOIN %s b ON %s",
		    select_cols, view_a, view_b, pk_join_cond);
		auto changed_result = con.Query(changed_query);
		if (!changed_result->HasError()) {
			for (idx_t row = 0; row < changed_result->RowCount(); row++) {
				vector<Value> pk_values;
				for (idx_t pk_idx = 0; pk_idx < bind_data.pk_columns.size(); pk_idx++) {
					pk_values.push_back(changed_result->GetValue(pk_idx, row));
				}
				idx_t col_offset = bind_data.pk_columns.size();
				for (idx_t col_idx = 0; col_idx < non_pk_columns.size(); col_idx++) {
					Value val_a = changed_result->GetValue(col_offset + col_idx * 2, row);
					Value val_b = changed_result->GetValue(col_offset + col_idx * 2 + 1, row);
					string str_a = val_a.IsNull() ? "" : val_a.ToString();
					string str_b = val_b.IsNull() ? "" : val_b.ToString();
					if (str_a != str_b) {
						vector<Value> result_row;
						result_row.push_back(Value("changed"));
						for (const auto &pk_val : pk_values) result_row.push_back(pk_val);
						result_row.push_back(Value(non_pk_columns[col_idx]));
						result_row.push_back(val_a.IsNull() ? Value() : Value(str_a));
						result_row.push_back(val_b.IsNull() ? Value() : Value(str_b));
						bind_data.results.push_back(std::move(result_row));
					}
				}
			}
		}
	}
}

struct ScenarioCompare3State : public GlobalTableFunctionState {
	idx_t current_row = 0;
};

static unique_ptr<GlobalTableFunctionState> ScenarioCompare3Init(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ScenarioCompare3State>();
}

static void ScenarioCompare3Function(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ScenarioCompare3BindData>();
	auto &state = data_p.global_state->Cast<ScenarioCompare3State>();

	ExecuteCompare3(context, bind_data);

	idx_t count = 0;
	while (state.current_row < bind_data.results.size() && count < STANDARD_VECTOR_SIZE) {
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
	// scenario_compare has two overloads: 2-arg (vs base) and 3-arg (scenario-to-scenario)
	TableFunctionSet scenario_compare_set("scenario_compare");
	scenario_compare_set.AddFunction(TableFunction("scenario_compare", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                               ScenarioCompareFunction, ScenarioCompareBind, ScenarioCompareInit));
	scenario_compare_set.AddFunction(TableFunction("scenario_compare",
	                                               {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                               ScenarioCompare3Function, ScenarioCompare3Bind, ScenarioCompare3Init));
	{
		CreateTableFunctionInfo info(scenario_compare_set);
		{
			FunctionDescription d;
			d.description     = "Compare a single table in a scenario against the base, returning rows with change_type ('inserted', 'updated', 'deleted') and column-level diffs.";
			d.examples        = {"SELECT * FROM scenario_compare('forecast_q1', 'products')"};
			d.categories      = {"comparison"};
			d.parameter_names = {"scenario_name", "table_name"};
			d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
			info.descriptions.push_back(std::move(d));
		}
		{
			FunctionDescription d;
			d.description     = "Compare a single table between two scenarios, returning rows with change_type and column-level diffs.";
			d.examples        = {"SELECT * FROM scenario_compare('forecast_q1', 'forecast_q2', 'products')"};
			d.categories      = {"comparison"};
			d.parameter_names = {"scenario_a", "scenario_b", "table_name"};
			d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
			info.descriptions.push_back(std::move(d));
		}
		loader.RegisterFunction(std::move(info));
	}

	// scenario_compare_all(scenario_name) -> TABLE
	TableFunction scenario_compare_all("scenario_compare_all", {LogicalType::VARCHAR},
	                                   ScenarioCompareAllFunction, ScenarioCompareAllBind, ScenarioCompareAllInit);
	{
		CreateTableFunctionInfo info(scenario_compare_all);
		FunctionDescription d;
		d.description     = "Compare all delta tables in a scenario against the base, returning a summary of changes per table with insert, update, and delete counts.";
		d.examples        = {"SELECT * FROM scenario_compare_all('forecast_q1')"};
		d.categories      = {"comparison"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace duckdb
