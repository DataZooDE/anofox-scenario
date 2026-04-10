#include "delta_storage_engine.hpp"
#include "scenario_manager.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

string DeltaStorageEngine::GetDeltaTableName(const string &base_table_name) {
	return "_delta_" + base_table_name;
}

bool DeltaStorageEngine::DeltaTableExists(ClientContext &context, const string &scenario_schema,
                                          const string &table_name) {
	Connection con(context.db->GetDatabase(context));
	auto delta_name = GetDeltaTableName(table_name);
	auto result = con.Query(StringUtil::Format(
	    "SELECT 1 FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
	    scenario_schema.c_str(), delta_name.c_str()));
	return result->RowCount() > 0;
}

bool DeltaStorageEngine::CreateDeltaTable(ClientContext &context, const string &scenario_schema,
                                          const string &base_table_name) {
	Connection con(context.db->GetDatabase(context));

	// Get base table schema information
	auto columns_result = con.Query(StringUtil::Format(
	    "SELECT column_name, data_type, is_nullable, column_default "
	    "FROM information_schema.columns "
	    "WHERE table_schema = 'main' AND table_name = '%s' "
	    "ORDER BY ordinal_position",
	    base_table_name.c_str()));

	if (columns_result->RowCount() == 0) {
		throw InvalidInputException("Base table '%s' not found in main schema", base_table_name);
	}

	// Get primary key columns
	auto pk_result = con.Query(StringUtil::Format(
	    "SELECT kcu.column_name "
	    "FROM information_schema.table_constraints tc "
	    "JOIN information_schema.key_column_usage kcu "
	    "  ON tc.constraint_name = kcu.constraint_name "
	    "WHERE tc.table_schema = 'main' AND tc.table_name = '%s' "
	    "  AND tc.constraint_type = 'PRIMARY KEY' "
	    "ORDER BY kcu.ordinal_position",
	    base_table_name.c_str()));

	vector<string> pk_columns;
	for (idx_t i = 0; i < pk_result->RowCount(); i++) {
		pk_columns.push_back(pk_result->GetValue(0, i).ToString());
	}

	// Build column definitions
	vector<string> column_defs;

	// Add operation metadata columns
	column_defs.push_back("_op VARCHAR NOT NULL");
	column_defs.push_back("_ts TIMESTAMP DEFAULT current_timestamp");
	column_defs.push_back("_version INTEGER DEFAULT 1");

	// Get CHECK constraints from base table
	// DuckDB stores check constraints in duckdb_constraints
	auto check_result = con.Query(StringUtil::Format(
	    "SELECT constraint_text FROM duckdb_constraints() "
	    "WHERE schema_name = 'main' AND table_name = '%s' "
	    "AND constraint_type = 'CHECK'",
	    base_table_name.c_str()));

	vector<string> check_constraints;
	for (idx_t i = 0; i < check_result->RowCount(); i++) {
		string constraint_text = check_result->GetValue(0, i).ToString();
		check_constraints.push_back(constraint_text);
	}

	// Add all columns from base table
	for (idx_t i = 0; i < columns_result->RowCount(); i++) {
		string col_name = columns_result->GetValue(0, i).ToString();
		string data_type = columns_result->GetValue(1, i).ToString();
		string is_nullable = columns_result->GetValue(2, i).ToString();
		auto default_val = columns_result->GetValue(3, i);

		string col_def = col_name + " " + data_type;

		// For PK columns or NOT NULL columns, enforce NOT NULL
		bool is_pk = std::find(pk_columns.begin(), pk_columns.end(), col_name) != pk_columns.end();
		if (is_pk || is_nullable == "NO") {
			col_def += " NOT NULL";
		}

		column_defs.push_back(col_def);
	}

	// Add CHECK constraint for _op
	column_defs.push_back("CHECK (_op IN ('I', 'U', 'D'))");

	// Add CHECK constraints from base table
	for (const auto &check : check_constraints) {
		// constraint_text already includes "CHECK(...)" format
		column_defs.push_back(check);
	}

	// Add primary key constraint if base table has one
	// Delta table PK includes _op to allow multiple versions (Insert, then Update, etc.)
	// Actually, we want only one active state per row, so PK should be just the base PK
	if (!pk_columns.empty()) {
		string pk_def = "PRIMARY KEY (" + StringUtil::Join(pk_columns, ", ") + ")";
		column_defs.push_back(pk_def);
	}

	// Build and execute CREATE TABLE
	string delta_table_name = GetDeltaTableName(base_table_name);
	string create_sql = StringUtil::Format("CREATE TABLE %s.%s (\n    %s\n)", scenario_schema.c_str(),
	                                       delta_table_name.c_str(), StringUtil::Join(column_defs, ",\n    "));

	auto create_result = con.Query(create_sql);
	if (create_result->HasError()) {
		throw InvalidInputException("Failed to create delta table: %s", create_result->GetError().c_str());
	}

	// Create index on PK columns for efficient anti-join during merge-on-read
	// This is a BEST-EFFORT optimization - the merge view works correctly without it.
	// Index creation may fail in some edge cases (e.g., unsupported column types)
	// but this should not prevent delta table creation from succeeding.
	// See docs/spec/error_handling.md for error handling policy.
	if (!pk_columns.empty()) {
		string index_name = "idx_" + delta_table_name + "_pk";
		string index_sql = StringUtil::Format("CREATE INDEX %s ON %s.%s(%s)", index_name.c_str(),
		                                      scenario_schema.c_str(), delta_table_name.c_str(),
		                                      StringUtil::Join(pk_columns, ", ").c_str());
		auto index_result = con.Query(index_sql);
		// Intentionally ignore index_result - see error_handling.md for rationale
		(void)index_result;
	}

	return true;
}

