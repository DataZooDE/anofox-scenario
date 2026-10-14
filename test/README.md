# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

## C++ tests (`test/cpp`)
Some paths can't be reached from SQL, for example a statement prepared through the client API
and executed in a later transaction, which is what embedding hosts (Rust, C) do. Those tests
are Catch test cases in `test/cpp/*.cpp`. `CMakeLists.txt` adds them to DuckDB's `unittest`
binary. Name each test case `test/cpp/<name>`, so the `make test` filter (`test/*`) picks it
up. Run them on their own with:
```bash
./build/release/test/unittest "test/cpp/*"
```
New files are picked up when CMake re-runs. Touch `CMakeLists.txt` (or reconfigure) after
adding one.
