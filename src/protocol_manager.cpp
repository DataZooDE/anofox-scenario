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
}

} // namespace duckdb