string DeltaStorageEngine::GetMergeViewName(const string &base_table_name) {
	// The merge view uses the same name as the base table (in the scenario schema)
	return base_table_name;
}

bool DeltaStorageEngine::CreateMergeView(ClientContext &context, const string &scenario_schema,
                                         const string &base_table_name, const vector<string> &pk_columns) {
	Connection con(context.db->GetDatabase(context));

	string delta_table = GetDeltaTableName(base_table_name);
	string view_name = GetMergeViewName(base_table_name);

	// Get all column names from base table (for SELECT list)
	auto columns_result = con.Query(StringUtil::Format(
	    "SELECT column_name FROM information_schema.columns "
	    "WHERE table_schema = 'main' AND table_name = '%s' "
	    "ORDER BY ordinal_position",
	    ScenarioManager::EscapeSQLString(base_table_name).c_str()));

	if (columns_result->RowCount() == 0) {
		throw InvalidInputException("Base table '%s' not found in main schema", base_table_name);
	}

	vector<string> columns;
	for (idx_t i = 0; i < columns_result->RowCount(); i++) {
		columns.push_back(columns_result->GetValue(0, i).ToString());
	}

	// Build column list for SELECT (quote each column name)
	vector<string> quoted_columns;
	for (const auto &col : columns) {
		quoted_columns.push_back(ScenarioManager::QuoteIdentifier(col));
	}
	string column_list = StringUtil::Join(quoted_columns, ", ");

	// Build the merge view SQL
	string view_sql;

	if (pk_columns.empty()) {
		// No PK: Use ALL columns as composite key for anti-join
		// This treats every unique combination of column values as a row identifier
		// Note: Duplicate rows in base table will all be affected by a single delta operation
		vector<string> all_join_conditions;
		for (const auto &col : columns) {
			string quoted_col = ScenarioManager::QuoteIdentifier(col);
			// Use IS NOT DISTINCT FROM to handle NULLs correctly
			all_join_conditions.push_back("d." + quoted_col + " IS NOT DISTINCT FROM base." + quoted_col);
		}
		string join_condition = StringUtil::Join(all_join_conditions, " AND ");

		// Build base SELECT with prefixed columns
		vector<string> base_columns;
		for (const auto &col : columns) {
			base_columns.push_back("base." + ScenarioManager::QuoteIdentifier(col));
		}
		string base_column_list = StringUtil::Join(base_columns, ", ");

		view_sql = StringUtil::Format(
		    "CREATE OR REPLACE VIEW %s.%s AS "
		    "SELECT %s FROM main.%s base "
		    "WHERE NOT EXISTS ("
		    "  SELECT 1 FROM %s.%s d "
		    "  WHERE %s AND d._op IN ('U', 'D')"
		    ") "
		    "UNION ALL "
		    "SELECT %s FROM %s.%s WHERE _op IN ('I', 'U')",
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(view_name).c_str(),
		    base_column_list.c_str(),
		    ScenarioManager::QuoteIdentifier(base_table_name).c_str(),
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(delta_table).c_str(),
		    join_condition.c_str(),
		    column_list.c_str(),
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(delta_table).c_str());
	} else {
		// With PK: Use anti-join to exclude rows that are updated/deleted in delta
		vector<string> pk_join_conditions;
		for (const auto &pk : pk_columns) {
			string quoted_pk = ScenarioManager::QuoteIdentifier(pk);
			pk_join_conditions.push_back("d." + quoted_pk + " = base." + quoted_pk);
		}
		string join_condition = StringUtil::Join(pk_join_conditions, " AND ");

		// Build base SELECT with prefixed columns
		vector<string> base_columns;
		for (const auto &col : columns) {
			base_columns.push_back("base." + ScenarioManager::QuoteIdentifier(col));
		}
		string base_column_list = StringUtil::Join(base_columns, ", ");

		view_sql = StringUtil::Format(
		    "CREATE OR REPLACE VIEW %s.%s AS "
		    "SELECT %s FROM main.%s base "
		    "WHERE NOT EXISTS ("
		    "  SELECT 1 FROM %s.%s d "
		    "  WHERE %s AND d._op IN ('U', 'D')"
		    ") "
		    "UNION ALL "
		    "SELECT %s FROM %s.%s WHERE _op IN ('I', 'U')",
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(view_name).c_str(),
		    base_column_list.c_str(),
		    ScenarioManager::QuoteIdentifier(base_table_name).c_str(),
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(delta_table).c_str(),
		    join_condition.c_str(),
		    column_list.c_str(),
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(delta_table).c_str());
	}

	auto create_result = con.Query(view_sql);
	if (create_result->HasError()) {
		throw InvalidInputException("Failed to create merge view: %s", create_result->GetError().c_str());
	}

	return true;
}

