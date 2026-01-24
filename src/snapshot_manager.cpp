#include "snapshot_manager.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

// ===== Helper Functions =====

bool SnapshotManager::ValidateName(const string &name) {
	// Must be alphanumeric + underscore, max 63 chars, and not empty
	if (name.empty() || name.length() > 63) {
		return false;
	}
	// Check each character: must be alphanumeric or underscore
	for (char c : name) {
		if (!std::isalnum(c) && c != '_') {
			return false;
		}
	}
	// Must not start with a digit
	if (std::isdigit(name[0])) {
		return false;
	}
	return true;
}

bool SnapshotManager::SnapshotExists(ClientContext &context, const string &name) {
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query("SELECT 1 FROM _scenario_snapshots WHERE snapshot_name = '" + name + "'");
	return result->RowCount() > 0;
}

bool SnapshotManager::ScenarioExists(ClientContext &context, const string &name) {
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query("SELECT 1 FROM _scenario_registry WHERE scenario_name = '" + name + "'");
	return result->RowCount() > 0;
}

string SnapshotManager::GetScenarioSchema(ClientContext &context, const string &scenario_name) {
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query("SELECT schema_name FROM _scenario_registry WHERE scenario_name = '" + scenario_name + "'");
	if (result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}
	return result->GetValue(0, 0).GetValue<string>();
}

// ===== Bind Data Structures =====

struct SnapshotCreateBindData : public FunctionData {
	string scenario_name;
	string snapshot_name;
	string description;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<SnapshotCreateBindData>();
		result->scenario_name = scenario_name;
		result->snapshot_name = snapshot_name;
		result->description = description;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<SnapshotCreateBindData>();
		return scenario_name == other.scenario_name &&
		       snapshot_name == other.snapshot_name &&
		       description == other.description;
	}
};

// ===== snapshot_create Implementation =====

static unique_ptr<FunctionData> SnapshotCreateBind(ClientContext &context, ScalarFunction &bound_function,
                                                    vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<SnapshotCreateBindData>();

	// Extract scenario_name (first argument)
	if (arguments.size() < 1 || arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("snapshot_create requires a scenario name as first argument");
	}
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("snapshot_create scenario name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	// Extract snapshot_name (second argument)
	if (arguments.size() < 2 || arguments[1]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("snapshot_create requires a snapshot name as second argument");
	}
	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("snapshot_create snapshot name must be a constant");
	}
	bind_data->snapshot_name = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	// Extract description (third argument, optional)
	if (arguments.size() > 2 && arguments[2]->IsFoldable()) {
		bind_data->description = ExpressionExecutor::EvaluateScalar(context, *arguments[2]).GetValue<string>();
	}

	// Validate scenario name
	if (!SnapshotManager::ValidateName(bind_data->scenario_name)) {
		throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.",
		                            bind_data->scenario_name);
	}

	// Validate snapshot name
	if (!SnapshotManager::ValidateName(bind_data->snapshot_name)) {
		throw InvalidInputException("Invalid snapshot name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.",
		                            bind_data->snapshot_name);
	}

	// Check if scenario exists
	if (!SnapshotManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	// Check if snapshot already exists
	if (SnapshotManager::SnapshotExists(context, bind_data->snapshot_name)) {
		throw InvalidInputException("Snapshot '%s' already exists", bind_data->snapshot_name);
	}

	return std::move(bind_data);
}

