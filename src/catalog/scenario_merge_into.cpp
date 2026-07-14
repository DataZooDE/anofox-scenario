//===----------------------------------------------------------------------===//
// MERGE INTO on scenario tables (WP5.4). Core's PhysicalMergeInto routes
// matched/not-matched rows to per-action child operators - we mirror
// DuckCatalog::PlanMergeInto, plugging in the scenario sinks. This also
// powers INSERT ... ON CONFLICT / INSERT OR REPLACE, which the v1.5 binder
// lowers through the MERGE machinery for non-duck catalogs.
//===----------------------------------------------------------------------===//

#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_dml.hpp"

#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"

namespace duckdb {

namespace {

unique_ptr<MergeIntoOperator> PlanScenarioMergeAction(ClientContext &context, LogicalMergeInto &op,
                                                      PhysicalPlanGenerator &planner, ScenarioTableEntry &entry,
                                                      BoundMergeIntoAction &action) {
	auto result = make_uniq<MergeIntoOperator>();
	result->action_type = action.action_type;
	result->condition = std::move(action.condition);
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	for (auto &constraint : op.bound_constraints) {
		bound_constraints.push_back(constraint->Copy());
	}
	auto return_types = op.types;
	auto cardinality = op.EstimateCardinality(context);

	switch (action.action_type) {
	case MergeActionType::MERGE_UPDATE: {
		vector<unique_ptr<Expression>> defaults;
		for (auto &def : op.bound_defaults) {
			defaults.push_back(def->Copy());
		}
		result->op = &MakeScenarioUpdateOperator(planner, std::move(return_types), entry, std::move(action.columns),
		                                         std::move(action.expressions), std::move(defaults),
		                                         std::move(bound_constraints), cardinality, false, op.row_id_start);
		break;
	}
	case MergeActionType::MERGE_DELETE: {
		result->op =
		    &MakeScenarioDeleteOperator(planner, std::move(return_types), entry, op.row_id_start, cardinality);
		break;
	}
	case MergeActionType::MERGE_INSERT: {
		result->op = &MakeScenarioInsertOperator(planner, std::move(return_types), entry,
		                                         std::move(bound_constraints), cardinality, false);
		// remap the insert expressions to the full column layout (defaults
		// for unmentioned columns) - same transformation core performs
		if (!action.column_index_map.empty()) {
			vector<unique_ptr<Expression>> new_expressions;
			for (auto &col : entry.GetColumns().Physical()) {
				auto storage_idx = col.StorageOid();
				auto mapped_index = action.column_index_map[col.Physical()];
				if (mapped_index == DConstants::INVALID_INDEX) {
					new_expressions.push_back(op.bound_defaults[storage_idx]->Copy());
				} else {
					new_expressions.push_back(std::move(action.expressions[mapped_index]));
				}
			}
			action.expressions = std::move(new_expressions);
		}
		result->expressions = std::move(action.expressions);
		break;
	}
	case MergeActionType::MERGE_ERROR:
		result->expressions = std::move(action.expressions);
		break;
	case MergeActionType::MERGE_DO_NOTHING:
		break;
	default:
		throw InternalException("Unsupported merge action on scenario table");
	}
	return result;
}

} // namespace

PhysicalOperator &ScenarioCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalMergeInto &op, PhysicalOperator &plan) {
	ThrowIfFrozen(context, "MERGE INTO");
	auto &entry = op.table.Cast<ScenarioTableEntry>();
	if (entry.key_columns.empty()) {
		throw NotImplementedException("MERGE INTO / ON CONFLICT in scenarios requires a PRIMARY KEY on the base "
		                              "table or key_columns declared at scenario_create (v1 limitation)");
	}
	if (op.return_chunk) {
		throw NotImplementedException(
		    "RETURNING on MERGE INTO scenario tables is not supported yet (planned for v0.4.1)");
	}

	map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>> actions;
	for (auto &action_entry : op.actions) {
		vector<unique_ptr<MergeIntoOperator>> planned_actions;
		for (auto &action : action_entry.second) {
			planned_actions.push_back(PlanScenarioMergeAction(context, op, planner, entry, *action));
		}
		actions.emplace(action_entry.first, std::move(planned_actions));
	}

	// scenario sinks are single-threaded (ParallelSink() = false)
	auto &result = planner.Make<PhysicalMergeInto>(op.types, std::move(actions), op.row_id_start, op.source_marker,
	                                               false /* parallel */, false /* return_chunk */);
	result.children.push_back(plan);
	return result;
}

} // namespace duckdb
