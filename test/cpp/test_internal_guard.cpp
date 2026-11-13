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

// CREATE TRIGGER exists only on DuckDB 2.0, which plans it with an operator
// type 1.5 lacks (duckdb_compat/create_trigger.hpp). On 1.5 the statement does
// not parse and the test case is skipped. In memory: 2.0 refuses triggers in
// database files of an older storage version.
TEST_CASE("test/cpp/internal_guard_trigger", "[anofox_scenario]") {
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	if (!LoadGuardTestExtension(db)) {
		return;
	}
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE forecast (id INTEGER PRIMARY KEY, qty INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE audit_log (n INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CALL scenario_create('evidence')"));
	REQUIRE_NO_FAIL(con.Query("CALL scenario_freeze('evidence')"));

	// a trigger on a user table is not the guard's business
	auto user_trigger = con.Query("CREATE TRIGGER forecast_audit AFTER INSERT ON forecast FOR EACH STATEMENT "
	                              "INSERT INTO audit_log VALUES (1)");
	if (user_trigger->HasError() && user_trigger->GetError().find("syntax error") != string::npos) {
		WARN("skipped: this DuckDB has no CREATE TRIGGER (" << user_trigger->GetError() << ")");
		return;
	}
	REQUIRE_NO_FAIL(*user_trigger);
	REQUIRE_NO_FAIL(con.Query("INSERT INTO forecast VALUES (1, 100)"));

	// a trigger on an internal table: rejected, schema-qualified or not
	auto on_registry = con.Query("CREATE TRIGGER registry_audit AFTER UPDATE ON __anofox_scenario.registry "
	                             "FOR EACH STATEMENT INSERT INTO audit_log VALUES (2)");
	REQUIRE(on_registry->HasError());
	REQUIRE(on_registry->GetError().find("Cannot modify __anofox_scenario.registry with SQL (CREATE TRIGGER)") !=
	        string::npos);
	REQUIRE_NO_FAIL(con.Query("SET search_path = '__anofox_scenario,main'"));
	auto unqualified = con.Query("CREATE TRIGGER registry_audit AFTER DELETE ON registry "
	                             "FOR EACH STATEMENT INSERT INTO main.audit_log VALUES (3)");
	REQUIRE(unqualified->HasError());
	REQUIRE(unqualified->GetError().find("Cannot modify __anofox_scenario.registry with SQL (CREATE TRIGGER)") !=
	        string::npos);
	REQUIRE_NO_FAIL(con.Query("RESET search_path"));
	REQUIRE(FrozenCount(con) == 1);
	auto triggers = con.Query("SELECT count(*) FROM duckdb_triggers() WHERE schema_name = '__anofox_scenario'");
	if (!triggers->HasError()) {
		auto chunk = triggers->Fetch();
		REQUIRE(chunk);
		REQUIRE(chunk->GetValue(0, 0).GetValue<int64_t>() == 0);
	}

	// a trigger on a user table whose body writes an internal table: the body
	// is bound into every statement that fires it, so those are rejected
	REQUIRE_NO_FAIL(con.Query("CREATE TRIGGER thaw AFTER INSERT ON audit_log FOR EACH STATEMENT "
	                          "UPDATE __anofox_scenario.registry SET frozen = false"));
	auto fired = con.Query("INSERT INTO audit_log VALUES (4)");
	REQUIRE(fired->HasError());
	REQUIRE(fired->GetError().find("Cannot modify __anofox_scenario.registry with SQL (UPDATE)") != string::npos);
	REQUIRE(FrozenCount(con) == 1);
}
