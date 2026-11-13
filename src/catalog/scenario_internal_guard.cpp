#include "catalog/scenario_internal_guard.hpp"
#include "catalog/scenario_compat.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb_compat/sql_ast.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/operator/helper/physical_execute.hpp"
#include "duckdb/execution/operator/persistent/physical_batch_insert.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_database.hpp"
#include "duckdb/execution/operator/persistent/physical_delete.hpp"
#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/execution/operator/persistent/physical_update.hpp"
#include "duckdb/execution/operator/schema/physical_alter.hpp"
#include "duckdb/execution/operator/schema/physical_create_function.hpp"
#include "duckdb/execution/operator/schema/physical_create_index.hpp"
#include "duckdb/execution/operator/schema/physical_create_schema.hpp"
#include "duckdb/execution/operator/schema/physical_create_sequence.hpp"
#include "duckdb/execution/operator/schema/physical_create_table.hpp"
#include "duckdb/execution/operator/schema/physical_create_type.hpp"
#include "duckdb/execution/operator/schema/physical_create_view.hpp"
#include "duckdb/execution/operator/schema/physical_drop.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/copy_database_info.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {

namespace {

bool IsInternalSchema(const string &schema_name) {
	return StringUtil::CIEquals(schema_name, ScenarioRegistry::SCHEMA_NAME);
}

//! Finds the first operator of a plan that writes the internal schema
class InternalWriteFinder {
public:
	//! the statement kind ("UPDATE") and its target ("__anofox_scenario.registry"
	//! or "schema __anofox_scenario"); empty while nothing was found
	string statement;
	string target;

	void Visit(const PhysicalOperator &op) {
		if (!statement.empty()) {
			return;
		}
		CheckOperator(op);
		for (auto &child : op.GetChildren()) {
			Visit(child.get());
		}
	}

private:
	void Found(const char *kind, const string &schema_name, const string &name) {
		if (statement.empty() && IsInternalSchema(schema_name)) {
			statement = kind;
			target = name.empty() ? "schema " + schema_name : schema_name + "." + name;
		}
	}
	void Table(const char *kind, const CatalogEntry &table) {
		Found(kind, dc::Str(table.ParentSchema().name), dc::Str(table.name));
	}
	//! PhysicalInsert / PhysicalBatchInsert: INSERT (also COPY FROM) or
	//! CREATE TABLE AS. The CTAS operator types share the class, which Cast<>
	//! rejects (it compares the operator type), hence the dynamic cast
	template <class INSERT>
	bool Insert(const PhysicalOperator &op) {
		auto insert = dynamic_cast<const INSERT *>(&op);
		if (!insert) {
			return false;
		}
		if (insert->insert_table) {
			Table("INSERT", *insert->insert_table);
		} else if (insert->schema) {
			Found("CREATE TABLE AS", dc::Str(insert->schema->name), "");
		}
		return true;
	}
	//! CREATE <entry> (CreateInfo): the schema the entry is created in
	void Create(const char *kind, const CreateInfo &info) {
		auto qualified = dc::SchemaTarget(info);
		Found(kind, qualified[1], "");
	}

