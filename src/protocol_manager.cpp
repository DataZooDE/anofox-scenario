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

// ===== Function Registration =====

void ProtocolManager::RegisterFunctions(ExtensionLoader &loader) {
	// protocol_set_why(scenario_name, why_text) - returns boolean
	ScalarFunction protocol_set_why("protocol_set_why", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                 ProtocolSetWhyFunction, ProtocolSetWhyBind, nullptr, nullptr, nullptr,
	                                 LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	loader.RegisterFunction(protocol_set_why);
}

} // namespace duckdb
