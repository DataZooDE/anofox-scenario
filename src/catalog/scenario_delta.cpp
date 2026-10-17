#include "catalog/scenario_delta.hpp"

#include "catalog/scenario_registry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/common/types/constraint_conflict_info.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/execution/index/art/iterator.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

// C++11: out-of-line definitions for static constexpr members
constexpr idx_t ScenarioDelta::OP_COL;
constexpr idx_t ScenarioDelta::TS_COL;
constexpr idx_t ScenarioDelta::PAYLOAD_START;
constexpr const char *ScenarioDelta::COUNT_COLUMN;

optional_idx ScenarioDelta::CountColumnIndex(const TableCatalogEntry &delta_table) {
	if (delta_table.HasPrimaryKey()) {
		return optional_idx();
	}
	auto &columns = delta_table.GetColumns();
	auto count = columns.LogicalColumnCount();
	if (count <= PAYLOAD_START + 1) {
		return optional_idx();
	}
	auto last = count - 1;
	if (columns.GetColumn(LogicalIndex(last)).Type() != LogicalType::BIGINT) {
		return optional_idx();
	}
	// the structural marker: only the multiplicity column of a bag delta
	// carries NOT NULL besides _op/_ts
	for (auto &constraint : delta_table.GetConstraints()) {
		if (constraint->type == ConstraintType::NOT_NULL &&
		    constraint->Cast<NotNullConstraint>().index.index == last) {
			return optional_idx(last);
		}
	}
	return optional_idx();
}

string ScenarioDelta::CountColumnName(const TableCatalogEntry &delta_table) {
	auto index = CountColumnIndex(delta_table);
	if (!index.IsValid()) {
		return string();
	}
	return delta_table.GetColumn(LogicalIndex(index.GetIndex())).Name();
}

string ScenarioDelta::NewCountColumnName(const ColumnList &payload_columns) {
	string name = COUNT_COLUMN;
	while (payload_columns.ColumnExists(name)) {
		name = "_" + name;
	}
	return name;
}

static optional_ptr<DuckTableEntry> TryGetInternalTable(ClientContext &context, Catalog &host_catalog,
                                                        const string &table_name);

bool ScenarioDelta::IsLegacyKeylessDelta(const TableCatalogEntry &delta_table) {
	return !delta_table.HasPrimaryKey() && !CountColumnIndex(delta_table).IsValid();
}

//! The scenario's delta tables (exact prefix match on the naming contract)
static vector<reference<DuckTableEntry>> ScenarioDeltaTables(ClientContext &context, Catalog &host_catalog,
                                                             int64_t scenario_id) {
	vector<reference<DuckTableEntry>> result;
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return result;
	}
	auto prefix = "s" + to_string(scenario_id) + "_delta_";
	schema->Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		if (entry.type == CatalogType::TABLE_ENTRY && StringUtil::StartsWith(entry.name, prefix)) {
			result.push_back(entry.Cast<DuckTableEntry>());
		}
	});
	return result;
}

bool ScenarioDelta::HasLegacyKeylessDeltas(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	for (auto &delta : ScenarioDeltaTables(context, host_catalog, scenario_id)) {
		if (IsLegacyKeylessDelta(delta.get())) {
			return true;
		}
	}
	return false;
}

