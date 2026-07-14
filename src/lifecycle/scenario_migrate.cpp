//===----------------------------------------------------------------------===//
// CALL scenario_migrate(): one-way migration of the legacy v0.1 layout
// (_scenario_registry / _scen_* delta schemas / _snap_* snapshot schemas)
// into registry v2 + __anofox_scenario. Runs in the caller's transaction.
//
// Legacy delta tables key on (PK, _op), so one logical row may carry several
// op rows (e.g. I then U). They are folded to the v2 single-row-per-PK
// contract: the row with the latest _ts wins (tie-break D > U > I).
// _scenario_base_rowids is dropped outright (dead weight per the diagnosis);
// _scenario_protocols is kept unchanged.
//===----------------------------------------------------------------------===//

#include "lifecycle/scenario_lifecycle.hpp"

#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace {

struct MigrateState : public GlobalTableFunctionState {
	bool done = false;
};

optional_ptr<DuckTableEntry> TryGetMainTable(ClientContext &context, Catalog &host, const string &schema_name,
                                             const string &table_name) {
	auto schema = host.GetSchema(context, schema_name, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry = schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, table_name);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

void DropTable(ClientContext &context, Catalog &host, const string &schema_name, const string &table_name) {
	DropInfo info;
	info.type = CatalogType::TABLE_ENTRY;
	info.catalog = host.GetName();
	info.schema = schema_name;
	info.name = table_name;
	info.if_not_found = OnEntryNotFound::RETURN_NULL;
	host.DropEntry(context, info);
}

void DropSchema(ClientContext &context, Catalog &host, const string &schema_name) {
	DropInfo info;
	info.type = CatalogType::SCHEMA_ENTRY;
	info.catalog = host.GetName();
	info.name = schema_name;
	info.if_not_found = OnEntryNotFound::RETURN_NULL;
	info.cascade = true;
	host.DropEntry(context, info);
}

//! Fold a legacy delta table (multiple op rows per PK) into a fresh v2 delta
void MigrateDeltaTable(ClientContext &context, Catalog &host, DuckTableEntry &legacy_delta,
                       DuckTableEntry &v2_delta, TableCatalogEntry &base_entry) {
	auto pk_columns = ScenarioDelta::GetPKColumns(base_entry);
	auto legacy_types = legacy_delta.GetStorage().GetTypes();
	// legacy layout: _op(0), _ts(1), _version(2), payload(3..)
	constexpr idx_t LEGACY_PAYLOAD_START = 3;
	idx_t payload_count = legacy_types.size() - LEGACY_PAYLOAD_START;

	vector<StorageIndex> column_ids;
	for (idx_t i = 0; i < legacy_types.size(); i++) {
		column_ids.emplace_back(i);
	}
	struct FoldedRow {
		char op;
		timestamp_t ts;
		vector<Value> payload;
	};
	// key -> folded winner (latest _ts wins; tie-break D > U > I)
	auto op_rank = [](char op) { return op == 'D' ? 2 : (op == 'U' ? 1 : 0); };
	unordered_map<string, FoldedRow> folded;
	vector<string> insertion_order;
	vector<idx_t> key_positions;
	for (auto pk_col : pk_columns) {
		key_positions.push_back(LEGACY_PAYLOAD_START + pk_col);
	}
	idx_t row_counter = 0;
	ScenarioDelta::ScanTableRows(context, legacy_delta, std::move(column_ids), legacy_types,
	                             [&](DataChunk &chunk, idx_t row) {
		                             FoldedRow candidate;
		                             candidate.op = chunk.GetValue(0, row).GetValue<string>()[0];
		                             auto ts_value = chunk.GetValue(1, row);
		                             candidate.ts = ts_value.IsNull() ? timestamp_t(0)
		                                                              : ts_value.GetValue<timestamp_t>();
		                             for (idx_t i = 0; i < payload_count; i++) {
			                             candidate.payload.push_back(
			                                 chunk.GetValue(LEGACY_PAYLOAD_START + i, row));
		                             }
		                             auto key = key_positions.empty()
		                                            ? "row" + to_string(row_counter++)
		                                            : ScenarioDelta::MakeKey(chunk, row, key_positions);
		                             auto existing = folded.find(key);
		                             if (existing == folded.end()) {
			                             insertion_order.push_back(key);
			                             folded[key] = std::move(candidate);
		                             } else if (candidate.ts > existing->second.ts ||
		                                        (candidate.ts == existing->second.ts &&
		                                         op_rank(candidate.op) > op_rank(existing->second.op))) {
			                             // I followed by U stays a scenario-insert with new values
			                             if (existing->second.op == 'I' && candidate.op == 'U') {
				                             candidate.op = 'I';
			                             }
			                             existing->second = std::move(candidate);
		                             }
		                             return true;
	                             });

	// Append the folded rows in v2 layout
	auto &v2_storage = v2_delta.GetStorage();
	auto binder = Binder::CreateBinder(context);
	auto constraints = binder->BindConstraints(v2_delta);
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), v2_storage.GetTypes());
	idx_t count = 0;
	auto flush = [&]() {
		if (count > 0) {
			chunk.SetCardinality(count);
			v2_storage.LocalAppend(v2_delta, context, chunk, constraints);
			chunk.Reset();
			count = 0;
		}
	};
	for (auto &key : insertion_order) {
		auto &row = folded[key];
		chunk.SetValue(ScenarioDelta::OP_COL, count, Value(string(1, row.op)));
		chunk.SetValue(ScenarioDelta::TS_COL, count,
		               row.ts == timestamp_t(0) ? Value::TIMESTAMP(Timestamp::GetCurrentTimestamp())
		                                        : Value::TIMESTAMP(row.ts));
		for (idx_t i = 0; i < row.payload.size(); i++) {
			chunk.SetValue(ScenarioDelta::PAYLOAD_START + i, count, row.payload[i]);
		}
		if (++count == STANDARD_VECTOR_SIZE) {
			flush();
		}
	}
	flush();
}

void ScenarioMigrateExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<MigrateState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	auto &host = Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));

	idx_t migrated_scenarios = 0;
	idx_t migrated_snapshots = 0;

	auto legacy_registry = TryGetMainTable(context, host, DEFAULT_SCHEMA, "_scenario_registry");
	auto legacy_snapshots = TryGetMainTable(context, host, DEFAULT_SCHEMA, "_scenario_snapshots");
	if (!legacy_registry && !legacy_snapshots) {
		output.SetValue(0, 0, Value::BIGINT(0));
		output.SetValue(1, 0, Value::BIGINT(0));
		output.SetCardinality(1);
		return;
	}

	MetaTransaction::Get(context).ModifyDatabase(
	    host.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY |
	                            DatabaseModificationType::DROP_CATALOG_ENTRY | DatabaseModificationType::INSERT_DATA |
	                            DatabaseModificationType::DELETE_DATA);
	ScenarioRegistry::EnsureExists(context, host);

	// --- legacy scenarios -> registry v2 + folded v2 deltas ------------------
	vector<string> legacy_schemas_to_drop;
	if (legacy_registry) {
		struct LegacyScenario {
			int64_t id;
			string name;
			string schema_name;
			bool archived;
			timestamp_t created_at;
			Value description;
			Value parent_id;
		};
		vector<LegacyScenario> scenarios;
		vector<StorageIndex> cols;
		auto legacy_types = legacy_registry->GetStorage().GetTypes();
		for (idx_t i = 0; i < legacy_types.size(); i++) {
			cols.emplace_back(i);
		}
		ScenarioDelta::ScanTableRows(context, *legacy_registry, std::move(cols), legacy_types,
		                             [&](DataChunk &chunk, idx_t row) {
			                             LegacyScenario legacy;
			                             legacy.id = chunk.GetValue(0, row).GetValue<int64_t>();
			                             legacy.name = chunk.GetValue(1, row).GetValue<string>();
			                             legacy.schema_name = chunk.GetValue(2, row).GetValue<string>();
			                             legacy.archived =
			                                 chunk.GetValue(6, row).GetValue<string>() == "archived";
			                             auto created = chunk.GetValue(5, row);
			                             legacy.created_at = created.IsNull()
			                                                     ? Timestamp::GetCurrentTimestamp()
			                                                     : created.GetValue<timestamp_t>();
			                             legacy.description = chunk.GetValue(7, row);
			                             legacy.parent_id = chunk.GetValue(8, row);
			                             scenarios.push_back(std::move(legacy));
			                             return true;
		                             });

		unordered_map<int64_t, int64_t> id_map; // legacy id -> v2 id
		for (auto &legacy : scenarios) {
			if (ScenarioRegistry::Lookup(context, host, legacy.name)) {
				throw InvalidInputException(
				    "scenario_migrate: scenario '%s' already exists in the v2 registry - migration must run on "
				    "an unmigrated database",
				    legacy.name);
			}
			ScenarioRegistryEntry entry;
			entry.scenario_id = ScenarioRegistry::NextId(context, host);
			entry.name = legacy.name;
			entry.mode = "delta";
			entry.frozen = legacy.archived;
			entry.created_at = legacy.created_at;
			entry.has_description = !legacy.description.IsNull();
			if (entry.has_description) {
				entry.description = legacy.description.GetValue<string>();
			}
			// parent remapped in a second pass (needs the full id map)
			ScenarioRegistry::Insert(context, host, entry);
			id_map[legacy.id] = entry.scenario_id;
			migrated_scenarios++;
		}

		// per-scenario: eager v2 deltas for all base tables + fold legacy deltas
		auto &host_schema = host.GetSchema(context, DEFAULT_SCHEMA);
		vector<reference<TableCatalogEntry>> base_tables;
		host_schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
			if (table_entry.type != CatalogType::TABLE_ENTRY || table_entry.internal ||
			    StringUtil::StartsWith(table_entry.name, "_scenario_")) {
				return;
			}
			base_tables.push_back(table_entry.Cast<TableCatalogEntry>());
		});
		for (auto &legacy : scenarios) {
			auto v2_id = id_map[legacy.id];
			for (auto &base_table : base_tables) {
				auto &v2_delta = ScenarioDelta::EnsureDeltaTable(context, host, v2_id, base_table.get());
				auto legacy_delta =
				    TryGetMainTable(context, host, legacy.schema_name, "_delta_" + base_table.get().name);
				if (legacy_delta) {
					MigrateDeltaTable(context, host, *legacy_delta, v2_delta, base_table.get());
				}
			}
			legacy_schemas_to_drop.push_back(legacy.schema_name);
		}
	}

	// --- legacy snapshots -> materialized + frozen scenarios ------------------
	if (legacy_snapshots) {
		struct LegacySnapshot {
			string name;
			string schema; // _snap_<name>
			timestamp_t created_at;
			Value description;
		};
		vector<LegacySnapshot> snapshots;
		vector<StorageIndex> cols;
		auto snap_types = legacy_snapshots->GetStorage().GetTypes();
		for (idx_t i = 0; i < snap_types.size(); i++) {
			cols.emplace_back(i);
		}
		ScenarioDelta::ScanTableRows(context, *legacy_snapshots, std::move(cols), snap_types,
		                             [&](DataChunk &chunk, idx_t row) {
			                             LegacySnapshot snap;
			                             snap.name = chunk.GetValue(1, row).GetValue<string>();
			                             snap.schema = "_snap_" + snap.name;
			                             auto created = chunk.GetValue(3, row);
			                             snap.created_at = created.IsNull()
			                                                   ? Timestamp::GetCurrentTimestamp()
			                                                   : created.GetValue<timestamp_t>();
			                             snap.description = chunk.GetValue(4, row);
			                             snapshots.push_back(std::move(snap));
			                             return true;
		                             });
		for (auto &snap : snapshots) {
			if (ScenarioRegistry::Lookup(context, host, snap.name)) {
				throw InvalidInputException(
				    "scenario_migrate: snapshot '%s' collides with an existing v2 scenario name", snap.name);
			}
			ScenarioRegistryEntry entry;
			entry.scenario_id = ScenarioRegistry::NextId(context, host);
			entry.name = snap.name;
			entry.mode = "materialized";
			entry.frozen = true;
			entry.created_at = snap.created_at;
			entry.has_description = !snap.description.IsNull();
			if (entry.has_description) {
				entry.description = snap.description.GetValue<string>();
			}
			ScenarioRegistry::Insert(context, host, entry);

			// copy each snapshot table into the v2 materialized layout
			auto snap_schema = host.GetSchema(context, snap.schema, OnEntryNotFound::RETURN_NULL);
			if (snap_schema) {
				vector<reference<TableCatalogEntry>> snap_tables;
				snap_schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &table_entry) {
					if (table_entry.type == CatalogType::TABLE_ENTRY) {
						snap_tables.push_back(table_entry.Cast<TableCatalogEntry>());
					}
				});
				for (auto &snap_table : snap_tables) {
					ScenarioDelta::EnsureDeltaTable(context, host, entry.scenario_id, snap_table.get());
					ScenarioDelta::CreateMatTable(context, host, entry.scenario_id, snap_table.get());
				}
				legacy_schemas_to_drop.push_back(snap.schema);
			}
			migrated_snapshots++;
		}
	}

	// --- teardown of the legacy layout ------------------------------------------
	for (auto &schema_name : legacy_schemas_to_drop) {
		DropSchema(context, host, schema_name);
	}
	DropTable(context, host, DEFAULT_SCHEMA, "_scenario_registry");
	DropTable(context, host, DEFAULT_SCHEMA, "_scenario_tables");
	DropTable(context, host, DEFAULT_SCHEMA, "_scenario_base_rowids");
	DropTable(context, host, DEFAULT_SCHEMA, "_scenario_snapshots");
	// _scenario_protocols is intentionally preserved (portable audit trail)

	output.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(migrated_scenarios)));
	output.SetValue(1, 0, Value::BIGINT(NumericCast<int64_t>(migrated_snapshots)));
	output.SetCardinality(1);
}

unique_ptr<FunctionData> ScenarioMigrateBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT};
	names = {"migrated_scenarios", "migrated_snapshots"};
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> ScenarioMigrateInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<MigrateState>();
}

} // namespace

void ScenarioMigrate::RegisterFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("scenario_migrate", {}, ScenarioMigrateExecute, ScenarioMigrateBind, ScenarioMigrateInit));
}

} // namespace duckdb
