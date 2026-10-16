//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_delta.hpp
//
// Delta storage contract. One delta table per modified scenario table:
// __anofox_scenario.s<id>_delta_<table>(_op, _ts, <base columns>) with the
// base PK as its PRIMARY KEY. The delta is a changelog consumed by three
// later phases (diff, branching, merge-back) -- its shape and the
// op-transition matrix are a frozen contract.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/storage/storage_index.hpp"

namespace duckdb {

class Catalog;
class ColumnList;
class ClientContext;
class DuckTableEntry;
class TableCatalogEntry;

class ScenarioDelta {
public:
	//! Delta column layout: _op, _ts, then all base columns in base order.
	//! Keyless tables (no PK, no declared key) append a trailing _count
	//! BIGINT: the delta is a bag changelog (I/D rows with multiplicities,
	//! aggregated on read) because whole-row identity cannot be a PK (NULLs).
	static constexpr idx_t OP_COL = 0;
	static constexpr idx_t TS_COL = 1;
	static constexpr idx_t PAYLOAD_START = 2;
	static constexpr const char *COUNT_COLUMN = "_count";

	//! Position of the trailing multiplicity column, if this delta has one.
	//! Detected structurally, never by name: a bag delta has no PRIMARY KEY
	//! and ends in a BIGINT column carrying a NOT NULL constraint. Payload
	//! columns never carry NOT NULL in a delta (base constraints are not
	//! mirrored), so a user column that happens to be named _count - or a
	//! pre-v0.4.1 delta whose last payload column is - is never mistaken for
	//! the multiplicity. Every reader goes through this helper.
	static optional_idx CountColumnIndex(const TableCatalogEntry &delta_table);
	//! Name of the multiplicity column (normally _count), or empty when the
	//! delta has none; for SQL that addresses it by name
	static string CountColumnName(const TableCatalogEntry &delta_table);
	//! Name for a new multiplicity column over the given payload columns:
	//! _count, prefixed with further underscores while a payload column
	//! already uses the name (a keyless base table with its own _count)
	static string NewCountColumnName(const ColumnList &payload_columns);

	//! A keyless delta written before v0.4.1: no PRIMARY KEY and no trailing
	//! multiplicity column. Such deltas only ever held 'I' rows (keyless
	//! UPDATE/DELETE was gated), so readers treat them as an implicit _count
	//! of 1.
	static bool IsLegacyKeylessDelta(const TableCatalogEntry &delta_table);
	//! True when the scenario owns at least one legacy keyless delta
	static bool HasLegacyKeylessDeltas(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);
	//! Rebuild every legacy keyless delta of a scenario into the current bag
	//! layout (trailing _count BIGINT NOT NULL = 1 per old row, mirrored
	//! UNIQUE indexes dropped), in the caller's transaction. The caller must
	//! have registered the host write. Returns the number of upgraded tables.
	static idx_t UpgradeLegacyKeylessDeltas(ClientContext &context, Catalog &host_catalog, int64_t scenario_id);
	//! Upgrade at ATTACH, opportunistically: registers the host write and
	//! upgrades only when that is safe - a writable host, an attach that is
	//! not READ_ONLY, an autocommit statement (or a transaction that already
	//! writes the host), no older transaction active on the host that might
	//! hold uncommitted writes to the legacy delta, and no catalog conflict.
	//! Otherwise the legacy layout stays (readers handle it as _count = 1)
	//! and ATTACH never fails on its account. Returns false when legacy
	//! deltas remain.
	static bool TryUpgradeLegacyKeylessDeltas(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                                          bool read_only_attach);

