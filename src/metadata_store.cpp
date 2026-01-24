#include "metadata_store.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

void MetadataStore::Initialize(DatabaseInstance &db) {
	// Only create tables if they don't exist
	if (TablesExist(db)) {
		return;
	}

	CreateRegistryTable(db);
	CreateTablesTable(db);
	CreateBaseRowidsTable(db);
	CreateSnapshotsTable(db);
	CreateProtocolsTable(db);
}

bool MetadataStore::TablesExist(DatabaseInstance &db) {
	Connection con(db);
	auto result = con.Query("SELECT 1 FROM information_schema.tables "
	                        "WHERE table_schema = 'main' AND table_name = '_scenario_registry'");
	return result->RowCount() > 0;
}

void MetadataStore::CreateRegistryTable(DatabaseInstance &db) {
	Connection con(db);
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS _scenario_registry (
			scenario_id INTEGER PRIMARY KEY,
			scenario_name VARCHAR NOT NULL UNIQUE,
			schema_name VARCHAR NOT NULL UNIQUE,
			base_schema VARCHAR NOT NULL,
			base_captured_at TIMESTAMP NOT NULL,
			created_at TIMESTAMP DEFAULT current_timestamp,
			status VARCHAR DEFAULT 'active' CHECK (status IN ('active', 'archived')),
			description VARCHAR,
			parent_scenario_id INTEGER REFERENCES _scenario_registry(scenario_id)
		)
	)");
}

void MetadataStore::CreateTablesTable(DatabaseInstance &db) {
	Connection con(db);
	// Note: FK constraint removed to avoid issues with scalar function execution model.
	// Referential integrity is managed by the application layer (scenario_drop handles cascading deletes).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS _scenario_tables (
			scenario_id INTEGER NOT NULL,
			table_name VARCHAR NOT NULL,
			base_row_count BIGINT,
			has_primary_key BOOLEAN,
			primary_key_columns VARCHAR[],
			created_at TIMESTAMP DEFAULT current_timestamp,
			PRIMARY KEY (scenario_id, table_name)
		)
	)");
}

void MetadataStore::CreateBaseRowidsTable(DatabaseInstance &db) {
	Connection con(db);
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS _scenario_base_rowids (
			scenario_id INTEGER NOT NULL,
			table_name VARCHAR NOT NULL,
			base_rowid BIGINT NOT NULL,
			PRIMARY KEY (scenario_id, table_name, base_rowid)
		)
	)");
}

void MetadataStore::CreateSnapshotsTable(DatabaseInstance &db) {
	Connection con(db);
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS _scenario_snapshots (
			snapshot_id INTEGER PRIMARY KEY,
			snapshot_name VARCHAR NOT NULL UNIQUE,
			source_schema VARCHAR NOT NULL,
			created_at TIMESTAMP DEFAULT current_timestamp,
			description VARCHAR,
			size_bytes BIGINT
		)
	)");
}

void MetadataStore::CreateProtocolsTable(DatabaseInstance &db) {
	Connection con(db);
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS _scenario_protocols (
			entity_type VARCHAR NOT NULL CHECK (entity_type IN ('scenario', 'snapshot')),
			entity_name VARCHAR NOT NULL,
			section VARCHAR NOT NULL CHECK (section IN ('why', 'changes', 'findings', 'plan', 'decision', 'metadata')),
			content VARCHAR,
			updated_at TIMESTAMP DEFAULT current_timestamp,
			PRIMARY KEY (entity_type, entity_name, section)
		)
	)");
}

} // namespace duckdb