static void SnapshotCreateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<SnapshotCreateBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	// Get the source schema for the scenario
	string source_schema = SnapshotManager::GetScenarioSchema(context, bind_data.scenario_name);

	// Create snapshot schema
	string snapshot_schema = "_snap_" + bind_data.snapshot_name;
	auto create_schema_result = con.Query("CREATE SCHEMA IF NOT EXISTS " + snapshot_schema);
	if (create_schema_result->HasError()) {
		throw InvalidInputException("Failed to create snapshot schema: %s", create_schema_result->GetError());
	}

	// Copy all merged views from source scenario to snapshot schema as materialized tables
	// This captures the scenario's state at snapshot time, including base data + deltas
	auto views_result = con.Query(StringUtil::Format(
	    "SELECT table_name FROM information_schema.tables "
	    "WHERE table_schema = '%s' AND table_type = 'VIEW' AND table_name NOT LIKE '_delta_%%'",
	    source_schema));

	int64_t total_size = 0;
	if (!views_result->HasError()) {
		for (idx_t i = 0; i < views_result->RowCount(); i++) {
			string view_name = views_result->GetValue(0, i).ToString();

			// Materialize the merged view into a table in snapshot schema
			auto copy_sql = StringUtil::Format(
			    "CREATE TABLE %s.%s AS SELECT * FROM %s.%s",
			    snapshot_schema, view_name, source_schema, view_name);
			auto copy_result = con.Query(copy_sql);
			if (copy_result->HasError()) {
				// Rollback: drop the schema
				con.Query("DROP SCHEMA " + snapshot_schema + " CASCADE");
				throw InvalidInputException("Failed to snapshot table '%s': %s", view_name, copy_result->GetError());
			}

			// Get size of copied table for size tracking
			auto size_result = con.Query(StringUtil::Format(
			    "SELECT COUNT(*) * 100 FROM %s.%s", snapshot_schema, view_name)); // rough estimate
			if (!size_result->HasError() && size_result->RowCount() > 0) {
				total_size += size_result->GetValue(0, 0).GetValue<int64_t>();
			}
		}
	}

	// Get next snapshot_id
	auto id_result = con.Query("SELECT COALESCE(MAX(snapshot_id), 0) + 1 FROM _scenario_snapshots");
	if (id_result->HasError()) {
		con.Query("DROP SCHEMA " + snapshot_schema + " CASCADE");
		throw InvalidInputException("Failed to get next snapshot ID: %s", id_result->GetError());
	}
	int64_t snapshot_id = id_result->GetValue(0, 0).GetValue<int64_t>();

	// Insert into _scenario_snapshots table
	string desc_value = bind_data.description.empty() ? "NULL" : "'" + bind_data.description + "'";
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO _scenario_snapshots (snapshot_id, snapshot_name, source_schema, description, size_bytes) "
	    "VALUES (%d, '%s', '%s', %s, %d)",
	    snapshot_id, bind_data.snapshot_name, source_schema, desc_value, total_size);

	auto insert_result = con.Query(insert_sql);
	if (insert_result->HasError()) {
		con.Query("DROP SCHEMA " + snapshot_schema + " CASCADE");
		throw InvalidInputException("Failed to create snapshot: %s", insert_result->GetError());
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== snapshot_list Implementation =====

struct SnapshotListBindData : public TableFunctionData {
	bool done = false;
};

struct SnapshotListGlobalState : public GlobalTableFunctionState {
	idx_t current_row = 0;
	vector<vector<Value>> rows;
};

static unique_ptr<FunctionData> SnapshotListBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	// Define output columns
	names.emplace_back("snapshot_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("source_schema");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("size_bytes");
	return_types.emplace_back(LogicalType::BIGINT);

	return make_uniq<SnapshotListBindData>();
}

static unique_ptr<GlobalTableFunctionState> SnapshotListInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<SnapshotListGlobalState>();

	// Query all snapshots ordered by created_at DESC
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query(
	    "SELECT snapshot_name, source_schema, created_at, description, size_bytes "
	    "FROM _scenario_snapshots ORDER BY created_at DESC");

	if (!result->HasError()) {
		for (idx_t i = 0; i < result->RowCount(); i++) {
			vector<Value> row;
			row.push_back(result->GetValue(0, i)); // snapshot_name
			row.push_back(result->GetValue(1, i)); // source_schema
			row.push_back(result->GetValue(2, i)); // created_at
			row.push_back(result->GetValue(3, i)); // description
			row.push_back(result->GetValue(4, i)); // size_bytes
			state->rows.push_back(std::move(row));
		}
	}

	return std::move(state);
}

