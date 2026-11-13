#include "catalog/scenario_schema_change.hpp"

#include "catalog/scenario_compat.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb_compat/sql_ast.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/schema/physical_alter.hpp"
#include "duckdb/execution/operator/schema/physical_drop.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Internal tables vs. the table they mirror
//===----------------------------------------------------------------------===//

namespace {

optional_ptr<DuckTableEntry> InternalTable(ClientContext &context, Catalog &host_catalog, const string &table_name) {
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry =
	    schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, dc::Ident(table_name));
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

//! Payload columns of an internal table: [first, first + count)
void PayloadRange(const TableCatalogEntry &internal, bool is_delta, idx_t &first, idx_t &count) {
	auto total = internal.GetColumns().LogicalColumnCount();
	if (!is_delta) {
		first = 0;
		count = total;
		return;
	}
	first = ScenarioDelta::PAYLOAD_START;
	count = total - ScenarioDelta::PAYLOAD_START - (ScenarioDelta::CountColumnIndex(internal).IsValid() ? 1 : 0);
}

//! The host base table of a logical name, if any
optional_ptr<TableCatalogEntry> HostBaseTable(ClientContext &context, Catalog &host_catalog,
                                              const string &logical_name) {
	string schema_name, table_name;
	ScenarioDelta::SplitLogicalName(logical_name, schema_name, table_name);
	auto schema = host_catalog.GetSchema(context, dc::Ident(schema_name), OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry =
	    schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, dc::Ident(table_name));
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto &table = entry->Cast<TableCatalogEntry>();
	if (!table.IsDuckTable()) {
		return nullptr;
	}
	return &table;
}

//! The value an ALTER TABLE ... ADD COLUMN gave the existing rows: the
//! column's DEFAULT, evaluated once, or NULL without one. A volatile default
//! (random(), nextval(...), uuid()) gave every base row its own value, which
//! an internal table can neither reproduce nor draw from a user sequence:
//! NULL then.
Value AddedColumnValue(ClientContext &context, const ColumnDefinition &column) {
	Value null_value(column.Type());
	if (!column.HasDefaultValue()) {
		return null_value;
	}
	try {
		auto expression = column.DefaultValue().Copy();
		auto binder = Binder::CreateBinder(context);
		ConstantBinder constant_binder(*binder, context, "DEFAULT value");
		auto type = column.Type();
		auto bound = constant_binder.Bind(expression, &type);
		if (bound->IsVolatile()) {
			return null_value;
		}
		Value result;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *bound, result)) {
			return null_value;
		}
		return result.DefaultCastAs(column.Type());
	} catch (std::exception &) {
		return null_value;
	}
}

//! The column to add to an internal table: the mirrored column's name and
//! type, with the value existing rows get as a constant DEFAULT (no
//! dependency on a user sequence or function)
ColumnDefinition InternalAddedColumn(ClientContext &context, const ColumnDefinition &reference, Value &value) {
	value = AddedColumnValue(context, reference);
	ColumnDefinition column(reference.Name(), reference.Type());
	if (!value.IsNull()) {
		auto parsed = dc::ParseExpressionListBuiltin(value.ToSQLString());
		if (parsed.size() == 1) {
			column.SetDefaultValue(std::move(parsed[0]));
		} else {
			value = Value(reference.Type());
		}
	}
	return column;
}

string QuotedColumnNames(const ColumnList &columns, idx_t first) {
	string result;
	for (idx_t i = first; i < columns.LogicalColumnCount(); i++) {
		result += (result.empty() ? "'" : ", '") + dc::Str(columns.GetColumn(LogicalIndex(i)).Name()) + "'";
	}
	return result;
}

//! Widen a keyed delta, a pre-image table or a mat copy in place:
//! ALTER TABLE ... ADD COLUMN per missing column (rows keep their row ids)
DuckTableEntry &WidenByAlter(ClientContext &context, Catalog &host_catalog, DuckTableEntry &internal,
                             idx_t payload_count, const ColumnList &reference) {
	auto table_name = dc::Str(internal.name);
	for (idx_t i = payload_count; i < reference.LogicalColumnCount(); i++) {
		Value value;
		auto column = InternalAddedColumn(context, reference.GetColumn(LogicalIndex(i)), value);
		auto info = scenario_compat::MakeAddColumnInfo(dc::Str(host_catalog.GetName()), ScenarioRegistry::SCHEMA_NAME,
		                                               table_name, std::move(column));
		host_catalog.Alter(context, *info);
	}
	auto widened = InternalTable(context, host_catalog, table_name);
	if (!widened) {
		throw InternalException("anofox_scenario: internal table '%s' vanished while adding columns", table_name);
	}
	return *widened;
}