bool DeltaStorageEngine::DropMergeView(ClientContext &context, const string &scenario_schema,
                                       const string &table_name) {
	Connection con(context.db->GetDatabase(context));

	string view_name = GetMergeViewName(table_name);
	auto drop_result = con.Query(StringUtil::Format(
	    "DROP VIEW IF EXISTS %s.%s",
	    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
	    ScenarioManager::QuoteIdentifier(view_name).c_str()));

	if (drop_result->HasError()) {
		throw InvalidInputException("Failed to drop merge view: %s", drop_result->GetError().c_str());
	}

	return true;
}

//===--------------------------------------------------------------------===//
// delta_create Scalar Function
//===--------------------------------------------------------------------===//

struct DeltaCreateBindData : public FunctionData {
	string scenario_name;
	string table_name;

	DeltaCreateBindData(string scenario, string table)
	    : scenario_name(std::move(scenario)), table_name(std::move(table)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DeltaCreateBindData>(scenario_name, table_name);
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<DeltaCreateBindData>();
		return scenario_name == o.scenario_name && table_name == o.table_name;
	}
};

static unique_ptr<FunctionData> DeltaCreateBind(ClientContext &context, ScalarFunction &bound_function,
                                                vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() < 2) {
		throw InvalidInputException("delta_create requires scenario_name and table_name parameters");
	}

	// Extract constant arguments
	string scenario_name, table_name;
	if (arguments[0]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
		if (val.IsNull()) {
			throw InvalidInputException("delta_create requires a non-NULL scenario_name");
		}
		scenario_name = val.ToString();
	}
	if (arguments[1]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
		if (val.IsNull()) {
			throw InvalidInputException("delta_create requires a non-NULL table_name");
		}
		table_name = val.ToString();
	}

	// Validate scenario name format
	if (!scenario_name.empty() && !ScenarioManager::ValidateName(scenario_name)) {
		throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.", scenario_name);
	}

	// Validate table name format to prevent SQL injection
	if (!table_name.empty() && !ScenarioManager::ValidateTableName(table_name)) {
		throw InvalidInputException("Invalid table name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.", table_name);
	}

	// Validate scenario exists
	if (!scenario_name.empty() && !ScenarioManager::ScenarioExists(context, scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}

	return make_uniq<DeltaCreateBindData>(scenario_name, table_name);
}

