// Regression tests for issue #22: INSERTs into scenario tables issued through
// the client API the way an embedding host does it (Rust `duckdb` crate, C API):
// one statement prepared once, executed per row in separate autocommit
// transactions. The sqllogictest runner cannot reach this path - SQL-level
// PREPARE/EXECUTE re-binds on its own.
//
// The schema mirrors the reporting host: a key/value run summary plus a
// composite-key order plan whose NOT NULL/CHECK constraints are declared before
// the PRIMARY KEY (the declaration order that tripped the base PK probe) and
// which references a supplier master (materialized copies of FK tables, #24).

#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

const char *const METRICS[] = {"total_cost",    "scored_cost",         "committed_unit_cost",
                               "orders_placed", "aggregate_fill_rate", "feasible"};
constexpr idx_t METRIC_COUNT = 6;

// Base plan: 500 suppliers x 100 periods
constexpr int64_t BASE_SUPPLIERS = 500;
constexpr int32_t PERIODS = 100;

void RequireSuccess(QueryResult &result, const string &what) {
	if (result.HasError()) {
		FAIL(what << ": " << result.GetError());
	}
}

int64_t QueryInt(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	RequireSuccess(*result, sql);
	return result->GetValue(0, 0).GetValue<int64_t>();
}

void SetupBase(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("LOAD anofox_scenario"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE p1d_summary (metric VARCHAR PRIMARY KEY, value DOUBLE NOT NULL)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE suppliers (supplier_id BIGINT PRIMARY KEY, name VARCHAR NOT NULL)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE order_plan (supplier_id BIGINT, period INTEGER, sku VARCHAR NOT NULL, "
	                          "qty INTEGER NOT NULL CHECK (qty >= 0), unit_cost DOUBLE NOT NULL, "
	                          "FOREIGN KEY (supplier_id) REFERENCES suppliers(supplier_id), "
	                          "PRIMARY KEY (supplier_id, period))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO suppliers SELECT s, 'Supplier ' || s FROM range(1, " +
	                          std::to_string(BASE_SUPPLIERS + 1) + ") r(s)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO p1d_summary VALUES ('total_cost', 1250000.0), ('scored_cost', 1180000.0), "
	                          "('committed_unit_cost', 12.75), ('orders_placed', 50000), "
	                          "('aggregate_fill_rate', 0.93), ('feasible', 1)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO order_plan SELECT s, p, 'SKU-' || lpad((s * 7 % 997)::VARCHAR, 4, '0'), "
	                          "((s * 31 + p * 17) % 400)::INTEGER, round(5 + ((s * 13 + p) % 2000) / 100.0, 2) "
	                          "FROM range(1, " +
	                          std::to_string(BASE_SUPPLIERS + 1) + ") r(s), range(1, " + std::to_string(PERIODS + 1) +
	                          ") q(p)"));
	// The reporter's chain: a frozen materialized seed, a delta fork that
	// replaces the whole plan
	REQUIRE_NO_FAIL(con.Query("CALL scenario_create('seed', 'solver input', mode := 'materialized')"));
	REQUIRE_NO_FAIL(con.Query("CALL scenario_freeze('seed')"));
	REQUIRE_NO_FAIL(con.Query("CALL scenario_create('winner', 'chosen plan', from_scenario := 'seed')"));
}

} // namespace