//! Widen a bag delta: its multiplicity column must stay the last one, so the
//! table is rebuilt (rows buffered, the table dropped and recreated with the
//! new payload columns before the multiplicity, the rows re-appended), as the
//! legacy keyless upgrade does
DuckTableEntry &WidenBagDelta(ClientContext &context, Catalog &host_catalog, DuckTableEntry &old_table,
                              idx_t payload_count, const ColumnList &reference) {
	auto table_name = dc::Str(old_table.name);
	auto count_index = ScenarioDelta::CountColumnIndex(old_table).GetIndex();
	auto old_types = old_table.GetStorage().GetTypes();
	ColumnDataCollection rows(context, old_types);
	{
		vector<StorageIndex> column_ids;
		for (idx_t i = 0; i < old_types.size(); i++) {
			column_ids.emplace_back(i);
		}
		ScenarioDelta::ScanTableChunks(context, old_table, std::move(column_ids), old_types,
		                               [&](DataChunk &chunk) { rows.Append(chunk); });
	}

	auto info = make_uniq<CreateTableInfo>();
	dc::SetQualifiedName(*info, dc::Str(host_catalog.GetName()), ScenarioRegistry::SCHEMA_NAME, table_name);
	info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	ColumnList payload;
	for (idx_t col = 0; col < count_index; col++) {
		auto &old_column = old_table.GetColumn(LogicalIndex(col));
		info->columns.AddColumn(ColumnDefinition(old_column.Name(), old_column.Type()));
		if (col >= ScenarioDelta::PAYLOAD_START) {
			payload.AddColumn(ColumnDefinition(old_column.Name(), old_column.Type()));
		}
	}
	vector<Value> added_values;
	for (idx_t i = payload_count; i < reference.LogicalColumnCount(); i++) {
		Value value;
		auto column = InternalAddedColumn(context, reference.GetColumn(LogicalIndex(i)), value);
		payload.AddColumn(ColumnDefinition(column.Name(), column.Type()));
		info->columns.AddColumn(std::move(column));
		added_values.push_back(std::move(value));
	}
	auto new_count_index = info->columns.LogicalColumnCount();
	info->columns.AddColumn(
	    ColumnDefinition(dc::Ident(ScenarioDelta::NewCountColumnName(payload)), LogicalType::BIGINT));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(ScenarioDelta::OP_COL)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(ScenarioDelta::TS_COL)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(new_count_index)));

	ScenarioDelta::DropInternalTable(context, host_catalog, table_name);
	host_catalog.CreateTable(context, std::move(info));
	auto new_table = InternalTable(context, host_catalog, table_name);
	if (!new_table) {
		throw InternalException("anofox_scenario: failed to recreate delta table '%s'", table_name);
	}
	auto binder = Binder::CreateBinder(context);
	auto constraints = binder->BindConstraints(*new_table);
	auto new_types = new_table->GetStorage().GetTypes();
	DataChunk append_chunk;
	append_chunk.Initialize(Allocator::Get(context), new_types);
	for (auto &chunk : rows.Chunks()) {
		append_chunk.Reset();
		for (idx_t col = 0; col < count_index; col++) {
			append_chunk.data[col].Reference(chunk.data[col]);
		}
		for (idx_t i = 0; i < added_values.size(); i++) {
			dc::ReferenceValue(append_chunk.data[count_index + i], added_values[i], chunk.size());
		}
		append_chunk.data[new_count_index].Reference(chunk.data[count_index]);
		append_chunk.SetChildCardinality(chunk.size());
		new_table->GetStorage().LocalAppend(*new_table, context, append_chunk, constraints);
	}
	return *new_table;
}