static void DeltaCreateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<DeltaCreateBindData>();
	auto &context = state.GetContext();

	string scenario_name = bind_data.scenario_name;
	string table_name = bind_data.table_name;

	// If not bound at compile time, get from arguments and validate
	if (scenario_name.empty()) {
		scenario_name = args.data[0].GetValue(0).ToString();
		if (!ScenarioManager::ValidateName(scenario_name)) {
			throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
			                            "max 63 characters, and not start with a digit.", scenario_name);
		}
	}
	if (table_name.empty()) {
		table_name = args.data[1].GetValue(0).ToString();
		if (!ScenarioManager::ValidateTableName(table_name)) {
			throw InvalidInputException("Invalid table name '%s'. Names must be alphanumeric with underscores, "
			                            "max 63 characters, and not start with a digit.", table_name);
		}
	}

	// Validate scenario exists
	if (!ScenarioManager::ScenarioExists(context, scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}

	// Get scenario schema name
	string schema_name = ScenarioManager::GetSchemaName(context, scenario_name);

	// Check if delta table already exists
	if (DeltaStorageEngine::DeltaTableExists(context, schema_name, table_name)) {
		throw InvalidInputException("Delta table for '%s' already exists in scenario '%s'", table_name, scenario_name);
	}

	// Create the delta table
	bool success = DeltaStorageEngine::CreateDeltaTable(context, schema_name, table_name);

	// Get PK columns for the merge view
	Connection con(context.db->GetDatabase(context));
	auto pk_result = con.Query(StringUtil::Format(
	    "SELECT kcu.column_name "
	    "FROM information_schema.table_constraints tc "
	    "JOIN information_schema.key_column_usage kcu "
	    "  ON tc.constraint_name = kcu.constraint_name "
	    "WHERE tc.table_schema = 'main' AND tc.table_name = '%s' "
	    "  AND tc.constraint_type = 'PRIMARY KEY' "
	    "ORDER BY kcu.ordinal_position",
	    ScenarioManager::EscapeSQLString(table_name).c_str()));

	vector<string> pk_columns;
	for (idx_t i = 0; i < pk_result->RowCount(); i++) {
		pk_columns.push_back(pk_result->GetValue(0, i).ToString());
	}

	// Register table in _scenario_tables if not already registered
	// This handles tables created after the scenario was created
	auto check_reg = con.Query(StringUtil::Format(
	    "SELECT COUNT(*) FROM _scenario_tables st "
	    "JOIN _scenario_registry sr ON st.scenario_id = sr.scenario_id "
	    "WHERE sr.scenario_name = '%s' AND st.table_name = '%s'",
	    ScenarioManager::EscapeSQLString(scenario_name).c_str(),
	    ScenarioManager::EscapeSQLString(table_name).c_str()));

	if (!check_reg->HasError() && check_reg->GetValue(0, 0).GetValue<int64_t>() == 0) {
		// Get scenario_id
		auto id_result = con.Query(StringUtil::Format(
		    "SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '%s'",
		    ScenarioManager::EscapeSQLString(scenario_name).c_str()));
		if (!id_result->HasError() && id_result->RowCount() > 0) {
			int64_t scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

			// Get row count
			auto count_result = con.Query("SELECT COUNT(*) FROM main." + ScenarioManager::QuoteIdentifier(table_name));
			int64_t row_count = 0;
			if (!count_result->HasError() && count_result->RowCount() > 0) {
				row_count = count_result->GetValue(0, 0).GetValue<int64_t>();
			}

			// Build PK array string
			string pk_array;
			if (pk_columns.empty()) {
				pk_array = "NULL";
			} else {
				pk_array = "[";
				for (idx_t i = 0; i < pk_columns.size(); i++) {
					if (i > 0) pk_array += ", ";
					pk_array += "'" + ScenarioManager::EscapeSQLString(pk_columns[i]) + "'";
				}
				pk_array += "]";
			}

			// Insert into _scenario_tables
			auto insert_sql = StringUtil::Format(
			    "INSERT INTO _scenario_tables (scenario_id, table_name, base_row_count, has_primary_key, primary_key_columns) "
			    "VALUES (%d, '%s', %d, %s, %s)",
			    scenario_id, ScenarioManager::EscapeSQLString(table_name).c_str(), row_count,
			    pk_columns.empty() ? "false" : "true", pk_array.c_str());
			con.Query(insert_sql);
		}
	}

	// Create the merge-on-read view
	DeltaStorageEngine::CreateMergeView(context, schema_name, table_name, pk_columns);

	result.SetValue(0, Value::BOOLEAN(success));
}

//===--------------------------------------------------------------------===//
// delta_drop Scalar Function
//===--------------------------------------------------------------------===//

struct DeltaDropBindData : public FunctionData {
	string scenario_name;
	string table_name;

	DeltaDropBindData(string scenario, string table) : scenario_name(std::move(scenario)), table_name(std::move(table)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DeltaDropBindData>(scenario_name, table_name);
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<DeltaDropBindData>();
		return scenario_name == o.scenario_name && table_name == o.table_name;
	}
};

