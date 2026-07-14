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
class ClientContext;
class DuckTableEntry;
class TableCatalogEntry;

class ScenarioDelta {
public:
	//! Delta column layout: _op, _ts, then all base columns in base order
	static constexpr idx_t OP_COL = 0;
	static constexpr idx_t TS_COL = 1;
	static constexpr idx_t PAYLOAD_START = 2;

	static string DeltaTableName(int64_t scenario_id, const string &table_name);
	//! Materialized base copy: __anofox_scenario.s<id>_mat_<table> (Phase 2)
	static string MatTableName(int64_t scenario_id, const string &table_name);
	//! Logical name of a base table within a scenario: plain for the main
	//! schema, '<schema>.<table>' otherwise. This is the key of the delta/mat
	//! naming contract for multi-schema bases.
	static string LogicalName(const TableCatalogEntry &table);
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
	//! (offset maps source column i -> target column i + offset)
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
	//! delta key map; deltas are small by invariant.
	static string MakeKey(DataChunk &chunk, idx_t row, const vector<idx_t> &key_positions);

	//! Scan the given columns of a table in the caller's transaction,
	//! invoking the callback per row until it returns false. Copes with
	//! freshly created tables and covers transaction-local storage.
	static void ScanTableRows(ClientContext &context, DuckTableEntry &table, vector<StorageIndex> column_ids,
	                          const vector<LogicalType> &scan_types,
	                          const std::function<bool(DataChunk &, idx_t)> &callback);
};

} // namespace duckdb