//! Nearest materialized scenario in the parent chain (including itself), -1 if none
int64_t MaterializedAncestor(ClientContext &context, Catalog &host_catalog, const ScenarioRegistryEntry &entry) {
	auto current = make_uniq<ScenarioRegistryEntry>(entry);
	idx_t depth = 0;
	while (current) {
		if (current->mode == "materialized") {
			return current->scenario_id;
		}
		if (current->parent_id < 0 || ++depth > 1000) {
			return -1;
		}
		current = ScenarioRegistry::LookupById(context, host_catalog, current->parent_id);
	}
	return -1;
}

//! Internal table names s<id>_<kind>_<logical>: scenario id, kind, logical name
bool ParseInternalName(const string &name, int64_t &scenario_id, string &kind, string &logical_name) {
	if (name.size() < 2 || name[0] != 's' || !StringUtil::CharacterIsDigit(name[1])) {
		return false;
	}
	idx_t pos = 1;
	while (pos < name.size() && StringUtil::CharacterIsDigit(name[pos])) {
		pos++;
	}
	for (auto candidate : {"_delta_", "_pre_", "_mat_"}) {
		string marker = candidate;
		if (name.compare(pos, marker.size(), marker) == 0) {
			scenario_id = std::stoll(name.substr(1, pos - 1));
			kind = marker.substr(1, marker.size() - 2);
			logical_name = name.substr(pos + marker.size());
			return true;
		}
	}
	return false;
}

//! Logical names of every table a scenario has internal tables for
vector<string> ScenarioLogicalTables(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	set<string> names;
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (schema) {
		schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			int64_t id;
			string kind, logical;
			if (entry.type == CatalogType::TABLE_ENTRY && ParseInternalName(dc::Str(entry.name), id, kind, logical) &&
			    id == scenario_id) {
				names.insert(logical);
			}
		});
	}
	return vector<string>(names.begin(), names.end());
}

} // namespace

ScenarioDelta::Shape ScenarioDelta::CompareShape(const TableCatalogEntry &internal, bool is_delta,
                                                 const ColumnList &reference, string &detail) {
	idx_t first, count;
	PayloadRange(internal, is_delta, first, count);
	auto reference_count = reference.LogicalColumnCount();
	for (idx_t i = 0; i < count; i++) {
		auto &column = internal.GetColumn(LogicalIndex(first + i));
		if (i >= reference_count) {
			detail = "column '" + dc::Str(column.Name()) + "' was dropped";
			return Shape::MISMATCH;
		}
		auto &reference_column = reference.GetColumn(LogicalIndex(i));
		if (!StringUtil::CIEquals(dc::Str(column.Name()), dc::Str(reference_column.Name()))) {
			detail = "column '" + dc::Str(column.Name()) + "' was dropped or renamed";
			return Shape::MISMATCH;
		}
		if (column.Type() != reference_column.Type()) {
			detail = "column '" + dc::Str(column.Name()) + "' changed type from " + column.Type().ToString() + " to " +
			         reference_column.Type().ToString();
			return Shape::MISMATCH;
		}
	}
	if (count < reference_count) {
		detail = QuotedColumnNames(reference, count);
		return Shape::NARROW;
	}
	return Shape::SAME;
}

void ScenarioDelta::ThrowIfShapeDiffers(const string &scenario_name, const string &logical_name,
                                        const TableCatalogEntry &internal, bool is_delta, const ColumnList &reference) {
	string detail;
	switch (CompareShape(internal, is_delta, reference, detail)) {
	case Shape::SAME:
		return;
	case Shape::NARROW:
		throw InvalidInputException(
		    "Scenario '%s' cannot use table '%s' yet: the base table gained column(s) %s that the scenario's "
		    "stored changes lack (the ALTER TABLE ran without anofox_scenario loaded). DETACH and ATTACH the scenario "
		    "from a writable connection, outside an explicit transaction, to add them",
		    scenario_name, logical_name, detail);
	case Shape::MISMATCH:
		throw InvalidInputException(
		    "Scenario '%s' cannot use table '%s': %s in the base table after the scenario changed the table, so "
		    "its stored changes no longer fit. Restore the column, or drop the scenario (scenario_drop)",
		    scenario_name, logical_name, detail);
	}
}