static unique_ptr<FunctionData> DeltaDropBind(ClientContext &context, ScalarFunction &bound_function,
                                              vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() < 2) {
		throw InvalidInputException("delta_drop requires scenario_name and table_name parameters");
	}

	string scenario_name, table_name;
	if (arguments[0]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
		if (val.IsNull()) {
			throw InvalidInputException("delta_drop requires a non-NULL scenario_name");
		}
		scenario_name = val.ToString();
	}
	if (arguments[1]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
		if (val.IsNull()) {
			throw InvalidInputException("delta_drop requires a non-NULL table_name");
		}
		table_name = val.ToString();
	}

	// Validate scenario name format
	if (!scenario_name.empty() && !ScenarioManager::ValidateName(scenario_name)) {
		throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.", scenario_name);
	}

	// Validate table name format to prevent SQL injection
	if (!table_name.empty() && !ScenarioManager::ValidateTableName(table_name)) {
		throw InvalidInputException("Invalid table name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.", table_name);
	}

	if (!scenario_name.empty() && !ScenarioManager::ScenarioExists(context, scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}

	return make_uniq<DeltaDropBindData>(scenario_name, table_name);
}

static void DeltaDropFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<DeltaDropBindData>();
	auto &context = state.GetContext();

	string scenario_name = bind_data.scenario_name;
	string table_name = bind_data.table_name;

	// If not bound at compile time, get from arguments and validate
	if (scenario_name.empty()) {
		scenario_name = args.data[0].GetValue(0).ToString();
		if (!ScenarioManager::ValidateName(scenario_name)) {
			throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
			                            "max 63 characters, and not start with a digit.", scenario_name);
		}
	}
	if (table_name.empty()) {
		table_name = args.data[1].GetValue(0).ToString();
		if (!ScenarioManager::ValidateTableName(table_name)) {
			throw InvalidInputException("Invalid table name '%s'. Names must be alphanumeric with underscores, "
			                            "max 63 characters, and not start with a digit.", table_name);
		}
	}

	if (!ScenarioManager::ScenarioExists(context, scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}

	string schema_name = ScenarioManager::GetSchemaName(context, scenario_name);

	if (!DeltaStorageEngine::DeltaTableExists(context, schema_name, table_name)) {
		throw InvalidInputException("Delta table for '%s' does not exist in scenario '%s'", table_name, scenario_name);
	}

	// Drop the merge view first (it depends on the delta table)
	DeltaStorageEngine::DropMergeView(context, schema_name, table_name);

	// Drop the delta table
	Connection con(context.db->GetDatabase(context));
	string delta_name = DeltaStorageEngine::GetDeltaTableName(table_name);
	auto drop_result = con.Query(StringUtil::Format(
	    "DROP TABLE %s.%s",
	    ScenarioManager::QuoteIdentifier(schema_name).c_str(),
	    ScenarioManager::QuoteIdentifier(delta_name).c_str()));

	if (drop_result->HasError()) {
		throw InvalidInputException("Failed to drop delta table: %s", drop_result->GetError().c_str());
	}

	result.SetValue(0, Value::BOOLEAN(true));
}

//===--------------------------------------------------------------------===//
// scenario_write: Generic write function for delta tables (Task 2.2 fallback)
//===--------------------------------------------------------------------===//

struct ScenarioWriteBindData : public FunctionData {
	string scenario_name;
	string table_name;
	string operation; // 'I', 'U', or 'D'

	ScenarioWriteBindData(string scenario, string table, string op)
	    : scenario_name(std::move(scenario)), table_name(std::move(table)), operation(std::move(op)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ScenarioWriteBindData>(scenario_name, table_name, operation);
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<ScenarioWriteBindData>();
		return scenario_name == o.scenario_name && table_name == o.table_name && operation == o.operation;
	}
};