static void SnapshotListFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SnapshotListGlobalState>();

	idx_t count = 0;
	while (state.current_row < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.current_row];
		output.SetValue(0, count, row[0]); // snapshot_name
		output.SetValue(1, count, row[1]); // source_schema
		output.SetValue(2, count, row[2]); // created_at
		output.SetValue(3, count, row[3]); // description
		output.SetValue(4, count, row[4]); // size_bytes
		state.current_row++;
		count++;
	}

	output.SetCardinality(count);
}

// ===== snapshot_compare Implementation =====

struct SnapshotCompareBindData : public TableFunctionData {
	string snapshot_name;
	string table_name;
	string snapshot_schema;
	string source_schema;
	vector<string> pk_columns;
	vector<string> all_columns;
};

static unique_ptr<FunctionData> SnapshotCompareBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<SnapshotCompareBindData>();

	if (input.inputs.size() < 2) {
		throw InvalidInputException("snapshot_compare requires snapshot_name and table_name");
	}

	result->snapshot_name = input.inputs[0].GetValue<string>();
	result->table_name = input.inputs[1].GetValue<string>();

	Connection con(context.db->GetDatabase(context));

	// Get snapshot metadata
	auto snap_result = con.Query(StringUtil::Format(
	    "SELECT source_schema FROM _scenario_snapshots WHERE snapshot_name = '%s'",
	    result->snapshot_name));
	if (snap_result->HasError() || snap_result->RowCount() == 0) {
		throw InvalidInputException("Snapshot '%s' does not exist", result->snapshot_name);
	}
	result->source_schema = snap_result->GetValue(0, 0).ToString();
	result->snapshot_schema = "_snap_" + result->snapshot_name;

	// Check snapshot schema exists
	auto schema_result = con.Query(StringUtil::Format(
	    "SELECT 1 FROM information_schema.schemata WHERE schema_name = '%s'",
	    result->snapshot_schema));
	if (schema_result->RowCount() == 0) {
		throw InvalidInputException("Snapshot data not found for '%s'", result->snapshot_name);
	}

	// Check table exists in snapshot schema (materialized from scenario's merged view)
	auto table_result = con.Query(StringUtil::Format(
	    "SELECT 1 FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
	    result->snapshot_schema, result->table_name));
	if (table_result->RowCount() == 0) {
		throw InvalidInputException("Table '%s' not found in snapshot '%s'", result->table_name, result->snapshot_name);
	}

	// Get columns from main table
	auto cols_result = con.Query(StringUtil::Format(
	    "SELECT column_name FROM information_schema.columns "
	    "WHERE table_schema = 'main' AND table_name = '%s' ORDER BY ordinal_position",
	    result->table_name));
	if (cols_result->HasError() || cols_result->RowCount() == 0) {
		throw InvalidInputException("Base table '%s' does not exist in main schema", result->table_name);
	}
	for (idx_t i = 0; i < cols_result->RowCount(); i++) {
		result->all_columns.push_back(cols_result->GetValue(0, i).ToString());
	}

	// Get primary key columns from source scenario's _scenario_tables
	auto pk_result = con.Query(StringUtil::Format(
	    "SELECT primary_key_columns FROM _scenario_tables WHERE "
	    "scenario_id = (SELECT scenario_id FROM _scenario_registry WHERE schema_name = '%s') "
	    "AND table_name = '%s'",
	    result->source_schema, result->table_name));
	if (!pk_result->HasError() && pk_result->RowCount() > 0 && !pk_result->GetValue(0, 0).IsNull()) {
		auto pk_list = ListValue::GetChildren(pk_result->GetValue(0, 0));
		for (auto &pk : pk_list) {
			result->pk_columns.push_back(pk.ToString());
		}
	}
	// If no PK, use all columns as composite key
	if (result->pk_columns.empty()) {
		result->pk_columns = result->all_columns;
	}

	// Build output schema: diff_type, pk_columns..., column_name, old_value, new_value
	names.push_back("diff_type");
	return_types.push_back(LogicalType::VARCHAR);

	for (const auto &pk : result->pk_columns) {
		names.push_back(pk);
		// Determine type for PK column
		auto type_result = con.Query(StringUtil::Format(
		    "SELECT data_type FROM information_schema.columns WHERE table_schema = 'main' AND table_name = '%s' AND column_name = '%s'",
		    result->table_name, pk));
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

struct SnapshotCompareGlobalState : public GlobalTableFunctionState {
	idx_t current_row = 0;
	vector<vector<Value>> rows;
};

static unique_ptr<GlobalTableFunctionState> SnapshotCompareInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<SnapshotCompareGlobalState>();
	auto &bind_data = input.bind_data->Cast<SnapshotCompareBindData>();

	Connection con(context.db->GetDatabase(context));

	// Snapshot table (materialized from scenario's merged view at snapshot time)
	string snap_table = bind_data.snapshot_schema + "." + bind_data.table_name;

	// Build PK column list for joins
	string pk_join;
	for (size_t i = 0; i < bind_data.pk_columns.size(); i++) {
		const auto &pk = bind_data.pk_columns[i];
		if (i > 0) {
			pk_join += " AND ";
		}
		pk_join += "(snap." + pk + " IS NOT DISTINCT FROM curr." + pk + ")";
	}

	// Non-PK columns for change detection
	vector<string> non_pk_cols;
	for (const auto &col : bind_data.all_columns) {
		bool is_pk = false;
		for (const auto &pk : bind_data.pk_columns) {
			if (col == pk) {
				is_pk = true;
				break;
			}
		}
		if (!is_pk) {
			non_pk_cols.push_back(col);
		}
	}

	// Find added rows (in current main but not in snapshot)
	string added_query = StringUtil::Format(
	    "SELECT 'added' AS diff_type, %s, NULL AS column_name, NULL AS old_value, NULL AS new_value "
	    "FROM main.%s curr "
	    "WHERE NOT EXISTS (SELECT 1 FROM %s snap WHERE %s)",
	    [&]() {
		    string cols;
		    for (size_t i = 0; i < bind_data.pk_columns.size(); i++) {
			    if (i > 0) cols += ", ";
			    cols += "curr." + bind_data.pk_columns[i];
		    }
		    return cols;
	    }(), bind_data.table_name, snap_table, pk_join);

	auto added_result = con.Query(added_query);
	if (!added_result->HasError()) {
		for (idx_t i = 0; i < added_result->RowCount(); i++) {
			vector<Value> row;
			for (idx_t c = 0; c < added_result->ColumnCount(); c++) {
				row.push_back(added_result->GetValue(c, i));
			}
			state->rows.push_back(std::move(row));
		}
	}

	// Find removed rows (in snapshot but not in current main)
	string removed_query = StringUtil::Format(
	    "SELECT 'removed' AS diff_type, %s, NULL AS column_name, NULL AS old_value, NULL AS new_value "
	    "FROM %s snap "
	    "WHERE NOT EXISTS (SELECT 1 FROM main.%s curr WHERE %s)",
	    [&]() {
		    string cols;
		    for (size_t i = 0; i < bind_data.pk_columns.size(); i++) {
			    if (i > 0) cols += ", ";
			    cols += "snap." + bind_data.pk_columns[i];
		    }
		    return cols;
	    }(), snap_table, bind_data.table_name, pk_join);

	auto removed_result = con.Query(removed_query);
	if (!removed_result->HasError()) {
		for (idx_t i = 0; i < removed_result->RowCount(); i++) {
			vector<Value> row;
			for (idx_t c = 0; c < removed_result->ColumnCount(); c++) {
				row.push_back(removed_result->GetValue(c, i));
			}
			state->rows.push_back(std::move(row));
		}
	}

	// Find changed rows (same PK, different values in non-PK columns)
	if (!non_pk_cols.empty()) {
		for (const auto &col : non_pk_cols) {
			string changed_query = StringUtil::Format(
			    "SELECT 'changed' AS diff_type, %s, '%s' AS column_name, "
			    "CAST(snap.%s AS VARCHAR) AS old_value, CAST(curr.%s AS VARCHAR) AS new_value "
			    "FROM %s snap "
			    "JOIN main.%s curr ON %s "
			    "WHERE snap.%s IS DISTINCT FROM curr.%s",
			    [&]() {
				    string cols;
				    for (size_t i = 0; i < bind_data.pk_columns.size(); i++) {
					    if (i > 0) cols += ", ";
					    cols += "snap." + bind_data.pk_columns[i];
				    }
				    return cols;
			    }(), col, col, col, snap_table, bind_data.table_name, pk_join, col, col);

			auto changed_result = con.Query(changed_query);
			if (!changed_result->HasError()) {
				for (idx_t i = 0; i < changed_result->RowCount(); i++) {
					vector<Value> row;
					for (idx_t c = 0; c < changed_result->ColumnCount(); c++) {
						row.push_back(changed_result->GetValue(c, i));
					}
					state->rows.push_back(std::move(row));
				}
			}
		}
	}

	return std::move(state);
}

