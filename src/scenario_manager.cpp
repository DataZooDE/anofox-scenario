#include "scenario_manager.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
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
	// Read from configuration option (default: "_scen_")
	Value prefix_value;
	auto result = context.TryGetCurrentSetting("scenario_schema_prefix", prefix_value);
	if (!result || prefix_value.IsNull()) {
		return "_scen_";
	}
	return prefix_value.ToString();
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

bool ScenarioManager::ValidateTableName(const string &name) {
	// Same rules as scenario name validation
	return ValidateName(name);
}

string ScenarioManager::EscapeSQLString(const string &str) {
	// Escape single quotes by doubling them (SQL standard)
	string result;
	result.reserve(str.size());
	for (char c : str) {
		if (c == '\'') {
			result += "''";
		} else {
			result += c;
		}
	}
	return result;
}

string ScenarioManager::QuoteIdentifier(const string &identifier) {
	// Quote identifiers with double quotes and escape internal double quotes
	string result = "\"";
	for (char c : identifier) {
		if (c == '"') {
			result += "\"\"";
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

bool ScenarioManager::ScenarioExists(ClientContext &context, const string &name) {
	// Note: name should already be validated via ValidateName() before calling this function,
	// but we escape it anyway for defense in depth
	Connection con(context.db->GetDatabase(context));
	auto result = con.Query("SELECT 1 FROM _scenario_registry WHERE scenario_name = '" + EscapeSQLString(name) + "'");
	return result->RowCount() > 0;
}

// ===== Bind Data Structures =====

struct ScenarioCreateBindData : public FunctionData {
	string scenario_name;
	string description;
	bool capture_rowids = true;  // Default: capture rowids for validation

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ScenarioCreateBindData>();
		result->scenario_name = scenario_name;
		result->description = description;
		result->capture_rowids = capture_rowids;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScenarioCreateBindData>();
		return scenario_name == other.scenario_name && description == other.description &&
		       capture_rowids == other.capture_rowids;
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

struct ScenarioArchiveBindData : public FunctionData {
	string scenario_name;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ScenarioArchiveBindData>();
		result->scenario_name = scenario_name;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScenarioArchiveBindData>();
		return scenario_name == other.scenario_name;
	}
};

struct ScenarioBranchBindData : public FunctionData {
	string source_scenario;
	string new_name;
	string description;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ScenarioBranchBindData>();
		result->source_scenario = source_scenario;
		result->new_name = new_name;
		result->description = description;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ScenarioBranchBindData>();
		return source_scenario == other.source_scenario && new_name == other.new_name && description == other.description;
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

	auto name_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (name_val.IsNull()) {
		throw InvalidInputException("scenario_create requires a non-NULL scenario name");
	}
	bind_data->scenario_name = name_val.GetValue<string>();

	if (arguments.size() > 1 && arguments[1]->IsFoldable()) {
		bind_data->description = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();
	}

	// Optional third argument: capture_rowids (default: true)
	if (arguments.size() > 2 && arguments[2]->IsFoldable()) {
		auto capture_val = ExpressionExecutor::EvaluateScalar(context, *arguments[2]);
		if (!capture_val.IsNull()) {
			bind_data->capture_rowids = capture_val.GetValue<bool>();
		}
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
	string quoted_schema = ScenarioManager::QuoteIdentifier(schema_name);

	// Start transaction for atomic operation
	auto begin_result = con.Query("BEGIN TRANSACTION");
	if (begin_result->HasError()) {
		throw InvalidInputException("Failed to begin transaction: %s", begin_result->GetError());
	}

	try {
		// Create the schema (using quoted identifier for safety)
		auto create_result = con.Query("CREATE SCHEMA " + quoted_schema);
		if (create_result->HasError()) {
			throw InvalidInputException("Failed to create schema '%s': %s", schema_name, create_result->GetError());
		}

		// Get next scenario_id
		auto id_result = con.Query("SELECT COALESCE(MAX(scenario_id), 0) + 1 FROM _scenario_registry");
		if (id_result->HasError()) {
			throw InvalidInputException("Failed to get next scenario ID: %s", id_result->GetError());
		}
		int64_t scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

		// Insert into registry (escape description to prevent SQL injection)
		string desc_value = bind_data.description.empty() ? "NULL" : "'" + ScenarioManager::EscapeSQLString(bind_data.description) + "'";
		auto insert_sql = StringUtil::Format(
		    "INSERT INTO _scenario_registry (scenario_id, scenario_name, schema_name, base_schema, base_captured_at, description) "
		    "VALUES (%d, '%s', '%s', 'main', current_timestamp, %s)",
		    scenario_id, bind_data.scenario_name, schema_name, desc_value);

		auto insert_result = con.Query(insert_sql);
		if (insert_result->HasError()) {
			throw InvalidInputException("Failed to register scenario: %s", insert_result->GetError());
		}

		// PERF-1: Batch PK column queries
		// Instead of N queries for N tables, use a single query to get all PK info
		// This query joins tables with their primary key columns using list aggregation
		auto tables_with_pk_result = con.Query(
		    "SELECT t.table_name, "
		    "       COALESCE(pk.has_pk, false) as has_pk, "
		    "       pk.pk_columns "
		    "FROM information_schema.tables t "
		    "LEFT JOIN ("
		    "    SELECT tc.table_name, "
		    "           true as has_pk, "
		    "           list(kcu.column_name ORDER BY kcu.ordinal_position) as pk_columns "
		    "    FROM information_schema.table_constraints tc "
		    "    JOIN information_schema.key_column_usage kcu "
		    "      ON tc.constraint_name = kcu.constraint_name "
		    "    WHERE tc.table_schema = 'main' AND tc.constraint_type = 'PRIMARY KEY' "
		    "    GROUP BY tc.table_name"
		    ") pk ON t.table_name = pk.table_name "
		    "WHERE t.table_schema = 'main' AND t.table_type = 'BASE TABLE' "
		    "AND t.table_name NOT LIKE '\\_%' ESCAPE '\\'");

		if (!tables_with_pk_result->HasError()) {
			while (true) {
				auto chunk = tables_with_pk_result->Fetch();
				if (!chunk || chunk->size() == 0) {
					break;
				}
				for (idx_t i = 0; i < chunk->size(); i++) {
					string table_name = chunk->GetValue(0, i).GetValue<string>();
					bool has_pk = chunk->GetValue(1, i).GetValue<bool>();

					// Get row count (still per-table, requires scanning each table)
					auto count_result = con.Query("SELECT COUNT(*) FROM main." + ScenarioManager::QuoteIdentifier(table_name));
					int64_t row_count = 0;
					if (!count_result->HasError() && count_result->RowCount() > 0) {
						row_count = count_result->GetValue(0, 0).GetValue<int64_t>();
					}

					// Format primary key columns as array from the batched result
					string pk_array = "NULL";
					if (has_pk && !chunk->GetValue(2, i).IsNull()) {
						// pk_columns is already a list, convert to our array format
						auto pk_list = chunk->GetValue(2, i);
						auto &list_children = ListValue::GetChildren(pk_list);
						if (!list_children.empty()) {
							pk_array = "[";
							for (size_t k = 0; k < list_children.size(); k++) {
								if (k > 0) pk_array += ", ";
								pk_array += "'" + ScenarioManager::EscapeSQLString(list_children[k].GetValue<string>()) + "'";
							}
							pk_array += "]";
						}
					}

					// Insert into _scenario_tables
					auto table_insert_sql = StringUtil::Format(
					    "INSERT INTO _scenario_tables (scenario_id, table_name, base_row_count, has_primary_key, primary_key_columns) "
					    "VALUES (%d, '%s', %d, %s, %s)",
					    scenario_id, ScenarioManager::EscapeSQLString(table_name), row_count, has_pk ? "true" : "false", pk_array);
					auto table_insert_result = con.Query(table_insert_sql);
					if (table_insert_result->HasError()) {
						throw InvalidInputException("Failed to register table '%s': %s", table_name, table_insert_result->GetError());
					}

					// Capture rowids for base rows (if enabled and table has rows)
					if (bind_data.capture_rowids && row_count > 0) {
						auto rowid_insert_sql = StringUtil::Format(
						    "INSERT INTO _scenario_base_rowids (scenario_id, table_name, base_rowid) "
						    "SELECT %d, '%s', rowid FROM main.%s",
						    scenario_id, ScenarioManager::EscapeSQLString(table_name), ScenarioManager::QuoteIdentifier(table_name));
						auto rowid_result = con.Query(rowid_insert_sql);
						if (rowid_result->HasError()) {
							throw InvalidInputException("Failed to capture rowids for '%s': %s", table_name, rowid_result->GetError());
						}
					}
				}
			}
		}

		// Commit transaction
		auto commit_result = con.Query("COMMIT");
		if (commit_result->HasError()) {
			throw InvalidInputException("Failed to commit transaction: %s", commit_result->GetError());
		}

	} catch (...) {
		// Rollback on any error
		con.Query("ROLLBACK");
		throw;
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

	auto name_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (name_val.IsNull()) {
		throw InvalidInputException("scenario_drop requires a non-NULL scenario name");
	}
	bind_data->scenario_name = name_val.GetValue<string>();

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
	string escaped_name = ScenarioManager::EscapeSQLString(bind_data.scenario_name);

	// Check if other scenarios depend on this one (before starting transaction)
	auto dep_result = con.Query(
	    "SELECT scenario_name FROM _scenario_registry WHERE parent_scenario_id = "
	    "(SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '" + escaped_name + "')");
	if (dep_result->RowCount() > 0) {
		string dependent = dep_result->GetValue(0, 0).GetValue<string>();
		throw InvalidInputException("Cannot drop scenario '%s': scenario '%s' depends on it",
		                            bind_data.scenario_name, dependent);
	}

	// Get the scenario_id before starting transaction
	auto id_result = con.Query("SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '" + escaped_name + "'");
	if (id_result->HasError() || id_result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' not found", bind_data.scenario_name);
	}
	int64_t scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

	// Start transaction for atomic operation
	auto begin_result = con.Query("BEGIN TRANSACTION");
	if (begin_result->HasError()) {
		throw InvalidInputException("Failed to begin transaction: %s", begin_result->GetError());
	}

	try {
		// Delete from child tables first
		auto del_tables = con.Query(StringUtil::Format("DELETE FROM _scenario_tables WHERE scenario_id = %d", scenario_id));
		if (del_tables->HasError()) {
			throw InvalidInputException("Failed to delete table registrations: %s", del_tables->GetError());
		}

		auto del_rowids = con.Query(StringUtil::Format("DELETE FROM _scenario_base_rowids WHERE scenario_id = %d", scenario_id));
		if (del_rowids->HasError()) {
			throw InvalidInputException("Failed to delete rowid records: %s", del_rowids->GetError());
		}

		auto del_protocols = con.Query("DELETE FROM _scenario_protocols WHERE entity_type = 'scenario' AND entity_name = '" + escaped_name + "'");
		if (del_protocols->HasError()) {
			throw InvalidInputException("Failed to delete protocols: %s", del_protocols->GetError());
		}

		// Delete from registry
		auto delete_result = con.Query(StringUtil::Format("DELETE FROM _scenario_registry WHERE scenario_id = %d", scenario_id));
		if (delete_result->HasError()) {
			throw InvalidInputException("Failed to delete scenario from registry: %s", delete_result->GetError());
		}

		// Drop the schema (CASCADE to remove all objects, using quoted identifier)
		auto drop_result = con.Query("DROP SCHEMA IF EXISTS " + ScenarioManager::QuoteIdentifier(schema_name) + " CASCADE");
		if (drop_result->HasError()) {
			throw InvalidInputException("Failed to drop schema '%s': %s", schema_name, drop_result->GetError());
		}

		// Commit transaction
		auto commit_result = con.Query("COMMIT");
		if (commit_result->HasError()) {
			throw InvalidInputException("Failed to commit transaction: %s", commit_result->GetError());
		}

	} catch (...) {
		// Rollback on any error
		con.Query("ROLLBACK");
		throw;
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== scenario_archive Implementation =====

static unique_ptr<FunctionData> ScenarioArchiveBind(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ScenarioArchiveBindData>();

	if (arguments.size() < 1 || arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_archive requires a scenario name");
	}

	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("scenario_archive name must be a constant");
	}

	auto name_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (name_val.IsNull()) {
		throw InvalidInputException("scenario_archive requires a non-NULL scenario name");
	}
	bind_data->scenario_name = name_val.GetValue<string>();

	// Check if scenario exists
	if (!ScenarioManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ScenarioArchiveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioArchiveBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	// Update status to 'archived' (name already validated)
	string escaped_name = ScenarioManager::EscapeSQLString(bind_data.scenario_name);
	auto update_result = con.Query("UPDATE _scenario_registry SET status = 'archived' WHERE scenario_name = '" +
	                               escaped_name + "'");
	if (update_result->HasError()) {
		throw InvalidInputException("Failed to archive scenario '%s': %s", bind_data.scenario_name,
		                            update_result->GetError());
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== scenario_unarchive Implementation =====

static unique_ptr<FunctionData> ScenarioUnarchiveBind(ClientContext &context, ScalarFunction &bound_function,
                                                       vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ScenarioArchiveBindData>();

	if (arguments.size() < 1 || arguments[0]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_unarchive requires a scenario name");
	}

	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("scenario_unarchive name must be a constant");
	}

	auto name_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
	if (name_val.IsNull()) {
		throw InvalidInputException("scenario_unarchive requires a non-NULL scenario name");
	}
	bind_data->scenario_name = name_val.GetValue<string>();

	// Check if scenario exists
	if (!ScenarioManager::ScenarioExists(context, bind_data->scenario_name)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->scenario_name);
	}

	return std::move(bind_data);
}

static void ScenarioUnarchiveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioArchiveBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	// Update status to 'active' (name already validated)
	string escaped_name = ScenarioManager::EscapeSQLString(bind_data.scenario_name);
	auto update_result = con.Query("UPDATE _scenario_registry SET status = 'active' WHERE scenario_name = '" +
	                               escaped_name + "'");
	if (update_result->HasError()) {
		throw InvalidInputException("Failed to unarchive scenario '%s': %s", bind_data.scenario_name,
		                            update_result->GetError());
	}

	// Return true for all rows
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, false);
	*ConstantVector::GetData<bool>(result) = true;
}

// ===== scenario_branch Implementation =====

static unique_ptr<FunctionData> ScenarioBranchBind(ClientContext &context, ScalarFunction &bound_function,
                                                    vector<unique_ptr<Expression>> &arguments) {
	auto bind_data = make_uniq<ScenarioBranchBindData>();

	// Extract source_scenario (required)
	if (arguments.size() < 2 || arguments[0]->return_type != LogicalType::VARCHAR ||
	    arguments[1]->return_type != LogicalType::VARCHAR) {
		throw InvalidInputException("scenario_branch requires source_scenario and new_name as arguments");
	}

	if (!arguments[0]->IsFoldable()) {
		throw InvalidInputException("scenario_branch source_scenario must be a constant");
	}
	bind_data->source_scenario = ExpressionExecutor::EvaluateScalar(context, *arguments[0]).GetValue<string>();

	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("scenario_branch new_name must be a constant");
	}
	bind_data->new_name = ExpressionExecutor::EvaluateScalar(context, *arguments[1]).GetValue<string>();

	// Extract optional description
	if (arguments.size() > 2 && arguments[2]->IsFoldable()) {
		bind_data->description = ExpressionExecutor::EvaluateScalar(context, *arguments[2]).GetValue<string>();
	}

	// Validate source scenario exists
	if (!ScenarioManager::ScenarioExists(context, bind_data->source_scenario)) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data->source_scenario);
	}

	// Validate new name
	if (!ScenarioManager::ValidateName(bind_data->new_name)) {
		throw InvalidInputException("Invalid scenario name '%s'. Names must be alphanumeric with underscores, "
		                            "max 63 characters, and not start with a digit.",
		                            bind_data->new_name);
	}

	// Check if new scenario already exists
	if (ScenarioManager::ScenarioExists(context, bind_data->new_name)) {
		throw InvalidInputException("Scenario '%s' already exists", bind_data->new_name);
	}

	return std::move(bind_data);
}

static void ScenarioBranchFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.bind_info->Cast<ScenarioBranchBindData>();
	auto &context = state.GetContext();

	// Use a new connection to avoid re-entrancy
	Connection con(context.db->GetDatabase(context));

	string schema_name = ScenarioManager::GetSchemaName(context, bind_data.new_name);
	string quoted_schema = ScenarioManager::QuoteIdentifier(schema_name);
	string escaped_source = ScenarioManager::EscapeSQLString(bind_data.source_scenario);

	// Get the source scenario_id before starting transaction
	auto source_id_result = con.Query("SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '" +
	                                   escaped_source + "'");
	if (source_id_result->HasError() || source_id_result->RowCount() == 0) {
		throw InvalidInputException("Source scenario '%s' not found", bind_data.source_scenario);
	}
	int64_t source_scenario_id = source_id_result->GetValue(0, 0).GetValue<int64_t>();

	// Start transaction for atomic operation
	auto begin_result = con.Query("BEGIN TRANSACTION");
	if (begin_result->HasError()) {
		throw InvalidInputException("Failed to begin transaction: %s", begin_result->GetError());
	}

	try {
		// Create the schema for the new scenario
		auto create_result = con.Query("CREATE SCHEMA " + quoted_schema);
		if (create_result->HasError()) {
			throw InvalidInputException("Failed to create schema '%s': %s", schema_name, create_result->GetError());
		}

		// Get next scenario_id for the new scenario
		auto id_result = con.Query("SELECT COALESCE(MAX(scenario_id), 0) + 1 FROM _scenario_registry");
		if (id_result->HasError()) {
			throw InvalidInputException("Failed to get next scenario ID: %s", id_result->GetError());
		}
		int64_t new_scenario_id = id_result->GetValue(0, 0).GetValue<int64_t>();

		// Insert into registry with parent_scenario_id pointing to source (escape description)
		string desc_value = bind_data.description.empty() ? "NULL" : "'" + ScenarioManager::EscapeSQLString(bind_data.description) + "'";
		auto insert_sql = StringUtil::Format(
		    "INSERT INTO _scenario_registry (scenario_id, scenario_name, schema_name, base_schema, base_captured_at, description, parent_scenario_id) "
		    "VALUES (%d, '%s', '%s', 'main', current_timestamp, %s, %d)",
		    new_scenario_id, bind_data.new_name, schema_name, desc_value, source_scenario_id);

		auto insert_result = con.Query(insert_sql);
		if (insert_result->HasError()) {
			throw InvalidInputException("Failed to register branched scenario: %s", insert_result->GetError());
		}

		// Copy table registrations from source scenario to new scenario
		auto copy_tables_sql = StringUtil::Format(
		    "INSERT INTO _scenario_tables (scenario_id, table_name, base_row_count, has_primary_key, primary_key_columns) "
		    "SELECT %d, table_name, base_row_count, has_primary_key, primary_key_columns "
		    "FROM _scenario_tables WHERE scenario_id = %d",
		    new_scenario_id, source_scenario_id);
		auto copy_tables_result = con.Query(copy_tables_sql);
		if (copy_tables_result->HasError()) {
			throw InvalidInputException("Failed to copy table registrations: %s", copy_tables_result->GetError());
		}

		// Copy base rowids from source scenario to new scenario
		auto copy_rowids_sql = StringUtil::Format(
		    "INSERT INTO _scenario_base_rowids (scenario_id, table_name, base_rowid) "
		    "SELECT %d, table_name, base_rowid "
		    "FROM _scenario_base_rowids WHERE scenario_id = %d",
		    new_scenario_id, source_scenario_id);
		auto copy_rowids_result = con.Query(copy_rowids_sql);
		if (copy_rowids_result->HasError()) {
			throw InvalidInputException("Failed to copy base rowids: %s", copy_rowids_result->GetError());
		}

		// Commit transaction
		auto commit_result = con.Query("COMMIT");
		if (commit_result->HasError()) {
			throw InvalidInputException("Failed to commit transaction: %s", commit_result->GetError());
		}

	} catch (...) {
		// Rollback on any error
		con.Query("ROLLBACK");
		throw;
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
	vector<Value> schema_names;
	vector<Value> statuses;
	vector<Value> descriptions;
	vector<Value> created_ats;
	vector<Value> base_schemas;
	vector<Value> parent_names;
	idx_t offset;
	bool finished;
};

// ===== scenario_stats Implementation =====

struct ScenarioStatsBindData : public TableFunctionData {
	string scenario_name;
};

struct ScenarioStatsData : public GlobalTableFunctionState {
	ScenarioStatsData() : done(false) {
	}

	int64_t table_count;
	int64_t total_base_rows;
	int64_t delta_row_count;
	Value created_at;
	string status;
	bool done;
};

static unique_ptr<FunctionData> ScenarioListBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("scenario_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_name");
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
	    "SELECT r.scenario_name, r.schema_name, r.status, r.description, r.created_at, r.base_schema, p.scenario_name as parent_name "
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
			result->schema_names.push_back(chunk->GetValue(1, i));
			result->statuses.push_back(chunk->GetValue(2, i));
			result->descriptions.push_back(chunk->GetValue(3, i));
			result->created_ats.push_back(chunk->GetValue(4, i));
			result->base_schemas.push_back(chunk->GetValue(5, i));
			result->parent_names.push_back(chunk->GetValue(6, i));
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
		output.SetValue(1, count, data.schema_names[data.offset]);
		output.SetValue(2, count, data.statuses[data.offset]);
		output.SetValue(3, count, data.descriptions[data.offset]);
		output.SetValue(4, count, data.created_ats[data.offset]);
		output.SetValue(5, count, data.base_schemas[data.offset]);
		output.SetValue(6, count, data.parent_names[data.offset]);
		data.offset++;
		count++;
	}
	output.SetCardinality(count);
}