TEST_CASE("test/cpp/prepared_insert_across_transactions", "[anofox_scenario]") {
	auto db_path = TestCreatePath("anofox_scenario_prepared_insert.db");
	DeleteDatabase(db_path);

	int64_t expected_qty = 0;
	idx_t expected_rows = 0;
	{
		DuckDB db(db_path);
		Connection con(db);
		SetupBase(con);
		REQUIRE_NO_FAIL(con.Query("ATTACH 'winner' AS ws (TYPE scenario)"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM ws.p1d_summary"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM ws.order_plan"));

		// --- run summary: prepared once, one autocommit transaction per row ---------
		auto summary = con.Prepare("INSERT INTO ws.p1d_summary (metric, value) VALUES ($1, $2)");
		REQUIRE(!summary->HasError());
		for (idx_t i = 0; i < METRIC_COUNT; i++) {
			auto result = summary->Execute(Value(METRICS[i]), Value::DOUBLE(double(i) * 1.5));
			RequireSuccess(*result, string("summary execute ") + METRICS[i]);
		}
		REQUIRE(QueryInt(con, "SELECT count(*) FROM ws.p1d_summary") == int64_t(METRIC_COUNT));
		REQUIRE(QueryInt(con, "SELECT count(*) FROM ws.p1d_summary WHERE value = 3.0 AND metric = "
		                      "'committed_unit_cost'") == 1);

		// --- order plan: new composite keys plus re-inserted (deleted) base keys ----
		auto plan = con.Prepare("INSERT INTO ws.order_plan (supplier_id, period, sku, qty, unit_cost) "
		                        "VALUES ($1, $2, $3, $4, $5)");
		REQUIRE(!plan->HasError());
		auto insert_plan_row = [&](int64_t supplier, int32_t period) {
			auto qty = int32_t((supplier * 11 + period * 3) % 250);
			auto result = plan->Execute(Value::BIGINT(supplier), Value::INTEGER(period),
			                            Value("SKU-" + std::to_string(supplier % 997)), Value::INTEGER(qty),
			                            Value::DOUBLE(7.25 + double(period) / 100.0));
			RequireSuccess(*result, "plan execute " + std::to_string(supplier) + "/" + std::to_string(period));
			expected_qty += qty;
			expected_rows++;
		};
		// 10 brand-new suppliers (keys never in the base; not in the supplier
		// master either - FKs are enforced at merge-back, not in the scenario) ...
		for (int64_t supplier = 1001; supplier <= 1010; supplier++) {
			for (int32_t period = 1; period <= PERIODS; period++) {
				insert_plan_row(supplier, period);
			}
		}
		// ... and 5 base suppliers re-planned after the DELETE (D -> U transitions)
		for (int64_t supplier = 1; supplier <= 5; supplier++) {
			for (int32_t period = 1; period <= PERIODS; period++) {
				insert_plan_row(supplier, period);
			}
		}

		// --- the same prepared statement inside one explicit transaction ------------
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		for (int32_t period = 1; period <= PERIODS; period++) {
			insert_plan_row(1011, period);
		}
		REQUIRE_NO_FAIL(con.Query("COMMIT"));

		REQUIRE(QueryInt(con, "SELECT count(*) FROM ws.order_plan") == int64_t(expected_rows));
		REQUIRE(QueryInt(con, "SELECT sum(qty) FROM ws.order_plan") == expected_qty);

		// the base is untouched
		REQUIRE(QueryInt(con, "SELECT count(*) FROM order_plan") == BASE_SUPPLIERS * PERIODS);
		REQUIRE(QueryInt(con, "SELECT count(*) FROM p1d_summary WHERE metric = 'orders_placed' AND value = 50000") ==
		        1);

		// the changelog agrees with what was written
		REQUIRE(QueryInt(con, "SELECT rows_added FROM scenario_diff_summary('winner') WHERE table_name = "
		                      "'order_plan'") == 11 * PERIODS);
		REQUIRE(QueryInt(con, "SELECT rows_modified FROM scenario_diff_summary('winner') WHERE table_name = "
		                      "'order_plan'") == 5 * PERIODS);
		REQUIRE(QueryInt(con, "SELECT rows_removed FROM scenario_diff_summary('winner') WHERE table_name = "
		                      "'order_plan'") == (BASE_SUPPLIERS - 5) * PERIODS);

		// --- a plan prepared before scenario_freeze must not write afterwards -------
		REQUIRE_NO_FAIL(con.Query("CALL scenario_freeze('winner')"));
		auto frozen = summary->Execute(Value("late_metric"), Value::DOUBLE(1.0));
		REQUIRE(frozen->HasError());
		REQUIRE(StringUtil::Contains(frozen->GetError(), "frozen"));
		REQUIRE(QueryInt(con, "SELECT count(*) FROM ws.p1d_summary") == int64_t(METRIC_COUNT));
	}

	// --- durable across a restart ----------------------------------------------------
	{
		DuckDB db(db_path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("LOAD anofox_scenario"));
		REQUIRE_NO_FAIL(con.Query("ATTACH 'winner' AS ws (TYPE scenario)"));
		REQUIRE(QueryInt(con, "SELECT count(*) FROM ws.order_plan") == int64_t(expected_rows));
		REQUIRE(QueryInt(con, "SELECT sum(qty) FROM ws.order_plan") == expected_qty);
		REQUIRE(QueryInt(con, "SELECT count(*) FROM ws.p1d_summary") == int64_t(METRIC_COUNT));
	}
	DeleteDatabase(db_path);
}
