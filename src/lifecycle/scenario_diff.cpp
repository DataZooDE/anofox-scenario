//===----------------------------------------------------------------------===//
// Phase 3: streaming diff engine. scenario_diff / scenario_diff_summary are
// explicit table functions using bind_replace: the generated diff SQL is
// handed back to the binder as a subquery, so results stream through the
// engine with native PK types (no row materialization).
//
// Compare-to-origin needs no anti-joins: the delta *is* the diff
// (added = _op 'I', removed = 'D', modified = 'U' joined to base for old
// values, one UNION ALL arm per non-PK column).
//===----------------------------------------------------------------------===//

#include "lifecycle/scenario_diff.hpp"

#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

namespace {

string Q(const string &identifier) {
	return KeywordHelper::WriteOptionallyQuoted(identifier);
}

Catalog &GetHostCatalogForDiff(ClientContext &context) {
	return Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
}

//! Fully-qualified physical names for the diff SQL
struct DiffTarget {
	string delta_name; // host.__anofox_scenario.s<id>_delta_<t>
	string base_name;  // host.main.<t> or host.__anofox_scenario.s<mat>_mat_<t>
	vector<string> pk_columns;
	vector<string> payload_columns;
};

DiffTarget ResolveDiffTarget(ClientContext &context, const string &scenario_name, const string &table_name) {
	auto &host_catalog = GetHostCatalogForDiff(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, scenario_name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", scenario_name);
	}
	auto delta = ScenarioDelta::TryGetDeltaTable(context, host_catalog, entry->scenario_id, table_name);
	if (!delta) {
		throw InvalidInputException("Table '%s' not found in scenario '%s'", table_name, scenario_name);
	}

	// Base side: nearest materialized ancestor or the live host table
	int64_t mat_base_id = -1;
	auto current = make_uniq<ScenarioRegistryEntry>(*entry);
	while (current) {
		if (current->mode == "materialized") {
			mat_base_id = current->scenario_id;
			break;
		}
		if (current->parent_id < 0) {
			break;
		}
		current = ScenarioRegistry::LookupById(context, host_catalog, current->parent_id);
	}

	DiffTarget target;
	auto host_prefix = Q(host_catalog.GetName()) + ".";
	target.delta_name = host_prefix + Q(ScenarioRegistry::SCHEMA_NAME) + "." +
	                    Q(ScenarioDelta::DeltaTableName(entry->scenario_id, table_name));

	optional_ptr<TableCatalogEntry> base_entry;
	if (mat_base_id >= 0) {
		auto mat = ScenarioDelta::TryGetMatTable(context, host_catalog, mat_base_id, table_name);
		if (!mat) {
			throw InvalidInputException("Table '%s' not found in scenario '%s'", table_name, scenario_name);
		}
		base_entry = mat.get();
		target.base_name = host_prefix + Q(ScenarioRegistry::SCHEMA_NAME) + "." + Q(mat->name);
	} else {
		// Phase 4: the base may live in another attached catalog
		optional_ptr<Catalog> base_catalog = &host_catalog;
		if (!entry->base_catalog.empty()) {
			base_catalog = Catalog::GetCatalogEntry(context, entry->base_catalog);
			if (!base_catalog) {
				throw InvalidInputException(
				    "Scenario '%s': base catalog '%s' is not attached - ATTACH it first", scenario_name,
				    entry->base_catalog);
			}
		}
		base_entry = base_catalog->GetEntry<TableCatalogEntry>(context, DEFAULT_SCHEMA, table_name,
		                                                       OnEntryNotFound::RETURN_NULL);
		if (!base_entry) {
			throw InvalidInputException("Table '%s' not found in the base schema", table_name);
		}
		target.base_name =
		    Q(base_catalog->GetName()) + "." + Q(string(DEFAULT_SCHEMA)) + "." + Q(table_name);
	}

	auto pk_columns = ScenarioDelta::GetPKColumns(*base_entry);
	if (pk_columns.empty()) {
		throw NotImplementedException(
		    "scenario_diff requires a PRIMARY KEY on the base table (v1 limitation)");
	}
	unordered_set<idx_t> pk_set(pk_columns.begin(), pk_columns.end());
	for (auto &col : base_entry->GetColumns().Logical()) {
		if (pk_set.find(col.Logical().index) != pk_set.end()) {
			target.pk_columns.push_back(col.Name());
		} else {
			target.payload_columns.push_back(col.Name());
		}
	}
	return target;
}

//! SQL of the merged relation of one diff side: 'main' = the live base;
//! otherwise the scenario's merged state (base source + its delta overlay)
string MergedRelationSQL(ClientContext &context, const string &side, const string &table_name,
                         const DiffTarget &shape) {
	auto &host_catalog = GetHostCatalogForDiff(context);
	auto host_prefix = Q(host_catalog.GetName()) + ".";
	if (side == "main") {
		return "(SELECT * FROM " + host_prefix + Q(string(DEFAULT_SCHEMA)) + "." + Q(table_name) + ")";
	}
	auto target = ResolveDiffTarget(context, side, table_name);
	string pk_match;
	for (auto &pk : shape.pk_columns) {
		if (!pk_match.empty()) {
			pk_match += " AND ";
		}
		pk_match += "d." + Q(pk) + " = b." + Q(pk);
	}
	string cols_d, cols_b;
	for (auto &pk : shape.pk_columns) {
		cols_d += (cols_d.empty() ? "" : ", ") + string("d.") + Q(pk);
		cols_b += (cols_b.empty() ? "" : ", ") + string("b.") + Q(pk);
	}
	for (auto &col : shape.payload_columns) {
		cols_d += ", d." + Q(col);
		cols_b += ", b." + Q(col);
	}
	return "(SELECT " + cols_d + " FROM " + target.delta_name + " d WHERE d._op IN ('I', 'U') UNION ALL SELECT " +
	       cols_b + " FROM " + target.base_name + " b WHERE NOT EXISTS (SELECT 1 FROM " + target.delta_name +
	       " d WHERE " + pk_match + " AND d._op IN ('U', 'D')))";
}

unique_ptr<TableRef> SQLToSubquery(ClientContext &context, const string &sql) {
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("anofox_scenario: generated diff SQL did not parse to a single SELECT");
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

//===----------------------------------------------------------------------===//
// scenario_diff(scenario, table)
//===----------------------------------------------------------------------===//

//! Generic two-relation diff: FULL-OUTER-JOIN shape over the merged
//! relations of both sides ('added' = only in b, 'removed' = only in a,
//! 'modified' = both sides differ; old = a, new = b)
unique_ptr<TableRef> ScenarioDiffGenericBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto side_a = input.inputs[0].GetValue<string>();
	auto side_b = input.inputs[1].GetValue<string>();
	auto table_name = input.inputs[2].GetValue<string>();

	// Column shape from whichever side is a scenario (identical table schema)
	auto shape = ResolveDiffTarget(context, side_a == "main" ? side_b : side_a, table_name);
	auto rel_a = MergedRelationSQL(context, side_a, table_name, shape);
	auto rel_b = MergedRelationSQL(context, side_b, table_name, shape);

	string pk_join, pk_list_a, pk_list_b, a_pk_null, b_pk_null;
	for (auto &pk : shape.pk_columns) {
		if (!pk_join.empty()) {
			pk_join += " AND ";
			a_pk_null += " AND ";
			b_pk_null += " AND ";
		}
		pk_join += "a." + Q(pk) + " = b." + Q(pk);
		a_pk_null += "a." + Q(pk) + " IS NULL";
		b_pk_null += "b." + Q(pk) + " IS NULL";
		pk_list_a += (pk_list_a.empty() ? "" : ", ") + string("a.") + Q(pk) + " AS " + Q(pk);
		pk_list_b += (pk_list_b.empty() ? "" : ", ") + string("b.") + Q(pk) + " AS " + Q(pk);
	}

	string sql;
	sql += "SELECT " + pk_list_b +
	       ", CAST('added' AS VARCHAR) AS change_type, CAST(NULL AS VARCHAR) AS column_name, CAST(NULL AS "
	       "VARCHAR) AS old_value, CAST(NULL AS VARCHAR) AS new_value FROM " +
	       rel_b + " b LEFT JOIN " + rel_a + " a ON (" + pk_join + ") WHERE " + a_pk_null;
	sql += " UNION ALL SELECT " + pk_list_a +
	       ", 'removed', CAST(NULL AS VARCHAR), CAST(NULL AS VARCHAR), CAST(NULL AS VARCHAR) FROM " + rel_a +
	       " a LEFT JOIN " + rel_b + " b ON (" + pk_join + ") WHERE " + b_pk_null;
	for (auto &col : shape.payload_columns) {
		sql += " UNION ALL SELECT " + pk_list_a + ", 'modified', CAST('" + col + "' AS VARCHAR), CAST(a." + Q(col) +
		       " AS VARCHAR), CAST(b." + Q(col) + " AS VARCHAR) FROM " + rel_a + " a JOIN " + rel_b + " b ON (" +
		       pk_join + ") WHERE (a." + Q(col) + " IS DISTINCT FROM b." + Q(col) + ")";
	}
	return SQLToSubquery(context, sql);
}

unique_ptr<TableRef> ScenarioDiffBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() == 3) {
		return ScenarioDiffGenericBindReplace(context, input);
	}
	auto scenario_name = input.inputs[0].GetValue<string>();
	auto table_name = input.inputs[1].GetValue<string>();
	auto target = ResolveDiffTarget(context, scenario_name, table_name);

	string pk_list_d;
	string join_cond;
	for (auto &pk : target.pk_columns) {
		if (!pk_list_d.empty()) {
			pk_list_d += ", ";
			join_cond += " AND ";
		}
		pk_list_d += "d." + Q(pk) + " AS " + Q(pk);
		join_cond += "d." + Q(pk) + " = b." + Q(pk);
	}

	string sql;
	sql += "SELECT " + pk_list_d +
	       ", CAST('added' AS VARCHAR) AS change_type, CAST(NULL AS VARCHAR) AS column_name, "
	       "CAST(NULL AS VARCHAR) AS old_value, CAST(NULL AS VARCHAR) AS new_value FROM " +
	       target.delta_name + " d WHERE d._op = 'I'";
	sql += " UNION ALL SELECT " + pk_list_d +
	       ", 'removed', CAST(NULL AS VARCHAR), CAST(NULL AS VARCHAR), CAST(NULL AS VARCHAR) FROM " +
	       target.delta_name + " d WHERE d._op = 'D'";
	for (auto &col : target.payload_columns) {
		sql += " UNION ALL SELECT " + pk_list_d + ", 'modified', CAST('" + col +
		       "' AS VARCHAR), CAST(b." + Q(col) + " AS VARCHAR), CAST(d." + Q(col) + " AS VARCHAR) FROM " +
		       target.delta_name + " d JOIN " + target.base_name + " b ON (" + join_cond + ") WHERE d._op = 'U' AND (d." +
		       Q(col) + " IS DISTINCT FROM b." + Q(col) + ")";
	}
	return SQLToSubquery(context, sql);
}

