#include "delta_storage_engine.hpp"
#include "scenario_manager.hpp"
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

	// Add all columns from base table
	for (idx_t i = 0; i < columns_result->RowCount(); i++) {
		string col_name = columns_result->GetValue(0, i).ToString();
		string data_type = columns_result->GetValue(1, i).ToString();
		string is_nullable = columns_result->GetValue(2, i).ToString();
		auto default_val = columns_result->GetValue(3, i);

		string col_def = col_name + " " + data_type;

		// For PK columns, enforce NOT NULL
		bool is_pk = std::find(pk_columns.begin(), pk_columns.end(), col_name) != pk_columns.end();
		if (is_pk || is_nullable == "NO") {
			col_def += " NOT NULL";
		}

		column_defs.push_back(col_def);
	}

	// Add CHECK constraint for _op
	column_defs.push_back("CHECK (_op IN ('I', 'U', 'D'))");

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
	if (!pk_columns.empty()) {
		string index_name = "idx_" + delta_table_name + "_pk";
		string index_sql = StringUtil::Format("CREATE INDEX %s ON %s.%s(%s)", index_name.c_str(),
		                                      scenario_schema.c_str(), delta_table_name.c_str(),
		                                      StringUtil::Join(pk_columns, ", ").c_str());
		con.Query(index_sql); // Best effort, ignore errors
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
		// No PK: Use rowid-based anti-join (fallback, less efficient)
		// This case is complex - for now, just union all base with delta for tables without PK
		view_sql = StringUtil::Format(
		    "CREATE OR REPLACE VIEW %s.%s AS "
		    "SELECT %s FROM main.%s "
		    "UNION ALL "
		    "SELECT %s FROM %s.%s WHERE _op IN ('I', 'U')",
		    ScenarioManager::QuoteIdentifier(scenario_schema).c_str(),
		    ScenarioManager::QuoteIdentifier(view_name).c_str(),
		    column_list.c_str(),
		    ScenarioManager::QuoteIdentifier(base_table_name).c_str(),
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
// Registration
//===--------------------------------------------------------------------===//

void DeltaStorageEngine::RegisterFunctions(ExtensionLoader &loader) {
	// delta_create(scenario_name, table_name) -> BOOLEAN
	ScalarFunction delta_create("delta_create", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                            DeltaCreateFunction, DeltaCreateBind, nullptr, nullptr, nullptr,
	                            LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                            FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(delta_create);

	// delta_drop(scenario_name, table_name) -> BOOLEAN
	ScalarFunction delta_drop("delta_drop", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                          DeltaDropFunction, DeltaDropBind, nullptr, nullptr, nullptr,
	                          LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                          FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(delta_drop);
}

} // namespace duckdb