// ===== scenario_stats Implementation (continued) =====

static unique_ptr<FunctionData> ScenarioStatsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ScenarioStatsBindData>();

	// Get scenario name from input
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw InvalidInputException("scenario_stats requires a scenario name");
	}
	bind_data->scenario_name = input.inputs[0].GetValue<string>();

	// Define return columns
	names.emplace_back("table_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("total_base_rows");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("delta_row_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("created_at");
	return_types.emplace_back(LogicalType::TIMESTAMP);

	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ScenarioStatsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<ScenarioStatsData>();
	auto &bind_data = input.bind_data->Cast<ScenarioStatsBindData>();

	// Check if scenario exists and get registry info
	Connection con(context.db->GetDatabase(context));
	string escaped_name = ScenarioManager::EscapeSQLString(bind_data.scenario_name);
	auto registry_result = con.Query(
	    "SELECT created_at, status FROM _scenario_registry WHERE scenario_name = '" + escaped_name + "'");

	if (registry_result->HasError() || registry_result->RowCount() == 0) {
		throw InvalidInputException("Scenario '%s' does not exist", bind_data.scenario_name);
	}

	result->created_at = registry_result->GetValue(0, 0);
	result->status = registry_result->GetValue(1, 0).GetValue<string>();

	// Get table statistics
	auto stats_result = con.Query(
	    "SELECT COUNT(*), COALESCE(SUM(base_row_count), 0) FROM _scenario_tables "
	    "WHERE scenario_id = (SELECT scenario_id FROM _scenario_registry WHERE scenario_name = '" +
	    escaped_name + "')");

	if (!stats_result->HasError() && stats_result->RowCount() > 0) {
		result->table_count = stats_result->GetValue(0, 0).GetValue<int64_t>();
		result->total_base_rows = stats_result->GetValue(1, 0).GetValue<int64_t>();
	} else {
		result->table_count = 0;
		result->total_base_rows = 0;
	}

	// Count delta rows by summing row counts from all _delta_* tables in the scenario schema
	string schema_name = ScenarioManager::GetSchemaName(context, bind_data.scenario_name);
	auto delta_tables_result = con.Query(
	    "SELECT table_name FROM information_schema.tables "
	    "WHERE table_schema = '" + ScenarioManager::EscapeSQLString(schema_name) + "' "
	    "AND table_name LIKE '\\_delta\\_%' ESCAPE '\\'");

	result->delta_row_count = 0;
	if (!delta_tables_result->HasError()) {
		while (true) {
			auto chunk = delta_tables_result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t i = 0; i < chunk->size(); i++) {
				string delta_table = chunk->GetValue(0, i).GetValue<string>();
				auto count_result = con.Query(
				    "SELECT COUNT(*) FROM " + ScenarioManager::QuoteIdentifier(schema_name) + "." +
				    ScenarioManager::QuoteIdentifier(delta_table));
				if (!count_result->HasError() && count_result->RowCount() > 0) {
					result->delta_row_count += count_result->GetValue(0, 0).GetValue<int64_t>();
				}
			}
		}
	}

	return std::move(result);
}

