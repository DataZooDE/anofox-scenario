#include "protocol_manager.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

// ===== Helper Functions =====

bool ProtocolManager::ScenarioExists(ClientContext &context, const string &name) {
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query("SELECT 1 FROM _scenario_registry WHERE scenario_name = '" + name + "'");
	return result->RowCount() > 0;
}

// ===== Bind Data Structures =====

struct ProtocolSetWhyBindData : public FunctionData {
	string scenario_name;
	string why_text;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ProtocolSetWhyBindData>();
		result->scenario_name = scenario_name;
		result->why_text = why_text;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ProtocolSetWhyBindData>();
		return scenario_name == other.scenario_name && why_text == other.why_text;
	}
};

// ===== protocol_set_why Implementation =====

static unique_ptr<FunctionData> ProtocolSetWhyBind(ClientContext &context, ScalarFunction &bound_function,
                                                    vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ProtocolSetWhyBindData>();

	// Validate first argument: scenario_name
	if (arguments.size() < 2) {
		throw InvalidInputException("protocol_set_why requires two arguments: scenario_name and why_text");
	}

	if (arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("protocol_set_why: scenario_name must be a VARCHAR");
	}

	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("protocol_set_why: scenario_name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	// Validate second argument: why_text
	if (arguments[1]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("protocol_set_why: why_text must be a VARCHAR");
	}

	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("protocol_set_why: why_text must be a constant");
	}
	bind_data->why_text = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	// Validate scenario exists
	if (!ProtocolManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ProtocolSetWhyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ProtocolSetWhyBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	// Escape single quotes in the why_text by doubling them
	string escaped_why = bind_data.why_text;
	size_t pos = 0;
	while ((pos = escaped_why.find('\'', pos)) != string::npos) {
		escaped_why.replace(pos, 1, "''");
		pos += 2;
	}

	// Use INSERT OR REPLACE to handle both insert and update cases
	// DuckDB supports INSERT OR REPLACE for primary key conflicts
	auto upsert_sql = StringUtil::Format(
	    "INSERT OR REPLACE INTO _scenario_protocols (entity_type, entity_name, section, content, updated_at) "
	    "VALUES ('scenario', '%s', 'why', '%s', current_timestamp)",
	    bind_data.scenario_name, escaped_why);

	auto upsert_result = con.Query(upsert_sql);
	if (upsert_result->HasError()) {
		throw InvalidInputException("Failed to set protocol 'why' for scenario '%s': %s",
		                            bind_data.scenario_name, upsert_result->GetError());
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== protocol_log_change Implementation =====

struct ProtocolLogChangeBindData : public FunctionData {
	string scenario_name;
	string change_text;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ProtocolLogChangeBindData>();
		result->scenario_name = scenario_name;
		result->change_text = change_text;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ProtocolLogChangeBindData>();
		return scenario_name == other.scenario_name && change_text == other.change_text;
	}
};

static unique_ptr<FunctionData> ProtocolLogChangeBind(ClientContext &context, ScalarFunction &bound_function,
                                                       vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ProtocolLogChangeBindData>();

	if (arguments.size() < 2) {
		throw InvalidInputException("protocol_log_change requires two arguments: scenario_name and change_text");
	}

	if (arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("protocol_log_change: scenario_name must be a VARCHAR");
	}
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("protocol_log_change: scenario_name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (arguments[1]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("protocol_log_change: change_text must be a VARCHAR");
	}
	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("protocol_log_change: change_text must be a constant");
	}
	bind_data->change_text = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	// Validate scenario exists
	if (!ProtocolManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ProtocolLogChangeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ProtocolLogChangeBindData>();
	auto &context = state.GetContext();

	Connection con(context.db->GetDatabase(context));

	// Escape single quotes in the change_text
	string escaped_change = bind_data.change_text;
	size_t pos = 0;
	while ((pos = escaped_change.find('\'', pos)) != string::npos) {
		escaped_change.replace(pos, 1, "''");
		pos += 2;
	}

	// Check if 'changes' section already exists
	auto check_result = con.Query(StringUtil::Format(
	    "SELECT content FROM _scenario_protocols WHERE entity_type = 'scenario' AND entity_name = '%s' AND section = 'changes'",
	    bind_data.scenario_name));

	string new_content;
	if (!check_result->HasError() && check_result->RowCount() > 0 && !check_result->GetValue(0, 0).IsNull()) {
		// Append to existing content with newline separator
		string existing = check_result->GetValue(0, 0).GetValue<string>();
		// Escape existing content for SQL
		pos = 0;
		while ((pos = existing.find('\'', pos)) != string::npos) {
			existing.replace(pos, 1, "''");
			pos += 2;
		}
		new_content = existing + "\n" + escaped_change;
	} else {
		// First entry
		new_content = escaped_change;
	}

	// Upsert the changes section
	auto upsert_sql = StringUtil::Format(
	    "INSERT OR REPLACE INTO _scenario_protocols (entity_type, entity_name, section, content, updated_at) "
	    "VALUES ('scenario', '%s', 'changes', '%s', current_timestamp)",
	    bind_data.scenario_name, new_content);

	auto upsert_result = con.Query(upsert_sql);
	if (upsert_result->HasError()) {
		throw InvalidInputException("Failed to log change for scenario '%s': %s",
		                            bind_data.scenario_name, upsert_result->GetError());
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== protocol_add_finding Implementation =====

struct ProtocolAddFindingBindData : public FunctionData {
	string scenario_name;
	string finding_text;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ProtocolAddFindingBindData>();
		result->scenario_name = scenario_name;
		result->finding_text = finding_text;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ProtocolAddFindingBindData>();
		return scenario_name == other.scenario_name && finding_text == other.finding_text;
	}
};

static unique_ptr<FunctionData> ProtocolAddFindingBind(ClientContext &context, ScalarFunction &bound_function,
                                                        vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ProtocolAddFindingBindData>();

	if (arguments.size() < 2) {
		throw InvalidInputException("protocol_add_finding requires two arguments: scenario_name and finding_text");
	}

	if (arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("protocol_add_finding: scenario_name must be a VARCHAR");
	}
	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("protocol_add_finding: scenario_name must be a constant");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (arguments[1]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("protocol_add_finding: finding_text must be a VARCHAR");
	}
	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("protocol_add_finding: finding_text must be a constant");
	}
	bind_data->finding_text = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	// Validate scenario exists
	if (!ProtocolManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ProtocolAddFindingFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ProtocolAddFindingBindData>();
	auto &context = state.GetContext();

	Connection con(context.db->GetDatabase(context));

	// Escape single quotes in the finding_text
	string escaped_finding = bind_data.finding_text;
	size_t pos = 0;
	while ((pos = escaped_finding.find('\'', pos)) != string::npos) {
		escaped_finding.replace(pos, 1, "''");
		pos += 2;
	}

	// Check if 'findings' section already exists
	auto check_result = con.Query(StringUtil::Format(
	    "SELECT content FROM _scenario_protocols WHERE entity_type = 'scenario' AND entity_name = '%s' AND section = 'findings'",
	    bind_data.scenario_name));

	string new_content;
	if (!check_result->HasError() && check_result->RowCount() > 0 && !check_result->GetValue(0, 0).IsNull()) {
		// Append to existing content with newline separator
		string existing = check_result->GetValue(0, 0).GetValue<string>();
		pos = 0;
		while ((pos = existing.find('\'', pos)) != string::npos) {
			existing.replace(pos, 1, "''");
			pos += 2;
		}
		new_content = existing + "\n" + escaped_finding;
	} else {
		// First entry
		new_content = escaped_finding;
	}

	// Upsert the findings section
	auto upsert_sql = StringUtil::Format(
	    "INSERT OR REPLACE INTO _scenario_protocols (entity_type, entity_name, section, content, updated_at) "
	    "VALUES ('scenario', '%s', 'findings', '%s', current_timestamp)",
	    bind_data.scenario_name, new_content);

	auto upsert_result = con.Query(upsert_sql);
	if (upsert_result->HasError()) {
		throw InvalidInputException("Failed to add finding for scenario '%s': %s",
		                            bind_data.scenario_name, upsert_result->GetError());
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== protocol_set_plan Implementation =====

struct ProtocolSetPlanBindData : public FunctionData {
	string scenario_name;
	string plan_text;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ProtocolSetPlanBindData>();
		result->scenario_name = scenario_name;
		result->plan_text = plan_text;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ProtocolSetPlanBindData>();
		return scenario_name == other.scenario_name && plan_text == other.plan_text;
	}
};

static unique_ptr<FunctionData> ProtocolSetPlanBind(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ProtocolSetPlanBindData>();

	if (arguments.size() < 2) {
		throw InvalidInputException("protocol_set_plan requires two arguments: scenario_name and plan_text");
	}

	if (arguments[0]->return_type != LogicalType::VARCHAR || !arguments[0]->IsFoldable()) {
		throw InvalidInputException("protocol_set_plan: scenario_name must be a constant VARCHAR");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (arguments[1]->return_type != LogicalType::VARCHAR || !arguments[1]->IsFoldable()) {
		throw InvalidInputException("protocol_set_plan: plan_text must be a constant VARCHAR");
	}
	bind_data->plan_text = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	if (!ProtocolManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ProtocolSetPlanFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ProtocolSetPlanBindData>();
	auto &context = state.GetContext();

	Connection con(context.db->GetDatabase(context));

	string escaped = bind_data.plan_text;
	size_t pos = 0;
	while ((pos = escaped.find('\'', pos)) != string::npos) {
		escaped.replace(pos, 1, "''");
		pos += 2;
	}

	auto upsert_sql = StringUtil::Format(
	    "INSERT OR REPLACE INTO _scenario_protocols (entity_type, entity_name, section, content, updated_at) "
	    "VALUES ('scenario', '%s', 'plan', '%s', current_timestamp)",
	    bind_data.scenario_name, escaped);

	auto upsert_result = con.Query(upsert_sql);
	if (upsert_result->HasError()) {
		throw InvalidInputException("Failed to set protocol 'plan' for scenario '%s': %s",
		                            bind_data.scenario_name, upsert_result->GetError());
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== protocol_set_decision Implementation =====

struct ProtocolSetDecisionBindData : public FunctionData {
	string scenario_name;
	string decision_text;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ProtocolSetDecisionBindData>();
		result->scenario_name = scenario_name;
		result->decision_text = decision_text;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ProtocolSetDecisionBindData>();
		return scenario_name == other.scenario_name && decision_text == other.decision_text;
	}
};

static unique_ptr<FunctionData> ProtocolSetDecisionBind(ClientContext &context, ScalarFunction &bound_function,
                                                         vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ProtocolSetDecisionBindData>();

	if (arguments.size() < 2) {
		throw InvalidInputException("protocol_set_decision requires two arguments: scenario_name and decision_text");
	}

	if (arguments[0]->return_type != LogicalType::VARCHAR || !arguments[0]->IsFoldable()) {
		throw InvalidInputException("protocol_set_decision: scenario_name must be a constant VARCHAR");
	}
	bind_data->scenario_name = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (arguments[1]->return_type != LogicalType::VARCHAR || !arguments[1]->IsFoldable()) {
		throw InvalidInputException("protocol_set_decision: decision_text must be a constant VARCHAR");
	}
	bind_data->decision_text = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	if (!ProtocolManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ProtocolSetDecisionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ProtocolSetDecisionBindData>();
	auto &context = state.GetContext();

	Connection con(context.db->GetDatabase(context));

	string escaped = bind_data.decision_text;
	size_t pos = 0;
	while ((pos = escaped.find('\'', pos)) != string::npos) {
		escaped.replace(pos, 1, "''");
		pos += 2;
	}

	auto upsert_sql = StringUtil::Format(
	    "INSERT OR REPLACE INTO _scenario_protocols (entity_type, entity_name, section, content, updated_at) "
	    "VALUES ('scenario', '%s', 'decision', '%s', current_timestamp)",
	    bind_data.scenario_name, escaped);

	auto upsert_result = con.Query(upsert_sql);
	if (upsert_result->HasError()) {
		throw InvalidInputException("Failed to set protocol 'decision' for scenario '%s': %s",
		                            bind_data.scenario_name, upsert_result->GetError());
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== protocol_read Implementation (Table Function) =====

struct ProtocolReadBindData : public TableFunctionData {
	string scenario_name;
};

struct ProtocolReadGlobalState : public GlobalTableFunctionState {
	idx_t current_row = 0;
	vector<vector<Value>> rows;
};

static unique_ptr<FunctionData> ProtocolReadBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ProtocolReadBindData>();

	if (input.inputs.empty()) {
		throw InvalidInputException("protocol_read requires scenario_name argument");
	}

	bind_data->scenario_name = input.inputs[0].GetValue<string>();

	// Validate scenario exists
	if (!ProtocolManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	// Define output columns
	names.emplace_back("section");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("content");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("updated_at");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ProtocolReadInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<ProtocolReadGlobalState>();
	auto &bind_data = input.bind_data->Cast<ProtocolReadBindData>();

	// Query all protocol sections for this scenario
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query(StringUtil::Format(
	    "SELECT section, content, updated_at FROM _scenario_protocols "
	    "WHERE entity_type = 'scenario' AND entity_name = '%s' ORDER BY section",
	    bind_data.scenario_name));

	if (!result->HasError()) {
		for (idx_t i = 0; i < result->RowCount(); i++) {
			vector<Value> row;
			row.push_back(result->GetValue(0, i)); // section
			row.push_back(result->GetValue(1, i)); // content
			row.push_back(result->GetValue(2, i)); // updated_at
			state->rows.push_back(std::move(row));
		}
	}

	return std::move(state);
}

static void ProtocolReadFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ProtocolReadGlobalState>();

	idx_t count = 0;
	while (state.current_row < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.current_row];
		output.SetValue(0, count, row[0]); // section
		output.SetValue(1, count, row[1]); // content
		output.SetValue(2, count, row[2]); // updated_at
		state.current_row++;
		count++;
	}

	output.SetCardinality(count);
}

// ===== Function Registration =====

void ProtocolManager::RegisterFunctions(ExtensionLoader &loader) {
	// protocol_set_why(scenario_name, why_text) - returns boolean
	ScalarFunction protocol_set_why("protocol_set_why", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                 ProtocolSetWhyFunction, ProtocolSetWhyBind, nullptr, nullptr, nullptr,
	                                 LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(protocol_set_why);

	// protocol_log_change(scenario_name, change_text) - returns boolean
	ScalarFunction protocol_log_change("protocol_log_change", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                    ProtocolLogChangeFunction, ProtocolLogChangeBind, nullptr, nullptr, nullptr,
	                                    LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(protocol_log_change);

	// protocol_add_finding(scenario_name, finding_text) - returns boolean
	ScalarFunction protocol_add_finding("protocol_add_finding", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                     ProtocolAddFindingFunction, ProtocolAddFindingBind, nullptr, nullptr, nullptr,
	                                     LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(protocol_add_finding);

	// protocol_set_plan(scenario_name, plan_text) - returns boolean
	ScalarFunction protocol_set_plan("protocol_set_plan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ProtocolSetPlanFunction, ProtocolSetPlanBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(protocol_set_plan);

	// protocol_set_decision(scenario_name, decision_text) - returns boolean
	ScalarFunction protocol_set_decision("protocol_set_decision", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                      ProtocolSetDecisionFunction, ProtocolSetDecisionBind, nullptr, nullptr, nullptr,
	                                      LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(protocol_set_decision);

	// protocol_read(scenario_name) - table function returning all protocol sections
	TableFunction protocol_read("protocol_read", {LogicalType::VARCHAR}, ProtocolReadFunction, ProtocolReadBind, ProtocolReadInit);
	loader.RegisterFunction(protocol_read);
}

} // namespace duckdb