idx_t ScenarioDelta::UpgradeLegacyKeylessDeltas(ClientContext &context, Catalog &host_catalog, int64_t scenario_id) {
	vector<string> legacy_names;
	for (auto &delta : ScenarioDeltaTables(context, host_catalog, scenario_id)) {
		if (IsLegacyKeylessDelta(delta.get())) {
			legacy_names.push_back(delta.get().name);
		}
	}
	for (auto &table_name : legacy_names) {
		auto old_table = TryGetInternalTable(context, host_catalog, table_name);
		if (!old_table) {
			continue;
		}
		// The new layout: the old columns (_op, _ts, payload) plus a trailing
		// _count, NOT NULL on the metadata columns, and no UNIQUE mirror (a
		// bag delta holds D rows next to the I rows they cancel)
		auto info = make_uniq<CreateTableInfo>();
		info->catalog = host_catalog.GetName();
		info->schema = ScenarioRegistry::SCHEMA_NAME;
		info->table = table_name;
		info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
		vector<LogicalType> old_types;
		for (auto &col : old_table->GetColumns().Physical()) {
			info->columns.AddColumn(ColumnDefinition(col.Name(), col.Type()));
			old_types.push_back(col.Type());
		}
		auto count_index = info->columns.LogicalColumnCount();
		auto count_name = NewCountColumnName(info->columns);
		info->columns.AddColumn(ColumnDefinition(count_name, LogicalType::BIGINT));
		info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(OP_COL)));
		info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(TS_COL)));
		info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(count_index)));

		// Buffer the old rows (deltas are small by invariant), then swap the
		// table: drop the old one with its indexes, recreate it under the
		// same name - no rename, so nothing depends on index-free renames
		ColumnDataCollection rows(context, old_types);
		{
			vector<StorageIndex> column_ids;
			for (idx_t i = 0; i < old_types.size(); i++) {
				column_ids.emplace_back(i);
			}
			auto &storage = old_table->GetStorage();
			auto &transaction = DuckTransaction::Get(context, old_table->catalog);
			ParallelTableScanState parallel_state;
			storage.InitializeParallelScan(context, parallel_state, {});
			TableScanState state;
			state.Initialize(std::move(column_ids), context);
			DataChunk chunk;
			chunk.Initialize(Allocator::Get(context), old_types);
			while (storage.NextParallelScan(context, parallel_state, state) > 0) {
				while (true) {
					chunk.Reset();
					storage.Scan(transaction, chunk, state);
					if (chunk.size() == 0) {
						break;
					}
					rows.Append(chunk);
				}
			}
		}

		DropInfo drop;
		drop.type = CatalogType::TABLE_ENTRY;
		drop.catalog = host_catalog.GetName();
		drop.schema = ScenarioRegistry::SCHEMA_NAME;
		drop.name = table_name;
		drop.if_not_found = OnEntryNotFound::THROW_EXCEPTION;
		host_catalog.DropEntry(context, drop);
		host_catalog.CreateTable(context, std::move(info));

		auto new_table = TryGetInternalTable(context, host_catalog, table_name);
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
			for (idx_t col = 0; col < old_types.size(); col++) {
				append_chunk.data[col].Reference(chunk.data[col]);
			}
			// every pre-v0.4.1 row is exactly one inserted copy
			append_chunk.data[count_index].Reference(Value::BIGINT(1));
			append_chunk.SetCardinality(chunk.size());
			new_table->GetStorage().LocalAppend(*new_table, context, append_chunk, constraints);
		}
	}
	return legacy_names.size();
}

bool ScenarioDelta::TryUpgradeLegacyKeylessDeltas(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                                  bool read_only_attach) {
	if (!HasLegacyKeylessDeltas(context, host_catalog, scenario_id)) {
		return true;
	}
	// Every skip below is safe: readers handle the legacy layout as
	// _count = 1, and the next ATTACH that is allowed to write upgrades.
	if (read_only_attach) {
		return false; // READ_ONLY promises not to write the host
	}
	auto &host_db = host_catalog.GetAttached();
	auto &meta = MetaTransaction::Get(context);
	if (host_db.IsReadOnly() || meta.IsReadOnly()) {
		return false;
	}
	auto modified = meta.ModifiedDatabase();
	if (modified ? modified.get() != &host_db : !context.transaction.IsAutoCommit()) {
		// inside an explicit transaction the upgrade would make the host the
		// transaction's single writer behind the user's back (and block a
		// later write to another database); only ride along when the
		// transaction already writes the host
		return false;
	}
	// another, older transaction on the host may hold uncommitted writes to
	// the legacy delta: dropping it now would fail that transaction's commit
	auto &transaction_manager = host_db.GetTransactionManager();
	if (transaction_manager.IsDuckTransactionManager()) {
		auto &duck_manager = DuckTransactionManager::Get(host_db);
		auto &transaction = DuckTransaction::Get(context, host_db);
		if (duck_manager.LowestActiveStart() < transaction.start_time) {
			return false;
		}
	}
	meta.ModifyDatabase(host_db, DatabaseModificationType::DROP_CATALOG_ENTRY |
	                                 DatabaseModificationType::CREATE_CATALOG_ENTRY |
	                                 DatabaseModificationType::INSERT_DATA);
	try {
		UpgradeLegacyKeylessDeltas(context, host_catalog, scenario_id);
	} catch (TransactionException &) {
		// a catalog conflict with a concurrent transaction: keep the legacy
		// layout rather than fail the ATTACH (each table's swap is
		// self-contained, so tables upgraded before the conflict stay valid)
		return false;
	}
	return true;
}