//===----------------------------------------------------------------------===//
// scenario_diff_summary(scenario)
//===----------------------------------------------------------------------===//

unique_ptr<TableRef> ScenarioDiffSummaryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto scenario_name = input.inputs[0].GetValue<string>();
	auto &host_catalog = GetHostCatalogForDiff(context);
	auto entry = ScenarioRegistry::Lookup(context, host_catalog, scenario_name);
	if (!entry) {
		throw InvalidInputException("Scenario '%s' not found", scenario_name);
	}

	// Enumerate the scenario's delta tables
	string prefix = "s" + to_string(entry->scenario_id) + "_delta_";
	vector<string> tables;
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (schema) {
		schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
			if (table_entry.type == CatalogType::TABLE_ENTRY && StringUtil::StartsWith(table_entry.name, prefix)) {
				tables.push_back(table_entry.name);
			}
		});
	}

	string sql;
	auto host_prefix = Q(host_catalog.GetName()) + "." + Q(ScenarioRegistry::SCHEMA_NAME) + ".";
	for (auto &delta_table : tables) {
		if (!sql.empty()) {
			sql += " UNION ALL ";
		}
		auto logical = delta_table.substr(prefix.size());
		sql += "SELECT CAST('" + logical +
		       "' AS VARCHAR) AS table_name, count(*) FILTER (_op = 'I') AS rows_added, count(*) FILTER (_op = "
		       "'U') AS rows_modified, count(*) FILTER (_op = 'D') AS rows_removed FROM " +
		       host_prefix + Q(delta_table);
	}
	if (sql.empty()) {
		sql = "SELECT CAST(NULL AS VARCHAR) AS table_name, CAST(0 AS BIGINT) AS rows_added, CAST(0 AS BIGINT) AS "
		      "rows_modified, CAST(0 AS BIGINT) AS rows_removed WHERE 1 = 0";
	}
	return SQLToSubquery(context, sql);
}

} // namespace

void ScenarioDiff::RegisterFunctions(ExtensionLoader &loader) {
	TableFunctionSet diff_set("scenario_diff");
	TableFunction diff2("scenario_diff", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr);
	diff2.bind_replace = ScenarioDiffBindReplace;
	diff_set.AddFunction(diff2);
	TableFunction diff3("scenario_diff", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                    nullptr);
	diff3.bind_replace = ScenarioDiffBindReplace;
	diff_set.AddFunction(diff3);
	loader.RegisterFunction(diff_set);

	TableFunction summary("scenario_diff_summary", {LogicalType::VARCHAR}, nullptr);
	summary.bind_replace = ScenarioDiffSummaryBindReplace;
	loader.RegisterFunction(summary);
}

} // namespace duckdb
