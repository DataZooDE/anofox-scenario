#include "snapshot_manager.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
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

	// Get next snapshot_id
	auto id_result = con.Query("SELECT COALESCE(MAX(snapshot_id), 0) + 1 FROM _scenario_snapshots");
	if (id_result->HasError()) {
		throw InvalidInputException("Failed to get next snapshot ID: %s", id_result->GetError());
	}
	int64_t snapshot_id = id_result->GetValue(0, 0).GetValue<int64_t>();

	// Insert into _scenario_snapshots table
	string desc_value = bind_data.description.empty() ? "NULL" : "'" + bind_data.description + "'";
	auto insert_sql = StringUtil::Format(
	    "INSERT INTO _scenario_snapshots (snapshot_id, snapshot_name, source_schema, description) "
	    "VALUES (%d, '%s', '%s', %s)",
	    snapshot_id, bind_data.snapshot_name, source_schema, desc_value);

	auto insert_result = con.Query(insert_sql);
	if (insert_result->HasError()) {
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
}

} // namespace duckdb