string ScenarioDelta::DeltaTableName(int64_t scenario_id, const string &table_name) {
	return "s" + to_string(scenario_id) + "_delta_" + table_name;
}

string ScenarioDelta::MatTableName(int64_t scenario_id, const string &table_name) {
	return "s" + to_string(scenario_id) + "_mat_" + table_name;
}

//! A name part as written in a logical name: double-quoted (inner quotes
//! doubled) only when it contains the '.' separator or starts with a quote,
//! so every other name is spelled exactly as before
static string EncodeNamePart(const string &part) {
	if (part.find('.') == string::npos && (part.empty() || part[0] != '"')) {
		return part;
	}
	return "\"" + StringUtil::Replace(part, "\"", "\"\"") + "\"";
}

//! Read one name part starting at pos: a quoted part up to its closing quote,
//! else up to the next '.' (or the end when stop_at_dot is false). pos ends
//! on the character after the part.
static string DecodeNamePart(const string &name, idx_t &pos, bool stop_at_dot) {
	string part;
	if (pos < name.size() && name[pos] == '"') {
		idx_t i = pos + 1;
		while (i < name.size()) {
			if (name[i] == '"') {
				if (i + 1 < name.size() && name[i + 1] == '"') {
					part += '"';
					i += 2;
					continue;
				}
				// closing quote: a well-formed part ends here
				if (i + 1 == name.size() || (stop_at_dot && name[i + 1] == '.')) {
					pos = i + 1;
					return part;
				}
				break;
			}
			part += name[i++];
		}
		// not a well-formed quoted part: take it literally
		part.clear();
	}
	auto end = stop_at_dot ? name.find('.', pos) : string::npos;
	if (end == string::npos) {
		end = name.size();
	}
	part = name.substr(pos, end - pos);
	pos = end;
	return part;
}

string ScenarioDelta::EncodeLogicalName(const string &schema_name, const string &table_name) {
	if (schema_name == DEFAULT_SCHEMA) {
		return EncodeNamePart(table_name);
	}
	return EncodeNamePart(schema_name) + "." + EncodeNamePart(table_name);
}

string ScenarioDelta::LogicalName(const TableCatalogEntry &table) {
	return EncodeLogicalName(table.ParentSchema().name, table.name);
}

void ScenarioDelta::SplitLogicalName(const string &logical_name, string &schema_name, string &table_name) {
	idx_t pos = 0;
	auto first = DecodeNamePart(logical_name, pos, true);
	if (pos >= logical_name.size()) {
		schema_name = DEFAULT_SCHEMA;
		table_name = first;
		return;
	}
	pos++; // the separating '.'
	schema_name = first;
	table_name = DecodeNamePart(logical_name, pos, false);
}