	static string DeltaTableName(int64_t scenario_id, const string &table_name);
	//! Materialized base copy: __anofox_scenario.s<id>_mat_<table> (Phase 2)
	static string MatTableName(int64_t scenario_id, const string &table_name);
	//! Logical name of a base table within a scenario: plain for the main
	//! schema, '<schema>.<table>' otherwise. A part containing '.' (or
	//! starting with '"') is double-quoted SQL-style ('"a.b"' = main table
	//! a.b, 'a.b' = schema a, table b); every other name is spelled as is, so
	//! existing databases keep their names. This is the key of the delta/mat
	//! naming contract, and the table name users pass to scenario_diff and
	//! key_columns.
	static string LogicalName(const TableCatalogEntry &table);
	static string EncodeLogicalName(const string &schema_name, const string &table_name);
	//! Split a logical name back into (schema, table)
	static void SplitLogicalName(const string &logical_name, string &schema_name, string &table_name);
	//! The delta table for a scenario table, if any writes happened yet
	static optional_ptr<DuckTableEntry> TryGetDeltaTable(ClientContext &context, Catalog &host_catalog,
	                                                     int64_t scenario_id, const string &table_name);
	//! The materialized base copy for a scenario table, if any
	static optional_ptr<DuckTableEntry> TryGetMatTable(ClientContext &context, Catalog &host_catalog,
	                                                   int64_t scenario_id, const string &table_name);
	//! Create the delta table lazily (caller's transaction); returns it.
	//! declared_keys (tables without a PK) become the delta's PRIMARY KEY -
	//! that constraint IS the persisted key declaration.
	static DuckTableEntry &EnsureDeltaTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                                        TableCatalogEntry &base_entry,
	                                        const vector<string> *declared_keys = nullptr,
	                                        const string *logical_name_override = nullptr);
	//! Create the materialized copy of a base table (schema incl. PK) and
	//! bulk-copy its rows, all in the caller's transaction
	static DuckTableEntry &CreateMatTable(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                                      TableCatalogEntry &base_entry, const string *logical_name_override = nullptr);
	//! Vectorized row copy between two tables with identical column layout
	//! (offset maps source column i -> target column i + offset). A legacy
	//! keyless delta copied into a bag delta (branching from a scenario that
	//! was never upgraded) gets a multiplicity of 1 per row, leaving the
	//! source untouched.
	static void CopyTableData(ClientContext &context, DuckTableEntry &from_table, DuckTableEntry &to_table,
	                          idx_t target_column_offset = 0);

	//! Logical column ids of the base table's PRIMARY KEY (empty when none)
	static vector<idx_t> GetPKColumns(const TableCatalogEntry &base_entry);
	//! Row identity for a scenario table: the base PK, or the key columns
	//! declared at scenario_create (persisted as the delta table's PK)
	static vector<idx_t> GetKeyColumns(ClientContext &context, Catalog &host_catalog, int64_t scenario_id,
	                                   const string &logical_name, TableCatalogEntry &base_entry);

	//! Serialize the key columns of a row into a canonical comparable string
	//! (length-prefixed, NULL-tagged). Used for suppression sets and the
	//! delta key map (keyed deltas resolve keys via ScenarioDeltaIndexProbe).
	static string MakeKey(DataChunk &chunk, idx_t row, const vector<idx_t> &key_positions);

	//! Scan the given columns of a table in the caller's transaction,
	//! invoking the callback per row until it returns false. Copes with
	//! freshly created tables and covers transaction-local storage.
	static void ScanTableRows(ClientContext &context, DuckTableEntry &table, vector<StorageIndex> column_ids,
	                          const vector<LogicalType> &scan_types,
	                          const std::function<bool(DataChunk &, idx_t)> &callback);
	//! Chunk-at-a-time variant of ScanTableRows (vectorized consumers)
	static void ScanTableChunks(ClientContext &context, DuckTableEntry &table, vector<StorageIndex> column_ids,
	                            const vector<LogicalType> &scan_types, const std::function<void(DataChunk &)> &callback);
};

//! Point lookups of keys in a keyed delta through its PRIMARY KEY index, so
//! a statement touching a few rows does not pay for the whole delta (#36).
//! Uses the two-phase lookup of core's INSERT ... ON CONFLICT: the committed
//! index, then the transaction's own appends; hits on rows the transaction
//! cannot see (deleted by it, or by a committed transaction) are dropped.
//! Lookup is thread-safe for concurrent readers once initialized.
class ScenarioDeltaIndexProbe {
public:
	//! False when the delta has no bound unique ART on exactly the key
	//! columns; callers then fall back to scanning the delta
	bool Initialize(ClientContext &context, DuckTableEntry &delta_table, const vector<idx_t> &key_columns);
	bool IsInitialized() const {
		return delta_table != nullptr;
	}
	//! For every row of `chunk` (key values at `key_positions`, in key order):
	//! the _op of its visible delta row ('\0' = none) and that row's rowid
	void Lookup(ClientContext &context, DataChunk &chunk, const vector<idx_t> &key_positions, vector<char> &ops,
	            vector<row_t> &row_ids);

private:
	optional_ptr<DuckTableEntry> delta_table;
	vector<idx_t> key_columns;
	unordered_set<column_t> key_ids;
};

} // namespace duckdb
