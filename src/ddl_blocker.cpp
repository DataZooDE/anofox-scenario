#include "ddl_blocker.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper to check if a schema is a scenario schema
//===--------------------------------------------------------------------===//
static bool IsScenarioSchema(const string &schema_name) {
	return StringUtil::StartsWith(schema_name, "_scen_");
}

//===--------------------------------------------------------------------===//
// Visitor to walk the logical plan and find DDL operations
//===--------------------------------------------------------------------===//
static void CheckDDLOperations(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_CREATE_TABLE: {
		auto &create_table = op.Cast<LogicalCreateTable>();
		string schema_name = create_table.schema.name;
		if (IsScenarioSchema(schema_name)) {
			// Allow creation of delta tables (managed by our extension)
			string table_name = create_table.info->Base().table;
			if (!StringUtil::StartsWith(table_name, "_delta_")) {
				throw NotImplementedException(
				    "CREATE TABLE is not permitted in scenario schemas. "
				    "Scenarios use copy-on-write delta tables for modifications. "
				    "Use delta_create() to enable modifications on existing tables.");
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ALTER: {
		// ALTER operations use LogicalSimple with AlterInfo
		auto &simple = op.Cast<LogicalSimple>();
		if (simple.info && simple.info->info_type == ParseInfoType::ALTER_INFO) {
			auto &alter_info = simple.info->Cast<AlterInfo>();
			if (IsScenarioSchema(alter_info.schema)) {
				throw NotImplementedException(
				    "ALTER TABLE is not permitted in scenario schemas. "
				    "Schema modifications cannot be made within scenarios. "
				    "Tables in scenarios are views that merge base data with delta changes.");
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_DROP: {
		// DROP operations use LogicalSimple with DropInfo
		auto &simple = op.Cast<LogicalSimple>();
		if (simple.info && simple.info->info_type == ParseInfoType::DROP_INFO) {
			auto &drop_info = simple.info->Cast<DropInfo>();
			if (IsScenarioSchema(drop_info.schema)) {
				// Allow dropping delta tables (managed by our extension)
				if (StringUtil::StartsWith(drop_info.name, "_delta_")) {
					break;
				}
				// Allow dropping views (merge views created by our extension)
				if (drop_info.type == CatalogType::VIEW_ENTRY) {
					break;
				}
				throw NotImplementedException(
				    "DROP is not permitted in scenario schemas. "
				    "Use delta_drop() to remove delta tables, or scenario_drop() to remove the entire scenario.");
			}
		}
		break;
	}
	default:
		break;
	}

	// Recurse into children
	for (auto &child : op.children) {
		CheckDDLOperations(*child);
	}
}

//===--------------------------------------------------------------------===//
// Pre-optimize hook for DDL blocking
//===--------------------------------------------------------------------===//
static void DDLBlockingPreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (plan) {
		CheckDDLOperations(*plan);
	}
}

//===--------------------------------------------------------------------===//
// DDL Blocker Extension class
//===--------------------------------------------------------------------===//
class DDLBlockerExtension : public OptimizerExtension {
public:
	DDLBlockerExtension() {
		pre_optimize_function = DDLBlockingPreOptimize;
	}
};

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
void DDLBlocker::Register(DatabaseInstance &db) {
	auto &config = DBConfig::GetConfig(db);
	config.optimizer_extensions.push_back(DDLBlockerExtension());
}

} // namespace duckdb