//! Internal: look up a table in the internal schema by exact name
static optional_ptr<DuckTableEntry> TryGetInternalTable(ClientContext &context, Catalog &host_catalog,
                                                        const string &table_name) {
	auto schema = host_catalog.GetSchema(context, ScenarioRegistry::SCHEMA_NAME, OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry = schema->GetEntry(schema->GetCatalogTransaction(context), CatalogType::TABLE_ENTRY, table_name);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

optional_ptr<DuckTableEntry> ScenarioDelta::TryGetDeltaTable(ClientContext &context, Catalog &host_catalog,
                                                             int64_t scenario_id, const string &table_name) {
	return TryGetInternalTable(context, host_catalog, DeltaTableName(scenario_id, table_name));
}

optional_ptr<DuckTableEntry> ScenarioDelta::TryGetMatTable(ClientContext &context, Catalog &host_catalog,
                                                           int64_t scenario_id, const string &table_name) {
	return TryGetInternalTable(context, host_catalog, MatTableName(scenario_id, table_name));
}

DuckTableEntry &ScenarioDelta::CreateMatTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                              TableCatalogEntry &base_entry, const string *logical_name_override) {
	auto logical_name = logical_name_override ? *logical_name_override : LogicalName(base_entry);
	// Schema copy: columns + NOT NULL/CHECK/PK/UNIQUE (the PK carries row
	// identity and backs the base collision probe). FOREIGN KEYs are dropped -
	// both sides, including the hidden FK_TYPE_PRIMARY_KEY_TABLE entry DuckDB
	// keeps on referenced tables: they would resolve inside __anofox_scenario
	// (or back at the base), and the copy is a data snapshot, not a place to
	// enforce them. FKs are enforced at merge-back against the base.
	auto create_info = base_entry.GetInfo();
	auto &info = create_info->Cast<CreateTableInfo>();
	auto &constraints = info.constraints;
	constraints.erase(std::remove_if(constraints.begin(), constraints.end(),
	                                 [](const unique_ptr<Constraint> &constraint) {
		                                 return constraint->type == ConstraintType::FOREIGN_KEY;
	                                 }),
	                  constraints.end());
	info.catalog = host_catalog.GetName();
	info.schema = ScenarioRegistry::SCHEMA_NAME;
	info.table = MatTableName(scenario_id, logical_name);
	info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	info.temporary = false;
	host_catalog.CreateTable(context, unique_ptr_cast<CreateInfo, CreateTableInfo>(std::move(create_info)));

	auto created = TryGetMatTable(context, host_catalog, scenario_id, logical_name);
	if (!created) {
		throw InternalException("anofox_scenario: failed to create materialized table for '%s'", base_entry.name);
	}
	CopyTableData(context, base_entry.Cast<DuckTableEntry>(), *created);
	return *created;
}

void ScenarioDelta::CopyTableData(ClientContext &context, DuckTableEntry &from_table, DuckTableEntry &to_table,
                                  idx_t target_column_offset) {
	auto &from_storage = from_table.GetStorage();
	auto &to_storage = to_table.GetStorage();
	auto from_types = from_storage.GetTypes();
	auto to_types = to_storage.GetTypes();

	vector<StorageIndex> column_ids;
	for (idx_t i = 0; i < from_types.size(); i++) {
		column_ids.emplace_back(i);
	}
	auto binder = Binder::CreateBinder(context);
	auto to_constraints = binder->BindConstraints(to_table);

	auto &transaction = DuckTransaction::Get(context, from_table.catalog);
	ParallelTableScanState parallel_state;
	from_storage.InitializeParallelScan(context, parallel_state, {});
	TableScanState state;
	state.Initialize(std::move(column_ids), context);

	DataChunk scan_chunk;
	scan_chunk.Initialize(Allocator::Get(context), from_types);
	// a legacy keyless delta into a bag delta: append each row with _count = 1
	bool fill_count = target_column_offset == 0 && to_types.size() == from_types.size() + 1 &&
	                  IsLegacyKeylessDelta(from_table) && CountColumnIndex(to_table).IsValid();
	DataChunk append_chunk;
	if (target_column_offset > 0 || fill_count) {
		append_chunk.Initialize(Allocator::Get(context), to_types);
	}
	while (from_storage.NextParallelScan(context, parallel_state, state) > 0) {
		while (true) {
			scan_chunk.Reset();
			from_storage.Scan(transaction, scan_chunk, state);
			if (scan_chunk.size() == 0) {
				break;
			}
			if (fill_count) {
				append_chunk.Reset();
				for (idx_t col = 0; col < from_types.size(); col++) {
					append_chunk.data[col].Reference(scan_chunk.data[col]);
				}
				append_chunk.data[from_types.size()].Reference(Value::BIGINT(1));
				append_chunk.SetCardinality(scan_chunk.size());
				to_storage.LocalAppend(to_table, context, append_chunk, to_constraints);
			} else if (target_column_offset == 0) {
				to_storage.LocalAppend(to_table, context, scan_chunk, to_constraints);
			} else {
				append_chunk.Reset();
				for (idx_t col = 0; col < from_types.size(); col++) {
					append_chunk.data[target_column_offset + col].Reference(scan_chunk.data[col]);
				}
				append_chunk.SetCardinality(scan_chunk.size());
				to_storage.LocalAppend(to_table, context, append_chunk, to_constraints);
			}
		}
	}
}

DuckTableEntry &ScenarioDelta::EnsureDeltaTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                                TableCatalogEntry &base_entry, const vector<string> *identity_keys,
                                                const string *logical_name_override, bool host_is_not_writer) {
	auto logical_name = logical_name_override ? *logical_name_override : LogicalName(base_entry);
	auto existing = TryGetDeltaTable(context, host_catalog, scenario_id, logical_name);
	if (existing) {
		return *existing;
	}
	// NOTE: the caller is responsible for registering the host write
	// (ScenarioCatalog::MarkHostWrite / MetaTransaction::ModifyDatabase).

	auto info = make_uniq<CreateTableInfo>();
	info->catalog = host_catalog.GetName();
	info->schema = ScenarioRegistry::SCHEMA_NAME;
	info->table = DeltaTableName(scenario_id, logical_name);
	info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	info->columns.AddColumn(ColumnDefinition("_op", LogicalType::VARCHAR));
	info->columns.AddColumn(ColumnDefinition("_ts", LogicalType::TIMESTAMP));
	for (auto &col : base_entry.GetColumns().Logical()) {
		info->columns.AddColumn(ColumnDefinition(col.Name(), col.Type()));
	}
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(OP_COL)));
	info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(TS_COL)));

	// The row identity becomes the delta PK: one delta row per user-visible
	// key. identity_keys, when given, is the identity exactly (a declared
	// key, or an inherited delta's shape; empty = keyless); otherwise the
	// base PK is. Keyless tables get a bag changelog (below).
	vector<string> pk_names;
	if (identity_keys) {
		pk_names = *identity_keys;
	} else {
		for (auto col_id : GetPKColumns(base_entry)) {
			pk_names.push_back(base_entry.GetColumn(LogicalIndex(col_id)).Name());
		}
	}
	bool keyless = pk_names.empty();
	if (!keyless) {
		info->constraints.push_back(make_uniq<UniqueConstraint>(std::move(pk_names), true));
	}

	if (keyless) {
		// Bag changelog: whole-row identity cannot be a PK (NULLs), so I/D
		// rows carry a multiplicity instead and readers aggregate. Secondary
		// UNIQUE constraints are not mirrored here (D rows carry real
		// payloads and would collide with their replacements).
		// (named _count unless a base column already is - readers find it
		// by position and its NOT NULL, see CountColumnIndex)
		info->columns.AddColumn(
		    ColumnDefinition(NewCountColumnName(base_entry.GetColumns()), LogicalType::BIGINT));
		info->constraints.push_back(
		    make_uniq<NotNullConstraint>(LogicalIndex(PAYLOAD_START + base_entry.GetColumns().LogicalColumnCount())));
	} else {
		// Secondary UNIQUE constraints are mirrored so scenario-written rows
		// are unique among themselves too (the base side is probed separately
		// at DML time). Tombstones carry NULL payloads, which never conflict.
		for (auto &constraint : base_entry.GetConstraints()) {
			if (constraint->type != ConstraintType::UNIQUE) {
				continue;
			}
			auto &unique = constraint->Cast<UniqueConstraint>();
			if (unique.IsPrimaryKey()) {
				continue;
			}
			vector<string> unique_names;
			if (unique.HasIndex()) {
				unique_names.push_back(base_entry.GetColumn(unique.GetIndex()).Name());
			} else {
				unique_names = unique.GetColumnNames();
			}
			info->constraints.push_back(make_uniq<UniqueConstraint>(std::move(unique_names), false));
		}
	}
	if (!host_is_not_writer) {
		host_catalog.CreateTable(context, std::move(info));
	} else {
		// The statement registered the scenario catalog itself as the
		// transaction's writer (COPY FROM DATABASE ... TO <scenario>), and a
		// transaction has exactly one: the host's transaction is flagged
		// read-write (MarkHostWrite) but cannot become the writer. DuckDB's
		// schema refuses entries for a database that is not the registered
		// writer when it sees a client context, so create the entry in the
		// host transaction without one. Everything else is unchanged: the
		// entry goes into the host transaction's undo buffer, so ROLLBACK
		// removes it and COMMIT writes it to the host's WAL.
		auto binder = Binder::CreateBinder(context);
		auto bound_info = binder->BindCreateTableInfo(std::move(info));
		auto catalog_transaction = host_catalog.GetCatalogTransaction(context);
		catalog_transaction.context = nullptr;
		host_catalog.CreateTable(catalog_transaction, *bound_info);
	}

	auto created = TryGetDeltaTable(context, host_catalog, scenario_id, logical_name);
	if (!created) {
		throw InternalException("anofox_scenario: failed to create delta table for '%s'", base_entry.name);
	}
	return *created;
}