void ScenarioDelta::SyncTableShape(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                   const string &logical_name, bool may_widen, bool throw_errors) {
	auto entry = ScenarioRegistry::LookupById(context, host_catalog, scenario_id);
	if (!entry || !entry->base_catalog.empty()) {
		return; // DuckLake / foreign bases: the scenario reads a pinned snapshot
	}
	auto base = HostBaseTable(context, host_catalog, logical_name);
	optional_ptr<TableCatalogEntry> reference;
	auto mat_id = MaterializedAncestor(context, host_catalog, *entry);
	if (mat_id >= 0) {
		auto mat = TryGetMatTable(context, host_catalog, mat_id, logical_name);
		if (!mat) {
			return; // created in the base after the materialized copy
		}
		if (base && may_widen) {
			// the mat copy follows the columns the base gained; a copy whose
			// columns no longer match the base (dropped, renamed or retyped
			// there) stays the scenario's own schema
			string detail;
			if (CompareShape(*mat, false, base->GetColumns(), detail) == Shape::NARROW) {
				mat = &WidenByAlter(context, host_catalog, *mat, mat->GetColumns().LogicalColumnCount(),
				                    base->GetColumns());
			}
		}
		reference = mat.get();
	} else {
		reference = base;
	}
	if (!reference) {
		return; // the base table is gone: merge-back reports it
	}
	auto &columns = reference->GetColumns();
	for (auto is_delta : {true, false}) {
		auto internal = is_delta ? TryGetDeltaTable(context, host_catalog, scenario_id, logical_name)
		                         : TryGetPreTable(context, host_catalog, scenario_id, logical_name);
		if (!internal) {
			continue;
		}
		string detail;
		auto shape = CompareShape(*internal, is_delta, columns, detail);
		if (shape == Shape::NARROW && may_widen) {
			idx_t first, count;
			PayloadRange(*internal, is_delta, first, count);
			if (is_delta && CountColumnIndex(*internal).IsValid()) {
				WidenBagDelta(context, host_catalog, *internal, count, columns);
			} else {
				WidenByAlter(context, host_catalog, *internal, count, columns);
			}
			continue;
		}
		if (throw_errors) {
			ThrowIfShapeDiffers(entry->name, logical_name, *internal, is_delta, columns);
		}
	}
}

void ScenarioDelta::SyncScenarioShapes(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                       bool may_widen, bool throw_errors) {
	for (auto &logical_name : ScenarioLogicalTables(context, host_catalog, scenario_id)) {
		SyncTableShape(context, host_catalog, scenario_id, logical_name, may_widen, throw_errors);
	}
}

bool ScenarioDelta::ScenarioNeedsWidening(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	auto entry = ScenarioRegistry::LookupById(context, host_catalog, scenario_id);
	if (!entry || !entry->base_catalog.empty()) {
		return false;
	}
	auto mat_id = MaterializedAncestor(context, host_catalog, *entry);
	for (auto &logical_name : ScenarioLogicalTables(context, host_catalog, scenario_id)) {
		auto base = HostBaseTable(context, host_catalog, logical_name);
		optional_ptr<TableCatalogEntry> reference = base;
		string detail;
		if (mat_id >= 0) {
			auto mat = TryGetMatTable(context, host_catalog, mat_id, logical_name);
			if (!mat) {
				continue;
			}
			if (base && CompareShape(*mat, false, base->GetColumns(), detail) == Shape::NARROW) {
				return true;
			}
			reference = mat.get();
		}
		if (!reference) {
			continue;
		}
		auto delta = TryGetDeltaTable(context, host_catalog, scenario_id, logical_name);
		if (delta && CompareShape(*delta, true, reference->GetColumns(), detail) == Shape::NARROW) {
			return true;
		}
		auto pre = TryGetPreTable(context, host_catalog, scenario_id, logical_name);
		if (pre && CompareShape(*pre, false, reference->GetColumns(), detail) == Shape::NARROW) {
			return true;
		}
	}
	return false;
}

vector<int64_t> ScenarioDelta::ScenariosWithTable(ClientContext &context, Catalog &host_catalog,
                                                  const string &logical_name) {
	set<int64_t> ids;
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (schema) {
		schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			int64_t id;
			string kind, logical;
			if (entry.type == CatalogType::TABLE_ENTRY && ParseInternalName(dc::Str(entry.name), id, kind, logical) &&
			    logical == logical_name) {
				ids.insert(id);
			}
		});
	}
	return vector<int64_t>(ids.begin(), ids.end());
}

