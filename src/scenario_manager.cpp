#include "scenario_manager.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

// ===== Helper Functions =====

string ScenarioManager::GetSchemaPrefix(ClientContext &context) {
	// TODO: Read from configuration when Task 1.9 is implemented
	return "_scen_";
}

string ScenarioManager::GetSchemaName(ClientContext &context, const string &scenario_name) {
	return GetSchemaPrefix(context) + scenario_name;
}

bool ScenarioManager::ValidateName(const string &name) {
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

bool ScenarioManager::ScenarioExists(ClientContext &context, const string &name) {
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query("SELECT 1 FROM _scenario_registry WHERE scenario_name = '" + name + "'");
	return result->RowCount() > 0;
}

// ===== Bind Data Structures =====

struct ScenarioCreateBindData : public FunctionData {
	string scenario_name;
	string description;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ScenarioCreateBindData>();
		result->scenario_name = scenario_name;
		result->description = description;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScenarioCreateBindData>();
		return scenario_name == other.scenario_name && description == other.description;
	}
};

struct ScenarioDropBindData : public FunctionData {
	string scenario_name;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ScenarioDropBindData>();
		result->scenario_name = scenario_name;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScenarioDropBindData>();
		return scenario_name == other.scenario_name;
	}
};

// ===== scenario_create Implementation =====

static unique_ptr<FunctionData> ScenarioCreateBind(ClientContext &context, ScalarFunction &bound_function,
                                                    vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ScenarioCreateBindData>();

	// Extract constant arguments
	if (arguments.size() < 1 || arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_create requires a scenario name as first argument");
	}

	// We can only bind constant expressions here
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("scenario_create name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (arguments.size() > 1 && arguments[1]->IsFoldable()) {
		bind_data->description = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();
	}

	// Validate name
	if (!ScenarioManager::ValidateName(bind_data->scenario_name)) {
		throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.",
		                            bind_data->scenario_name);
	}

	// Check if scenario already exists
	if (ScenarioManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' already exists", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ScenarioCreateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioCreateBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	string schema_name = ScenarioManager::GetSchemaName(context, bind_data.scenario_name);

	// Create the schema
	auto create_result = con.Query("CREATE SCHEMA \"" + schema_name + "\"");
	if (create_result->HasError()) {
		throw InvalidInputException("Failed to create schema '%s': %s", schema_name, create_result->GetError());
	}

	// Get next scenario_id
	auto id_result = con.Query("SELECT COALESCE(MAX(scenario_id), 0) + 1 FROM _scenario_registry");
	if (id_result->HasError()) {
		con.Query("DROP SCHEMA \"" + schema_name + "\"");
		throw InvalidInputException("Failed to get next scenario ID: %s", id_result->GetError());
	}
	int64_t scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

	// Insert into registry
	string desc_value = bind_data.description.empty() ? "NULL" : "'" + bind_data.description + "'";
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO _scenario_registry (scenario_id, scenario_name, schema_name, base_schema, base_captured_at, description) "
	    "VALUES (%d, '%s', '%s', 'main', current_timestamp, %s)",
	    scenario_id, bind_data.scenario_name, schema_name, desc_value);

	auto insert_result = con.Query(insert_sql);
	if (insert_result->HasError()) {
		con.Query("DROP SCHEMA \"" + schema_name + "\"");
		throw InvalidInputException("Failed to register scenario: %s", insert_result->GetError());
	}

	// Register all user tables from the base schema
	auto tables_result = con.Query(
	    "SELECT table_name FROM information_schema.tables "
	    "WHERE table_schema = 'main' AND table_type = 'BASE TABLE' "
	    "AND table_name NOT LIKE '\\_%' ESCAPE '\\'");

	if (!tables_result->HasError()) {
		while (true) {
			auto chunk = tables_result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t i = 0; i < chunk->size(); i++) {
				string table_name = chunk->GetValue(0, i).GetValue<string>();

				// Get row count
				auto count_result = con.Query("SELECT COUNT(*) FROM main.\"" + table_name + "\"");
				int64_t row_count = 0;
				if (!count_result->HasError() && count_result->RowCount() > 0) {
					row_count = count_result->GetValue(0, 0).GetValue<int64_t>();
				}

				// Get primary key columns
				auto pk_result = con.Query(
				    "SELECT column_name FROM information_schema.table_constraints tc "
				    "JOIN information_schema.key_column_usage kcu "
				    "ON tc.constraint_name = kcu.constraint_name "
				    "WHERE tc.table_schema = 'main' AND tc.table_name = '" + table_name + "' "
				    "AND tc.constraint_type = 'PRIMARY KEY' "
				    "ORDER BY kcu.ordinal_position");

				bool has_pk = false;
				vector<string> pk_columns;
				if (!pk_result->HasError()) {
					while (true) {
						auto pk_chunk = pk_result->Fetch();
						if (!pk_chunk || pk_chunk->size() == 0) {
							break;
						}
						has_pk = true;
						for (idx_t j = 0; j < pk_chunk->size(); j++) {
							pk_columns.push_back(pk_chunk->GetValue(0, j).GetValue<string>());
						}
					}
				}

				// Format primary key columns as array
				string pk_array = "NULL";
				if (!pk_columns.empty()) {
					pk_array = "[";
					for (size_t k = 0; k < pk_columns.size(); k++) {
						if (k > 0) pk_array += ", ";
						pk_array += "'" + pk_columns[k] + "'";
					}
					pk_array += "]";
				}

				// Insert into _scenario_tables
				auto table_insert_sql = StringUtil::Format(
				    "INSERT INTO _scenario_tables (scenario_id, table_name, base_row_count, has_primary_key, primary_key_columns) "
				    "VALUES (%d, '%s', %d, %s, %s)",
				    scenario_id, table_name, row_count, has_pk ? "true" : "false", pk_array);
				con.Query(table_insert_sql);

				// Capture rowids for base rows (if table has rows)
				if (row_count > 0) {
					auto rowid_insert_sql = StringUtil::Format(
					    "INSERT INTO _scenario_base_rowids (scenario_id, table_name, base_rowid) "
					    "SELECT %d, '%s', rowid FROM main.\"%s\"",
					    scenario_id, table_name, table_name);
					con.Query(rowid_insert_sql);
				}
			}
		}
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== scenario_drop Implementation =====

