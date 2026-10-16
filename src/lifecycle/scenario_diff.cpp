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
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

namespace {

string Q(const string &identifier) {
	return KeywordHelper::WriteOptionallyQuoted(identifier);
}

//! SQL single-quoted string literal with proper escaping
string QL(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

Catalog &GetHostCatalogForDiff(ClientContext &context) {
	return Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
}

//! Fully-qualified physical names for the diff SQL
struct DiffTarget {
	string delta_name; // host.__anofox_scenario.s<id>_delta_<t>
	string base_name;  // host.main.<t> or host.__anofox_scenario.s<mat>_mat_<t>
	//! " AT (TIMESTAMP => ...)" pinning a versioned (DuckLake) base to the
	//! scenario's creation time, like the scan and merge-back; else empty
	string base_at;
	//! The scenario's origin table in its base catalog (not a mat copy):
	//! what the 3-arg diff's 'main' side reads, pinned by origin_at
	string origin_name;
	string origin_at;
	string base_catalog_name; // the scenario's base := catalog (empty = host)
	vector<string> pk_columns;
	vector<string> payload_columns;
	//! Keyless table (bag delta with _count): every column is the identity
	//! (pk_columns), payload_columns is empty, and changes are net I/D counts
	bool keyless = false;
	//! Keyless multiplicity per delta row: the _count column, or 1 for a
	//! pre-v0.4.1 delta not yet upgraded (each old row is one inserted copy)
	string count_expr = "_count";
};

//! FROM-clause reference to the base side under an alias (AT follows the alias)
string BaseFrom(const DiffTarget &target, const string &alias) {
	return target.base_name + " AS " + alias + target.base_at;
}

//! SQL list of quoted columns with an optional alias prefix ("d.")
string ColumnList(const vector<string> &columns, const string &prefix = "") {
	string list;
	for (auto &col : columns) {
		list += (list.empty() ? "" : ", ") + prefix + Q(col);
	}
	return list;
}

//! One row per copy of a keyless delta's rows with the given op (_count
//! multiplicity expanded)
string ExpandedDeltaRows(const DiffTarget &target, const char *op) {
	auto cols = ColumnList(target.pk_columns);
	return "SELECT " + cols + " FROM (SELECT " + cols + ", unnest(range(" + target.count_expr + ")) AS __anofox_copy FROM " +
	       target.delta_name + " WHERE _op = '" + op + "')";
}

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

	// Origin: the base catalog the scenario reads, pinned when versioned. A
	// materialized side reads its copy instead, so it needs the catalog only
	// for a 'main' side (origin_name stays empty when it is not attached)
	optional_ptr<Catalog> base_catalog = &host_catalog;
	if (!entry->base_catalog.empty()) {
		base_catalog = Catalog::GetCatalogEntry(context, entry->base_catalog);
		if (!base_catalog && mat_base_id < 0) {
			throw InvalidInputException("Scenario '%s': base catalog '%s' is not attached - ATTACH it first",
			                            scenario_name, entry->base_catalog);
		}
	}
	target.base_catalog_name = entry->base_catalog;
	string schema_name, plain_name;
	ScenarioDelta::SplitLogicalName(table_name, schema_name, plain_name);
	auto &origin_at = target.origin_at;
	if (base_catalog) {
		target.origin_name = Q(base_catalog->GetName()) + "." + Q(schema_name) + "." + Q(plain_name);
		if (base_catalog->GetCatalogType() == "ducklake") {
			origin_at = " AT (TIMESTAMP => TIMESTAMP '" + Timestamp::ToString(entry->created_at) + "')";
		}
	}

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
		base_entry = base_catalog->GetEntry<TableCatalogEntry>(context, schema_name, plain_name,
		                                                       OnEntryNotFound::RETURN_NULL);
		if (!base_entry) {
			throw InvalidInputException("Table '%s' not found in the base schema", table_name);
		}
		target.base_name = target.origin_name;
		target.base_at = origin_at;
	}

	auto pk_columns =
	    ScenarioDelta::GetKeyColumns(context, host_catalog, entry->scenario_id, table_name, *base_entry);
	if (pk_columns.empty()) {
		// keyless: a bag diff over whole rows, netting the delta's I/D counts
		auto count_name = ScenarioDelta::CountColumnName(*delta);
		target.count_expr = count_name.empty() ? "1" : Q(count_name);
		target.keyless = true;
	}
	unordered_set<idx_t> pk_set(pk_columns.begin(), pk_columns.end());
	for (auto &col : base_entry->GetColumns().Logical()) {
		if (target.keyless || pk_set.find(col.Logical().index) != pk_set.end()) {
			target.pk_columns.push_back(col.Name());
		} else {
			target.payload_columns.push_back(col.Name());
		}
	}
	return target;
}