static void ScenarioStatsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ScenarioStatsData>();

	if (data.done) {
		return;
	}

	output.SetValue(0, 0, Value::BIGINT(data.table_count));
	output.SetValue(1, 0, Value::BIGINT(data.total_base_rows));
	output.SetValue(2, 0, Value::BIGINT(data.delta_row_count));
	output.SetValue(3, 0, data.created_at);
	output.SetValue(4, 0, Value(data.status));

	output.SetCardinality(1);
	data.done = true;
}

// ===== Function Registration =====

void ScenarioManager::RegisterFunctions(ExtensionLoader &loader) {
	// scenario_create(name, description, capture_rowids) - returns boolean
	ScalarFunctionSet scenario_create_set("scenario_create");

	// 3-argument version: name, description, capture_rowids (boolean)
	ScalarFunction scenario_create_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN}, LogicalType::BOOLEAN,
	                                  ScenarioCreateFunction, ScenarioCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                  FunctionNullHandling::SPECIAL_HANDLING);
	scenario_create_set.AddFunction(scenario_create_3);

	// 2-argument version: name, description
	ScalarFunction scenario_create_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ScenarioCreateFunction, ScenarioCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                  FunctionNullHandling::SPECIAL_HANDLING);
	scenario_create_set.AddFunction(scenario_create_2);

	// 1-argument version: name only
	ScalarFunction scenario_create_1({LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ScenarioCreateFunction, ScenarioCreateBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                  FunctionNullHandling::SPECIAL_HANDLING);
	scenario_create_set.AddFunction(scenario_create_1);

	{
		CreateScalarFunctionInfo info(scenario_create_set);
		{
			FunctionDescription d;
			d.description    = "Create a new scenario branching from the current database state, with optional description and row-id capture.";
			d.examples       = {"scenario_create('forecast_q1', 'Q1 forecast what-if', true)"};
			d.categories     = {"scenario"};
			d.parameter_names = {"scenario_name", "description", "capture_rowids"};
			d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN};
			info.descriptions.push_back(std::move(d));
		}
		{
			FunctionDescription d;
			d.description    = "Create a new scenario branching from the current database state with a description.";
			d.examples       = {"scenario_create('forecast_q1', 'Q1 forecast what-if')"};
			d.categories     = {"scenario"};
			d.parameter_names = {"scenario_name", "description"};
			d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
			info.descriptions.push_back(std::move(d));
		}
		{
			FunctionDescription d;
			d.description    = "Create a new scenario branching from the current database state.";
			d.examples       = {"scenario_create('forecast_q1')"};
			d.categories     = {"scenario"};
			d.parameter_names = {"scenario_name"};
			d.parameter_types = {LogicalType::VARCHAR};
			info.descriptions.push_back(std::move(d));
		}
		loader.RegisterFunction(std::move(info));
	}

	// scenario_drop(name) - returns boolean
	ScalarFunction scenario_drop("scenario_drop", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                              ScenarioDropFunction, ScenarioDropBind, nullptr, nullptr, nullptr,
	                              LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                              FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(scenario_drop);
		FunctionDescription d;
		d.description    = "Drop a scenario and remove all its delta tables.";
		d.examples       = {"scenario_drop('forecast_q1')"};
		d.categories     = {"scenario"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_archive(name) - returns boolean
	ScalarFunction scenario_archive("scenario_archive", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                 ScenarioArchiveFunction, ScenarioArchiveBind, nullptr, nullptr, nullptr,
	                                 LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                 FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(scenario_archive);
		FunctionDescription d;
		d.description    = "Archive a scenario, preserving its delta data but removing it from the active list.";
		d.examples       = {"scenario_archive('forecast_q1')"};
		d.categories     = {"scenario"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_unarchive(name) - returns boolean
	ScalarFunction scenario_unarchive("scenario_unarchive", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                   ScenarioUnarchiveFunction, ScenarioUnarchiveBind, nullptr, nullptr, nullptr,
	                                   LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                   FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(scenario_unarchive);
		FunctionDescription d;
		d.description    = "Restore an archived scenario back to active status.";
		d.examples       = {"scenario_unarchive('forecast_q1')"};
		d.categories     = {"scenario"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_branch(source_scenario, new_name, description?) - returns boolean
	ScalarFunctionSet scenario_branch_set("scenario_branch");

	ScalarFunction scenario_branch_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ScenarioBranchFunction, ScenarioBranchBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                  FunctionNullHandling::SPECIAL_HANDLING);
	scenario_branch_set.AddFunction(scenario_branch_3);

	ScalarFunction scenario_branch_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                  ScenarioBranchFunction, ScenarioBranchBind, nullptr, nullptr, nullptr,
	                                  LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	                                  FunctionNullHandling::SPECIAL_HANDLING);
	scenario_branch_set.AddFunction(scenario_branch_2);

	{
		CreateScalarFunctionInfo info(scenario_branch_set);
		{
			FunctionDescription d;
			d.description    = "Create a new scenario as a branch of an existing scenario, with a description.";
			d.examples       = {"scenario_branch('forecast_q1', 'forecast_q1_v2', 'revised assumptions')"};
			d.categories     = {"scenario"};
			d.parameter_names = {"source_scenario", "new_name", "description"};
			d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
			info.descriptions.push_back(std::move(d));
		}
		{
			FunctionDescription d;
			d.description    = "Create a new scenario as a branch of an existing scenario.";
			d.examples       = {"scenario_branch('forecast_q1', 'forecast_q1_v2')"};
			d.categories     = {"scenario"};
			d.parameter_names = {"source_scenario", "new_name"};
			d.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
			info.descriptions.push_back(std::move(d));
		}
		loader.RegisterFunction(std::move(info));
	}

	// scenario_list() - returns table
	TableFunction scenario_list("scenario_list", {}, ScenarioListFunction, ScenarioListBind, ScenarioListInit);
	{
		CreateTableFunctionInfo info(scenario_list);
		FunctionDescription d;
		d.description = "List all scenarios with their name, status, description, parent, and creation time.";
		d.examples    = {"SELECT * FROM scenario_list()"};
		d.categories  = {"scenario"};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_stats(name) - returns table with statistics
	TableFunction scenario_stats("scenario_stats", {LogicalType::VARCHAR}, ScenarioStatsFunction, ScenarioStatsBind, ScenarioStatsInit);
	{
		CreateTableFunctionInfo info(scenario_stats);
		FunctionDescription d;
		d.description     = "Return per-table delta statistics for a scenario: insert, update, and delete counts.";
		d.examples        = {"SELECT * FROM scenario_stats('forecast_q1')"};
		d.categories      = {"scenario"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}

	// scenario_schema(name) - returns the schema name for a scenario (for use with SET search_path)
	ScalarFunction scenario_schema("scenario_schema", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
	        auto &context = state.GetContext();
	        auto scenario_name = args.data[0].GetValue(0).ToString();

	        // Validate scenario exists
	        if (!ScenarioManager::ScenarioExists(context, scenario_name)) {
	            throw InvalidInputException("Scenario '%s' does not exist", scenario_name);
	        }

	        string schema_name = ScenarioManager::GetSchemaName(context, scenario_name);
	        result.SetValue(0, Value(schema_name));
	    },
	    nullptr, nullptr, nullptr, nullptr, LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE,
	    FunctionNullHandling::SPECIAL_HANDLING);
	{
		CreateScalarFunctionInfo info(scenario_schema);
		FunctionDescription d;
		d.description    = "Return the internal DuckDB schema name for a scenario, for use with SET search_path.";
		d.examples       = {"scenario_schema('forecast_q1')"};
		d.categories     = {"scenario"};
		d.parameter_names = {"scenario_name"};
		d.parameter_types = {LogicalType::VARCHAR};
		info.descriptions.push_back(std::move(d));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace duckdb