static unique_ptr<FunctionData> ScenarioDropBind(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ScenarioDropBindData>();

	if (arguments.size() < 1 || arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_drop requires a scenario name");
	}

	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("scenario_drop name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	// Check if scenario exists
	if (!ScenarioManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ScenarioDropFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioDropBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	string schema_name = ScenarioManager::GetSchemaName(context, bind_data.scenario_name);

	// Check if other scenarios depend on this one
	auto dep_result = con.Query(
	    "SELECT scenario_name FROM _scenario_registry WHERE parent_scenario_id = "
	    "(SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '" + bind_data.scenario_name + "')");
	if (dep_result->RowCount() > 0) {
		string dependent = dep_result->GetValue(0, 0).GetValue<string>();
		throw InvalidInputException("Cannot drop scenario '%s': scenario '%s' depends on it",
		                            bind_data.scenario_name, dependent);
	}

	// First get the scenario_id to use in all queries
	auto id_result = con.Query("SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '" + bind_data.scenario_name + "'");
	if (id_result->HasError() || id_result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.scenario_name);
	}
	int64_t scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

	// Delete from child tables first
	con.Query(StringUtil::Format("DELETE FROM _scenario_tables WHERE scenario_id = %d", scenario_id));
	con.Query(StringUtil::Format("DELETE FROM _scenario_base_rowids WHERE scenario_id = %d", scenario_id));
	con.Query("DELETE FROM _scenario_protocols WHERE entity_type = 'scenario' AND entity_name = '" + bind_data.scenario_name + "'");

	// Delete from registry
	auto delete_result = con.Query(StringUtil::Format("DELETE FROM _scenario_registry WHERE scenario_id = %d", scenario_id));
	if (delete_result->HasError()) {
		throw InvalidInputException("Failed to delete scenario from registry: %s", delete_result->GetError());
	}

	// Drop the schema (CASCADE to remove all objects)
	auto drop_result = con.Query("DROP SCHEMA IF EXISTS \"" + schema_name + "\" CASCADE");
	if (drop_result->HasError()) {
		throw InvalidInputException("Failed to drop schema '%s': %s", schema_name, drop_result->GetError());
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== scenario_list Implementation =====

struct ScenarioListData : public GlobalTableFunctionState {
	ScenarioListData() : offset(0), finished(false) {
	}

	vector<Value> scenario_names;
	vector<Value> statuses;
	vector<Value> descriptions;
	vector<Value> created_ats;
	vector<Value> base_schemas;
	vector<Value> parent_names;
	idx_t offset;
	bool finished;
};

static unique_ptr<FunctionData> ScenarioListBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("scenario_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	names.emplace_back("base_schema");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("parent_scenario");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> ScenarioListInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<ScenarioListData>();

	// Query the registry
	Connection con(context.db->GetDatabase(context));
	auto query_result = con.Query(
	    "SELECT r.scenario_name, r.status, r.description, r.created_at, r.base_schema, p.scenario_name as parent_name "
	    "FROM _scenario_registry r "
	    "LEFT JOIN _scenario_registry p ON r.parent_scenario_id = p.scenario_id "
	    "ORDER BY r.scenario_name");

	if (query_result->HasError()) {
		return std::move(result);
	}

	// Collect all results
	while (true) {
		auto chunk = query_result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t i = 0; i < chunk->size(); i++) {
			result->scenario_names.push_back(chunk->GetValue(0, i));
			result->statuses.push_back(chunk->GetValue(1, i));
			result->descriptions.push_back(chunk->GetValue(2, i));
			result->created_ats.push_back(chunk->GetValue(3, i));
			result->base_schemas.push_back(chunk->GetValue(4, i));
			result->parent_names.push_back(chunk->GetValue(5, i));
		}
	}

	return std::move(result);
}

static void ScenarioListFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ScenarioListData>();
	if (data.offset >= data.scenario_names.size()) {
		return;
	}

	idx_t count = 0;
	while (data.offset < data.scenario_names.size() && count < STANDARD_VECTOR_SIZE) {
		output.SetValue(0, count, data.scenario_names[data.offset]);
		output.SetValue(1, count, data.statuses[data.offset]);
		output.SetValue(2, count, data.descriptions[data.offset]);
		output.SetValue(3, count, data.created_ats[data.offset]);
		output.SetValue(4, count, data.base_schemas[data.offset]);
		output.SetValue(5, count, data.parent_names[data.offset]);
		data.offset++;
		count++;
	}
	output.SetCardinality(count);
}

// ===== Function Registration =====

void ScenarioManager::RegisterFunctions(ExtensionLoader &loader) {
	// scenario_create(name, description) - returns boolean
	ScalarFunctionSet scenario_create_set("scenario_create");

	// Constructor: (arguments, return_type, function, bind, bind_extended, statistics, init_local_state, varargs, stability, null_handling, bind_lambda)
	ScalarFunction scenario_create_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ScenarioCreateFunction, ScenarioCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	scenario_create_set.AddFunction(scenario_create_2);

	ScalarFunction scenario_create_1({LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ScenarioCreateFunction, ScenarioCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	scenario_create_set.AddFunction(scenario_create_1);

	loader.RegisterFunction(scenario_create_set);

	// scenario_drop(name) - returns boolean
	ScalarFunction scenario_drop("scenario_drop", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                              ScenarioDropFunction, ScenarioDropBind, nullptr, nullptr, nullptr,
	                              LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(scenario_drop);

	// scenario_list() - returns table
	TableFunction scenario_list("scenario_list", {}, ScenarioListFunction, ScenarioListBind, ScenarioListInit);
	loader.RegisterFunction(scenario_list);
}

} // namespace duckdb