vector<idx_t> ScenarioDelta::GetPKColumns(const TableCatalogEntry &base_entry) {
	vector<idx_t> result;
	auto pk = base_entry.GetPrimaryKey();
	if (!pk) {
		return result;
	}
	auto &unique = pk->Cast<UniqueConstraint>();
	auto &columns = base_entry.GetColumns();
	if (unique.HasIndex()) {
		result.push_back(unique.GetIndex().index);
		return result;
	}
	for (auto &name : unique.GetColumnNames()) {
		result.push_back(columns.GetColumn(name).Logical().index);
	}
	return result;
}

vector<idx_t> ScenarioDelta::GetKeyColumns(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
                                           const string &logical_name, TableCatalogEntry &base_entry) {
	// Identity resolution (#34). Once a delta exists its shape is the
	// identity: its PRIMARY KEY (by position), or none for a bag delta. It
	// was created from the order below, so the two only disagree after a
	// race (a scenario_refresh declaration committing next to a concurrent
	// first write) or a base schema change, and readers must follow the rows
	// that are actually there. Without a delta: (1) a key declared in
	// scenario_tables, (2) the base PK, (3) none: keyless, a bag delta.
	auto delta = TryGetDeltaTable(context, host_catalog, scenario_id, logical_name);
	if (delta) {
		auto delta_pk = GetPKColumns(*delta);
		vector<idx_t> result;
		for (auto delta_col : delta_pk) {
			if (delta_col < PAYLOAD_START) {
				throw InternalException("anofox_scenario: delta PK references a metadata column");
			}
			result.push_back(delta_col - PAYLOAD_START);
		}
		return result;
	}
	vector<string> declared;
	if (ScenarioRegistry::LookupDeclaredKey(context, host_catalog, scenario_id, logical_name, declared)) {
		vector<idx_t> result;
		auto &columns = base_entry.GetColumns();
		for (auto &name : declared) {
			if (!columns.ColumnExists(name)) {
				throw InvalidInputException(
				    "Scenario table '%s': declared key column '%s' no longer exists in the base table. Re-declare "
				    "the key with scenario_refresh(<scenario>, key_columns := MAP {'%s': [...]})",
				    logical_name, name, logical_name);
			}
			result.push_back(columns.GetColumn(name).Logical().index);
		}
		return result;
	}
	return GetPKColumns(base_entry);
}

