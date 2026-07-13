#include "catalog/scenario_catalog.hpp"
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
	auto &scenario_catalog = GetScenarioCatalog();
	auto &transaction = scenario_catalog.GetScenarioTransaction(context);

	auto cached = transaction.table_entries.find(base_table.name);
	if (cached != transaction.table_entries.end()) {
		return *cached->second;
	}

	// Mirror the base table's columns and constraints
	auto create_info = base_table.GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();
	table_info.catalog = scenario_catalog.GetName();
	table_info.schema = name;

	auto entry = make_uniq<ScenarioTableEntry>(scenario_catalog, *this, table_info, base_table);
	auto &result = *entry;
	transaction.table_entries[base_table.name] = std::move(entry);
	return result;
}

void ScenarioSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	auto &scenario_catalog = GetScenarioCatalog();
	auto &host_catalog = scenario_catalog.GetHostCatalog(context);
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