static unique_ptr<FunctionData> ScenarioWriteBind(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	if (arguments.size() < 4) {
		throw InvalidInputException("scenario_write requires scenario_name, table_name, operation, and row_data parameters");
	}

	string scenario_name, table_name, operation;

	if (arguments[0]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
		if (val.IsNull()) {
			throw InvalidInputException("scenario_write requires a non-NULL scenario_name");
		}
		scenario_name = val.ToString();
	}

	if (arguments[1]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
		if (val.IsNull()) {
			throw InvalidInputException("scenario_write requires a non-NULL table_name");
		}
		table_name = val.ToString();
	}

	if (arguments[2]->IsFoldable()) {
		auto val = ExpressionExecutor::EvaluateScalar(context, *arguments[2]);
		if (val.IsNull()) {
			throw InvalidInputException("scenario_write requires a non-NULL operation ('I', 'U', or 'D')");
		}
		operation = val.ToString();
		if (operation != "I" && operation != "U" && operation != "D") {
			throw InvalidInputException("scenario_write operation must be 'I' (insert), 'U' (update), or 'D' (delete)");
		}
	}

	// Validate scenario exists
	if (!scenario_name.empty() && !ScenarioManager::ScenarioExists(context, scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}

	return make_uniq<ScenarioWriteBindData>(scenario_name, table_name, operation);
}

static void ScenarioWriteFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioWriteBindData>();
	auto &context = state.GetContext();

	string scenario_name = bind_data.scenario_name;
	string table_name = bind_data.table_name;
	string operation = bind_data.operation;

	// Get runtime values if not bound at compile time
	if (scenario_name.empty()) {
		scenario_name = args.data[0].GetValue(0).ToString();
	}
	if (table_name.empty()) {
		table_name = args.data[1].GetValue(0).ToString();
	}
	if (operation.empty()) {
		operation = args.data[2].GetValue(0).ToString();
	}

	// Get the row data as a struct
	auto row_data = args.data[3].GetValue(0);
	if (row_data.IsNull()) {
		throw InvalidInputException("scenario_write requires non-NULL row_data");
	}

	// Validate scenario and delta table exist
	if (!ScenarioManager::ScenarioExists(context, scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	}

	string schema_name = ScenarioManager::GetSchemaName(context, scenario_name);

	if (!DeltaStorageEngine::DeltaTableExists(context, schema_name, table_name)) {
		throw InvalidInputException("Delta table for '%s' does not exist in scenario '%s'. Call delta_create first.",
		                            table_name, scenario_name);
	}

	// Build INSERT statement for delta table
	Connection con(context.db->GetDatabase(context));

	// Get column names from delta table
	string delta_table = DeltaStorageEngine::GetDeltaTableName(table_name);
	auto cols_result = con.Query(StringUtil::Format(
	    "SELECT column_name FROM information_schema.columns "
	    "WHERE table_schema = '%s' AND table_name = '%s' "
	    "AND column_name NOT IN ('_op', '_ts', '_version') "
	    "ORDER BY ordinal_position",
	    ScenarioManager::EscapeSQLString(schema_name).c_str(),
	    ScenarioManager::EscapeSQLString(delta_table).c_str()));

	if (cols_result->RowCount() == 0) {
		throw InvalidInputException("Could not get column information for delta table");
	}

	// Extract values from the struct based on column names
	vector<string> columns;
	vector<string> values;
	columns.push_back("_op");
	values.push_back("'" + operation + "'");

	auto &struct_children = StructValue::GetChildren(row_data);
	auto &struct_type = row_data.type();
	auto &child_types = StructType::GetChildTypes(struct_type);

	for (idx_t i = 0; i < cols_result->RowCount(); i++) {
		string col_name = cols_result->GetValue(0, i).ToString();
		columns.push_back(ScenarioManager::QuoteIdentifier(col_name));

		// Find the value in the struct
		bool found = false;
		for (idx_t j = 0; j < child_types.size(); j++) {
			if (child_types[j].first == col_name) {
				auto &val = struct_children[j];
				if (val.IsNull()) {
					values.push_back("NULL");
				} else if (val.type().id() == LogicalTypeId::VARCHAR) {
					values.push_back("'" + ScenarioManager::EscapeSQLString(val.ToString()) + "'");
				} else {
					values.push_back(val.ToString());
				}
				found = true;
				break;
			}
		}
		if (!found) {
			values.push_back("NULL");
		}
	}

	// Execute INSERT OR REPLACE to handle updates
	string insert_sql = StringUtil::Format(
	    "INSERT OR REPLACE INTO %s.%s (%s) VALUES (%s)",
	    ScenarioManager::QuoteIdentifier(schema_name).c_str(),
	    ScenarioManager::QuoteIdentifier(delta_table).c_str(),
	    StringUtil::Join(columns, ", ").c_str(),
	    StringUtil::Join(values, ", ").c_str());

	auto insert_result = con.Query(insert_sql);
	if (insert_result->HasError()) {
		throw InvalidInputException("Failed to write to delta table: %s", insert_result->GetError().c_str());
	}

	result.SetValue(0, Value::BOOLEAN(true));
}

//===--------------------------------------------------------------------===//
// scenario_validate Table Function
//===--------------------------------------------------------------------===//

struct ScenarioValidateBindData : public TableFunctionData {
	string scenario_name;
};

struct ScenarioValidateGlobalState : public GlobalTableFunctionState {
	idx_t current_row = 0;
	vector<vector<Value>> rows;
};

static unique_ptr<FunctionData> ScenarioValidateBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ScenarioValidateBindData>();
	bind_data->scenario_name = input.inputs[0].GetValue<string>();

	// Define output columns
	names.push_back("table_name");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("validation_status");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("base_table_exists");
	return_types.push_back(LogicalType::BOOLEAN);

	names.push_back("delta_table_exists");
	return_types.push_back(LogicalType::BOOLEAN);

	names.push_back("captured_row_count");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("current_row_count");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("missing_rowids");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("message");
	return_types.push_back(LogicalType::VARCHAR);

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ScenarioValidateInit(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ScenarioValidateBindData>();
	auto state = make_uniq<ScenarioValidateGlobalState>();

	Connection con(context.db->GetDatabase(context));

	// Check if scenario exists
	auto scenario_result = con.Query(StringUtil::Format(
	    "SELECT scenario_id, schema_name FROM _scenario_registry WHERE scenario_name = '%s'",
	    ScenarioManager::EscapeSQLString(bind_data.scenario_name).c_str()));

	if (scenario_result->RowCount() == 0) {
		// Return single row indicating scenario not found
		state->rows.push_back({
		    Value("(scenario)"),
		    Value("ERROR"),
		    Value::BOOLEAN(false),
		    Value::BOOLEAN(false),
		    Value::BIGINT(0),
		    Value::BIGINT(0),
		    Value::BIGINT(0),
		    Value(StringUtil::Format("Scenario '%s' not found", bind_data.scenario_name))
		});
		return std::move(state);
	}

	int64_t scenario_id = scenario_result->GetValue(0, 0).GetValue<int64_t>();
	string schema_name = scenario_result->GetValue(1, 0).ToString();

	// Get all registered tables for this scenario
	auto tables_result = con.Query(StringUtil::Format(
	    "SELECT table_name, base_row_count FROM _scenario_tables WHERE scenario_id = %d ORDER BY table_name",
	    scenario_id));

	if (tables_result->RowCount() == 0) {
		// Return single row indicating no tables registered
		state->rows.push_back({
		    Value("(no tables)"),
		    Value("WARNING"),
		    Value::BOOLEAN(true),
		    Value::BOOLEAN(false),
		    Value::BIGINT(0),
		    Value::BIGINT(0),
		    Value::BIGINT(0),
		    Value("No tables registered for this scenario")
		});
		return std::move(state);
	}

	// Validate each table
	for (idx_t i = 0; i < tables_result->RowCount(); i++) {
		string table_name = tables_result->GetValue(0, i).ToString();
		int64_t captured_row_count = tables_result->GetValue(1, i).GetValue<int64_t>();

		string status = "OK";
		bool base_exists = false;
		bool delta_exists = false;
		int64_t current_row_count = 0;
		int64_t missing_rowids = 0;
		string message;

		// Check if base table exists
		auto base_check = con.Query(StringUtil::Format(
		    "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' AND table_name = '%s'",
		    ScenarioManager::EscapeSQLString(table_name).c_str()));
		base_exists = base_check->RowCount() > 0;

		// Check if delta table exists
		string delta_table = "_delta_" + table_name;
		auto delta_check = con.Query(StringUtil::Format(
		    "SELECT 1 FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
		    ScenarioManager::EscapeSQLString(schema_name).c_str(),
		    ScenarioManager::EscapeSQLString(delta_table).c_str()));
		delta_exists = delta_check->RowCount() > 0;

		if (!base_exists) {
			status = "ERROR";
			message = "Base table no longer exists";
		} else {
			// Get current row count
			auto count_result = con.Query(StringUtil::Format(
			    "SELECT COUNT(*) FROM main.%s",
			    ScenarioManager::QuoteIdentifier(table_name).c_str()));
			current_row_count = count_result->GetValue(0, 0).GetValue<int64_t>();

			// Check if rowids were captured for this table
			auto rowid_count_result = con.Query(StringUtil::Format(
			    "SELECT COUNT(*) FROM _scenario_base_rowids WHERE scenario_id = %d AND table_name = '%s'",
			    scenario_id, ScenarioManager::EscapeSQLString(table_name).c_str()));
			int64_t captured_rowid_count = rowid_count_result->GetValue(0, 0).GetValue<int64_t>();

			if (captured_rowid_count > 0) {
				// Rowids were captured - perform full validation
				// Check for missing rowids (rowids captured but no longer in base table)
				auto missing_result = con.Query(StringUtil::Format(
				    "SELECT COUNT(*) FROM _scenario_base_rowids r "
				    "WHERE r.scenario_id = %d AND r.table_name = '%s' "
				    "AND NOT EXISTS (SELECT 1 FROM main.%s t WHERE t.rowid = r.base_rowid)",
				    scenario_id, ScenarioManager::EscapeSQLString(table_name).c_str(),
				    ScenarioManager::QuoteIdentifier(table_name).c_str()));
				missing_rowids = missing_result->GetValue(0, 0).GetValue<int64_t>();

				if (missing_rowids > 0) {
					status = "WARNING";
					message = StringUtil::Format("%d captured rowids no longer exist (possible VACUUM or DELETE)", missing_rowids);
				} else if (current_row_count != captured_row_count) {
					status = "INFO";
					message = StringUtil::Format("Row count changed: %d -> %d", captured_row_count, current_row_count);
				} else {
					message = "All validations passed";
				}
			} else if (captured_row_count > 0) {
				// No rowids captured but table had rows - capture was disabled or table added via delta_create
				// Return INFO to indicate limited validation
				status = "INFO";
				message = "Rowid validation skipped (capture_rowids=false at creation)";
			} else {
				// Table was empty at registration, no rowids to capture - this is OK
				if (current_row_count != captured_row_count) {
					status = "INFO";
					message = StringUtil::Format("Row count changed: %d -> %d", captured_row_count, current_row_count);
				} else {
					message = "All validations passed";
				}
			}
		}

		state->rows.push_back({
		    Value(table_name),
		    Value(status),
		    Value::BOOLEAN(base_exists),
		    Value::BOOLEAN(delta_exists),
		    Value::BIGINT(captured_row_count),
		    Value::BIGINT(current_row_count),
		    Value::BIGINT(missing_rowids),
		    Value(message)
		});
	}

	return std::move(state);
}

static void ScenarioValidateScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ScenarioValidateGlobalState>();

	idx_t count = 0;
	while (state.current_row < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.current_row];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		state.current_row++;
		count++;
	}

	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void DeltaStorageEngine::RegisterFunctions(ExtensionLoader &loader) {
	// delta_create(scenario_name, table_name) -> BOOLEAN
	ScalarFunction delta_create("delta_create", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                            DeltaCreateFunction, DeltaCreateBind, nullptr, nullptr, nullptr,
	                            LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                            FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(delta_create);
		FunctionDescription d;
		d.description     = "Initialize a delta table for a given table within a scenario, enabling copy-on-write tracking.";
		d.examples        = {"delta_create('forecast_q1', 'products')"};
		d.categories      = {"delta"};
		d.parameter_names = {"scenario_name", "table_name"};
		d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// delta_drop(scenario_name, table_name) -> BOOLEAN
	ScalarFunction delta_drop("delta_drop", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                          DeltaDropFunction, DeltaDropBind, nullptr, nullptr, nullptr,
	                          LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                          FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(delta_drop);
		FunctionDescription d;
		d.description     = "Remove the delta table for a given table within a scenario, discarding all scenario-specific changes for that table.";
		d.examples        = {"delta_drop('forecast_q1', 'products')"};
		d.categories      = {"delta"};
		d.parameter_names = {"scenario_name", "table_name"};
		d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_write(scenario_name, table_name, operation, row_data) -> BOOLEAN
	// operation: 'I' for insert, 'U' for update, 'D' for delete
	// row_data: struct with column values
	ScalarFunction scenario_write("scenario_write",
	                              {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY},
	                              LogicalType::BOOLEAN, ScenarioWriteFunction, ScenarioWriteBind, nullptr, nullptr, nullptr,
	                              LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                              FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(scenario_write);
		FunctionDescription d;
		d.description     = "Write a single row change to a scenario's delta table. Operation is 'I' (insert), 'U' (update), or 'D' (delete); row_data is a struct of column values.";
		d.examples        = {"scenario_write('forecast_q1', 'products', 'U', {id: 42, price: 19.99})"};
		d.categories      = {"delta"};
		d.parameter_names = {"scenario_name", "table_name", "operation", "row_data"};
		d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_validate(scenario_name) -> TABLE
	// Returns validation status for each table in the scenario
	TableFunction scenario_validate("scenario_validate", {LogicalType::VARCHAR}, ScenarioValidateScan,
	                                 ScenarioValidateBind, ScenarioValidateInit);
	{
		CreateTableFunctionInfo info(scenario_validate);
		FunctionDescription d;
		d.description     = "Validate delta integrity for all tables in a scenario, returning per-table status and any constraint violations.";
		d.examples        = {"SELECT * FROM scenario_validate('forecast_q1')"};
		d.categories      = {"delta"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace duckdb