optional_ptr<TableCatalogEntry> ScenarioDelta::GetPinnedBaseEntry(ClientContext &context, Catalog &base_catalog,
                                                                 const string &schema_name, const string &table_name,
                                                                 timestamp_t at) {
	BoundAtClause at_clause("timestamp", Value::TIMESTAMP(at));
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, table_name, &at_clause, QueryErrorContext());
	optional_ptr<CatalogEntry> pinned;
	try {
		pinned = base_catalog.GetEntry(context, schema_name, lookup, OnEntryNotFound::RETURN_NULL);
	} catch (CatalogException &) {
		pinned = nullptr; // e.g. the schema is not in the snapshot either
	}
	if (!pinned || pinned->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return &pinned->Cast<TableCatalogEntry>();
}

string ScenarioDelta::MakeKey(DataChunk &chunk, idx_t row, const vector<idx_t> &key_positions) {
	string key;
	for (auto pos : key_positions) {
		auto value = chunk.GetValue(pos, row);
		if (value.IsNull()) {
			key += "N|";
			continue;
		}
		auto str = value.ToString();
		key += to_string(str.size());
		key += ":";
		key += str;
	}
	return key;
}

void ScenarioDelta::ScanTableRows(ClientContext &context, DuckTableEntry &table, vector<StorageIndex> column_ids,
                                  const vector<LogicalType> &scan_types,
                                  const std::function<bool(DataChunk &, idx_t)> &callback) {
	auto &data_table = table.GetStorage();
	auto &transaction = DuckTransaction::Get(context, table.catalog);

	ParallelTableScanState parallel_state;
	data_table.InitializeParallelScan(context, parallel_state, {});
	TableScanState state;
	state.Initialize(std::move(column_ids), context);

	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), scan_types);
	while (data_table.NextParallelScan(context, parallel_state, state) > 0) {
		while (true) {
			chunk.Reset();
			data_table.Scan(transaction, chunk, state);
			if (chunk.size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk.size(); row++) {
				if (!callback(chunk, row)) {
					return;
				}
			}
		}
	}
}

void ScenarioDelta::ScanTableChunks(ClientContext &context, DuckTableEntry &table, vector<StorageIndex> column_ids,
                                    const vector<LogicalType> &scan_types,
                                    const std::function<void(DataChunk &)> &callback) {
	auto &data_table = table.GetStorage();
	auto &transaction = DuckTransaction::Get(context, table.catalog);

	ParallelTableScanState parallel_state;
	data_table.InitializeParallelScan(context, parallel_state, {});
	TableScanState state;
	state.Initialize(std::move(column_ids), context);

	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), scan_types);
	while (data_table.NextParallelScan(context, parallel_state, state) > 0) {
		while (true) {
			chunk.Reset();
			data_table.Scan(transaction, chunk, state);
			if (chunk.size() == 0) {
				break;
			}
			callback(chunk);
		}
	}
}

