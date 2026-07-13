#include "catalog/scenario_catalog.hpp"
#include "catalog/scenario_delta.hpp"
#include "catalog/scenario_registry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

ScenarioSchemaEntry::ScenarioSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

ScenarioCatalog &ScenarioSchemaEntry::GetScenarioCatalog() {
	return catalog.Cast<ScenarioCatalog>();
}

bool ScenarioSchemaEntry::ShouldExpose(CatalogEntry &base_entry) {
	if (base_entry.type != CatalogType::TABLE_ENTRY) {
		// Views are not exposed in v1: their SQL is bound against the host
		// catalog and cannot be transparently retargeted.
		return false;
	}
	if (base_entry.internal) {
		return false;
	}
	// Hide the legacy v0.1 metadata tables from scenario reads
	if (StringUtil::StartsWith(base_entry.name, "_scenario_")) {
		return false;
	}
	return true;
}

CatalogEntry &ScenarioSchemaEntry::GetOrCreateTableEntry(ClientContext &context, TableCatalogEntry &base_table) {
	return GetOrCreateTableEntryAs(context, base_table, base_table.name);
}

CatalogEntry &ScenarioSchemaEntry::GetOrCreateTableEntryAs(ClientContext &context, TableCatalogEntry &base_table,
                                                           const string &logical_name) {
	auto &scenario_catalog = GetScenarioCatalog();
	auto &transaction = scenario_catalog.GetScenarioTransaction(context);

	auto cached = transaction.table_entries.find(logical_name);
	if (cached != transaction.table_entries.end()) {
		return *cached->second;
	}

	// Mirror the base table's columns and constraints. For materialized
	// bases the physical name (s<id>_mat_<t>) is replaced by the logical one.
	auto create_info = base_table.GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	table_info.catalog = scenario_catalog.GetName();
	table_info.schema = name;
	table_info.table = logical_name;

	auto entry = make_uniq<ScenarioTableEntry>(scenario_catalog, *this, table_info, base_table);
	auto &result = *entry;
	transaction.table_entries[logical_name] = std::move(entry);
	return result;
}

void ScenarioSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	auto &scenario_catalog = GetScenarioCatalog();
	auto &host_catalog = scenario_catalog.GetHostCatalog(context);
	if (scenario_catalog.mat_base_scenario_id >= 0) {
		// Materialized base: enumerate the frozen copies, exposing them
		// under their logical names (full isolation from the live base)
		auto internal_schema =
		    host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
		if (!internal_schema) {
			return;
		}
		string prefix = "s" + to_string(scenario_catalog.mat_base_scenario_id) + "_mat_";
		internal_schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &mat_entry) {
			if (mat_entry.type != CatalogType::TABLE_ENTRY || !StringUtil::StartsWith(mat_entry.name, prefix)) {
				return;
			}
			auto logical_name = mat_entry.name.substr(prefix.size());
			callback(GetOrCreateTableEntryAs(context, mat_entry.Cast<TableCatalogEntry>(), logical_name));
		});
		return;
	}
	auto &host_schema = host_catalog.GetSchema(context, DEFAULT_SCHEMA);
	host_schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &base_entry) {
		if (!ShouldExpose(base_entry)) {
			return;
		}
		callback(GetOrCreateTableEntry(context, base_entry.Cast<TableCatalogEntry>()));
	});
}

void ScenarioSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	// Committed-only scan without a transaction context: scenario entries are
	// per-transaction projections of the host catalog, so there is nothing to
	// enumerate here.
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	auto type = lookup_info.GetCatalogType();
	if (type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	if (!transaction.context) {
		return nullptr;
	}
	auto &context = *transaction.context;
	auto &scenario_catalog = GetScenarioCatalog();
	auto &host_catalog = scenario_catalog.GetHostCatalog(context);

	if (scenario_catalog.mat_base_scenario_id >= 0) {
		// Materialized base: resolve against the frozen copy
		auto mat_entry = ScenarioDelta::TryGetMatTable(context, host_catalog, scenario_catalog.mat_base_scenario_id,
		                                               lookup_info.GetEntryName());
		if (!mat_entry) {
			return nullptr;
		}
		return &GetOrCreateTableEntryAs(context, *mat_entry, lookup_info.GetEntryName());
	}

	auto &host_schema = host_catalog.GetSchema(context, DEFAULT_SCHEMA);
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, lookup_info.GetEntryName());
	auto base_entry = host_schema.LookupEntry(host_catalog.GetCatalogTransaction(context), table_lookup);
	if (!base_entry || !ShouldExpose(*base_entry)) {
		return nullptr;
	}
	return &GetOrCreateTableEntry(context, base_entry->Cast<TableCatalogEntry>());
}

//===----------------------------------------------------------------------===//
// DDL: uniformly rejected (REQ-COW-007)
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                            TableCatalogEntry &table) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                               CreateFunctionInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                               CreateSequenceInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                    CreateTableFunctionInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                   CreateCopyFunctionInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                     CreatePragmaFunctionInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                                CreateCollationInfo &info) {
	ThrowScenarioDDLError();
}

optional_ptr<CatalogEntry> ScenarioSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	ThrowScenarioDDLError();
}

void ScenarioSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	ThrowScenarioDDLError();
}

void ScenarioSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	ThrowScenarioDDLError();
}

} // namespace duckdb