static void SnapshotCompareFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SnapshotCompareGlobalState>();

	idx_t count = 0;
	while (state.current_row < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.current_row];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		state.current_row++;
		count++;
	}

	output.SetCardinality(count);
}

// ===== scenario_from_snapshot Implementation =====

struct ScenarioFromSnapshotBindData : public FunctionData {
	string snapshot_name;
	string scenario_name;
	string description;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ScenarioFromSnapshotBindData>();
		result->snapshot_name = snapshot_name;
		result->scenario_name = scenario_name;
		result->description = description;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScenarioFromSnapshotBindData>();
		return snapshot_name == other.snapshot_name &&
		       scenario_name == other.scenario_name &&
		       description == other.description;
	}
};

static unique_ptr<FunctionData> ScenarioFromSnapshotBind(ClientContext &context, ScalarFunction &bound_function,
                                                          vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ScenarioFromSnapshotBindData>();

	if (arguments.size() < 1 || arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_from_snapshot requires a snapshot name as first argument");
	}
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("scenario_from_snapshot snapshot name must be a constant");
	}
	bind_data->snapshot_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (arguments.size() < 2 || arguments[1]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_from_snapshot requires a scenario name as second argument");
	}
	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("scenario_from_snapshot scenario name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	if (arguments.size() > 2 && arguments[2]->IsFoldable()) {
		bind_data->description = ExpressionExecutor::EvaluateScalar(context, *arguments[2]).GetValue<string>();
	}

	// Validate snapshot name
	if (!SnapshotManager::ValidateName(bind_data->snapshot_name)) {
		throw InvalidInputException("Invalid snapshot name '%s'", bind_data->snapshot_name);
	}

	// Validate scenario name
	if (!SnapshotManager::ValidateName(bind_data->scenario_name)) {
		throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.",
		                            bind_data->scenario_name);
	}

	// Check if snapshot exists
	if (!SnapshotManager::SnapshotExists(context, bind_data->snapshot_name)) {
		throw InvalidInputException("Snapshot '%s' does not exist", bind_data->snapshot_name);
	}

	// Check if scenario already exists
	Connection con(context.db->GetDatabase(context));
	auto existing = con.Query(StringUtil::Format(
	    "SELECT 1 FROM _scenario_registry WHERE scenario_name = '%s'",
	    bind_data->scenario_name));
	if (existing->RowCount() > 0) {
		throw InvalidInputException("Scenario '%s' already exists", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ScenarioFromSnapshotFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioFromSnapshotBindData>();
	auto &context = state.GetContext();

	Connection con(context.db->GetDatabase(context));

	string snapshot_schema = "_snap_" + bind_data.snapshot_name;
	string scenario_schema = "_scen_" + bind_data.scenario_name;

	// Create the scenario schema
	auto create_result = con.Query("CREATE SCHEMA " + scenario_schema);
	if (create_result->HasError()) {
		throw InvalidInputException("Failed to create scenario schema: %s", create_result->GetError());
	}

	// Get next scenario_id
	auto id_result = con.Query("SELECT COALESCE(MAX(scenario_id), 0) + 1 FROM _scenario_registry");
	if (id_result->HasError()) {
		con.Query("DROP SCHEMA " + scenario_schema + " CASCADE");
		throw InvalidInputException("Failed to get next scenario ID: %s", id_result->GetError());
	}
	int64_t scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

	// Register the scenario with snapshot as base_schema
	string desc_value = bind_data.description.empty() ? "NULL" : "'" + bind_data.description + "'";
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO _scenario_registry (scenario_id, scenario_name, schema_name, base_schema, base_captured_at, description) "
	    "VALUES (%d, '%s', '%s', '%s', current_timestamp, %s)",
	    scenario_id, bind_data.scenario_name, scenario_schema, snapshot_schema, desc_value);

	auto insert_result = con.Query(insert_sql);
	if (insert_result->HasError()) {
		con.Query("DROP SCHEMA " + scenario_schema + " CASCADE");
		throw InvalidInputException("Failed to register scenario: %s", insert_result->GetError());
	}

	// Get all tables in the snapshot schema and create delta tables + merge views for each
	auto tables_result = con.Query(StringUtil::Format(
	    "SELECT table_name FROM information_schema.tables WHERE table_schema = '%s' AND table_type = 'BASE TABLE'",
	    snapshot_schema));

	if (!tables_result->HasError()) {
		for (idx_t i = 0; i < tables_result->RowCount(); i++) {
			string table_name = tables_result->GetValue(0, i).ToString();

			// Get columns from snapshot table
			auto cols_result = con.Query(StringUtil::Format(
			    "SELECT column_name, data_type, is_nullable FROM information_schema.columns "
			    "WHERE table_schema = '%s' AND table_name = '%s' ORDER BY ordinal_position",
			    snapshot_schema, table_name));

			if (cols_result->HasError() || cols_result->RowCount() == 0) {
				continue;
			}

			// Build column definitions for delta table
			string col_defs;
			vector<string> columns;
			for (idx_t c = 0; c < cols_result->RowCount(); c++) {
				string col_name = cols_result->GetValue(0, c).ToString();
				string col_type = cols_result->GetValue(1, c).ToString();
				string nullable = cols_result->GetValue(2, c).ToString();

				columns.push_back(col_name);
				if (!col_defs.empty()) col_defs += ", ";
				col_defs += col_name + " " + col_type;
				if (nullable == "NO") {
					col_defs += " NOT NULL";
				}
			}

			// Create delta table
			string delta_table = "_delta_" + table_name;
			auto delta_sql = StringUtil::Format(
			    "CREATE TABLE %s.%s (_op VARCHAR NOT NULL CHECK (_op IN ('I', 'U', 'D')), "
			    "_ts TIMESTAMP DEFAULT current_timestamp, _version INTEGER DEFAULT 1, %s)",
			    scenario_schema, delta_table, col_defs);
			con.Query(delta_sql);

			// Build merge view (using snapshot as base instead of main)
			vector<string> quoted_columns;
			for (const auto &col : columns) {
				quoted_columns.push_back(col);
			}
			string column_list = StringUtil::Join(quoted_columns, ", ");

			// For now, use all columns as composite key (no PK info in snapshot)
			vector<string> all_join_conditions;
			for (const auto &col : columns) {
				all_join_conditions.push_back("d." + col + " IS NOT DISTINCT FROM base." + col);
			}
			string join_condition = StringUtil::Join(all_join_conditions, " AND ");

			vector<string> base_columns;
			for (const auto &col : columns) {
				base_columns.push_back("base." + col);
			}
			string base_column_list = StringUtil::Join(base_columns, ", ");

			auto view_sql = StringUtil::Format(
			    "CREATE VIEW %s.%s AS "
			    "SELECT %s FROM %s.%s base "
			    "WHERE NOT EXISTS ("
			    "  SELECT 1 FROM %s.%s d "
			    "  WHERE %s AND d._op IN ('U', 'D')"
			    ") "
			    "UNION ALL "
			    "SELECT %s FROM %s.%s WHERE _op IN ('I', 'U')",
			    scenario_schema, table_name,
			    base_column_list, snapshot_schema, table_name,
			    scenario_schema, delta_table,
			    join_condition,
			    column_list, scenario_schema, delta_table);
			con.Query(view_sql);

			// Register table
			auto reg_sql = StringUtil::Format(
			    "INSERT INTO _scenario_tables (scenario_id, table_name, base_row_count, has_primary_key, primary_key_columns) "
			    "VALUES (%d, '%s', (SELECT COUNT(*) FROM %s.%s), false, NULL)",
			    scenario_id, table_name, snapshot_schema, table_name);
			con.Query(reg_sql);
		}
	}

	// Return true
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== Function Registration =====

void SnapshotManager::RegisterFunctions(ExtensionLoader &loader) {
	// snapshot_create(scenario_name, snapshot_name, description) - returns boolean
	ScalarFunctionSet snapshot_create_set("snapshot_create");

	// 3-argument version: scenario_name, snapshot_name, description
	ScalarFunction snapshot_create_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                  LogicalType::BOOLEAN,
	                                  SnapshotCreateFunction, SnapshotCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	snapshot_create_set.AddFunction(snapshot_create_3);

	// 2-argument version: scenario_name, snapshot_name (no description)
	ScalarFunction snapshot_create_2({LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                  LogicalType::BOOLEAN,
	                                  SnapshotCreateFunction, SnapshotCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	snapshot_create_set.AddFunction(snapshot_create_2);

	loader.RegisterFunction(snapshot_create_set);

	// snapshot_list() - returns table of snapshots
	TableFunction snapshot_list("snapshot_list", {}, SnapshotListFunction, SnapshotListBind, SnapshotListInit);
	loader.RegisterFunction(snapshot_list);

	// snapshot_compare(snapshot_name, table_name) - compare current state against snapshot
	TableFunction snapshot_compare("snapshot_compare", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                               SnapshotCompareFunction, SnapshotCompareBind, SnapshotCompareInit);
	loader.RegisterFunction(snapshot_compare);

	// scenario_from_snapshot(snapshot_name, scenario_name, description) - create scenario from snapshot
	ScalarFunctionSet scenario_from_snapshot_set("scenario_from_snapshot");

	// 3-argument version: snapshot_name, scenario_name, description
	ScalarFunction scenario_from_snapshot_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                         LogicalType::BOOLEAN,
	                                         ScenarioFromSnapshotFunction, ScenarioFromSnapshotBind, nullptr, nullptr, nullptr,
	                                         LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	scenario_from_snapshot_set.AddFunction(scenario_from_snapshot_3);

	// 2-argument version: snapshot_name, scenario_name (no description)
	ScalarFunction scenario_from_snapshot_2({LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                         LogicalType::BOOLEAN,
	                                         ScenarioFromSnapshotFunction, ScenarioFromSnapshotBind, nullptr, nullptr, nullptr,
	                                         LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	scenario_from_snapshot_set.AddFunction(scenario_from_snapshot_2);

	loader.RegisterFunction(scenario_from_snapshot_set);
}

} // namespace duckdb