//===----------------------------------------------------------------------===//
// ScenarioDeltaIndexProbe
//===----------------------------------------------------------------------===//

bool ScenarioDeltaIndexProbe::Initialize(ClientContext &context, DuckTableEntry &delta, const vector<idx_t> &keys) {
	delta_table = nullptr;
	key_columns = keys;
	key_ids.clear();
	if (key_columns.empty()) {
		return false;
	}
	for (auto key_col : key_columns) {
		key_ids.insert(ScenarioDelta::PAYLOAD_START + key_col);
	}
	auto &storage = delta.GetStorage();
	storage.BindIndexes(context);
	ConflictInfo conflict_info(key_ids);
	for (auto &index : storage.GetDataTableInfo()->GetIndexes().Indexes()) {
		if (index.IsBound() && index.IsUnique() && index.GetIndexType() == ART::TYPE_NAME &&
		    conflict_info.ConflictTargetMatches(index)) {
			delta_table = &delta;
			return true;
		}
	}
	return false;
}

namespace {

//! Every row id stored under `key` in one ART (a unique index can briefly hold
//! two: a row deleted by a running transaction plus its replacement; the
//! deleted_rows_in_use / checkpoint delta ARTs are not unique at all)
void CollectARTRowIds(ART &art, const ARTKey &key, vector<row_t> &result) {
	if (key.Empty() || !art.tree.HasMetadata()) {
		return;
	}
	auto leaf = ARTOperator::Lookup(art, art.tree, key, 0);
	if (!leaf) {
		return;
	}
	if (leaf->GetType() == NType::LEAF_INLINED) {
		result.push_back(leaf->GetRowId());
		return;
	}
	// a nested leaf (gate): iterate its row ids, exactly as ART::SearchEqual
	set<row_t> row_ids;
	Iterator it(art);
	it.FindMinimum(*leaf);
	ARTKey empty_key;
	RowIdSetOutput output(row_ids, NumericLimits<idx_t>::Maximum());
	it.Scan(empty_key, output, false);
	result.insert(result.end(), row_ids.begin(), row_ids.end());
}

//! Look the probe chunk's keys up in `art` (row i -> candidates[i])
void CollectCandidates(ClientContext &context, ART &art, DataChunk &probe, vector<vector<row_t>> &candidates) {
	DataChunk expr_chunk;
	expr_chunk.Initialize(Allocator::Get(context), art.logical_types, probe.size());
	art.ExecuteExpressions(probe, expr_chunk);
	ArenaAllocator arena(Allocator::Get(context));
	unsafe_vector<ARTKey> keys(probe.size());
	art.GenerateKeys<>(arena, expr_chunk, keys);
	for (idx_t i = 0; i < probe.size(); i++) {
		CollectARTRowIds(art, keys[i], candidates[i]);
	}
}

ART &CheckedART(BoundIndex &index) {
	if (index.GetIndexType() != ART::TYPE_NAME) {
		throw InternalException("anofox_scenario: concurrent changes made to a non-ART delta index");
	}
	return index.Cast<ART>();
}

} // namespace