	void CheckOperator(const PhysicalOperator &op) {
		switch (op.type) {
		case PhysicalOperatorType::INSERT:
		case PhysicalOperatorType::CREATE_TABLE_AS:
		case PhysicalOperatorType::BATCH_INSERT:
		case PhysicalOperatorType::BATCH_CREATE_TABLE_AS:
			if (!Insert<PhysicalInsert>(op)) {
				Insert<PhysicalBatchInsert>(op);
			}
			break;
		case PhysicalOperatorType::UPDATE:
			Table("UPDATE", op.Cast<PhysicalUpdate>().tableref);
			break;
		case PhysicalOperatorType::DELETE_OPERATOR:
			// also TRUNCATE
			Table("DELETE", op.Cast<PhysicalDelete>().tableref);
			break;
		case PhysicalOperatorType::MERGE_INTO:
			// the action sinks are not (all) children of the merge operator
			for (auto &action : op.Cast<PhysicalMergeInto>().actions) {
				if (action && action->op) {
					Visit(*action->op);
				}
			}
			break;
		case PhysicalOperatorType::CREATE_TABLE:
			Found("CREATE TABLE", dc::Str(op.Cast<PhysicalCreateTable>().schema.name), "");
			break;
		case PhysicalOperatorType::CREATE_INDEX:
			Table("CREATE INDEX", op.Cast<PhysicalCreateIndex>().table);
			break;
		case PhysicalOperatorType::CREATE_VIEW:
			Create("CREATE VIEW", *op.Cast<PhysicalCreateView>().info);
			break;
		case PhysicalOperatorType::CREATE_SEQUENCE:
			Create("CREATE SEQUENCE", *op.Cast<PhysicalCreateSequence>().info);
			break;
		case PhysicalOperatorType::CREATE_MACRO:
			Create("CREATE MACRO", *op.Cast<PhysicalCreateFunction>().info);
			break;
		case PhysicalOperatorType::CREATE_TYPE:
			Create("CREATE TYPE", *op.Cast<PhysicalCreateType>().info);
			break;
		case PhysicalOperatorType::CREATE_SCHEMA:
			Create("CREATE SCHEMA", *op.Cast<PhysicalCreateSchema>().info);
			break;
		case PhysicalOperatorType::DROP: {
			auto &info = *op.Cast<PhysicalDrop>().info;
			auto qualified = dc::CreateTarget(info);
			if (info.type == CatalogType::SCHEMA_ENTRY) {
				// the dropped schema's name sits in the name slot on 1.x and
				// in the schema slot of the qualified name on 2.0
				Found("DROP SCHEMA", IsInternalSchema(qualified[2]) ? qualified[2] : qualified[1], "");
			} else {
				Found("DROP", qualified[1], qualified[2]);
			}
			break;
		}
		case PhysicalOperatorType::ALTER: {
			// ALTER TABLE/VIEW/SEQUENCE ..., COMMENT ON ...
			auto qualified = dc::CreateTarget(*op.Cast<PhysicalAlter>().info);
			Found("ALTER", qualified[1], qualified[2]);
			break;
		}
		case PhysicalOperatorType::COPY_DATABASE:
			// COPY FROM DATABASE: the schema and table entries it creates; the
			// data copy is a separate INSERT
			for (auto &entry : op.Cast<PhysicalCopyDatabase>().info->entries) {
				if (entry) {
					Create("COPY FROM DATABASE", *entry);
				}
			}
			break;
		case PhysicalOperatorType::EXECUTE:
			// EXECUTE of a statement prepared earlier
			Visit(op.Cast<PhysicalExecute>().plan);
			break;
		default:
			break;
		}
	}
};

} // namespace

bool ScenarioInternalGuard::InternalWritesAllowed(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting(SETTING_NAME, setting) && !setting.IsNull()) {
		return BooleanValue::Get(setting.DefaultCastAs(LogicalType::BOOLEAN));
	}
	return false;
}

void ScenarioInternalGuard::Check(ClientContext &context, PreparedStatementData &prepared) {
	if (!prepared.physical_plan) {
		return;
	}
	InternalWriteFinder finder;
	finder.Visit(prepared.physical_plan->Root());
	if (finder.statement.empty()) {
		return;
	}
	if (InternalWritesAllowed(context)) {
		prepared.properties.always_require_rebind = true;
		return;
	}
	throw PermissionException(
	    "Cannot modify %s with SQL (%s): the %s schema holds the scenario registry and the scenario data, which SQL "
	    "may only read. Change scenarios with the scenario functions (scenario_create, scenario_drop, "
	    "scenario_freeze, scenario_unfreeze, scenario_refresh, scenario_merge, ...) or write through an attached "
	    "scenario. For backup/restore or manual repair: SET %s = true",
	    finder.target, finder.statement, ScenarioRegistry::SCHEMA_NAME, SETTING_NAME);
}

} // namespace duckdb