bool ScenarioDelta::HasRows(ClientContext &context, DuckTableEntry &table) {
	bool found = false;
	ScanTableRows(context, table, {StorageIndex(0)}, {table.GetColumn(LogicalIndex(0)).Type()},
	              [&](DataChunk &, idx_t) {
		              found = true;
		              return false;
	              });
	return found;
}

void ScenarioDelta::DropInternalTable(ClientContext &context, Catalog &host_catalog, const string &table_name) {
	DropInfo drop;
	drop.type = CatalogType::TABLE_ENTRY;
	dc::SetQualifiedName(drop, dc::Str(host_catalog.GetName()), ScenarioRegistry::SCHEMA_NAME, table_name);
	drop.if_not_found = OnEntryNotFound::RETURN_NULL;
	host_catalog.DropEntry(context, drop);
}

//===----------------------------------------------------------------------===//
// ALTER TABLE / DROP TABLE of a base table
//===----------------------------------------------------------------------===//

namespace {

enum class BaseChange : uint8_t { ADD_COLUMN, RESHAPE };

//! Wraps a PhysicalAlter / PhysicalDrop of a host table: runs the original
//! operator and keeps the scenarios' internal tables in step, in the same
//! statement and transaction. The wrapped operator stays a plain source (it is
//! not a child: a source with children would be planned as an operator).
class PhysicalScenarioBaseChange : public PhysicalOperator {
public:
	PhysicalScenarioBaseChange(PhysicalPlan &physical_plan, PhysicalOperator &inner_p, BaseChange change_p,
	                           string catalog_p, string schema_p, string table_p, string action_p)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, inner_p.types,
		                   inner_p.estimated_cardinality),
	      inner(inner_p), change(change_p), catalog_name(std::move(catalog_p)), schema_name(std::move(schema_p)),
	      table_name(std::move(table_p)), action(std::move(action_p)) {
	}

	PhysicalOperator &inner;
	BaseChange change;
	string catalog_name;
	string schema_name;
	string table_name;
	//! For messages: "drop a column of", "rename", ...
	string action;

	bool IsSource() const override {
		return true;
	}
	string GetName() const override {
		return inner.GetName();
	}

protected:
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override {
		auto &client = context.client;
		auto catalog = Catalog::GetCatalogEntry(client, dc::Ident(catalog_name));
		optional_ptr<TableCatalogEntry> table;
		if (catalog && catalog->GetCatalogType() == "duckdb") {
			auto schema = catalog->GetSchema(client, dc::Ident(schema_name), OnEntryNotFound::RETURN_NULL);
			if (schema) {
				auto entry = schema->GetEntry(schema->GetCatalogTransaction(client), CatalogType::TABLE_ENTRY,
				                              dc::Ident(table_name));
				if (entry && entry->type == CatalogType::TABLE_ENTRY) {
					table = &entry->Cast<TableCatalogEntry>();
				}
			}
		}
		if (!table) {
			return inner.GetData(context, chunk, input);
		}
		auto &host_catalog = *catalog;
		auto logical_name = ScenarioDelta::LogicalName(*table);
		auto scenario_ids = ScenarioDelta::ScenariosWithTable(client, host_catalog, logical_name);
		if (scenario_ids.empty()) {
			return inner.GetData(context, chunk, input);
		}
		// the scenarios over this host (not over another catalog), with
		// whether they mirror the base directly (no materialized ancestor)
		vector<pair<ScenarioRegistryEntry, bool>> scenarios;
		for (auto id : scenario_ids) {
			auto entry = ScenarioRegistry::LookupById(client, host_catalog, id);
			if (entry && entry->base_catalog.empty()) {
				bool mirrors_base = MaterializedAncestor(client, host_catalog, *entry) < 0;
				scenarios.emplace_back(*entry, mirrors_base);
			}
		}
		if (change == BaseChange::RESHAPE) {
			// unmerged changes are stored in the table's current shape: refuse
			for (auto &scenario : scenarios) {
				auto delta =
				    ScenarioDelta::TryGetDeltaTable(client, host_catalog, scenario.first.scenario_id, logical_name);
				if (delta && ScenarioDelta::HasRows(client, *delta)) {
					throw InvalidInputException(
					    "Cannot %s table '%s': scenario '%s' has unmerged changes to it, stored in the table's "
					    "current columns. Merge the scenario (scenario_merge) or drop it (scenario_drop) first. "
					    "ADD COLUMN works while scenarios have changes to a table",
					    action, logical_name, dc::Str(scenario.first.name));
				}
			}
		}
		auto result = inner.GetData(context, chunk, input);
		auto &meta = MetaTransaction::Get(client);
		meta.ModifyDatabase(host_catalog.GetAttached(),
		                    DatabaseModificationType::ALTER_TABLE | DatabaseModificationType::DROP_CATALOG_ENTRY |
		                        DatabaseModificationType::CREATE_CATALOG_ENTRY | DatabaseModificationType::INSERT_DATA);
		for (auto &scenario : scenarios) {
			auto scenario_id = scenario.first.scenario_id;
			if (change == BaseChange::ADD_COLUMN) {
				ScenarioDelta::SyncTableShape(client, host_catalog, scenario_id, logical_name, true, false);
				continue;
			}
			if (!scenario.second) {
				continue; // reads a materialized copy, which keeps its columns
			}
			// empty deltas and pre-images of the old shape: a table without a
			// delta reads as never written, and the next write recreates them
			// in the new shape (a delta-less table is an empty changelog, #34)
			ScenarioDelta::DropInternalTable(client, host_catalog,
			                                 ScenarioDelta::DeltaTableName(scenario_id, logical_name));
			ScenarioDelta::DropInternalTable(client, host_catalog,
			                                 ScenarioDelta::PreTableName(scenario_id, logical_name));
		}
		return result;
	}
};