void ScenarioDeltaIndexProbe::Lookup(ClientContext &context, DataChunk &chunk, const vector<idx_t> &key_positions,
                                     vector<char> &ops, vector<row_t> &row_ids) {
	D_ASSERT(delta_table);
	D_ASSERT(key_positions.size() == key_columns.size());
	auto count = chunk.size();
	ops.assign(count, '\0');
	row_ids.assign(count, -1);
	if (count == 0) {
		return;
	}
	auto &storage = delta_table->GetStorage();
	auto types = storage.GetTypes();
	// The probe chunk is in delta layout: key columns hold the keys, every
	// other column is NULL (the key index expressions only read the keys)
	DataChunk probe;
	probe.InitializeEmpty(types);
	for (idx_t col = 0; col < types.size(); col++) {
		probe.data[col].Reference(Value(types[col]));
	}
	for (idx_t k = 0; k < key_columns.size(); k++) {
		auto delta_col = ScenarioDelta::PAYLOAD_START + key_columns[k];
		auto &source = chunk.data[key_positions[k]];
		if (source.GetType() == types[delta_col]) {
			probe.data[delta_col].Reference(source);
		} else {
			Vector cast(types[delta_col], count);
			VectorOperations::Cast(context, source, cast, count);
			probe.data[delta_col].Reference(cast);
		}
	}
	probe.SetCardinality(count);

	auto &transaction = DuckTransaction::Get(context, delta_table->catalog);
	auto &local_storage = LocalStorage::Get(transaction);
	auto local = local_storage.GetStorage(storage);
	ConflictInfo conflict_info(key_ids);
	vector<StorageIndex> op_column {StorageIndex(ScenarioDelta::OP_COL)};

	// Read-only lookups of every key in the key index, mirroring core's index
	// scan (TableScanFunction): besides the main ART, a committed row id can
	// live in deleted_rows_in_use (deleted while older transactions still see
	// it) or added_data_during_checkpoint (appended during a checkpoint). An
	// older snapshot's visible delta row is often only found there.
	auto collect = [&](TableIndexList &indexes, bool global) {
		vector<vector<row_t>> candidates(count);
		bool found = false;
		for (auto &entry : indexes.IndexEntries()) {
			auto &index = *entry.index;
			if (!index.IsBound() || !index.IsUnique() || index.GetIndexType() != ART::TYPE_NAME ||
			    !conflict_info.ConflictTargetMatches(index)) {
				continue;
			}
			lock_guard<mutex> guard(entry.lock);
			CollectCandidates(context, index.Cast<ART>(), probe, candidates);
			if (entry.deleted_rows_in_use) {
				CollectCandidates(context, CheckedART(*entry.deleted_rows_in_use), probe, candidates);
			}
			if (entry.added_data_during_checkpoint) {
				CollectCandidates(context, CheckedART(*entry.added_data_during_checkpoint), probe, candidates);
			}
			found = true;
			break;
		}
		if (!found) {
			return;
		}
		// Keep the one candidate this transaction can see (the key is a PK:
		// at most one delta row per key is visible to any snapshot)
		Vector visible_ids(LogicalType::ROW_TYPE, count);
		auto visible_data = FlatVector::GetData<row_t>(visible_ids);
		vector<idx_t> visible_rows;
		for (idx_t i = 0; i < count; i++) {
			for (auto row_id : candidates[i]) {
				if (global ? storage.CanFetch(transaction, row_id) : local_storage.CanFetch(storage, row_id)) {
					visible_data[visible_rows.size()] = row_id;
					visible_rows.push_back(i);
					break;
				}
			}
		}
		if (visible_rows.empty()) {
			return;
		}
		DataChunk fetched;
		fetched.Initialize(Allocator::Get(context), {LogicalType::VARCHAR}, visible_rows.size());
		ColumnFetchState fetch_state;
		if (global) {
			storage.Fetch(transaction, fetched, op_column, visible_ids, visible_rows.size(), fetch_state);
		} else {
			local_storage.FetchChunk(storage, visible_ids, visible_rows.size(), op_column, fetched, fetch_state);
		}
		if (fetched.size() != visible_rows.size()) {
			throw InternalException("anofox_scenario: delta index probe fetched %llu of %llu rows", fetched.size(),
			                        visible_rows.size());
		}
		for (idx_t i = 0; i < visible_rows.size(); i++) {
			ops[visible_rows[i]] = fetched.GetValue(0, i).GetValue<string>()[0];
			row_ids[visible_rows[i]] = visible_data[i];
		}
	};
	collect(storage.GetDataTableInfo()->GetIndexes(), true);
	if (local) {
		// the transaction's own appends; they supersede any committed hit
		collect(local->append_indexes, false);
	}
}

void ScenarioDeltaIndexProbe::FetchRow(ClientContext &context, row_t row_id, const vector<StorageIndex> &column_ids,
                                       DataChunk &result) {
	D_ASSERT(delta_table);
	auto &storage = delta_table->GetStorage();
	auto &transaction = DuckTransaction::Get(context, delta_table->catalog);
	Vector row_ids(Value::BIGINT(row_id));
	row_ids.Flatten(1);
	ColumnFetchState fetch_state;
	result.Reset();
	if (row_id >= MAX_ROW_ID) {
		LocalStorage::Get(transaction).FetchChunk(storage, row_ids, 1, column_ids, result, fetch_state);
	} else {
		storage.Fetch(transaction, result, column_ids, row_ids, 1, fetch_state);
	}
}

} // namespace duckdb