//! SQL of the merged relation of one diff side: 'main' = the scenario
//! side's origin table (pinned to its snapshot for versioned bases, live
//! otherwise); else the scenario's merged state (base source + delta overlay)
string MergedRelationSQL(ClientContext &context, const string &side, const string &table_name,
                         const DiffTarget &shape) {
	if (side == "main") {
		if (shape.origin_name.empty()) {
			throw InvalidInputException("Base catalog '%s' is not attached - ATTACH it first",
			                            shape.base_catalog_name);
		}
		auto origin = shape.origin_name + " AS o" + shape.origin_at;
		if (shape.keyless) {
			return "(SELECT " + ColumnList(shape.pk_columns) + " FROM " + origin + ")";
		}
		return "(SELECT * FROM " + origin + ")";
	}
	auto target = ResolveDiffTarget(context, side, table_name);
	if (shape.keyless) {
		// bag: origin plus inserted copies, minus deleted copies
		return "((SELECT " + ColumnList(target.pk_columns) + " FROM " + BaseFrom(target, "b") + " UNION ALL " +
		       ExpandedDeltaRows(target, "I") + ") EXCEPT ALL (" + ExpandedDeltaRows(target, "D") + "))";
	}
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
	       cols_b + " FROM " + BaseFrom(target, "b") + " WHERE NOT EXISTS (SELECT 1 FROM " + target.delta_name +
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
	if (shape.keyless) {
		// bag difference both ways; EXCEPT ALL matches whole rows NULL-safely
		auto cols = ColumnList(shape.pk_columns);
		string sql = "SELECT " + cols +
		             ", CAST('added' AS VARCHAR) AS change_type, CAST(NULL AS VARCHAR) AS column_name, CAST(NULL "
		             "AS VARCHAR) AS old_value, CAST(NULL AS VARCHAR) AS new_value FROM (" +
		             rel_b + " EXCEPT ALL " + rel_a + ")";
		sql += " UNION ALL SELECT " + cols +
		       ", 'removed', CAST(NULL AS VARCHAR), CAST(NULL AS VARCHAR), CAST(NULL AS VARCHAR) FROM (" + rel_a +
		       " EXCEPT ALL " + rel_b + ")";
		return SQLToSubquery(context, sql);
	}

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
		sql += " UNION ALL SELECT " + pk_list_a + ", 'modified', CAST(" + QL(col) + " AS VARCHAR), CAST(a." + Q(col) +
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
	if (target.keyless) {
		// net I/D count per distinct row, one output row per copy; GROUP BY
		// treats NULLs as equal (the delta's NULL-safe whole-row identity)
		auto cols = ColumnList(target.pk_columns);
		string sql = "SELECT " + cols +
		             ", CAST(CASE WHEN __anofox_net > 0 THEN 'added' ELSE 'removed' END AS VARCHAR) AS change_type, "
		             "CAST(NULL AS VARCHAR) AS column_name, CAST(NULL AS VARCHAR) AS old_value, CAST(NULL AS "
		             "VARCHAR) AS new_value FROM (SELECT " +
		             cols + ", __anofox_net, unnest(range(abs(__anofox_net))) AS __anofox_copy FROM (SELECT " + cols +
		             ", CAST(sum(CASE WHEN _op = 'I' THEN " + target.count_expr + " ELSE -" + target.count_expr +
		             " END) AS BIGINT) AS __anofox_net FROM " +
		             target.delta_name + " GROUP BY ALL) WHERE __anofox_net <> 0)";
		return SQLToSubquery(context, sql);
	}

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
		sql += " UNION ALL SELECT " + pk_list_d + ", 'modified', CAST(" + QL(col) +
		       " AS VARCHAR), CAST(b." + Q(col) + " AS VARCHAR), CAST(d." + Q(col) + " AS VARCHAR) FROM " +
		       target.delta_name + " d JOIN " + BaseFrom(target, "b") + " ON (" + join_cond + ") WHERE d._op = 'U' AND (d." +
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
	//! keyless (bag) deltas: their payload columns; empty for keyed deltas
	vector<vector<string>> bag_columns;
	//! keyless (bag) deltas: the multiplicity column's name
	vector<string> count_names;
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (schema) {
		schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
			if (table_entry.type == CatalogType::TABLE_ENTRY && StringUtil::StartsWith(table_entry.name, prefix)) {
				tables.push_back(table_entry.name);
				auto &table = table_entry.Cast<TableCatalogEntry>();
				vector<string> columns;
				auto count_column = ScenarioDelta::CountColumnIndex(table);
				if (count_column.IsValid()) {
					for (idx_t col = ScenarioDelta::PAYLOAD_START; col < count_column.GetIndex(); col++) {
						columns.push_back(table.GetColumn(LogicalIndex(col)).Name());
					}
				}
				bag_columns.push_back(std::move(columns));
				count_names.push_back(ScenarioDelta::CountColumnName(table));
			}
		});
	}

	string sql;
	auto host_prefix = Q(host_catalog.GetName()) + "." + Q(ScenarioRegistry::SCHEMA_NAME) + ".";
	for (idx_t t = 0; t < tables.size(); t++) {
		auto &delta_table = tables[t];
		if (!sql.empty()) {
			sql += " UNION ALL ";
		}
		auto logical = delta_table.substr(prefix.size());
		if (!bag_columns[t].empty()) {
			// keyless: net I/D counts per distinct row (no "modified" without
			// identity: an update is a removed + an added row)
			sql += "SELECT CAST(" + QL(logical) +
			       " AS VARCHAR) AS table_name, CAST(coalesce(sum(__anofox_net) FILTER (__anofox_net > 0), 0) AS "
			       "BIGINT) AS rows_added, CAST(0 AS BIGINT) AS rows_modified, CAST(coalesce(-sum(__anofox_net) "
			       "FILTER (__anofox_net < 0), 0) AS BIGINT) AS rows_removed FROM (SELECT sum(CASE WHEN _op = 'I' "
			       "THEN " + Q(count_names[t]) + " ELSE -" + Q(count_names[t]) + " END) AS __anofox_net FROM " +
			       host_prefix + Q(delta_table) + " GROUP BY " + ColumnList(bag_columns[t]) + ")";
			continue;
		}
		sql += "SELECT CAST(" + QL(logical) +
		       " AS VARCHAR) AS table_name, count(*) FILTER (_op = 'I') AS rows_added, count(*) FILTER (_op = "
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
	{
		// The two arities genuinely differ -- one diffs against the scenario's origin,
		// the other diffs any two named sides -- so each carries its own description,
		// keyed by parameter_types. Descriptions come from the function table in
		// README.md.
		CreateTableFunctionInfo info(std::move(diff_set));
		FunctionDescription two_sided;
		two_sided.description =
		    "Diff a scenario against its origin, returning the key columns (every column for keyless "
		    "tables) plus change_type ('added', 'removed' or 'modified'), column_name, old_value and new_value.";
		two_sided.examples = {"SELECT * FROM scenario_diff('price_increase', 'products');"};
		two_sided.parameter_names = {"scenario", "table_name"};
		two_sided.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		two_sided.categories = {"scenario"};
		info.descriptions.push_back(std::move(two_sided));

		FunctionDescription three_sided;
		three_sided.description =
		    "Diff any two sides against each other -- 'main' or any scenario name -- where old_value "
		    "comes from side a and new_value from side b.";
		three_sided.examples = {"SELECT * FROM scenario_diff('main', 'price_increase', 'products');"};
		three_sided.parameter_names = {"a", "b", "table_name"};
		three_sided.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
		three_sided.categories = {"scenario"};
		info.descriptions.push_back(std::move(three_sided));
		loader.RegisterFunction(std::move(info));
	}

	TableFunction summary("scenario_diff_summary", {LogicalType::VARCHAR}, nullptr);
	summary.bind_replace = ScenarioDiffSummaryBindReplace;
	{
		CreateTableFunctionInfo info(std::move(summary));
		FunctionDescription desc;
		desc.description =
		    "Summarise a scenario's changes per table as rows_added, rows_modified and rows_removed.";
		desc.examples = {"SELECT * FROM scenario_diff_summary('price_increase');"};
		desc.parameter_names = {"scenario"};
		desc.categories = {"scenario"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace duckdb