const char *ReshapeAction(AlterTableType type) {
	switch (type) {
	case AlterTableType::REMOVE_COLUMN:
		return "drop a column of";
	case AlterTableType::RENAME_COLUMN:
		return "rename a column of";
	case AlterTableType::ALTER_COLUMN_TYPE:
		return "change a column type of";
	case AlterTableType::RENAME_TABLE:
		return "rename";
	case AlterTableType::ADD_FIELD:
	case AlterTableType::REMOVE_FIELD:
	case AlterTableType::RENAME_FIELD:
		return "change a struct column of";
	default:
		return nullptr;
	}
}

} // namespace

void ScenarioBaseSchemaChange::WrapPlan(ClientContext &context, PreparedStatementData &prepared) {
	if (!prepared.physical_plan) {
		return;
	}
	auto &plan = *prepared.physical_plan;
	auto &root = plan.Root();
	BaseChange change;
	string action;
	std::array<string, 3> target;
	if (root.type == PhysicalOperatorType::ALTER) {
		auto &info = *root.Cast<PhysicalAlter>().info;
		if (info.type != AlterType::ALTER_TABLE) {
			return;
		}
		auto alter_type = info.Cast<AlterTableInfo>().alter_table_type;
		if (alter_type == AlterTableType::ADD_COLUMN) {
			change = BaseChange::ADD_COLUMN;
			action = "add a column to";
		} else {
			auto reshape = ReshapeAction(alter_type);
			if (!reshape) {
				return; // defaults, NOT NULL, constraints, options: no column change
			}
			change = BaseChange::RESHAPE;
			action = reshape;
		}
		target = dc::CreateTarget(info);
	} else if (root.type == PhysicalOperatorType::DROP) {
		auto &info = *root.Cast<PhysicalDrop>().info;
		if (info.type != CatalogType::TABLE_ENTRY) {
			return;
		}
		change = BaseChange::RESHAPE;
		action = "drop";
		target = dc::CreateTarget(info);
	} else {
		return;
	}
	if (target[0].empty() || StringUtil::CIEquals(target[1], ScenarioRegistry::SCHEMA_NAME)) {
		return; // unresolved (IF EXISTS of a missing table), or the internal schema itself
	}
	auto &wrapper = plan.Make<PhysicalScenarioBaseChange>(root, change, target[0], target[1], target[2], action);
	plan.SetRoot(wrapper);
}

} // namespace duckdb
