// Issue #55: the internal-schema guard on client-API prepared statements, which
// the sqllogictest runner cannot reach (SQL-level EXECUTE re-plans through the
// binder). A statement prepared through the client API is planned once and
// executed later:
// - prepared while anofox_scenario_allow_internal_writes was on, executed after
//   it was turned off (allowed internal writes are re-planned per execution);
// - a connection that was open before the extension was loaded is guarded.

#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/extension_helper.hpp"

using namespace duckdb;

namespace {

//! See test_prepared_insert.cpp: the linked copy (DuckDB 1.5) or the built
//! file (2.0); false (skip) on a DuckDB version mismatch
bool LoadGuardTestExtension(DuckDB &db) {
	if (ExtensionHelper::LoadExtension(db, "anofox_scenario") == ExtensionLoadResult::LOADED_EXTENSION) {
		return true;
	}
	Connection con(db);
	auto result = con.Query(string("LOAD '") + ANOFOX_SCENARIO_EXTENSION_FILE + "'");
	if (result->HasError() && result->GetError().find("was built specifically for DuckDB version") != string::npos) {
		WARN("skipped: " << result->GetError());
		return false;
	}
	REQUIRE_NO_FAIL(*result);
	return true;
}

bool IsGuardError(QueryResult &result) {
	return result.HasError() && result.GetError().find("Cannot modify __anofox_scenario.registry") != string::npos;
}

int64_t FrozenCount(Connection &con) {
	auto result = con.Query("SELECT count(*) FROM __anofox_scenario.registry WHERE frozen");
	REQUIRE_NO_FAIL(*result);
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	return chunk->GetValue(0, 0).GetValue<int64_t>();
}

} // namespace

TEST_CASE("test/cpp/internal_guard_prepared", "[anofox_scenario]") {
	auto db_path = TestCreatePath("anofox_scenario_internal_guard.db");
	DeleteDatabase(db_path);

	{
		auto config = GetTestConfig();
		DuckDB db(db_path, config.get());
		if (!LoadGuardTestExtension(db)) {
			return;
		}
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE forecast (id INTEGER PRIMARY KEY, qty INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CALL scenario_create('evidence')"));
		REQUIRE_NO_FAIL(con.Query("CALL scenario_freeze('evidence')"));

		// prepared with the escape hatch on, executed after it was turned off
		REQUIRE_NO_FAIL(con.Query("SET anofox_scenario_allow_internal_writes = true"));
		auto flip = con.Prepare("UPDATE __anofox_scenario.registry SET frozen = false");
		REQUIRE(!flip->HasError());
		REQUIRE_NO_FAIL(con.Query("RESET anofox_scenario_allow_internal_writes"));
		auto rejected = flip->Execute();
		REQUIRE(IsGuardError(*rejected));
		REQUIRE(FrozenCount(con) == 1);

		// and with it on again, the same handle runs (the escape hatch works)
		REQUIRE_NO_FAIL(con.Query("SET anofox_scenario_allow_internal_writes = true"));
		REQUIRE_NO_FAIL(*flip->Execute());
		REQUIRE(FrozenCount(con) == 0);
		REQUIRE_NO_FAIL(con.Query("RESET anofox_scenario_allow_internal_writes"));
		REQUIRE_NO_FAIL(con.Query("CALL scenario_freeze('evidence')"));

		// a parameterized statement is planned at its first execution
		auto by_name = con.Prepare("UPDATE __anofox_scenario.registry SET frozen = false WHERE name = $1");
		REQUIRE(!by_name->HasError());
		REQUIRE(IsGuardError(*by_name->Execute(Value("evidence"))));
		REQUIRE(FrozenCount(con) == 1);
	}

	// a connection opened before the extension was loaded is guarded once it
	// is (on DuckDB 1.5 unittest links the extension in and loads it at
	// startup, so this only exercises LOAD on 2.0)
	{
		auto config = GetTestConfig();
		DuckDB db(db_path, config.get());
		Connection early(db);
		if (!LoadGuardTestExtension(db)) {
			return;
		}
		REQUIRE(IsGuardError(*early.Query("UPDATE __anofox_scenario.registry SET frozen = false")));
		REQUIRE(FrozenCount(early) == 1);
	}
	DeleteDatabase(db_path);
}
